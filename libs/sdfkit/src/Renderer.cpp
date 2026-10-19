#include "sdfkit/Renderer.h"

#include "ffglkit/GLState.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <ffgl/FFGLLog.h>

namespace sdfkit
{
namespace
{
constexpr int kAttribs = 10;// vec4 attributes per instance (ShapeInstance)
static_assert( sizeof( ShapeInstance ) == kAttribs * 4 * sizeof( float ), "ShapeInstance layout" );

// ---------------------------------------------------------------------------
// Field passes: one vertex shader for every primitive
//
// Glyphs may turn in 3D (rotation X/Y around their centre), move towards the
// back (depth) and slant (skew). With a perspective, the vertices are divided
// in clip space, so everything the fragment shader interpolates stays
// perspective-correct (atlas coordinates, rest positions for wipes).
// ---------------------------------------------------------------------------
const char* kShapeVS = R"GLSL(#version 410 core
layout( location = 0 ) in vec2 aCorner;       // 0..1
layout( location = 1 ) in vec4 iQuad;         // l, b, r, t  (shape units, y up)
layout( location = 2 ) in vec4 iUV;           // u0, v0, u1, v1 (atlas texels)
layout( location = 3 ) in vec4 iPosScaleRot;  // x, y (px, y down), scale, rotation
layout( location = 4 ) in vec4 iMisc;         // alpha, index, pxPerAtlasPx, spreadAtlasPx
layout( location = 5 ) in vec4 iTint;         // rgb, amount
layout( location = 6 ) in vec4 iRest;         // rest x, y (layout space), rest scale, -
layout( location = 7 ) in vec4 iClip;         // visible rectangle in layout space
layout( location = 8 ) in vec4 iBox;          // box half width, half height, radius, slant
layout( location = 9 ) in vec4 iXform;        // glyphs: rotation x, rotation y (radians), depth (px), skew (tan)
layout( location = 10 ) in vec4 iFx;          // glyphs: blur (px), weight (px)

uniform vec2 uViewport;    // px
uniform vec2 uOrigin;      // anchor position, px, y down
uniform vec2 uBlockRot;    // cos, sin of the block rotation (screen frame)
uniform float uSize;       // px per shape unit (font size for glyphs, 1 for boxes)
uniform int uKind;         // 0 glyph, 1 box
uniform float uPerspective;// eye distance in px, 0 = parallel

out vec2 vUV;
out vec2 vLocal;           // px from the shape origin, y down, before animation scale and rotation
out vec2 vRest;            // layout space
out vec2 vBlock;           // block space (after animation)
flat out vec4 vMisc;
flat out vec4 vTint;
flat out vec4 vClip;
flat out vec4 vBox;
flat out vec4 vFx;
flat out float vId;
flat out float vScale;
flat out float vProjected;

vec2 rotate( vec2 p, vec2 cs ) { return vec2( cs.x * p.x - cs.y * p.y, cs.y * p.x + cs.x * p.y ); }

void main()
{
	vec2 q        = mix( iQuad.xy, iQuad.zw, aCorner );
	vec2 unscaled = vec2( q.x, -q.y ) * uSize;
	vec4 xf       = uKind == 0 ? iXform : vec4( 0.0 );
	vec2 local    = unscaled * iPosScaleRot.z;
	local.x      -= local.y * xf.w;// slant: y is down, so the top moves right
	vec2 cs       = vec2( cos( iPosScaleRot.w ), sin( iPosScaleRot.w ) );
	local         = rotate( local, cs );
	vec3 p3       = vec3( local, 0.0 );
	if( xf.x != 0.0 || xf.y != 0.0 )
	{
		// turn around the glyph's centre
		vec2 c = vec2( 0.5 * ( iQuad.x + iQuad.z ), -0.5 * ( iQuad.y + iQuad.w ) ) * uSize * iPosScaleRot.z;
		c.x   -= c.y * xf.w;
		c      = rotate( c, cs );
		p3.xy -= c;
		float cx = cos( xf.x ), sx = sin( xf.x );
		p3       = vec3( p3.x, cx * p3.y + sx * p3.z, -sx * p3.y + cx * p3.z );// top (y < 0) goes away (z > 0)
		float cy = cos( xf.y ), sy = sin( xf.y );
		p3       = vec3( cy * p3.x - sy * p3.z, p3.y, sy * p3.x + cy * p3.z );// right side goes away
		p3.xy   += c;
	}
	p3.z      += xf.z;
	vec2 pb    = iPosScaleRot.xy + p3.xy;
	vec2 p     = rotate( pb, uBlockRot ) + uOrigin;
	// Perspective towards the viewport centre: written as clip coordinates with
	// w = (eye + z) / eye, so interpolation stays perspective-correct.
	float w    = uPerspective > 0.0 ? max( ( uPerspective + p3.z ) / uPerspective, 0.05 ) : 1.0;
	vec2 c     = 0.5 * uViewport;
	vec2 pc    = c * w + ( p - c );
	gl_Position = vec4( pc.x / uViewport.x * 2.0 - w, w - pc.y / uViewport.y * 2.0, 0.0, w );
	vUV    = mix( iUV.xy, iUV.zw, aCorner );
	vLocal = unscaled;
	vRest  = iRest.xy + unscaled * iRest.z;
	vBlock = pb;
	vMisc  = iMisc;
	vTint  = iTint;
	vClip  = iClip;
	vBox   = iBox;
	vFx    = uKind == 0 ? iFx : vec4( 0.0 );
	vScale = iPosScaleRot.z;
	vProjected = ( xf.x != 0.0 || xf.y != 0.0 || xf.w != 0.0 || ( xf.z != 0.0 && uPerspective > 0.0 ) ) ? 1.0 : 0.0;
	vId    = float( gl_InstanceID % 2048 );// exact in half float
}
)GLSL";

// Two ways to fill the layers:
//  uPeelMode 0 (same opacity everywhere, colours differ): layer k keeps the
//    k-th NEAREST shape (depth = distance, shapes of earlier layers skipped).
//  uPeelMode 1 (opacities differ): layer k keeps the most OPAQUE shape among
//    those strictly closer than layer k-1's shape, nearest first on ties. The
//    layers then hold exactly the shapes that matter for the opacity integral
//    (the front "more opaque or closer"); a shape that is both farther and
//    less opaque than another can never show. However many shapes overlap, an
//    opaque shape is never pushed out by fainter neighbours.
// Masks (wipes, the window) lower a shape's opacity per pixel, so they take
// part in the same integral.
const char* kFieldFS = R"GLSL(#version 410 core
uniform sampler2D uAtlas;
uniform vec2 uAtlasSize;
uniform float uSharpCorners;// 1 = MSDF everywhere (mitred corners), 0 = true distance away from the edge (round)
uniform int uKind;          // 0 glyph, 1 box
uniform int uClipMode;      // 0 = mask (glyphs), 1 = cut (boxes)
uniform vec4 uWindow;       // block space
uniform float uWindowFade;  // px; < 0 = no window
uniform float uDepthRange;
uniform float uMaxReach;// px: beyond this no effect can show, so the shape takes no layer
uniform int uLayer;
uniform int uPeelMode;
uniform sampler2D uPrev0;
uniform sampler2D uPrev1;
uniform sampler2D uPrev2;

in vec2 vUV;
in vec2 vLocal;
in vec2 vRest;
in vec2 vBlock;
flat in vec4 vMisc;
flat in vec4 vTint;
flat in vec4 vClip;
flat in vec4 vBox;
flat in vec4 vFx;
flat in float vId;
flat in float vScale;
flat in float vProjected;

layout( location = 0 ) out vec4 oA;// distance, shape id, opacity, tint amount
layout( location = 1 ) out vec4 oB;// tint colour, blur / 64 px

float median( vec3 v ) { return max( min( v.r, v.g ), min( max( v.r, v.g ), v.b ) ); }

// Signed distance to this glyph's outline in shape pixels (before projection), outside positive.
float glyphDistance()
{
	vec4 s      = texture( uAtlas, vUV / uAtlasSize );
	float range = 2.0 * vMisc.w;
	float dMsd  = ( median( s.rgb ) - 0.5 ) * range;// sharp corners, exact near the outline
	float dTrue = ( s.a - 0.5 ) * range;            // true distance: round offsets further out
	// The true distance is smoothed by bilinear filtering within ~1 texel of a
	// corner, so switch from MSDF to it 1..2 atlas texels away from the outline.
	float d = mix( dMsd, dTrue, ( 1.0 - uSharpCorners ) * smoothstep( 1.0, 2.0, abs( dTrue ) ) );
	return -d * vMisc.z;
}

// Exact distance to a parallelogram (Inigo Quilez): half width wi, half height he,
// horizontal offset sk of the top edge. y up.
float sdParallelogram( vec2 p, float wi, float he, float sk )
{
	vec2 e  = vec2( sk, he );
	p       = ( p.y < 0.0 ) ? -p : p;
	vec2 w  = p - e;
	w.x    -= clamp( w.x, -wi, wi );
	vec2 d  = vec2( dot( w, w ), -w.y );
	float s = p.x * e.y - p.y * e.x;
	p       = ( s < 0.0 ) ? -p : p;
	vec2 v  = p - vec2( wi, 0.0 );
	v      -= e * clamp( dot( v, e ) / dot( e, e ), -1.0, 1.0 );
	d       = min( d, vec2( dot( v, v ), wi * he - abs( s ) ) );
	return sqrt( d.x ) * sign( -d.y );
}

// Rounded, slanted box around the origin, in output pixels.
float boxDistancePx()
{
	vec2 p   = vec2( vLocal.x, -vLocal.y );// y up
	float s  = vBox.w;                     // horizontal shift per unit of height
	float k  = sqrt( 1.0 + s * s );
	float r  = clamp( vBox.z, 0.0, min( vBox.y, vBox.x / k ) );
	float he = max( vBox.y - r, 1e-3 );
	float wi = max( vBox.x - r * k, 1e-3 );
	return ( sdParallelogram( p, wi, he, s * he ) - r ) * vScale;
}

// Signed distance to a rectangle given by its edges. Computed edge by edge: an
// "open" edge at 1e9 would lose all precision in a centre/half-size form.
float rectDistance( vec2 p, vec4 rc )
{
	vec2 d = max( rc.xy - p, p - rc.zw );
	return length( max( d, 0.0 ) ) + min( max( d.x, d.y ), 0.0 );
}

void main()
{
	float gd = glyphDistance();
	// Projected or slanted glyphs: shape pixels are no longer output pixels.
	// Dividing by the screen-space gradient converts the distance to output
	// pixels (exact for the true distance, which has gradient 1 in shape space).
	// Derivatives are taken before any branch.
	float gg = length( vec2( dFdx( gd ), dFdy( gd ) ) );
	if( vProjected > 0.5 )
		gd /= clamp( gg, 0.05, 20.0 );
	float dist  = uKind == 0 ? gd - vFx.y : boxDistancePx();
	float alpha = vMisc.x;
	if( vClip.x > -1.0e8 || vClip.y > -1.0e8 || vClip.z < 1.0e8 || vClip.w < 1.0e8 )
	{
		float dc = rectDistance( vRest, vClip );
		if( uClipMode == 1 )
			dist = max( dist, dc * vScale );
		else
			alpha *= clamp( 0.5 - dc, 0.0, 1.0 );
	}
	if( uWindowFade >= 0.0 )
	{
		float dw = rectDistance( vBlock, uWindow );
		alpha *= uWindowFade > 0.0 ? clamp( -dw / uWindowFade, 0.0, 1.0 ) : clamp( 0.5 - dw, 0.0, 1.0 );
	}
	if( alpha <= 0.001 )
		discard;
	if( dist > uMaxReach )
		discard;
	if( uLayer > 0 )
	{
		ivec2 ip = ivec2( gl_FragCoord.xy );
		vec4 p0  = texelFetch( uPrev0, ip, 0 );
		vec4 p1  = uLayer > 1 ? texelFetch( uPrev1, ip, 0 ) : vec4( 1.0e4, -1.0, 0.0, 0.0 );
		vec4 p2  = uLayer > 2 ? texelFetch( uPrev2, ip, 0 ) : vec4( 1.0e4, -1.0, 0.0, 0.0 );
		if( p0.g == vId || p1.g == vId || p2.g == vId )
			discard;
		if( uPeelMode == 1 )
		{
			// strictly closer than the previous layer's shape (stored as half float)
			float prevD = uLayer == 1 ? p0.r : uLayer == 2 ? p1.r : p2.r;
			if( dist >= prevD - max( 0.02, abs( prevD ) * 0.002 ) )
				discard;
		}
	}
	float dn = clamp( dist / uDepthRange * 0.5 + 0.5, 0.0, 0.999 );
	if( uPeelMode == 1 )
	{
		// opacity first (more opaque = smaller depth), distance second
		float aq     = floor( ( 1.0 - clamp( alpha, 0.0, 1.0 ) ) * 255.0 + 0.5 );
		gl_FragDepth = ( aq + dn ) / 257.0;
	}
	else
		gl_FragDepth = dn;
	oA = vec4( dist, vId, alpha, vTint.a );
	oB = vec4( vTint.rgb, clamp( vFx.x / 64.0, 0.0, 1.0 ) );
}
)GLSL";

// ---------------------------------------------------------------------------
// Composite: layers -> colour
//
// Opacity per shape is treated as a threshold: the picture is the integral
// over t in 0..1 of "the picture made of all shapes with opacity >= t". For
// one fading glyph that is a cross-fade between the text with and without it,
// so merged outlines never show a hole where a nearly invisible glyph sits,
// and nothing jumps when it disappears. A uniform opacity a gives a * picture.
//
// The field is read at several places ("sets"): set 0 at the pixel, sets
// 1..5 at the offsets of drop shadows and inner shadows. Plates are read at
// the pixel and at the drop-shadow offsets.
//
// The style stack, bottom to top (the order follows from the effect types):
//   drop shadows   cast by text (outer edge) and plates together
//   plate          colour, border inside
//   extrude        2.5D depth behind the text
//   contours       rings around the text
//   glows          text only
//   text           outlines (outermost first), fill with bevel, inner glows,
//                  inner contours, inner shadows
// Effects behind the text are knocked out under its silhouette, the shadows
// also under the plates (Photoshop's default).
// ---------------------------------------------------------------------------
const char* kCompositeVS = R"GLSL(#version 410 core
void main()
{
	vec2 p      = vec2( ( gl_VertexID << 1 ) & 2, gl_VertexID & 2 );// full-screen triangle
	gl_Position = vec4( p * 2.0 - 1.0, 0.0, 1.0 );
}
)GLSL";

const char* kCompositeFS = R"GLSL(#version 410 core
uniform sampler2D uA0;
uniform sampler2D uA1;
uniform sampler2D uA2;
uniform sampler2D uA3;
uniform sampler2D uB0;
uniform sampler2D uB1;
uniform sampler2D uB2;
uniform sampler2D uB3;
uniform sampler2D uPA0;
uniform sampler2D uPA1;
uniform sampler2D uPB0;
uniform sampler2D uPB1;
uniform sampler2D uInput;
uniform int uLayers;         // text layers in use (0 = no text)
uniform int uPlateLayers;    // plate layers in use (0 = no plates)
uniform vec2 uFieldSize;
uniform vec2 uViewportOrigin;
uniform float uAttrSoftness; // px: colour transition width where glyphs merge
uniform float uReach;        // px: the field holds distances up to here

// Fill
uniform vec4 uFill;          // straight colours
uniform vec4 uFill2;
uniform int uFillMode;       // FillMode (2 = per character, done as tint)
uniform vec2 uBlockOrigin;   // anchor, px, y down
uniform vec2 uBlockRotInv;   // cos, sin to go from screen back into block space
uniform vec4 uBox;           // block metric box l, t, r, b (px, y down, block space)
uniform vec2 uGradDir;       // gradient direction in block space
uniform float uFillScale;    // px: stripe / dot period, radial size
uniform int uInputMode;      // 0 none, 1 over, 2 fill, 3 cut out
uniform vec2 uInputMax;      // texture coordinates of the input's top-right corner

// Shape
uniform float uWeight;       // px, thicker > 0
uniform vec4 uRough;         // scale px, strength px, evolution offset x, y (strength 0 = off)

// Outlines: band k spans uOutlineEdge[k-1] .. uOutlineEdge[k] (band 0 starts at the fill edge)
uniform int uNumOutlines;
uniform vec4 uOutlineColor[ 6 ];
uniform float uOutlineEdge[ 6 ];
uniform float uFillEdge;
uniform float uOuterEdge;

// Glows (outer) and inner glows: colour, (radius, strength)
uniform int uNumGlows;
uniform vec4 uGlowColor[ 6 ];
uniform vec2 uGlowParam[ 6 ];
uniform int uNumInnerGlows;
uniform vec4 uInnerGlowColor[ 6 ];
uniform vec2 uInnerGlowParam[ 6 ];

// Sets 1..uNumSets: drop shadows (kind 1) and inner shadows (kind 2)
uniform int uNumSets;
uniform ivec2 uSetOffset[ 6 ];// whole px, GL orientation (y up)
uniform int uSetKind[ 6 ];
uniform vec4 uSetColor[ 6 ];
uniform vec2 uSetParam[ 6 ];  // softness, spread

// Bevel
uniform int uBevelOn;
uniform vec4 uBevelHi;
uniform vec4 uBevelLo;
uniform vec4 uBevelParam;    // size, softness, depth, style (0 smooth, 1 chisel, 2 chrome)
uniform vec2 uBevelLight;    // light direction, GL orientation

// Extrude
uniform int uExtrudeOn;
uniform vec4 uExtrudeColor;
uniform vec4 uExtrudeColor2;
uniform vec2 uExtrudeDir;    // unit vector, GL orientation: where the depth goes
uniform vec2 uExtrudeParam;  // depth px, shading

// Contours
uniform int uContoursOn;
uniform vec4 uContourColor;
uniform vec4 uContourParam;  // spacing, reach, line width share, side (0 outside, 1 inside, 2 both)

// Plates
uniform vec4 uPlate;
uniform vec4 uPlateBorder;
uniform float uPlateBorderWidth;
uniform float uPlateMerge;
uniform float uEdge;         // anti-aliasing width in px

out vec4 oColor;

const float kFar = 1.0e4;
const int kMaxEntries = 32;
const float kPlateSet = 8.0;

vec4 premul( vec4 c ) { return vec4( c.rgb * c.a, c.a ); }
float inside( float d, float edge, float aa ) { return clamp( 0.5 - ( d - edge ) / aa, 0.0, 1.0 ); }// coverage of "d < edge"
vec4 over( vec4 top, vec4 bottom ) { return top + bottom * ( 1.0 - top.a ); }

vec4 E[ kMaxEntries ];// distance, set, opacity, tint amount
vec4 Et[ kMaxEntries ];// tint colour, blur px
int nE = 0;

bool outsideField( ivec2 ip ) { return any( lessThan( ip, ivec2( 0 ) ) ) || any( greaterThanEqual( ip, ivec2( uFieldSize ) ) ); }

// Screen px (GL, y up) -> block space (px, y down, relative to the anchor, unrotated)
vec2 toBlock( vec2 frag )
{
	vec2 s = vec2( frag.x, uFieldSize.y - frag.y ) - uBlockOrigin;
	return vec2( uBlockRotInv.x * s.x - uBlockRotInv.y * s.y, uBlockRotInv.y * s.x + uBlockRotInv.x * s.y );
}

float hash12( vec2 p ) { return fract( sin( dot( p, vec2( 127.1, 311.7 ) ) ) * 43758.5453 ); }
float vnoise( vec2 p )
{
	vec2 i = floor( p ), f = fract( p );
	vec2 u = f * f * ( 3.0 - 2.0 * f );
	float a = hash12( i ), b = hash12( i + vec2( 1.0, 0.0 ) ), c = hash12( i + vec2( 0.0, 1.0 ) ), d = hash12( i + vec2( 1.0, 1.0 ) );
	return mix( mix( a, b, u.x ), mix( c, d, u.x ), u.y ) * 2.0 - 1.0;
}
// Edge displacement at a field texel, in px (moves with the block, loops with the evolution)
float rough( ivec2 ip )
{
	if( uRough.y <= 0.0 )
		return 0.0;
	vec2 q = toBlock( vec2( ip ) + 0.5 ) / max( uRough.x, 1.0 ) + uRough.zw;
	return uRough.y * ( 0.7 * vnoise( q ) + 0.3 * vnoise( q * 2.31 + 17.3 ) );
}

void addEntry( vec4 a, vec4 b, float set )
{
	if( nE >= kMaxEntries )
		return;
	E[ nE ]  = vec4( a.r, set, a.b, a.a );
	Et[ nE ] = vec4( b.rgb, b.a * 64.0 );
	nE++;
}

vec4 textA( int k, ivec2 ip )
{
	return k == 0 ? texelFetch( uA0, ip, 0 ) : k == 1 ? texelFetch( uA1, ip, 0 ) : k == 2 ? texelFetch( uA2, ip, 0 ) : texelFetch( uA3, ip, 0 );
}
vec4 textB( int k, ivec2 ip )
{
	return k == 0 ? texelFetch( uB0, ip, 0 ) : k == 1 ? texelFetch( uB1, ip, 0 ) : k == 2 ? texelFetch( uB2, ip, 0 ) : texelFetch( uB3, ip, 0 );
}

void loadText( ivec2 ip, float set )
{
	if( outsideField( ip ) )
		return;
	float r = rough( ip ) - uWeight;
	for( int k = 0; k < uLayers; ++k )
	{
		vec4 a = textA( k, ip );
		if( a.r >= 0.5 * kFar )
			break;// no further shape near this pixel
		a.r += r;
		addEntry( a, textB( k, ip ), set );
	}
}

void loadPlates( ivec2 ip, float set )
{
	if( outsideField( ip ) )
		return;
	for( int k = 0; k < uPlateLayers; ++k )
	{
		vec4 a, b;
		if( k == 0 ) { a = texelFetch( uPA0, ip, 0 ); b = texelFetch( uPB0, ip, 0 ); }
		else { a = texelFetch( uPA1, ip, 0 ); b = texelFetch( uPB1, ip, 0 ); }
		if( a.r >= 0.5 * kFar )
			break;
		addEntry( a, vec4( b.rgb, 0.0 ), set );
	}
}

// Nearest text (layer 0) at a texel, relative to the outer edge; far = beyond the field
float nearestText( ivec2 ip, out float alpha )
{
	alpha = 0.0;
	if( outsideField( ip ) )
		return uReach;
	vec4 a = texelFetch( uA0, ip, 0 );
	if( a.r >= 0.5 * kFar )
		return uReach;
	alpha = a.b;
	return a.r + rough( ip ) - uWeight;
}

float smoothUnion( float d, float b, float k )
{
	if( k <= 0.0 || d >= 0.5 * kFar )
		return min( d, b );
	float h = max( k - abs( d - b ), 0.0 ) / k;
	return min( d, b ) - h * h * k * 0.25;
}

vec4 inputColor( vec2 frag )
{
	vec2 uv = frag / uFieldSize * uInputMax;
	return texture( uInput, uv );// premultiplied (FFGL convention)
}

void main()
{
	vec2 frag = gl_FragCoord.xy - uViewportOrigin;
	ivec2 ip  = ivec2( frag );
	bool platesOn = uPlateLayers > 0;
	loadText( ip, 0.0 );
	bool anyShadow = false;
	for( int s = 1; s <= uNumSets; ++s )
	{
		loadText( ip - uSetOffset[ s ], float( s ) );
		anyShadow = anyShadow || uSetKind[ s ] == 1;
	}
	if( platesOn )
	{
		loadPlates( ip, kPlateSet );
		for( int s = 1; s <= uNumSets; ++s )
			if( uSetKind[ s ] == 1 )
				loadPlates( ip - uSetOffset[ s ], kPlateSet + float( s ) );
	}

	vec4 inputC = uInputMode != 0 ? inputColor( frag ) : vec4( 0.0 );
	if( nE == 0 && uExtrudeOn == 0 )
	{
		oColor = uInputMode == 1 || uInputMode == 3 ? inputC : vec4( 0.0 );
		if( oColor.a > 0.0 )
			oColor.rgb /= oColor.a;
		return;
	}

	// Most opaque first
	int order[ kMaxEntries ];
	for( int i = 0; i < nE; ++i )
	{
		int j = i;
		while( j > 0 && E[ order[ j - 1 ] ].b < E[ i ].b )
		{
			order[ j ] = order[ j - 1 ];
			--j;
		}
		order[ j ] = i;
	}

	// ---- Per pixel, the same for every opacity level ----
	vec2 local = toBlock( frag );
	vec2 bc    = 0.5 * ( uBox.xy + uBox.zw );
	vec2 halfs = 0.5 * ( uBox.zw - uBox.xy );
	vec4 baseFill  = uFill;
	if( uFillMode == 1 || uFillMode == 5 )
	{
		float ext = max( abs( halfs.x * uGradDir.x ) + abs( halfs.y * uGradDir.y ), 1e-3 );
		float t   = clamp( dot( local - bc, uGradDir ) / ext * 0.5 + 0.5, 0.0, 1.0 );
		if( uFillMode == 1 )
			baseFill = mix( uFill, uFill2, t );
		else
		{
			// halftone: dots of colour 2 on a grid along the gradient, growing with t
			vec2 gdir = uGradDir, gn = vec2( -gdir.y, gdir.x );
			vec2 g    = vec2( dot( local, gdir ), dot( local, gn ) ) / uFillScale;
			vec2 cell = fract( g ) - 0.5;
			float r   = 0.5 * 1.42 * sqrt( t );
			float dd  = ( length( cell ) - r ) * uFillScale;
			baseFill  = mix( uFill, uFill2, clamp( 0.5 - dd / uEdge, 0.0, 1.0 ) );
		}
	}
	else if( uFillMode == 3 )
	{
		float t  = clamp( length( local - bc ) / max( uFillScale, 1.0 ), 0.0, 1.0 );
		baseFill = mix( uFill, uFill2, t );
	}
	else if( uFillMode == 4 )
	{
		float x  = dot( local, uGradDir ) / uFillScale;
		float f  = abs( fract( x ) - 0.5 ) * uFillScale;// distance to the middle of a colour-1 band
		baseFill = mix( uFill2, uFill, clamp( ( 0.25 * uFillScale - f ) / uEdge + 0.5, 0.0, 1.0 ) );
	}
	else if( uFillMode == 6 && uInputMode != 0 )
	{
		vec4 c   = inputC;
		baseFill = vec4( c.a > 0.0 ? c.rgb / c.a : vec3( 0.0 ), c.a * uFill.a );
	}

	// Bevel and extrude look at the nearest text around the pixel.
	float alpha0;
	vec2 grad = vec2( 0.0 );
	if( uBevelOn == 1 || uExtrudeOn == 1 )
	{
		float a;
		float dx = nearestText( ip + ivec2( 1, 0 ), a ) - nearestText( ip - ivec2( 1, 0 ), a );
		float dy = nearestText( ip + ivec2( 0, 1 ), a ) - nearestText( ip - ivec2( 0, 1 ), a );
		grad     = vec2( dx, dy );
		grad     = dot( grad, grad ) > 1e-6 ? normalize( grad ) : vec2( 0.0 );
	}

	// Extrude: walk from the pixel against the depth direction through the text
	// silhouette (outer edge). Coverage is gathered front to back: each sample
	// adds only what it covers beyond the samples before it, so a walk that
	// grazes a corner blends with what lies behind it instead of leaving a seam.
	vec4 extrudeC = vec4( 0.0 );
	if( uExtrudeOn == 1 && uLayers > 0 )
	{
		float depth = uExtrudeParam.x;
		float k = 0.0, A = 0.0, bestGain = 0.0;
		vec3 C = vec3( 0.0 );
		ivec2 mainP = ip;
		for( int it = 0; it < 128; ++it )
		{
			if( k > depth || A >= 0.999 )
				break;
			ivec2 q  = ivec2( floor( frag - uExtrudeDir * k ) );
			float a;
			float dd = nearestText( q, a ) - uOuterEdge;
			float c  = clamp( 0.5 - dd / uEdge, 0.0, 1.0 ) * a;
			if( c > A )
			{
				vec4 col = mix( uExtrudeColor, uExtrudeColor2, clamp( k / max( depth, 1.0 ), 0.0, 1.0 ) );
				float gain = c - A;
				C += gain * col.rgb * col.a;
				// the extrusion's own opacity: a transparent far end lets what is behind show
				A = c;
				extrudeC.a += gain * col.a;
				if( gain > bestGain )
				{
					bestGain = gain;
					mainP    = q;
				}
			}
			k += clamp( dd, 0.6, uReach );
		}
		if( extrudeC.a > 0.0 )
		{
			// side shading: the silhouette's normal at the main hit against the light
			float a;
			vec2 n    = vec2( nearestText( mainP + ivec2( 1, 0 ), a ) - nearestText( mainP - ivec2( 1, 0 ), a ),
							  nearestText( mainP + ivec2( 0, 1 ), a ) - nearestText( mainP - ivec2( 0, 1 ), a ) );
			n         = dot( n, n ) > 1e-6 ? normalize( n ) : vec2( 0.0 );
			vec2 l    = normalize( vec2( -0.6, 0.8 ) );
			float lit = 0.6 + 0.4 * dot( n, l );
			extrudeC.rgb = C * mix( 1.0, lit, uExtrudeParam.y );
		}
	}

	// ---- The integral over the opacity threshold t ----
	// Going from the most opaque shape down, the shapes with opacity >= t only
	// ever grow, so unions and tint sums are running values and every level is
	// shaded once.
	float dT[ 6 ];
	float dP[ 6 ];
	for( int s = 0; s < 6; ++s )
	{
		dT[ s ] = kFar;
		dP[ s ] = kFar;
	}
	float tw = 0.0, twt = 0.0, tb = 0.0, pw = 0.0, pwt = 0.0;
	vec3 tc = vec3( 0.0 ), pc3 = vec3( 0.0 );
	vec4 result = vec4( 0.0 );
	float maxT  = 0.0;
	for( int s = 0; s < nE; ++s )
	{
		int i   = order[ s ];
		vec4 e  = E[ i ];
		int set = int( e.g + 0.5 );
		if( set == 0 )
		{
			dT[ 0 ]  = min( dT[ 0 ], e.r );
			float w  = exp( clamp( -e.r / uAttrSoftness, -40.0, 40.0 ) );
			tw      += w;
			twt     += w * e.a;
			tc      += w * e.a * Et[ i ].rgb;
			tb      += w * Et[ i ].a;
		}
		else if( set < 8 )
			dT[ set ] = min( dT[ set ], e.r );
		else if( set == 8 )
		{
			dP[ 0 ]  = smoothUnion( dP[ 0 ], e.r, uPlateMerge );
			float w  = exp( clamp( -e.r / uAttrSoftness, -40.0, 40.0 ) );
			pw      += w;
			pwt     += w * e.a;
			pc3     += w * e.a * Et[ i ].rgb;
		}
		else
			dP[ set - 8 ] = smoothUnion( dP[ set - 8 ], e.r, uPlateMerge );

		float t      = e.b;
		maxT         = max( maxT, t );
		float next   = s + 1 < nE ? E[ order[ s + 1 ] ].b : 0.0;
		float weight = t - next;
		if( weight <= 0.0 )
			continue;// more shapes at the same opacity follow

		float d    = dT[ 0 ];
		float blur = tw > 0.0 ? tb / tw : 0.0;
		float aa   = uEdge + blur;
		vec4 fill  = baseFill;
		if( twt > 0.0 && tw > 0.0 )
			fill.rgb = mix( fill.rgb, tc / twt, clamp( twt / tw, 0.0, 1.0 ) );
		vec4 fillP = premul( fill );

		// ---- Fill effects ----
		float depthIn = uFillEdge - d;// px inside the fill
		if( uBevelOn == 1 && d < kFar * 0.5 )
		{
			float size = max( uBevelParam.x, 0.5 );
			float soft = max( uBevelParam.y, 0.0 );
			float u    = clamp( depthIn / size, 0.0, 1.0 );
			float slope;
			int style  = int( uBevelParam.w + 0.5 );
			if( style == 1 )
				slope = 1.0 - smoothstep( 1.0 - soft / size - 0.02, 1.0, u );// chisel: flat facet
			else
				slope = cos( u * 1.5707963 );// smooth: rounded edge
			slope *= uBevelParam.z * 1.5;
			vec3 N  = normalize( vec3( grad * slope, 1.0 ) );
			if( style == 2 )
			{
				// chrome: a horizon reflected in the surface; sky (highlight colour) above, ground (shadow colour) below
				float ty  = clamp( ( local.y - bc.y ) / max( halfs.y, 1.0 ), -1.0, 1.0 );
				float e2  = -ty * 0.8 + N.y * 1.6;
				vec3 sky  = mix( uBevelHi.rgb * 0.55, uBevelHi.rgb, clamp( e2, 0.0, 1.0 ) );
				vec3 gnd  = mix( uBevelLo.rgb, uBevelLo.rgb * 0.35, clamp( -e2, 0.0, 1.0 ) );
				vec3 env  = e2 > 0.0 ? sky : gnd;
				env      += vec3( pow( clamp( dot( N, normalize( vec3( uBevelLight, 1.2 ) ) ), 0.0, 1.0 ), 24.0 ) ) * 0.8;
				fillP     = mix( fillP, vec4( env * fillP.a, fillP.a ), uBevelHi.a );
			}
			else
			{
				vec3 L      = normalize( vec3( uBevelLight, 1.0 ) );
				float shade = dot( N, L ) - L.z;
				vec4 hi     = premul( uBevelHi ) * clamp( shade * 2.0, 0.0, 1.0 );
				vec4 lo     = premul( uBevelLo ) * clamp( -shade * 2.0, 0.0, 1.0 );
				fillP       = over( lo, fillP );
				fillP       = over( hi, fillP );
			}
		}
		for( int g = 0; g < uNumInnerGlows; ++g )
		{
			float kk = clamp( 1.0 - max( depthIn, 0.0 ) / max( uInnerGlowParam[ g ].x, 0.5 ), 0.0, 1.0 );
			fillP    = over( premul( uInnerGlowColor[ g ] ) * clamp( uInnerGlowParam[ g ].y * kk * kk, 0.0, 1.0 ), fillP );
		}
		if( uContoursOn == 1 && uContourParam.w > 0.5 && depthIn > 0.0 )
		{
			float sp  = max( uContourParam.x, 2.0 );
			float f   = abs( fract( depthIn / sp + 0.5 ) - 0.5 ) * sp;
			float cov = clamp( ( 0.5 * uContourParam.z * sp - f ) / uEdge + 0.5, 0.0, 1.0 ) * ( 1.0 - smoothstep( uContourParam.y - sp, uContourParam.y, depthIn ) );
			fillP     = over( premul( uContourColor ) * cov, fillP );
		}
		for( int k = 1; k <= uNumSets; ++k )
			if( uSetKind[ k ] == 2 )
			{
				float w = max( uSetParam[ k ].x, aa );
				vec4 is = premul( uSetColor[ k ] ) * smoothstep( -0.5 * w, 0.5 * w, dT[ k ] - uFillEdge );// the shape moved by the offset
				fillP   = over( is, fillP );
			}

		// ---- Text: inner layers REPLACE outer ones (mix by coverage), so a
		// transparent fill shows through (hollow text) and bands have no seams.
		vec4 text = vec4( 0.0 );
		for( int k = uNumOutlines - 1; k >= 0; --k )
			text = mix( text, premul( uOutlineColor[ k ] ), inside( d, uOutlineEdge[ k ], aa ) );
		text = mix( text, fillP, inside( d, uFillEdge, aa ) );
		float textShape = inside( d, uOuterEdge, aa );

		// ---- Behind the text ----
		vec4 glowC = vec4( 0.0 );
		for( int g = 0; g < uNumGlows; ++g )
		{
			float kk = clamp( 1.0 - max( d - uOuterEdge, 0.0 ) / max( uGlowParam[ g ].x, 0.5 ), 0.0, 1.0 );
			glowC    = over( premul( uGlowColor[ g ] ) * clamp( uGlowParam[ g ].y * kk * kk, 0.0, 1.0 ), glowC );
		}
		vec4 contourC = vec4( 0.0 );
		if( uContoursOn == 1 && uContourParam.w != 1.0 )
		{
			float x   = d - uOuterEdge;
			float sp  = max( uContourParam.x, 2.0 );
			float f   = abs( fract( x / sp + 0.5 ) - 0.5 ) * sp;
			float cov = x >= 0.5 * sp ? clamp( ( 0.5 * uContourParam.z * sp - f ) / uEdge + 0.5, 0.0, 1.0 ) * ( 1.0 - smoothstep( uContourParam.y - sp, uContourParam.y, x ) ) : 0.0;
			contourC  = premul( uContourColor ) * cov;
		}
		vec4 shadowC = vec4( 0.0 );
		float plateShape = 0.0;
		vec4 plateC = vec4( 0.0 );
		if( platesOn )
		{
			vec4 pcol = uPlate;
			if( pwt > 0.0 && pw > 0.0 )
				pcol.rgb = mix( pcol.rgb, pc3 / pwt, clamp( pwt / pw, 0.0, 1.0 ) );
			plateShape = inside( dP[ 0 ], 0.0, uEdge );
			if( uPlateBorderWidth > 0.0 )
				plateC = mix( plateC, premul( uPlateBorder ), plateShape );
			plateC = mix( plateC, premul( pcol ), inside( dP[ 0 ], -uPlateBorderWidth, uEdge ) );
		}
		for( int k = 1; k <= uNumSets; ++k )
			if( uSetKind[ k ] == 1 )
			{
				float w  = max( uSetParam[ k ].x, uEdge );
				float ds = min( dT[ k ] - uOuterEdge, platesOn ? dP[ k ] : kFar ) - uSetParam[ k ].y;
				shadowC  = over( premul( uSetColor[ k ] ) * ( 1.0 - smoothstep( -0.5 * w, 0.5 * w, ds ) ), shadowC );
			}
		// Effects behind the text are knocked out under its silhouette; the
		// shadows also under the plates, the plates never.
		vec4 knock = over( glowC, over( contourC, extrudeC ) );
		vec4 back;
		if( !platesOn )
			back = over( knock, shadowC ) * ( 1.0 - textShape );
		else
			back = over( knock * ( 1.0 - textShape ), over( plateC, shadowC * ( 1.0 - plateShape ) * ( 1.0 - textShape ) ) );
		result += weight * over( text, back );
	}
	// The levels add up to the most opaque shape's opacity; the extrusion of an
	// opaque glyph far from fainter ones keeps its full strength.
	float topT = nE > 0 ? E[ order[ 0 ] ].b : 0.0;
	result     = over( result, extrudeC * ( 1.0 - topT ) );

	if( uInputMode == 1 )
		result = over( result, inputC );
	else if( uInputMode == 3 )
	{
		// the video with the text's silhouette cut out (effects of the text stay)
		float cut = nE > 0 ? inside( dT[ 0 ], uOuterEdge, uEdge ) * maxT : 0.0;
		result    = over( result * ( 1.0 - cut ), inputC * ( 1.0 - cut ) );
	}
	if( result.a > 0.0 )
		result.rgb /= result.a;// Resolume composites sources as straight alpha
	oColor = result;
}
)GLSL";

const char* kCopyFS = R"GLSL(#version 410 core
uniform sampler2D uInput;
uniform vec2 uFieldSize;
uniform vec2 uViewportOrigin;
uniform vec2 uInputMax;
out vec4 oColor;
void main()
{
	vec4 c = texture( uInput, ( gl_FragCoord.xy - uViewportOrigin ) / uFieldSize * uInputMax );
	oColor = c.a > 0.0 ? vec4( c.rgb / c.a, c.a ) : vec4( 0.0 );
}
)GLSL";

void setColor( GLuint prog, const char* name, const Color& c )
{
	glUniform4f( glGetUniformLocation( prog, name ), c.r, c.g, c.b, c.a );
}

GLuint makeTexture( GLint internalFormat, GLenum format, GLenum type, int w, int h, GLint filter )
{
	GLuint t = 0;
	glGenTextures( 1, &t );
	glBindTexture( GL_TEXTURE_2D, t );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, w, h, 0, format, type, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	return t;
}

struct Rect
{
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;// px, y down
	bool empty() const { return x0 >= x1 || y0 >= y1; }
	void add( float x, float y )
	{
		x0 = std::min( x0, x );
		y0 = std::min( y0, y );
		x1 = std::max( x1, x );
		y1 = std::max( y1, y );
	}
	void add( const Rect& r )
	{
		if( r.empty() )
			return;
		add( r.x0, r.y0 );
		add( r.x1, r.y1 );
	}
	Rect shifted( float dx, float dy ) const { return Rect{ x0 + dx, y0 + dy, x1 + dx, y1 + dy }; }
};

// Scissor rectangle in GL window coordinates (y up), clipped to the viewport.
void setScissor( const Rect& r, int vpX, int vpY, int w, int h )
{
	int x0 = std::max( 0, (int)std::floor( r.x0 ) ), x1 = std::min( w, (int)std::ceil( r.x1 ) );
	int y0 = std::max( 0, (int)std::floor( r.y0 ) ), y1 = std::min( h, (int)std::ceil( r.y1 ) );
	if( x1 <= x0 || y1 <= y0 )
	{
		glScissor( vpX, vpY, 0, 0 );
		return;
	}
	glScissor( vpX + x0, vpY + ( h - y1 ), x1 - x0, y1 - y0 );
}

void varies( const std::vector< ShapeInstance >& items, bool& alphaVaries, bool& tintVaries )
{
	alphaVaries = tintVaries = false;
	if( items.empty() )
		return;
	const ShapeInstance& g0 = items.front();
	for( const ShapeInstance& g : items )
	{
		alphaVaries = alphaVaries || std::fabs( g.alpha - g0.alpha ) > 1e-4f || g.clipped();
		tintVaries  = tintVaries || g.tintAmount != g0.tintAmount || g.tintR != g0.tintR || g.tintG != g0.tintG || g.tintB != g0.tintB;
	}
}

}//namespace

// ---------------------------------------------------------------------------
// Programs and scratch render targets exist once per GL context and are shared
// by all plugin instances: instances in one context render one after another,
// and nothing in the targets survives a render() call.
// ---------------------------------------------------------------------------
struct Renderer::SharedPrograms
{
	void* context = nullptr;
	GLuint field = 0, composite = 0, copy = 0;

	// Layer k: A = distance, shape id, opacity, tint amount (RGBA16F); B = tint colour (RGBA8)
	GLuint layerFbo[ kLayers ]{}, layerA[ kLayers ]{}, layerB[ kLayers ]{};
	GLuint plateFbo[ kPlateLayers ]{}, plateA[ kPlateLayers ]{}, plateB[ kPlateLayers ]{};
	GLuint depth = 0;
	int w = 0, h = 0;

	void destroy()
	{
		glDeleteProgram( field );
		glDeleteProgram( composite );
		glDeleteProgram( copy );
		releaseTargets();
	}

	void releaseTargets()
	{
		glDeleteFramebuffers( kLayers, layerFbo );
		glDeleteTextures( kLayers, layerA );
		glDeleteTextures( kLayers, layerB );
		glDeleteFramebuffers( kPlateLayers, plateFbo );
		glDeleteTextures( kPlateLayers, plateA );
		glDeleteTextures( kPlateLayers, plateB );
		if( depth )
			glDeleteRenderbuffers( 1, &depth );
		for( int k = 0; k < kLayers; ++k )
			layerFbo[ k ] = layerA[ k ] = layerB[ k ] = 0;
		for( int k = 0; k < kPlateLayers; ++k )
			plateFbo[ k ] = plateA[ k ] = plateB[ k ] = 0;
		depth = 0;
		w = h = 0;
	}

	bool makeLayers( int n, GLuint* fbos, GLuint* a, GLuint* b, int nw, int nh )
	{
		bool ok = true;
		glGenFramebuffers( n, fbos );
		for( int k = 0; k < n; ++k )
		{
			a[ k ] = makeTexture( GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, nw, nh, GL_NEAREST );
			b[ k ] = makeTexture( GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nw, nh, GL_NEAREST );
			glBindFramebuffer( GL_FRAMEBUFFER, fbos[ k ] );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a[ k ], 0 );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, b[ k ], 0 );
			glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth );
			const GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
			glDrawBuffers( 2, bufs );
			ok = ok && glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
		}
		return ok;
	}

	// Grows (never shrinks) to the largest viewport seen in this context.
	bool ensureTargets( int needW, int needH )
	{
		if( layerFbo[ 0 ] && needW <= w && needH <= h )
			return true;
		const int nw = std::max( needW, w ), nh = std::max( needH, h );
		releaseTargets();
		glGenRenderbuffers( 1, &depth );
		glBindRenderbuffer( GL_RENDERBUFFER, depth );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, nw, nh );
		glBindRenderbuffer( GL_RENDERBUFFER, 0 );
		bool ok = makeLayers( kLayers, layerFbo, layerA, layerB, nw, nh );
		ok      = makeLayers( kPlateLayers, plateFbo, plateA, plateB, nw, nh ) && ok;
		w       = nw;
		h       = nh;
		if( !ok )
			FFGLLog::LogToHost( "sdfkit: offscreen framebuffer incomplete" );
		return ok;
	}
};

namespace
{
ffglkit::PerContext< Renderer::SharedPrograms > g_programs;
}//namespace

// ---------------------------------------------------------------------------
const Effect* Style::first( EffectType t ) const
{
	for( const Effect& e : effects )
		if( e.type == t )
			return &e;
	return nullptr;
}

float Style::roughStrength() const
{
	const Effect* r = first( EffectType::Roughen );
	return r ? std::max( 0.0f, r->distance ) : 0.0f;
}

float Style::fillEdge() const
{
	for( const Effect& e : effects )
		if( e.type == EffectType::Outline && e.size > 0 )
			return e.mode == 2 ? -e.size : e.mode == 1 ? -0.5f * e.size : 0.0f;
	return 0.0f;
}

float Style::outerEdge() const
{
	float edge  = 0.0f;
	bool first_ = true;
	for( const Effect& e : effects )
		if( e.type == EffectType::Outline && e.size > 0 )
		{
			edge += first_ ? ( e.mode == 2 ? 0.0f : e.mode == 1 ? 0.5f * e.size : e.size ) : e.size;
			first_ = false;
		}
	return edge;
}

float Style::reach() const
{
	const float outer = outerEdge();
	const float inner = -fillEdge();
	float r           = std::max( outer, inner );
	for( const Effect& e : effects )
		switch( e.type )
		{
		case EffectType::Glow:
			if( e.color.a > 0 && e.amount > 0 )
				r = std::max( r, outer + e.size );
			break;
		case EffectType::Shadow:
			if( e.color.a > 0 )
				r = std::max( r, outer + e.size + e.softness * 0.5f );
			break;
		case EffectType::InnerGlow:
			r = std::max( r, inner + e.size );
			break;
		case EffectType::InnerShadow:
			r = std::max( r, inner + e.softness * 0.5f );
			break;
		case EffectType::Bevel:
			r = std::max( r, inner + e.size + e.softness + 1.0f );
			break;
		case EffectType::Contours:
			r = std::max( r, ( e.mode == 1 ? inner : outer ) + e.distance );
			if( e.mode == 2 )
				r = std::max( r, inner + e.distance );
			break;
		case EffectType::Extrude:
			r = std::max( r, outer + 4.0f );
			break;
		default:
			break;
		}
	return r + roughStrength() + std::fabs( weight ) + std::max( 0.0f, glyphWeightMax ) + 0.5f * glyphBlurMax + edgeSoftness + 1.0f;
}

float Style::plateReach() const
{
	float r = 0;
	for( const Effect& e : effects )
		if( e.type == EffectType::Shadow && e.color.a > 0 )
			r = std::max( r, e.size + e.softness * 0.5f );
	return r + edgeSoftness + 1.0f;
}

void Style::clampTo( float maxReachPx )
{
	const float avail = std::max( 0.0f, maxReachPx - edgeSoftness - 1.0f - roughStrength() - std::fabs( weight ) - std::max( 0.0f, glyphWeightMax ) -
										  0.5f * glyphBlurMax );
	float outer = outerEdge(), inner = -fillEdge();
	if( std::max( outer, inner ) > avail && std::max( outer, inner ) > 0 )
	{
		const float k = avail / std::max( outer, inner );
		for( Effect& e : effects )
			if( e.type == EffectType::Outline )
				e.size *= k;
		outer = outerEdge();
		inner = -fillEdge();
	}
	const float rest = std::max( 0.0f, avail - outer );
	const float in   = std::max( 0.0f, avail - inner );
	for( Effect& e : effects )
		switch( e.type )
		{
		case EffectType::Glow:
			e.size = std::min( e.size, rest );
			break;
		case EffectType::Shadow:
			e.size     = std::min( e.size, rest );
			e.softness = std::min( e.softness, 2.0f * std::max( 0.0f, rest - e.size ) );
			break;
		case EffectType::InnerGlow:
			e.size = std::min( e.size, in );
			break;
		case EffectType::InnerShadow:
			e.softness = std::min( e.softness, 2.0f * in );
			break;
		case EffectType::Bevel:
			e.size     = std::min( e.size, std::max( 0.5f, in - 1.0f ) );
			e.softness = std::min( e.softness, std::max( 0.0f, in - 1.0f - e.size ) );
			break;
		case EffectType::Contours:
			e.distance = std::min( e.distance, e.mode == 1 ? in : rest );
			break;
		default:
			break;
		}
}

// ---------------------------------------------------------------------------
namespace
{
// Same projection as the vertex shader, for bounding boxes on the CPU.
struct Projector
{
	const BlockTransform& t;
	float blockCos, blockSin, vpW, vpH;
	void point( Rect& bbox, const ShapeInstance& g, float unitPx, bool glyph, float qx, float qy ) const
	{
		const float k = unitPx * g.scale;
		float lx = qx * k, ly = -qy * k;
		const bool xf = glyph && ( g.rotX != 0 || g.rotY != 0 || g.skew != 0 || g.depth != 0 );
		if( xf )
			lx -= ly * g.skew;
		const float c = std::cos( g.rotation ), s = std::sin( g.rotation );
		float rx = c * lx - s * ly, ry = s * lx + c * ly, rz = 0.0f;
		if( xf && ( g.rotX != 0 || g.rotY != 0 ) )
		{
			float cxq = 0.5f * ( g.quadL + g.quadR ) * k, cyq = -0.5f * ( g.quadB + g.quadT ) * k;
			cxq -= cyq * g.skew;
			const float pcx = c * cxq - s * cyq, pcy = s * cxq + c * cyq;
			float x = rx - pcx, y = ry - pcy, z = 0.0f;
			const float ca = std::cos( g.rotX ), sa = std::sin( g.rotX );
			const float y2 = ca * y + sa * z, z2 = -sa * y + ca * z;
			const float cb = std::cos( g.rotY ), sb = std::sin( g.rotY );
			const float x3 = cb * x - sb * z2, z3 = sb * x + cb * z2;
			rx = x3 + pcx;
			ry = y2 + pcy;
			rz = z3;
		}
		if( glyph )
			rz += g.depth;
		const float bx = rx + g.x, by = ry + g.y;
		float px = blockCos * bx - blockSin * by + t.originX, py = blockSin * bx + blockCos * by + t.originY;
		if( t.perspective > 0.0f )
		{
			const float w = std::max( ( t.perspective + rz ) / t.perspective, 0.05f );
			px            = 0.5f * vpW + ( px - 0.5f * vpW ) / w;
			py            = 0.5f * vpH + ( py - 0.5f * vpH ) / w;
		}
		bbox.add( px, py );
	}
	void quads( Rect& bbox, const std::vector< ShapeInstance >& items, float unitPx, bool glyph ) const
	{
		for( const ShapeInstance& g : items )
		{
			point( bbox, g, unitPx, glyph, g.quadL, g.quadB );
			point( bbox, g, unitPx, glyph, g.quadR, g.quadB );
			point( bbox, g, unitPx, glyph, g.quadL, g.quadT );
			point( bbox, g, unitPx, glyph, g.quadR, g.quadT );
		}
	}
};

// Whole-pixel offset for an effect at `angle` (radians, counter-clockwise) and
// `distance`, in GL orientation (y up).
void offsetOf( float distance, float angle, int& dx, int& dy )
{
	dx = (int)std::lround( distance * std::cos( angle ) );
	dy = (int)std::lround( distance * std::sin( angle ) );
}
}//namespace

// ---------------------------------------------------------------------------
bool Renderer::init()
{
	release();
	programs = g_programs.acquire( []() -> SharedPrograms* {
		auto* sp      = new SharedPrograms();
		sp->field     = ffglkit::linkProgram( kShapeVS, kFieldFS, "sdfkit field" );
		sp->composite = ffglkit::linkProgram( kCompositeVS, kCompositeFS, "sdfkit composite" );
		sp->copy      = ffglkit::linkProgram( kCompositeVS, kCopyFS, "sdfkit copy" );
		if( !sp->field || !sp->composite || !sp->copy )
		{
			if( sp->field )
				glDeleteProgram( sp->field );
			if( sp->composite )
				glDeleteProgram( sp->composite );
			if( sp->copy )
				glDeleteProgram( sp->copy );
			delete sp;
			return nullptr;
		}
		return sp;
	} );
	if( !programs )
		return false;

	GLint prevVao = 0, prevBuf = 0;
	glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &prevVao );
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &prevBuf );

	glGenVertexArrays( 1, &shapeVao );
	glGenVertexArrays( 1, &emptyVao );
	glGenBuffers( 1, &cornerVbo );
	glGenBuffers( 1, &glyphVbo );
	glGenBuffers( 1, &boxVbo );

	glBindVertexArray( shapeVao );
	const float corners[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	glBindBuffer( GL_ARRAY_BUFFER, cornerVbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof( corners ), corners, GL_STATIC_DRAW );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 0, nullptr );
	for( int i = 0; i < kAttribs; ++i )
	{
		glEnableVertexAttribArray( 1 + i );
		glVertexAttribDivisor( 1 + i, 1 );
	}
	glBindBuffer( GL_ARRAY_BUFFER, glyphVbo );
	glBufferData( GL_ARRAY_BUFFER, 64 * sizeof( ShapeInstance ), nullptr, GL_DYNAMIC_DRAW );
	glyphCapacity = 64;
	glBindBuffer( GL_ARRAY_BUFFER, boxVbo );
	glBufferData( GL_ARRAY_BUFFER, 8 * sizeof( ShapeInstance ), nullptr, GL_DYNAMIC_DRAW );
	boxCapacity = 8;

	glBindVertexArray( (GLuint)prevVao );
	glBindBuffer( GL_ARRAY_BUFFER, (GLuint)prevBuf );
	return true;
}

void Renderer::release()
{
	if( programs )
	{
		g_programs.release( programs );
		programs = nullptr;
	}
	if( shapeVao )
		glDeleteVertexArrays( 1, &shapeVao );
	if( emptyVao )
		glDeleteVertexArrays( 1, &emptyVao );
	if( cornerVbo )
		glDeleteBuffers( 1, &cornerVbo );
	if( glyphVbo )
		glDeleteBuffers( 1, &glyphVbo );
	if( boxVbo )
		glDeleteBuffers( 1, &boxVbo );
	shapeVao = emptyVao = cornerVbo = glyphVbo = boxVbo = 0;
	glyphCapacity = boxCapacity = 0;
}

void Renderer::uploadInstances( GLuint vbo, size_t& capacity, const std::vector< ShapeInstance >& items )
{
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	if( items.size() > capacity )
	{
		capacity = std::max( items.size(), capacity * 2 );
		glBufferData( GL_ARRAY_BUFFER, capacity * sizeof( ShapeInstance ), nullptr, GL_DYNAMIC_DRAW );
	}
	if( !items.empty() )
		glBufferSubData( GL_ARRAY_BUFFER, 0, items.size() * sizeof( ShapeInstance ), items.data() );
}

void Renderer::drawField( GLuint vbo, size_t count, int kind, int layers, bool alphaVaries, float maxReach, const GLuint* fbos, const GLuint* prevA )
{
	const GLuint fp = programs->field;
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	for( int i = 0; i < kAttribs; ++i )
		glVertexAttribPointer( 1 + i, 4, GL_FLOAT, GL_FALSE, sizeof( ShapeInstance ), (const void*)( sizeof( float ) * 4 * i ) );
	glUniform1i( glGetUniformLocation( fp, "uKind" ), kind );
	glUniform1i( glGetUniformLocation( fp, "uClipMode" ), kind == 0 ? 0 : 1 );
	glUniform1i( glGetUniformLocation( fp, "uPeelMode" ), alphaVaries ? 1 : 0 );
	glUniform1f( glGetUniformLocation( fp, "uMaxReach" ), maxReach );
	for( int k = 0; k < layers; ++k )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, fbos[ k ] );
		const GLfloat farA[] = { 1.0e4f, -1.0f, 0.0f, 0.0f }, zeroB[] = { 0, 0, 0, 0 };
		glClearBufferfv( GL_COLOR, 0, farA );
		glClearBufferfv( GL_COLOR, 1, zeroB );
		glClear( GL_DEPTH_BUFFER_BIT );
		for( int j = 0; j < 3; ++j )
		{
			glActiveTexture( GL_TEXTURE1 + j );
			glBindTexture( GL_TEXTURE_2D, j < k ? prevA[ j ] : 0 );
		}
		glUniform1i( glGetUniformLocation( fp, "uLayer" ), k );
		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, (GLsizei)count );
	}
}

void Renderer::copyInput( const FFGLViewportStruct& vp, GLuint hostFbo, const Input& input )
{
	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glViewport( (GLint)vp.x, (GLint)vp.y, (GLsizei)vp.width, (GLsizei)vp.height );
	glScissor( (GLint)vp.x, (GLint)vp.y, (GLsizei)vp.width, (GLsizei)vp.height );
	const GLuint prog = programs->copy;
	glUseProgram( prog );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, input.texture );
	glUniform1i( glGetUniformLocation( prog, "uInput" ), 0 );
	glUniform2f( glGetUniformLocation( prog, "uFieldSize" ), (float)vp.width, (float)vp.height );
	glUniform2f( glGetUniformLocation( prog, "uViewportOrigin" ), (float)vp.x, (float)vp.y );
	glUniform2f( glGetUniformLocation( prog, "uInputMax" ), input.maxU, input.maxV );
	glBindVertexArray( emptyVao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
}

void Renderer::render( const FFGLViewportStruct& vp, GLuint hostFbo, const std::vector< ShapeInstance >& glyphs, const std::vector< ShapeInstance >& boxes,
					   const BlockTransform& t, const Style& style, const Window& window, GLuint atlasTexture, int atlasSize, const Input& input )
{
	if( !isReady() || vp.width == 0 || vp.height == 0 )
		return;
	ffglkit::StateGuard guard;
	const int w = (int)vp.width, h = (int)vp.height;
	const float blockCos = std::cos( -t.rotation ), blockSin = std::sin( -t.rotation );
	const bool withInput = input.mode != InputMode::None && input.texture != 0;
	const bool inputShowsOutside = withInput && ( input.mode == InputMode::Over || input.mode == InputMode::CutOut );

	glDisable( GL_BLEND );
	glDisable( GL_CULL_FACE );
	glEnable( GL_SCISSOR_TEST );

	// Clear the whole output (or show the input video); only the shapes'
	// bounding box gets shaded afterwards.
	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glViewport( (GLint)vp.x, (GLint)vp.y, w, h );
	glScissor( (GLint)vp.x, (GLint)vp.y, w, h );
	glClearColor( 0, 0, 0, 0 );
	glClear( GL_COLOR_BUFFER_BIT );
	if( inputShowsOutside )
		copyInput( vp, hostFbo, input );
	const bool haveText   = !glyphs.empty() && atlasTexture != 0;
	const bool havePlates = !boxes.empty();
	if( ( !haveText && !havePlates ) || !programs->ensureTargets( w, h ) )
		return;

	Projector proj{ t, blockCos, blockSin, (float)w, (float)h };
	Rect bbox;
	if( haveText )
		proj.quads( bbox, glyphs, t.sizePx, true );
	if( havePlates )
		proj.quads( bbox, boxes, 1.0f, false );
	if( bbox.empty() )
		return;

	// Shapes of different opacity need several shapes per pixel; different tints
	// need two for smooth colour transitions; otherwise the nearest is enough.
	bool alphaVaries = false, tintVaries = false, plateAlphaVaries = false, plateTintVaries = false;
	varies( glyphs, alphaVaries, tintVaries );
	varies( boxes, plateAlphaVaries, plateTintVaries );
	if( window.on )
		alphaVaries = plateAlphaVaries = true;
	const int layers      = !haveText ? 0 : alphaVaries ? kLayers : tintVaries ? 2 : 1;
	// Melting plates need the two nearest plates per pixel
	const bool plateMerge = style.plateMerge > 0.0f && boxes.size() > 1;
	const int plateLayers = !havePlates ? 0 : plateAlphaVaries || plateTintVaries || plateMerge ? kPlateLayers : 1;
	const float windowFade = window.on ? std::max( 0.0f, window.fade ) : -1.0f;

	bbox = Rect{ bbox.x0 - 2, bbox.y0 - 2, bbox.x1 + 2, bbox.y1 + 2 };

	// ---- Effects: sets (field read at offsets), outlines, glows ----
	struct Set
	{
		int kind, dx, dy;// 1 drop shadow, 2 inner shadow; whole px, GL orientation (y up)
		Color color;
		float softness, spread;
	};
	std::vector< Set > sets;
	int numOutlines = 0, numGlows = 0, numInnerGlows = 0;
	float outlineColor[ 6 * 4 ], outlineEdge[ 6 ], glowColor[ 6 * 4 ], glowParam[ 6 * 2 ], innerGlowColor[ 6 * 4 ], innerGlowParam[ 6 * 2 ];
	const float fillEdge = style.fillEdge();
	float edge           = 0.0f;
	const Effect *bevel = nullptr, *extrude = nullptr, *contours = nullptr, *roughen = nullptr;
	int drops = 0, inners = 0;
	for( const Effect& e : style.effects )
	{
		auto put = []( float* dst, const Color& c ) {
			dst[ 0 ] = c.r;
			dst[ 1 ] = c.g;
			dst[ 2 ] = c.b;
			dst[ 3 ] = c.a;
		};
		switch( e.type )
		{
		case EffectType::Outline:
			if( e.size > 0 && numOutlines < 6 )
			{
				edge += numOutlines == 0 ? ( e.mode == 2 ? 0.0f : e.mode == 1 ? 0.5f * e.size : e.size ) : e.size;
				put( outlineColor + 4 * numOutlines, e.color );
				outlineEdge[ numOutlines++ ] = edge;
			}
			break;
		case EffectType::Glow:
			if( e.size > 0 && e.amount > 0 && e.color.a > 0 && numGlows < 6 )
			{
				put( glowColor + 4 * numGlows, e.color );
				glowParam[ 2 * numGlows ]     = e.size;
				glowParam[ 2 * numGlows + 1 ] = e.amount;
				++numGlows;
			}
			break;
		case EffectType::InnerGlow:
			if( haveText && e.size > 0 && e.amount > 0 && e.color.a > 0 && numInnerGlows < 6 )
			{
				put( innerGlowColor + 4 * numInnerGlows, e.color );
				innerGlowParam[ 2 * numInnerGlows ]     = e.size;
				innerGlowParam[ 2 * numInnerGlows + 1 ] = e.amount;
				++numInnerGlows;
			}
			break;
		case EffectType::Shadow:
			// Distance 0 and no spread = hidden behind the text: skip the work
			if( e.color.a > 0 && ( e.distance > 0 || e.size > 0 ) && drops < 3 && (int)sets.size() < 5 )
			{
				Set st{ 1, 0, 0, e.color, e.softness, e.size };
				offsetOf( e.distance, e.angle, st.dx, st.dy );
				sets.push_back( st );
				++drops;
			}
			break;
		case EffectType::InnerShadow:
			if( haveText && e.color.a > 0 && e.distance > 0 && inners < 2 && (int)sets.size() < 5 )
			{
				Set st{ 2, 0, 0, e.color, e.softness, 0 };
				offsetOf( e.distance, e.angle, st.dx, st.dy );
				sets.push_back( st );
				++inners;
			}
			break;
		case EffectType::Bevel:
			bevel = bevel ? bevel : ( haveText && e.size > 0 ? &e : nullptr );
			break;
		case EffectType::Extrude:
			extrude = extrude ? extrude : ( haveText && e.size > 0 && ( e.color.a > 0 || e.color2.a > 0 ) ? &e : nullptr );
			break;
		case EffectType::Contours:
			contours = contours ? contours : ( haveText && e.size > 0 && e.distance > 0 && e.color.a > 0 ? &e : nullptr );
			break;
		case EffectType::Roughen:
			roughen = roughen ? roughen : ( e.distance > 0 ? &e : nullptr );
			break;
		default:
			break;
		}
	}
	const float outerEdge = numOutlines ? outlineEdge[ numOutlines - 1 ] : 0.0f;
	// Extrusion direction (GL orientation) and length
	float exX = 0, exY = 0, exLen = 0;
	if( extrude )
	{
		exX   = std::cos( extrude->angle );
		exY   = std::sin( extrude->angle );
		exLen = extrude->size;
	}

	Rect shadedArea = bbox;// what the composite writes (screen px, y down)
	for( const Set& st : sets )
		if( st.kind == 1 )
			shadedArea.add( bbox.shifted( (float)st.dx, (float)-st.dy ) );
	if( extrude )
		shadedArea.add( bbox.shifted( exX * exLen, -exY * exLen ) );
	Rect fieldArea = shadedArea;// what the composite reads
	for( const Set& st : sets )
		fieldArea.add( shadedArea.shifted( (float)-st.dx, (float)st.dy ) );
	if( extrude )
		fieldArea.add( shadedArea.shifted( -exX * exLen, exY * exLen ) );
	fieldArea = Rect{ fieldArea.x0 - 2, fieldArea.y0 - 2, fieldArea.x1 + 2, fieldArea.y1 + 2 };

	// ---- Field passes ----
	glBindVertexArray( shapeVao );
	uploadInstances( glyphVbo, glyphCapacity, glyphs );
	uploadInstances( boxVbo, boxCapacity, boxes );

	const float reach = style.reach() + 2.0f;
	const GLuint fp   = programs->field;
	glUseProgram( fp );
	glUniform2f( glGetUniformLocation( fp, "uViewport" ), (float)w, (float)h );
	glUniform2f( glGetUniformLocation( fp, "uOrigin" ), t.originX, t.originY );
	glUniform2f( glGetUniformLocation( fp, "uBlockRot" ), blockCos, blockSin );
	glUniform1f( glGetUniformLocation( fp, "uPerspective" ), std::max( 0.0f, t.perspective ) );
	glUniform2f( glGetUniformLocation( fp, "uAtlasSize" ), (float)std::max( 1, atlasSize ), (float)std::max( 1, atlasSize ) );
	glUniform1f( glGetUniformLocation( fp, "uSharpCorners" ), style.sharpCorners ? 1.0f : 0.0f );
	glUniform1f( glGetUniformLocation( fp, "uDepthRange" ), 4096.0f );
	glUniform1i( glGetUniformLocation( fp, "uAtlas" ), 0 );
	glUniform1i( glGetUniformLocation( fp, "uPrev0" ), 1 );
	glUniform1i( glGetUniformLocation( fp, "uPrev1" ), 2 );
	glUniform1i( glGetUniformLocation( fp, "uPrev2" ), 3 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, atlasTexture );

	glViewport( 0, 0, w, h );
	setScissor( fieldArea, 0, 0, w, h );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glDepthMask( GL_TRUE );
	glClearDepth( 1.0 );
	if( haveText )
	{
		glUniform1f( glGetUniformLocation( fp, "uSize" ), t.sizePx );
		glUniform4f( glGetUniformLocation( fp, "uWindow" ), window.x0, window.y0, window.x1, window.y1 );
		glUniform1f( glGetUniformLocation( fp, "uWindowFade" ), windowFade );
		drawField( glyphVbo, glyphs.size(), 0, layers, alphaVaries, reach, programs->layerFbo, programs->layerA );
	}
	if( havePlates )
	{
		glUniform1f( glGetUniformLocation( fp, "uSize" ), 1.0f );
		glUniform4f( glGetUniformLocation( fp, "uWindow" ), window.x0, window.y0, window.x1, window.y1 );
		glUniform1f( glGetUniformLocation( fp, "uWindowFade" ), windowFade );
		drawField( boxVbo, boxes.size(), 1, plateLayers, plateAlphaVaries, style.plateReach() + 2.0f, programs->plateFbo, programs->plateA );
	}
	glDisable( GL_DEPTH_TEST );

	// ---- Composite into the host framebuffer ----
	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glViewport( (GLint)vp.x, (GLint)vp.y, w, h );
	setScissor( shadedArea, (int)vp.x, (int)vp.y, w, h );
	const GLuint prog = programs->composite;
	glUseProgram( prog );
	const char* aNames[ kLayers ] = { "uA0", "uA1", "uA2", "uA3" };
	const char* bNames[ kLayers ] = { "uB0", "uB1", "uB2", "uB3" };
	for( int k = 0; k < kLayers; ++k )
	{
		glActiveTexture( GL_TEXTURE0 + k );
		glBindTexture( GL_TEXTURE_2D, programs->layerA[ k ] );
		glActiveTexture( GL_TEXTURE0 + kLayers + k );
		glBindTexture( GL_TEXTURE_2D, programs->layerB[ k ] );
		glUniform1i( glGetUniformLocation( prog, aNames[ k ] ), k );
		glUniform1i( glGetUniformLocation( prog, bNames[ k ] ), kLayers + k );
	}
	const char* paNames[ kPlateLayers ] = { "uPA0", "uPA1" };
	const char* pbNames[ kPlateLayers ] = { "uPB0", "uPB1" };
	for( int k = 0; k < kPlateLayers; ++k )
	{
		const int ua = 2 * kLayers + k, ub = 2 * kLayers + kPlateLayers + k;
		glActiveTexture( GL_TEXTURE0 + ua );
		glBindTexture( GL_TEXTURE_2D, programs->plateA[ k ] );
		glActiveTexture( GL_TEXTURE0 + ub );
		glBindTexture( GL_TEXTURE_2D, programs->plateB[ k ] );
		glUniform1i( glGetUniformLocation( prog, paNames[ k ] ), ua );
		glUniform1i( glGetUniformLocation( prog, pbNames[ k ] ), ub );
	}
	const int inputUnit = 2 * kLayers + 2 * kPlateLayers;
	glActiveTexture( GL_TEXTURE0 + inputUnit );
	glBindTexture( GL_TEXTURE_2D, withInput ? input.texture : 0 );
	glUniform1i( glGetUniformLocation( prog, "uInput" ), inputUnit );
	glUniform1i( glGetUniformLocation( prog, "uInputMode" ), withInput ? (int)input.mode : 0 );
	glUniform2f( glGetUniformLocation( prog, "uInputMax" ), input.maxU, input.maxV );

	glUniform1i( glGetUniformLocation( prog, "uLayers" ), layers );
	glUniform1i( glGetUniformLocation( prog, "uPlateLayers" ), plateLayers );
	glUniform2f( glGetUniformLocation( prog, "uFieldSize" ), (float)w, (float)h );
	glUniform2f( glGetUniformLocation( prog, "uViewportOrigin" ), (float)vp.x, (float)vp.y );
	glUniform1f( glGetUniformLocation( prog, "uReach" ), reach );
	// Transition width between neighbouring glyphs' colours: grows with the outline
	// so merged outlines change colour smoothly from one glyph to the next.
	glUniform1f( glGetUniformLocation( prog, "uAttrSoftness" ), std::max( 1.0f, 0.3f * outerEdge + 0.01f * t.sizePx ) );

	setColor( prog, "uFill", style.fill );
	setColor( prog, "uFill2", style.fill2 );
	glUniform1i( glGetUniformLocation( prog, "uFillMode" ), (int)style.fillMode );
	glUniform2f( glGetUniformLocation( prog, "uBlockOrigin" ), t.originX, t.originY );
	glUniform2f( glGetUniformLocation( prog, "uBlockRotInv" ), std::cos( t.rotation ), std::sin( t.rotation ) );
	glUniform4f( glGetUniformLocation( prog, "uBox" ), t.boxLeft, t.boxTop, t.boxRight, t.boxBottom );
	glUniform2f( glGetUniformLocation( prog, "uGradDir" ), std::cos( style.gradientAngle ), -std::sin( style.gradientAngle ) );
	glUniform1f( glGetUniformLocation( prog, "uFillScale" ), std::max( 2.0f, style.fillScale * t.sizePx ) );

	glUniform1f( glGetUniformLocation( prog, "uWeight" ), style.weight );
	if( roughen )
		glUniform4f( glGetUniformLocation( prog, "uRough" ), std::max( 1.0f, roughen->size ), roughen->distance, 3.0f * std::cos( roughen->angle ),
					 3.0f * std::sin( roughen->angle ) );
	else
		glUniform4f( glGetUniformLocation( prog, "uRough" ), 1.0f, 0.0f, 0.0f, 0.0f );

	glUniform1i( glGetUniformLocation( prog, "uNumOutlines" ), numOutlines );
	if( numOutlines )
	{
		glUniform4fv( glGetUniformLocation( prog, "uOutlineColor" ), numOutlines, outlineColor );
		glUniform1fv( glGetUniformLocation( prog, "uOutlineEdge" ), numOutlines, outlineEdge );
	}
	glUniform1f( glGetUniformLocation( prog, "uFillEdge" ), numOutlines ? fillEdge : 0.0f );
	glUniform1f( glGetUniformLocation( prog, "uOuterEdge" ), outerEdge );
	glUniform1i( glGetUniformLocation( prog, "uNumGlows" ), numGlows );
	if( numGlows )
	{
		glUniform4fv( glGetUniformLocation( prog, "uGlowColor" ), numGlows, glowColor );
		glUniform2fv( glGetUniformLocation( prog, "uGlowParam" ), numGlows, glowParam );
	}
	glUniform1i( glGetUniformLocation( prog, "uNumInnerGlows" ), numInnerGlows );
	if( numInnerGlows )
	{
		glUniform4fv( glGetUniformLocation( prog, "uInnerGlowColor" ), numInnerGlows, innerGlowColor );
		glUniform2fv( glGetUniformLocation( prog, "uInnerGlowParam" ), numInnerGlows, innerGlowParam );
	}
	// Sets live at indices 1..n of the uniform arrays (index 0 = the pixel itself)
	{
		int offs[ 12 ] = { 0 }, kinds[ 6 ] = { 0 };
		float cols[ 24 ] = { 0 }, params[ 12 ] = { 0 };
		for( size_t i = 0; i < sets.size(); ++i )
		{
			const Set& st         = sets[ i ];
			offs[ 2 * ( i + 1 ) ] = st.dx;
			offs[ 2 * ( i + 1 ) + 1 ] = st.dy;
			kinds[ i + 1 ]        = st.kind;
			cols[ 4 * ( i + 1 ) ] = st.color.r;
			cols[ 4 * ( i + 1 ) + 1 ] = st.color.g;
			cols[ 4 * ( i + 1 ) + 2 ] = st.color.b;
			cols[ 4 * ( i + 1 ) + 3 ] = st.color.a;
			params[ 2 * ( i + 1 ) ] = st.softness;
			params[ 2 * ( i + 1 ) + 1 ] = st.spread;
		}
		glUniform1i( glGetUniformLocation( prog, "uNumSets" ), (int)sets.size() );
		glUniform2iv( glGetUniformLocation( prog, "uSetOffset" ), 6, offs );
		glUniform1iv( glGetUniformLocation( prog, "uSetKind" ), 6, kinds );
		glUniform4fv( glGetUniformLocation( prog, "uSetColor" ), 6, cols );
		glUniform2fv( glGetUniformLocation( prog, "uSetParam" ), 6, params );
	}
	glUniform1i( glGetUniformLocation( prog, "uBevelOn" ), bevel ? 1 : 0 );
	if( bevel )
	{
		setColor( prog, "uBevelHi", bevel->color );
		setColor( prog, "uBevelLo", bevel->color2 );
		glUniform4f( glGetUniformLocation( prog, "uBevelParam" ), bevel->size, bevel->softness, bevel->amount, (float)bevel->mode );
		glUniform2f( glGetUniformLocation( prog, "uBevelLight" ), std::cos( bevel->angle ), std::sin( bevel->angle ) );
	}
	glUniform1i( glGetUniformLocation( prog, "uExtrudeOn" ), extrude ? 1 : 0 );
	if( extrude )
	{
		setColor( prog, "uExtrudeColor", extrude->color );
		setColor( prog, "uExtrudeColor2", extrude->color2 );
		glUniform2f( glGetUniformLocation( prog, "uExtrudeDir" ), exX, exY );
		glUniform2f( glGetUniformLocation( prog, "uExtrudeParam" ), exLen, std::clamp( extrude->amount, 0.0f, 1.0f ) );
	}
	glUniform1i( glGetUniformLocation( prog, "uContoursOn" ), contours ? 1 : 0 );
	if( contours )
	{
		setColor( prog, "uContourColor", contours->color );
		glUniform4f( glGetUniformLocation( prog, "uContourParam" ), contours->size, contours->distance, std::clamp( contours->amount, 0.02f, 1.0f ),
					 (float)contours->mode );
	}
	setColor( prog, "uPlate", style.plate );
	setColor( prog, "uPlateBorder", style.plateBorderColor );
	glUniform1f( glGetUniformLocation( prog, "uPlateBorderWidth" ), style.plateBorderWidth );
	glUniform1f( glGetUniformLocation( prog, "uPlateMerge" ), plateMerge ? style.plateMerge : 0.0f );
	glUniform1f( glGetUniformLocation( prog, "uEdge" ), std::max( 0.25f, style.edgeSoftness ) );

	glBindVertexArray( emptyVao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
}

}//namespace sdfkit
