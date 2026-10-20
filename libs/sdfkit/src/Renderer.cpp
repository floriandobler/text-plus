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
constexpr int kInstanceVec4s = 9;// ShapeInstance = 9 vec4 attributes (locations 1..9)
static_assert( sizeof( ShapeInstance ) == kInstanceVec4s * 4 * sizeof( float ), "ShapeInstance layout" );
// ---------------------------------------------------------------------------
// Field passes: one vertex shader for every primitive
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
layout( location = 9 ) in vec4 iExtra;        // skew, weight px, blur px, -

uniform vec2 uViewport;    // px
uniform vec2 uOrigin;      // anchor position, px, y down
uniform vec2 uBlockRot;    // cos, sin of the block rotation (screen frame)
uniform float uSize;       // px per shape unit (font size for glyphs, 1 for boxes)
uniform float uBlockScale; // output px per block px (1, or less when the 3D path renders a large block)

out vec2 vUV;
out vec2 vLocal;           // px from the shape origin, y down, before animation scale and rotation
out vec2 vRest;            // layout space
out vec2 vBlock;           // block space (after animation)
flat out vec4 vMisc;
flat out vec4 vTint;
flat out vec4 vClip;
flat out vec4 vBox;
flat out float vId;
flat out float vScale;
flat out vec4 vExtra;

vec2 rotate( vec2 p, vec2 cs ) { return vec2( cs.x * p.x - cs.y * p.y, cs.y * p.x + cs.x * p.y ); }

void main()
{
	vec2 q        = mix( iQuad.xy, iQuad.zw, aCorner );
	vec2 unscaled = vec2( q.x, -q.y ) * uSize;
	// Skew shears the quad; UVs and the local box coordinates stay, so the shape shears with it.
	vec2 local    = vec2( unscaled.x - iExtra.x * unscaled.y, unscaled.y ) * iPosScaleRot.z;
	local         = rotate( local, vec2( cos( iPosScaleRot.w ), sin( iPosScaleRot.w ) ) );
	vec2 pb       = iPosScaleRot.xy + local;
	vec2 p        = rotate( pb * uBlockScale, uBlockRot ) + uOrigin;
	gl_Position   = vec4( p.x / uViewport.x * 2.0 - 1.0, 1.0 - p.y / uViewport.y * 2.0, 0.0, 1.0 );
	vUV    = mix( iUV.xy, iUV.zw, aCorner );
	vLocal = unscaled;
	vRest  = iRest.xy + unscaled * iRest.z;
	vBlock = pb;
	vMisc  = iMisc;
	vTint  = iTint;
	vClip  = iClip;
	vBox   = iBox;
	vScale = iPosScaleRot.z;
	vExtra = iExtra;
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
uniform float uBlockScale;
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
flat in float vId;
flat in float vScale;
flat in vec4 vExtra;

layout( location = 0 ) out vec4 oA;// distance, shape id, opacity, tint amount
layout( location = 1 ) out vec4 oB;// tint colour, blur / 64 px

float median( vec3 v ) { return max( min( v.r, v.g ), min( max( v.r, v.g ), v.b ) ); }

// Signed distance to this glyph's outline in output pixels, outside positive.
float glyphDistancePx()
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
	// Output px; the weight grows or thins the shape.
	float dist  = ( ( uKind == 0 ? glyphDistancePx() : boxDistancePx() ) - vExtra.y ) * uBlockScale;
	float alpha = vMisc.x;
	if( vClip.x > -1.0e8 || vClip.y > -1.0e8 || vClip.z < 1.0e8 || vClip.w < 1.0e8 )
	{
		float dc = rectDistance( vRest, vClip );
		if( uClipMode == 1 )
			dist = max( dist, dc * vScale * uBlockScale );
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
	oB = vec4( vTint.rgb, clamp( vExtra.z * uBlockScale / 64.0, 0.0, 1.0 ) );
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
// The style stack, bottom to top:
//   drop shadow   cast by text (outer edge) and plates together
//   plate         colour, border inside
//   glow          text only
//   text          outline 2, outline 1, fill (+ inner glow, inner shadow)
// Shadow and glow are knocked out under the text silhouette, the shadow also
// under the plates (Photoshop's default).
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
uniform sampler2D uVideo;
uniform int uLayers;         // text layers in use (0 = no text)
uniform int uPlateLayers;    // plate layers in use (0 = no plates)
uniform vec2 uFieldSize;
uniform vec2 uViewportOrigin;
uniform float uAttrSoftness; // px: colour transition width where glyphs merge
uniform float uBlockScale;   // output px per block px

uniform vec4 uFill;          // straight colours
uniform vec4 uFill2;
uniform int uFillMode;       // 0 solid, 1 box gradient, 2 stripes, 3 halftone, 4 checker, 5 noise, 6 LED
uniform vec2 uBlockOrigin;   // anchor, px, y down
uniform vec2 uBlockRotInv;   // cos, sin to go from screen back into block space
uniform vec4 uBox;           // block metric box l, t, r, b (px, y down, block space)
uniform vec2 uGradDir;       // gradient direction in block space
uniform vec2 uPatDir;        // pattern direction in block space
uniform float uPatScale;     // block px
uniform float uPatOffset;    // 0..1

uniform vec4 uOutline1;
uniform vec4 uOutline2;
uniform vec3 uOutline1Edges; // width, outer edge, inner edge (px, distance space)
uniform float uOutline2Width;
uniform vec4 uShadow;
uniform ivec2 uShadowOffset; // whole px, GL orientation (y up)
uniform float uShadowSoftness;
uniform float uShadowSpread;
uniform vec4 uGlow;
uniform float uGlowRadius;
uniform float uGlowStrength;
uniform vec4 uInnerGlow;
uniform float uInnerGlowRadius;
uniform vec4 uInnerShadow;
uniform ivec2 uInnerShadowOffset;// whole px, GL orientation
uniform float uInnerShadowSoftness;
uniform int uInnerShadowOn;
uniform vec4 uPlate;
uniform vec4 uPlateBorder;
uniform float uPlateBorderWidth;
uniform float uPlateMerge;
uniform float uEdge;         // anti-aliasing width in px

// Split output (3D path): 0 = the picture as straight alpha; 1 = three targets:
// the whole picture (premultiplied), what lies behind the text (premultiplied)
// and the silhouette's distance field.
uniform int uSplit;
uniform float uSilClamp;     // px: distances beyond this are not known
// Video: 0 none, 1 over, 2 fill, 3 knockout. In split mode only "fill" applies here.
uniform int uVideoMode;
uniform vec2 uVideoMax;      // used part of the video texture
uniform int uVideoMap;       // 0: screen = this pass; 1: offscreen block space, mapped onto the screen
uniform vec2 uScreenOrigin;  // anchor on the screen, px, y down (uVideoMap 1)
uniform vec2 uScreenRot;     // cos, sin of the block rotation on the screen
uniform vec2 uScreenSize;

layout( location = 0 ) out vec4 oColor;
layout( location = 1 ) out vec4 oBack;
layout( location = 2 ) out vec4 oSil;

const float kFar = 1.0e4;

vec4 premul( vec4 c ) { return vec4( c.rgb * c.a, c.a ); }
float coverW( float d, float edge, float w ) { return clamp( 0.5 - ( d - edge ) / w, 0.0, 1.0 ); }// coverage of "d < edge"
float inside( float d, float edge ) { return coverW( d, edge, uEdge ); }

// All shapes that matter at this pixel, from up to five places:
//   set 0 text at p, 1 text at the shadow offset, 2 text at the inner shadow
//   offset, 3 plates at p, 4 plates at the shadow offset.
const int kMaxEntries = 16;
vec4 E[ kMaxEntries ];// distance, set, opacity, tint amount
vec4 Et[ kMaxEntries ];// tint colour, blur / 64 px
int nE = 0;

bool outsideField( ivec2 ip ) { return any( lessThan( ip, ivec2( 0 ) ) ) || any( greaterThanEqual( ip, ivec2( uFieldSize ) ) ); }

void addLayer( vec4 a, vec4 b, float set )
{
	E[ nE ]  = vec4( a.r, set, a.b, a.a );
	Et[ nE ] = b;
	nE++;
}

void loadText( ivec2 ip, float set )
{
	if( outsideField( ip ) )
		return;
	for( int k = 0; k < uLayers; ++k )
	{
		vec4 a;
		vec4 b;
		if( k == 0 ) { a = texelFetch( uA0, ip, 0 ); b = texelFetch( uB0, ip, 0 ); }
		else if( k == 1 ) { a = texelFetch( uA1, ip, 0 ); b = texelFetch( uB1, ip, 0 ); }
		else if( k == 2 ) { a = texelFetch( uA2, ip, 0 ); b = texelFetch( uB2, ip, 0 ); }
		else { a = texelFetch( uA3, ip, 0 ); b = texelFetch( uB3, ip, 0 ); }
		if( a.r >= 0.5 * kFar )
			break;// no further shape near this pixel
		addLayer( a, b, set );
	}
}

// Nearest text distance at another texel (LED cells)
float textDistanceAt( ivec2 ip )
{
	if( outsideField( ip ) || uLayers == 0 )
		return kFar;
	float d = texelFetch( uA0, ip, 0 ).r;
	if( uLayers > 1 ) d = min( d, texelFetch( uA1, ip, 0 ).r );
	if( uLayers > 2 ) d = min( d, texelFetch( uA2, ip, 0 ).r );
	if( uLayers > 3 ) d = min( d, texelFetch( uA3, ip, 0 ).r );
	return d;
}

void loadPlates( ivec2 ip, float set )
{
	if( outsideField( ip ) )
		return;
	for( int k = 0; k < uPlateLayers; ++k )
	{
		vec4 a;
		vec4 b;
		if( k == 0 ) { a = texelFetch( uPA0, ip, 0 ); b = texelFetch( uPB0, ip, 0 ); }
		else { a = texelFetch( uPA1, ip, 0 ); b = texelFetch( uPB1, ip, 0 ); }
		if( a.r >= 0.5 * kFar )
			break;
		addLayer( a, b, set );
	}
}

float smoothUnion( float d, float b, float k )
{
	if( k <= 0.0 || d >= 0.5 * kFar )
		return min( d, b );
	float h = max( k - abs( d - b ), 0.0 ) / k;
	return min( d, b ) - h * h * k * 0.25;
}

vec2 rot( vec2 p, vec2 cs ) { return vec2( cs.x * p.x - cs.y * p.y, cs.y * p.x + cs.x * p.y ); }

// Screen px (y down, from the anchor) -> block px
vec2 toBlock( vec2 frag )
{
	vec2 s = vec2( frag.x, uFieldSize.y - frag.y ) - uBlockOrigin;
	return rot( s, uBlockRotInv ) / uBlockScale;
}
// Block px -> texel of this pass
ivec2 toTexel( vec2 local )
{
	vec2 s = rot( local * uBlockScale, vec2( uBlockRotInv.x, -uBlockRotInv.y ) ) + uBlockOrigin;
	return ivec2( floor( vec2( s.x, uFieldSize.y - s.y ) ) );
}

vec4 videoAt( vec2 frag )
{
	vec2 gl = frag;
	if( uVideoMap == 1 )
	{
		vec2 p   = toBlock( frag );
		vec2 scr = rot( p, uScreenRot ) + uScreenOrigin;
		gl       = vec2( scr.x, uScreenSize.y - scr.y );
		return texture( uVideo, clamp( gl / uScreenSize, 0.0, 1.0 ) * uVideoMax );
	}
	return texture( uVideo, clamp( gl / uFieldSize, 0.0, 1.0 ) * uVideoMax );
}

float hash12( vec2 p )
{
	vec3 p3 = fract( vec3( p.xyx ) * 0.1031 );
	p3 += dot( p3, p3.yzx + 33.33 );
	return fract( ( p3.x + p3.y ) * p3.z );
}
float valueNoise( vec2 p )
{
	vec2 i = floor( p ), f = fract( p );
	vec2 u = f * f * ( 3.0 - 2.0 * f );
	return mix( mix( hash12( i ), hash12( i + vec2( 1, 0 ) ), u.x ), mix( hash12( i + vec2( 0, 1 ) ), hash12( i + vec2( 1, 1 ) ), u.x ), u.y );
}
// Smooth square wave in -1..1 (antialiased sign of sin(pi x))
float aaSquare( float x )
{
	float w = max( fwidth( x ), 1e-4 );
	return clamp( sin( 3.14159265 * x ) / ( 3.14159265 * w ), -1.0, 1.0 );
}

// Position of p along `dir` across the block, 0..1
float acrossBlock( vec2 local, vec2 dir )
{
	vec2 c     = 0.5 * ( uBox.xy + uBox.zw );
	vec2 halfs = 0.5 * ( uBox.zw - uBox.xy );
	float ext  = max( abs( halfs.x * dir.x ) + abs( halfs.y * dir.y ), 1e-3 );
	return clamp( dot( local - c, dir ) / ext * 0.5 + 0.5, 0.0, 1.0 );
}

void writeOut( vec4 picturePremul, vec4 backPremul, float dSil, float silAlpha, vec2 frag )
{
	if( uSplit == 1 )
	{
		oColor = picturePremul;
		oBack  = backPremul;
		oSil   = vec4( min( dSil, uSilClamp ), silAlpha, 0.0, 1.0 );
		return;
	}
	vec4 r = picturePremul;
	if( uVideoMode == 1 )
		r = r + premul( videoAt( frag ) ) * ( 1.0 - r.a );
	else if( uVideoMode == 3 )
		r = premul( videoAt( frag ) ) * ( 1.0 - r.a );
	if( r.a > 0.0 )
		r.rgb /= r.a;// Resolume composites sources as straight alpha
	oColor = r;
	oBack  = vec4( 0.0 );
	oSil   = vec4( 0.0 );
}

void main()
{
	vec2 frag = gl_FragCoord.xy - uViewportOrigin;
	ivec2 ip  = ivec2( frag );
	bool shadowOn = uShadow.a > 0.0;
	bool platesOn = uPlateLayers > 0;
	loadText( ip, 0.0 );
	if( shadowOn )
		loadText( ip - uShadowOffset, 1.0 );
	if( uInnerShadowOn == 1 )
		loadText( ip - uInnerShadowOffset, 2.0 );
	if( platesOn )
	{
		loadPlates( ip, 3.0 );
		if( shadowOn )
			loadPlates( ip - uShadowOffset, 4.0 );
	}
	if( nE == 0 )
	{
		writeOut( vec4( 0.0 ), vec4( 0.0 ), kFar, 0.0, frag );
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

	// Fill colour: solid, box gradient, pattern or video (position based, the same for every threshold)
	vec4 baseFill = uFill;
	vec2 local    = toBlock( frag );
	float ledCover = -1.0;// LED: coverage of the fill by the lit dots (< 0 = not LED)
	if( uFillMode == 1 )
		baseFill = mix( uFill, uFill2, acrossBlock( local, uGradDir ) );
	else if( uFillMode >= 2 )
	{
		float u = dot( local, uPatDir ), v = dot( local, vec2( -uPatDir.y, uPatDir.x ) );
		float sc = max( uPatScale, 1.0 );
		float edgeB = uEdge / uBlockScale;
		if( uFillMode == 2 )// stripes, half on, half off
			baseFill = mix( uFill, uFill2, 0.5 - 0.5 * aaSquare( 2.0 * ( u / sc + uPatOffset ) ) );
		else if( uFillMode == 3 )// halftone: dots grow along the pattern direction
		{
			vec2 g  = vec2( u / sc + uPatOffset, v / sc );
			vec2 cc = floor( g ) + 0.5;
			float r = 0.5 * 1.08 * sqrt( acrossBlock( local, uPatDir ) );
			float m = clamp( 0.5 - ( length( g - cc ) - r ) * sc / edgeB, 0.0, 1.0 );
			baseFill = mix( uFill, uFill2, m );
		}
		else if( uFillMode == 4 )// checker
			baseFill = mix( uFill, uFill2, 0.5 - 0.5 * aaSquare( u / sc + 2.0 * uPatOffset ) * aaSquare( v / sc ) );
		else if( uFillMode == 5 )// noise, looping: the sample point circles
		{
			float a  = 6.2831853 * uPatOffset;
			vec2 q   = vec2( u, v ) / sc + 1.5 * vec2( cos( a ), sin( a ) );
			float n  = 0.6 * valueNoise( q ) + 0.3 * valueNoise( q * 2.13 + 17.0 ) + 0.1 * valueNoise( q * 4.7 + 3.0 );
			baseFill = mix( uFill, uFill2, smoothstep( 0.35, 0.65, n ) );
		}
		else if( uFillMode == 6 )// LED: lit where the cell centre is inside the fill
		{
			vec2 g     = local / sc;
			vec2 cell  = ( floor( g ) + 0.5 ) * sc;
			float dc   = textDistanceAt( toTexel( cell ) );
			float lit  = dc < uOutline1Edges.z ? 1.0 : 0.0;
			float dd   = length( local - cell );
			ledCover   = lit * clamp( 0.5 - ( dd - 0.42 * sc ) / edgeB, 0.0, 1.0 );
		}
	}
	if( uVideoMode == 2 )
	{
		vec4 vc  = videoAt( frag );
		baseFill = vec4( vc.rgb, vc.a * uFill.a );
	}

	// The integral over the opacity threshold t: going from the most opaque shape
	// down, the shapes with opacity >= t only ever grow, so unions and tint sums
	// are kept as running values and every level is shaded once.
	float dText = kFar, dShadowText = kFar, dInner = kFar, dPlate = kFar, dShadowPlate = kFar;
	float tw = 0.0, twt = 0.0, pw = 0.0, pwt = 0.0;// tint weights (text, plates)
	vec3 tc = vec3( 0.0 ), pc3 = vec3( 0.0 );
	float blurNear = 0.0, silAlpha = 0.0;// blur and opacity of the nearest text shape
	float outer = uOutline1Edges.y + ( uOutline2Width > 0.0 ? uOutline2Width : 0.0 );
	vec4 result = vec4( 0.0 ), backAll = vec4( 0.0 );
	for( int s = 0; s < nE; ++s )
	{
		int i     = order[ s ];
		vec4 e    = E[ i ];
		int set   = int( e.g + 0.5 );
		if( set == 0 )
		{
			if( e.r < dText )
			{
				blurNear = Et[ i ].a * 64.0;
				silAlpha = e.b;
			}
			dText    = min( dText, e.r );
			float w  = exp( clamp( -e.r / uAttrSoftness, -40.0, 40.0 ) );
			tw      += w;
			twt     += w * e.a;
			tc      += w * e.a * Et[ i ].rgb;
		}
		else if( set == 1 )
			dShadowText = min( dShadowText, e.r );
		else if( set == 2 )
			dInner = min( dInner, e.r );
		else if( set == 3 )
		{
			dPlate   = smoothUnion( dPlate, e.r, uPlateMerge );
			float w  = exp( clamp( -e.r / uAttrSoftness, -40.0, 40.0 ) );
			pw      += w;
			pwt     += w * e.a;
			pc3     += w * e.a * Et[ i ].rgb;
		}
		else
			dShadowPlate = smoothUnion( dShadowPlate, e.r, uPlateMerge );

		float t    = e.b;
		float next = s + 1 < nE ? E[ order[ s + 1 ] ].b : 0.0;
		float weight = t - next;
		if( weight <= 0.0 )
			continue;// more shapes at the same opacity follow

		float d   = dText;
		float aa  = max( uEdge, blurNear );
		vec4 fill = baseFill;
		if( twt > 0.0 && tw > 0.0 )
			fill.rgb = mix( fill.rgb, tc / twt, clamp( twt / tw, 0.0, 1.0 ) );
		vec4 fillP = premul( fill );
		if( uInnerGlowRadius > 0.0 && uInnerGlow.a > 0.0 )
		{
			float gi = max( uOutline1Edges.z - d, 0.0 );// depth inside the fill
			float kk = clamp( 1.0 - gi / uInnerGlowRadius, 0.0, 1.0 );
			vec4 ig  = premul( uInnerGlow ) * ( kk * kk );
			fillP    = ig + fillP * ( 1.0 - ig.a );
		}
		if( uInnerShadowOn == 1 )
		{
			float w  = max( uInnerShadowSoftness, uEdge );
			vec4 is  = premul( uInnerShadow ) * smoothstep( -0.5 * w, 0.5 * w, dInner - uOutline1Edges.z );// the shape moved by the offset
			fillP    = is + fillP * ( 1.0 - is.a );
		}

		// Text stack: inner layers REPLACE outer ones (mix by coverage), so a
		// transparent fill shows through (hollow text) and bands have no seams.
		float cOut2 = uOutline2Width > 0.0 ? coverW( d, outer, aa ) : 0.0;
		float cOut1 = uOutline1Edges.x > 0.0 ? coverW( d, uOutline1Edges.y, aa ) : 0.0;
		float cFill = ledCover >= 0.0 ? ledCover : coverW( d, uOutline1Edges.z, aa );
		vec4 text = vec4( 0.0 );
		text = mix( text, premul( uOutline2 ), cOut2 );
		text = mix( text, premul( uOutline1 ), cOut1 );
		text = mix( text, fillP, cFill );

		float textShape = max( max( cOut2, cOut1 ), cFill );
		vec4 glowC      = vec4( 0.0 );
		if( uGlowRadius > 0.0 && uGlowStrength > 0.0 && uGlow.a > 0.0 )
		{
			float g  = max( d - outer, 0.0 );
			float kk = clamp( 1.0 - g / uGlowRadius, 0.0, 1.0 );
			glowC    = premul( uGlow ) * clamp( uGlowStrength * kk * kk, 0.0, 1.0 );
		}

		vec4 back = vec4( 0.0 ), backRaw = vec4( 0.0 );
		if( !platesOn )
		{
			// Effects behind the text, knocked out under the text silhouette (Photoshop's default).
			if( shadowOn )
			{
				float w  = max( uShadowSoftness, uEdge );
				float ds = dShadowText - outer - uShadowSpread;
				back     = premul( uShadow ) * ( 1.0 - smoothstep( -0.5 * w, 0.5 * w, ds ) );
			}
			back    = glowC + back * ( 1.0 - glowC.a );
			backRaw = back;
			back   *= 1.0 - textShape;
		}
		else
		{
			vec4 pcol = uPlate;
			if( pwt > 0.0 && pw > 0.0 )
				pcol.rgb = mix( pcol.rgb, pc3 / pwt, clamp( pwt / pw, 0.0, 1.0 ) );
			float plateShape = inside( dPlate, 0.0 );
			vec4 plateC = vec4( 0.0 );
			if( uPlateBorderWidth > 0.0 )
				plateC = mix( plateC, premul( uPlateBorder ), plateShape );
			plateC = mix( plateC, premul( pcol ), inside( dPlate, -uPlateBorderWidth ) );
			vec4 shadowC = vec4( 0.0 );
			if( shadowOn )
			{
				float w  = max( uShadowSoftness, uEdge );
				float ds = min( dShadowText - outer, dShadowPlate ) - uShadowSpread;
				shadowC  = premul( uShadow ) * ( 1.0 - smoothstep( -0.5 * w, 0.5 * w, ds ) ) * ( 1.0 - plateShape );
			}
			backRaw = plateC + shadowC * ( 1.0 - plateC.a );
			backRaw = glowC + backRaw * ( 1.0 - glowC.a );
			shadowC *= 1.0 - textShape;
			glowC *= 1.0 - textShape;
			back = plateC + shadowC * ( 1.0 - plateC.a );
			back = glowC + back * ( 1.0 - glowC.a );
		}
		result += weight * ( text + back * ( 1.0 - text.a ) );
		backAll += weight * backRaw;
	}
	writeOut( result, backAll, dText - outer, dText - outer < 2.0 * uEdge ? silAlpha : 0.0, frag );
}
)GLSL";

// ---------------------------------------------------------------------------
// View pass (3D): the composite's block-space textures seen through a camera.
// Object space: block px (x right, y down), z towards the viewer; the letters
// stand on the plane z = 0 and reach to z = depth. The world is the object
// rotated about the front face's anchor; the camera sits at z = focal.
// ---------------------------------------------------------------------------
const char* kViewFS = R"GLSL(#version 410 core
uniform sampler2D uFront;   // whole 2D picture, premultiplied
uniform sampler2D uBack;    // plates, shadow, glow, premultiplied
uniform sampler2D uSil;     // r: silhouette distance (offscreen px), g: opacity of the nearest letter
uniform sampler2D uVideo;
uniform vec2 uTexSize;      // allocated size of the view targets
uniform vec2 uUsed;         // used size (offscreen px)
uniform vec4 uRect;         // block px covered by the textures: x0 y0 x1 y1
uniform float uScale;       // offscreen px per block px
uniform vec2 uViewportOrigin;
uniform vec2 uViewportSize;
uniform vec2 uAnchor;       // screen px, y down
uniform mat3 uRot;          // object -> world
uniform float uFocal;       // px, 0 = orthographic
uniform int uMode;          // 1 tilt, 2 extrude
uniform float uDepth;
uniform float uBevel;
uniform float uSilClamp;    // block px
uniform vec4 uSide;         // straight
uniform vec3 uLight;        // world, normalised, towards the light
uniform float uShading;
uniform float uGloss;
uniform float uEdge;
uniform int uVideoMode;     // 1 over, 3 knockout (fill is done by the composite)
uniform vec2 uVideoMax;
out vec4 oColor;

vec2 uvOf( vec2 p )
{
	vec2 tp = ( p - uRect.xy ) * uScale;
	return vec2( tp.x, uUsed.y - tp.y ) / uTexSize;
}
bool inRect( vec2 p ) { return p.x >= uRect.x && p.y >= uRect.y && p.x <= uRect.z && p.y <= uRect.w; }
vec4 frontAt( vec2 p ) { return inRect( p ) ? texture( uFront, uvOf( p ) ) : vec4( 0.0 ); }
vec4 backAt( vec2 p ) { return inRect( p ) ? texture( uBack, uvOf( p ) ) : vec4( 0.0 ); }
vec2 silAt( vec2 p )
{
	if( !inRect( p ) )
		return vec2( uSilClamp, 0.0 );
	vec4 s = texture( uSil, uvOf( p ) );
	return vec2( min( s.r / uScale, uSilClamp ), s.g );
}

float sdSolid( vec3 p )
{
	float d2 = silAt( p.xy ).x;
	float hz = 0.5 * uDepth;
	float dz = abs( p.z - hz ) - hz;
	float b  = min( uBevel, hz );
	vec2 w   = vec2( d2 + b, dz + b );
	return min( max( w.x, w.y ), 0.0 ) + length( max( w, 0.0 ) ) - b;
}

void main()
{
	vec2 frag = gl_FragCoord.xy - uViewportOrigin;
	vec2 s    = vec2( frag.x, uViewportSize.y - frag.y ) - uAnchor;
	vec3 ro, rd;
	float span = max( uRect.z - uRect.x, uRect.w - uRect.y ) + uDepth;
	if( uFocal > 0.0 )
	{
		ro = vec3( 0.0, 0.0, uFocal );
		rd = normalize( vec3( s, -uFocal ) );
	}
	else
	{
		ro = vec3( s, 4.0 * span + 10.0 );
		rd = vec3( 0.0, 0.0, -1.0 );
	}
	mat3 toObj = transpose( uRot );
	vec3 o     = toObj * ro + vec3( 0.0, 0.0, uDepth );
	vec3 d     = toObj * rd;

	// The plane z = 0: the whole picture (tilt) or what lies behind the letters (extrude)
	float tPlane = abs( d.z ) > 1e-6 ? -o.z / d.z : -1.0;
	vec4 plane   = vec4( 0.0 );
	if( tPlane > 0.0 )
	{
		vec2 pp = ( o + d * tPlane ).xy;
		plane   = uMode == 1 || uDepth <= 0.0 ? frontAt( pp ) : backAt( pp );// no depth: the flat picture
	}

	vec4 col = plane;
	if( uMode == 2 && uDepth > 0.0 )
	{
		// Ray against the box around the solid, then sphere tracing inside it
		vec3 sd    = vec3( abs( d.x ) < 1e-7 ? 1e-7 : d.x, abs( d.y ) < 1e-7 ? 1e-7 : d.y, abs( d.z ) < 1e-7 ? 1e-7 : d.z );
		vec3 inv   = 1.0 / sd;
		vec3 t0    = ( vec3( uRect.xy, 0.0 ) - o ) * inv, t1 = ( vec3( uRect.zw, uDepth ) - o ) * inv;
		vec3 tmin  = min( t0, t1 ), tmax = max( t0, t1 );
		float tIn  = max( max( tmin.x, tmin.y ), max( tmin.z, 0.0 ) );
		float tOut = min( min( tmax.x, tmax.y ), tmax.z );
		float best = 1e9, tBest = tIn;
		if( tOut > tIn )
		{
			float t = tIn;
			for( int i = 0; i < 192; ++i )
			{
				vec3 p   = o + d * t;
				float fp = uFocal > 0.0 ? max( t, 1.0 ) / uFocal : 1.0;// block px per screen px
				float dd = sdSolid( p );
				float sp = dd / fp;
				if( sp < best )
				{
					best  = sp;
					tBest = t;
				}
				if( sp < 0.02 )
				{
					best = -1.0;
					break;
				}
				t += max( dd * 0.9, 0.2 * fp );
				if( t > tOut )
					break;
			}
		}
		float cover = clamp( 0.5 - best / max( uEdge, 0.5 ), 0.0, 1.0 );
		vec4 solid  = vec4( 0.0 );
		if( cover > 0.0 )
		{
			vec3 hp  = o + d * tBest;
			float e  = max( 0.35, ( uFocal > 0.0 ? tBest / uFocal : 1.0 ) * 0.6 );
			vec3 n   = vec3( sdSolid( hp + vec3( e, 0, 0 ) ) - sdSolid( hp - vec3( e, 0, 0 ) ), sdSolid( hp + vec3( 0, e, 0 ) ) - sdSolid( hp - vec3( 0, e, 0 ) ),
							 sdSolid( hp + vec3( 0, 0, e ) ) - sdSolid( hp - vec3( 0, 0, e ) ) );
			n        = length( n ) > 1e-6 ? normalize( n ) : vec3( 0.0, 0.0, 1.0 );
			vec3 nw  = uRot * n;
			float diff = max( dot( nw, uLight ), 0.0 );
			float flatL = max( dot( uRot * vec3( 0.0, 0.0, 1.0 ), uLight ), 0.0 );
			vec4 fr    = frontAt( hp.xy );
			vec3 frC   = fr.a > 1e-4 ? fr.rgb / fr.a : uSide.rgb;
			// Front and bevel carry the 2D picture (lit relative to the flat face, so
			// the face keeps its colours); the sides carry the side colour.
			float faceW = smoothstep( 0.25, 0.85, n.z );
			vec3 faceC  = mix( uSide.rgb, frC, clamp( fr.a, 0.0, 1.0 ) ) * max( 0.0, 1.0 + uShading * ( diff - flatL ) );
			vec3 sideC  = uSide.rgb * mix( 1.0, 0.25 + 0.75 * diff, uShading );
			vec3 h      = normalize( uLight - rd );
			float spec  = uGloss * pow( max( dot( nw, h ), 0.0 ), 48.0 ) * ( 1.0 - 0.7 * faceW * step( 0.97, n.z ) );
			vec3 c      = mix( sideC, faceC, faceW ) + vec3( spec );
			// Opacity of the letter (fading characters); edge samples just outside read 0
			float la    = silAt( hp.xy ).y;
			float a     = clamp( cover * ( la > 0.0 ? la : 1.0 ), 0.0, 1.0 );
			solid       = vec4( c * a, a );
			// Seen from behind, the plane lies in front of the solid
			if( tPlane > 0.0 && tPlane < tBest )
				col = plane + solid * ( 1.0 - plane.a );
			else
				col = solid + plane * ( 1.0 - solid.a );
		}
	}

	if( uVideoMode == 1 || uVideoMode == 3 )
	{
		vec4 v = texture( uVideo, clamp( frag / uViewportSize, 0.0, 1.0 ) * uVideoMax );
		v.rgb *= v.a;
		col = uVideoMode == 1 ? col + v * ( 1.0 - col.a ) : v * ( 1.0 - col.a );
	}
	if( col.a > 0.0 )
		col.rgb /= col.a;
	oColor = col;
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
	auto px = [ & ]( float v, int hi ) { return (int)std::clamp( v, 0.0f, (float)hi ); };
	int x0 = px( std::floor( r.x0 ), w ), x1 = px( std::ceil( r.x1 ), w );
	int y0 = px( std::floor( r.y0 ), h ), y1 = px( std::ceil( r.y1 ), h );
	if( x1 <= x0 || y1 <= y0 )
	{
		glScissor( vpX, vpY, 0, 0 );
		return;
	}
	glScissor( vpX + x0, vpY + ( h - y1 ), x1 - x0, y1 - y0 );
}

// Bounding box of instanced quads (they already include the effect reach),
// transformed by scale `k`, rotation (blockCos, blockSin) and translation (ox, oy).
void addQuads( Rect& bbox, const std::vector< ShapeInstance >& items, float unitPx, float k, float blockCos, float blockSin, float ox, float oy )
{
	for( const ShapeInstance& g : items )
	{
		const float c = std::cos( g.rotation ), s = std::sin( g.rotation );
		const float ex[ 2 ] = { g.quadL, g.quadR }, ey[ 2 ] = { g.quadB, g.quadT };
		for( float emx : ex )
			for( float emy : ey )
			{
				float ly = -emy * unitPx;
				float lx = ( emx * unitPx - g.skew * ly ) * g.scale;
				ly *= g.scale;
				float rx = ( c * lx - s * ly + g.x ) * k, ry = ( s * lx + c * ly + g.y ) * k;
				bbox.add( blockCos * rx - blockSin * ry + ox, blockSin * rx + blockCos * ry + oy );
			}
	}
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
		tintVaries  = tintVaries || g.tintAmount != g0.tintAmount || g.tintR != g0.tintR || g.tintG != g0.tintG || g.tintB != g0.tintB || g.blurPx != g0.blurPx;
	}
}

// Object -> world rotation of the 3D view (column-major for glUniformMatrix3fv)
struct Mat3
{
	float m[ 9 ];// column-major
	float at( int r, int c ) const { return m[ c * 3 + r ]; }
};
Mat3 mul( const Mat3& a, const Mat3& b )
{
	Mat3 r{};
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
		{
			float s = 0;
			for( int k = 0; k < 3; ++k )
				s += a.at( i, k ) * b.at( k, j );
			r.m[ j * 3 + i ] = s;
		}
	return r;
}
Mat3 rotX( float a )
{
	const float c = std::cos( a ), s = std::sin( a );
	return Mat3{ { 1, 0, 0, 0, c, s, 0, -s, c } };
}
Mat3 rotY( float a )
{
	const float c = std::cos( a ), s = std::sin( a );
	return Mat3{ { c, 0, -s, 0, 1, 0, s, 0, c } };
}
Mat3 rotZ( float a )
{
	const float c = std::cos( a ), s = std::sin( a );
	return Mat3{ { c, s, 0, -s, c, 0, 0, 0, 1 } };
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
	GLuint field = 0, composite = 0, view = 0;

	// Layer k: A = distance, shape id, opacity, tint amount (RGBA16F); B = tint colour, blur (RGBA8)
	GLuint layerFbo[ kLayers ]{}, layerA[ kLayers ]{}, layerB[ kLayers ]{};
	GLuint plateFbo[ kPlateLayers ]{}, plateA[ kPlateLayers ]{}, plateB[ kPlateLayers ]{};
	GLuint depth = 0;
	int w = 0, h = 0;
	// 3D: the composite's block-space picture (front), what lies behind (back), silhouette
	GLuint viewFbo = 0, viewFront = 0, viewBack = 0, viewSil = 0;
	int vw = 0, vh = 0;

	void destroy()
	{
		glDeleteProgram( field );
		glDeleteProgram( composite );
		glDeleteProgram( view );
		releaseTargets();
		releaseViewTargets();
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

	void releaseViewTargets()
	{
		if( viewFbo )
			glDeleteFramebuffers( 1, &viewFbo );
		const GLuint tex[ 3 ] = { viewFront, viewBack, viewSil };
		glDeleteTextures( 3, tex );
		viewFbo = viewFront = viewBack = viewSil = 0;
		vw = vh = 0;
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

	// Grows (never shrinks) to the largest size seen in this context.
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

	bool ensureViewTargets( int needW, int needH )
	{
		if( viewFbo && needW <= vw && needH <= vh )
			return true;
		const int nw = std::max( needW, vw ), nh = std::max( needH, vh );
		releaseViewTargets();
		viewFront = makeTexture( GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, nw, nh, GL_LINEAR );
		viewBack  = makeTexture( GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, nw, nh, GL_LINEAR );
		viewSil   = makeTexture( GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, nw, nh, GL_LINEAR );
		glGenFramebuffers( 1, &viewFbo );
		glBindFramebuffer( GL_FRAMEBUFFER, viewFbo );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, viewFront, 0 );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, viewBack, 0 );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, viewSil, 0 );
		const GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
		glDrawBuffers( 3, bufs );
		const bool ok = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
		vw = nw;
		vh = nh;
		if( !ok )
			FFGLLog::LogToHost( "sdfkit: 3D framebuffer incomplete" );
		return ok;
	}
};

namespace
{
ffglkit::PerContext< Renderer::SharedPrograms > g_programs;
}//namespace

// ---------------------------------------------------------------------------
float Style::outerEdge() const
{
	return ( outline1Position == OutlinePosition::Outside ? outline1Width : outline1Position == OutlinePosition::Center ? outline1Width * 0.5f : 0.0f ) + outline2Width;
}

static float innerEdgeOf( const Style& s )
{
	if( s.outline1Width <= 0 )
		return 0.0f;
	return s.outline1Position == OutlinePosition::Inside ? -s.outline1Width : s.outline1Position == OutlinePosition::Center ? -s.outline1Width * 0.5f : 0.0f;
}

float Style::reach() const
{
	const float outer = outerEdge();
	float r           = outer;
	if( glow.a > 0 && glowStrength > 0 )
		r = std::max( r, outer + glowRadius );
	if( shadow.a > 0 )
		r = std::max( r, outer + shadowSpread + shadowSoftness * 0.5f );
	if( outline1Position != OutlinePosition::Outside )
		r = std::max( r, outline1Position == OutlinePosition::Inside ? outline1Width : outline1Width * 0.5f );// inner band
	// Inner effects need the distance INSIDE the glyph
	const float inner = -innerEdgeOf( *this );
	if( innerGlowOn() )
		r = std::max( r, inner + innerGlowRadius );
	if( innerShadowOn() )
		r = std::max( r, inner + innerShadowSoftness * 0.5f );
	return std::max( r, outer + extraReach ) + edgeSoftness + 1.0f;
}

float Style::plateReach() const
{
	float r = 0;
	if( shadow.a > 0 )
		r = shadowSpread + shadowSoftness * 0.5f;
	return r + edgeSoftness + 1.0f;
}

void Style::clampTo( float maxReachPx )
{
	const float avail = std::max( 0.0f, maxReachPx - edgeSoftness - 1.0f );
	float outer       = outerEdge();
	if( outer > avail && outer > 0 )
	{
		const float k = avail / outer;
		outline1Width *= k;
		outline2Width *= k;
		outer = outerEdge();
	}
	if( outline1Position == OutlinePosition::Inside )
		outline1Width = std::min( outline1Width, avail );
	const float rest = std::max( 0.0f, avail - outer );
	glowRadius       = std::min( glowRadius, rest );
	shadowSpread     = std::min( shadowSpread, rest );
	shadowSoftness   = std::min( shadowSoftness, 2.0f * std::max( 0.0f, rest - shadowSpread ) );
	extraReach       = std::min( extraReach, rest );
	const float in   = std::max( 0.0f, avail + innerEdgeOf( *this ) );
	innerGlowRadius  = std::min( innerGlowRadius, in );
	innerShadowSoftness = std::min( innerShadowSoftness, 2.0f * in );
}

Style Style::scaled( float k ) const
{
	Style s = *this;
	s.outline1Width *= k;
	s.outline2Width *= k;
	s.glowRadius *= k;
	s.innerGlowRadius *= k;
	s.innerShadowDx *= k;
	s.innerShadowDy *= k;
	s.innerShadowSoftness *= k;
	s.plateBorderWidth *= k;
	s.plateMerge *= k;
	s.shadowDx *= k;
	s.shadowDy *= k;
	s.shadowSoftness *= k;
	s.shadowSpread *= k;
	s.extraReach *= k;
	return s;
}

// ---------------------------------------------------------------------------
bool Renderer::init()
{
	release();
	programs = g_programs.acquire( []() -> SharedPrograms* {
		auto* sp      = new SharedPrograms();
		sp->field     = ffglkit::linkProgram( kShapeVS, kFieldFS, "sdfkit field" );
		sp->composite = ffglkit::linkProgram( kCompositeVS, kCompositeFS, "sdfkit composite" );
		sp->view      = ffglkit::linkProgram( kCompositeVS, kViewFS, "sdfkit view" );
		if( !sp->field || !sp->composite || !sp->view )
		{
			for( GLuint p : { sp->field, sp->composite, sp->view } )
				if( p )
					glDeleteProgram( p );
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
	for( int i = 0; i < kInstanceVec4s; ++i )
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
	for( int i = 0; i < kInstanceVec4s; ++i )
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

// What one composite pass needs, in the coordinates of its target.
struct Renderer::FrameSetup
{
	const Style* style = nullptr;
	int layers = 0, plateLayers = 0;
	int targetW = 0, targetH = 0;  // field size (px)
	float vpX = 0, vpY = 0;        // viewport origin of the target
	float originX = 0, originY = 0;// block anchor in the target, px, y down
	float rotation = 0;            // block rotation in the target
	float blockScale = 1;
	float sizePx = 100;
	float boxLeft = 0, boxTop = 0, boxRight = 0, boxBottom = 0;
	bool plateMerge = false;
	bool split = false;
	float silClamp = 1e4f;
	int shadowDx = 0, shadowDy = 0, innerDx = 0, innerDy = 0;
	bool innerShadow = false;
	const VideoInput* video = nullptr;
	bool videoMapped = false;      // composite runs offscreen in block space
	float screenOriginX = 0, screenOriginY = 0, screenRotation = 0, screenW = 1, screenH = 1;
};

void Renderer::setCompositeUniforms( const FrameSetup& f )
{
	const Style& style = *f.style;
	const GLuint prog  = programs->composite;
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
	const int videoUnit = 2 * kLayers + 2 * kPlateLayers;
	const bool videoOn  = f.video && f.video->on();
	glActiveTexture( GL_TEXTURE0 + videoUnit );
	glBindTexture( GL_TEXTURE_2D, videoOn ? f.video->texture : 0 );
	glUniform1i( glGetUniformLocation( prog, "uVideo" ), videoUnit );
	int videoMode = videoOn ? (int)f.video->mode : 0;
	if( f.split && videoMode != (int)VideoInput::Fill )
		videoMode = 0;// over / knockout happen in the view pass
	glUniform1i( glGetUniformLocation( prog, "uVideoMode" ), videoMode );
	glUniform2f( glGetUniformLocation( prog, "uVideoMax" ), videoOn ? f.video->maxU : 1.0f, videoOn ? f.video->maxV : 1.0f );
	glUniform1i( glGetUniformLocation( prog, "uVideoMap" ), f.videoMapped ? 1 : 0 );
	glUniform2f( glGetUniformLocation( prog, "uScreenOrigin" ), f.screenOriginX, f.screenOriginY );
	glUniform2f( glGetUniformLocation( prog, "uScreenRot" ), std::cos( -f.screenRotation ), std::sin( -f.screenRotation ) );
	glUniform2f( glGetUniformLocation( prog, "uScreenSize" ), f.screenW, f.screenH );

	glUniform1i( glGetUniformLocation( prog, "uLayers" ), f.layers );
	glUniform1i( glGetUniformLocation( prog, "uPlateLayers" ), f.plateLayers );
	glUniform2f( glGetUniformLocation( prog, "uFieldSize" ), (float)f.targetW, (float)f.targetH );
	glUniform2f( glGetUniformLocation( prog, "uViewportOrigin" ), f.vpX, f.vpY );
	glUniform1f( glGetUniformLocation( prog, "uBlockScale" ), f.blockScale );
	glUniform1i( glGetUniformLocation( prog, "uSplit" ), f.split ? 1 : 0 );
	glUniform1f( glGetUniformLocation( prog, "uSilClamp" ), f.silClamp );
	// Transition width between neighbouring glyphs' colours: grows with the outline
	// so merged outlines change colour smoothly from one glyph to the next.
	glUniform1f( glGetUniformLocation( prog, "uAttrSoftness" ), std::max( 1.0f, 0.3f * style.outerEdge() + 0.01f * f.sizePx * f.blockScale ) );

	float w1 = style.outline1Width, outerEdge = 0, innerEdge = 0;
	switch( style.outline1Position )
	{
	case OutlinePosition::Outside:
		outerEdge = w1;
		break;
	case OutlinePosition::Center:
		outerEdge = w1 * 0.5f;
		innerEdge = -w1 * 0.5f;
		break;
	case OutlinePosition::Inside:
		innerEdge = -w1;
		break;
	}
	if( w1 <= 0 )
		outerEdge = innerEdge = 0;

	int fillMode = 0;
	switch( style.fillMode )
	{
	case FillMode::GradientBox: fillMode = 1; break;
	case FillMode::Stripes: fillMode = 2; break;
	case FillMode::Halftone: fillMode = 3; break;
	case FillMode::Checker: fillMode = 4; break;
	case FillMode::Noise: fillMode = 5; break;
	case FillMode::Led: fillMode = 6; break;
	default: fillMode = 0; break;
	}
	setColor( prog, "uFill", style.fill );
	setColor( prog, "uFill2", style.fill2 );
	glUniform1i( glGetUniformLocation( prog, "uFillMode" ), fillMode );
	glUniform2f( glGetUniformLocation( prog, "uBlockOrigin" ), f.originX, f.originY );
	glUniform2f( glGetUniformLocation( prog, "uBlockRotInv" ), std::cos( f.rotation ), std::sin( f.rotation ) );
	glUniform4f( glGetUniformLocation( prog, "uBox" ), f.boxLeft, f.boxTop, f.boxRight, f.boxBottom );
	glUniform2f( glGetUniformLocation( prog, "uGradDir" ), std::cos( style.gradientAngle ), -std::sin( style.gradientAngle ) );
	glUniform2f( glGetUniformLocation( prog, "uPatDir" ), std::cos( style.patternAngle ), -std::sin( style.patternAngle ) );
	glUniform1f( glGetUniformLocation( prog, "uPatScale" ), style.patternScale );
	glUniform1f( glGetUniformLocation( prog, "uPatOffset" ), style.patternOffset - std::floor( style.patternOffset ) );
	setColor( prog, "uOutline1", style.outline1 );
	setColor( prog, "uOutline2", style.outline2 );
	glUniform3f( glGetUniformLocation( prog, "uOutline1Edges" ), w1, outerEdge, innerEdge );
	glUniform1f( glGetUniformLocation( prog, "uOutline2Width" ), style.outline2Width );
	setColor( prog, "uShadow", style.shadow );
	glUniform2i( glGetUniformLocation( prog, "uShadowOffset" ), f.shadowDx, -f.shadowDy );
	glUniform1f( glGetUniformLocation( prog, "uShadowSoftness" ), style.shadowSoftness );
	glUniform1f( glGetUniformLocation( prog, "uShadowSpread" ), style.shadowSpread );
	setColor( prog, "uGlow", style.glow );
	glUniform1f( glGetUniformLocation( prog, "uGlowRadius" ), style.glowRadius );
	glUniform1f( glGetUniformLocation( prog, "uGlowStrength" ), style.glowStrength );
	setColor( prog, "uInnerGlow", style.innerGlow );
	glUniform1f( glGetUniformLocation( prog, "uInnerGlowRadius" ), style.innerGlowOn() ? style.innerGlowRadius : 0.0f );
	setColor( prog, "uInnerShadow", style.innerShadow );
	glUniform2i( glGetUniformLocation( prog, "uInnerShadowOffset" ), f.innerDx, -f.innerDy );
	glUniform1f( glGetUniformLocation( prog, "uInnerShadowSoftness" ), style.innerShadowSoftness );
	glUniform1i( glGetUniformLocation( prog, "uInnerShadowOn" ), f.innerShadow ? 1 : 0 );
	setColor( prog, "uPlate", style.plate );
	setColor( prog, "uPlateBorder", style.plateBorderColor );
	glUniform1f( glGetUniformLocation( prog, "uPlateBorderWidth" ), style.plateBorderWidth );
	glUniform1f( glGetUniformLocation( prog, "uPlateMerge" ), f.plateMerge ? style.plateMerge : 0.0f );
	glUniform1f( glGetUniformLocation( prog, "uEdge" ), std::max( 0.25f, style.edgeSoftness ) );
}

void Renderer::render( const FFGLViewportStruct& vp, GLuint hostFbo, const std::vector< ShapeInstance >& glyphs, const std::vector< ShapeInstance >& boxes,
					   const BlockTransform& t, const Style& style, const Window& window, GLuint atlasTexture, int atlasSize, const View3D& view,
					   const VideoInput& video )
{
	if( !isReady() || vp.width == 0 || vp.height == 0 )
		return;
	ffglkit::StateGuard guard;
	const int w = (int)vp.width, h = (int)vp.height;
	const bool videoFull = video.on() && ( video.mode == VideoInput::Over || video.mode == VideoInput::Knockout );

	glDisable( GL_BLEND );
	glDisable( GL_CULL_FACE );
	glEnable( GL_SCISSOR_TEST );

	// Clear the whole output; only the shapes' bounding box gets shaded afterwards.
	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glViewport( (GLint)vp.x, (GLint)vp.y, w, h );
	glScissor( (GLint)vp.x, (GLint)vp.y, w, h );
	glClearColor( 0, 0, 0, 0 );
	glClear( GL_COLOR_BUFFER_BIT );
	const bool haveText   = !glyphs.empty() && atlasTexture != 0;
	const bool havePlates = !boxes.empty();
	if( !haveText && !havePlates && !videoFull )
		return;
	if( !programs->ensureTargets( w, h ) )
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

	// ---- Where the passes render: the screen (2D) or a block-space target (3D) ----
	const bool threeD = view.on() && ( haveText || havePlates );
	float k = 1.0f;// target px per block px
	float originX = t.originX, originY = t.originY, rotation = t.rotation;
	int targetW = w, targetH = h;
	Rect blockRect;// 3D: block px covered by the target
	Style scaledStyle;
	const Style* st = &style;
	if( threeD )
	{
		if( haveText )
			addQuads( blockRect, glyphs, t.sizePx, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f );
		if( havePlates )
			addQuads( blockRect, boxes, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f );
		if( blockRect.empty() )
			return;
		if( style.shadow.a > 0 )
			blockRect.add( blockRect.shifted( style.shadowDx, style.shadowDy ) );
		blockRect = Rect{ blockRect.x0 - 4, blockRect.y0 - 4, blockRect.x1 + 4, blockRect.y1 + 4 };
		GLint maxTex = 4096;
		glGetIntegerv( GL_MAX_TEXTURE_SIZE, &maxTex );
		const float maxDim = (float)std::min( 4096, (int)maxTex ) - 4.0f;
		// At most 4096 px a side and 1.5 x the output's area: the shared targets grow
		// to the largest size seen, so a huge block must not claim gigabytes.
		const float rw = blockRect.x1 - blockRect.x0, rh = blockRect.y1 - blockRect.y0;
		k = std::min( { 1.0f, maxDim / rw, maxDim / rh, std::sqrt( 1.5f * (float)w * (float)h / std::max( 1.0f, rw * rh ) ) } );
		targetW = (int)std::ceil( ( blockRect.x1 - blockRect.x0 ) * k ) + 1;
		targetH = (int)std::ceil( ( blockRect.y1 - blockRect.y0 ) * k ) + 1;
		originX  = -blockRect.x0 * k;
		originY  = -blockRect.y0 * k;
		rotation = 0.0f;
		scaledStyle = style.scaled( k );
		st          = &scaledStyle;
		if( !programs->ensureTargets( targetW, targetH ) || !programs->ensureViewTargets( targetW, targetH ) )
			return;
	}
	const Style& s = *st;
	const float blockCos = std::cos( -rotation ), blockSin = std::sin( -rotation );

	Rect bbox;
	if( haveText )
		addQuads( bbox, glyphs, t.sizePx, k, blockCos, blockSin, originX, originY );
	if( havePlates )
		addQuads( bbox, boxes, 1.0f, k, blockCos, blockSin, originX, originY );
	bbox = bbox.empty() ? Rect{ 0, 0, 0, 0 } : Rect{ bbox.x0 - 2, bbox.y0 - 2, bbox.x1 + 2, bbox.y1 + 2 };// no shapes: only the video
	const bool shadow = s.shadow.a > 0;
	// Whole pixels, so the shadows read the layers texel-exact.
	const int shadowDx = (int)std::lround( s.shadowDx ), shadowDy = (int)std::lround( s.shadowDy );
	const bool innerShadow = haveText && s.innerShadowOn();
	const int innerDx = (int)std::lround( s.innerShadowDx ), innerDy = (int)std::lround( s.innerShadowDy );
	Rect shadedArea = bbox;// what the composite writes
	if( shadow )
		shadedArea.add( bbox.shifted( (float)shadowDx, (float)shadowDy ) );
	Rect fieldArea = shadedArea;// what the composite reads
	if( shadow )
		fieldArea.add( shadedArea.shifted( (float)-shadowDx, (float)-shadowDy ) );
	if( innerShadow )
		fieldArea.add( shadedArea.shifted( (float)-innerDx, (float)-innerDy ) );
	if( threeD )
		shadedArea = fieldArea = Rect{ 0, 0, (float)targetW, (float)targetH };
	else if( videoFull )
		shadedArea = Rect{ 0, 0, (float)w, (float)h };

	// ---- Field passes ----
	glBindVertexArray( shapeVao );
	uploadInstances( glyphVbo, glyphCapacity, glyphs );
	uploadInstances( boxVbo, boxCapacity, boxes );

	const GLuint fp = programs->field;
	glUseProgram( fp );
	glUniform2f( glGetUniformLocation( fp, "uViewport" ), (float)targetW, (float)targetH );
	glUniform2f( glGetUniformLocation( fp, "uOrigin" ), originX, originY );
	glUniform2f( glGetUniformLocation( fp, "uBlockRot" ), blockCos, blockSin );
	glUniform1f( glGetUniformLocation( fp, "uBlockScale" ), k );
	glUniform2f( glGetUniformLocation( fp, "uAtlasSize" ), (float)std::max( 1, atlasSize ), (float)std::max( 1, atlasSize ) );
	glUniform1f( glGetUniformLocation( fp, "uSharpCorners" ), s.sharpCorners ? 1.0f : 0.0f );
	glUniform1f( glGetUniformLocation( fp, "uDepthRange" ), 4096.0f );
	glUniform1i( glGetUniformLocation( fp, "uAtlas" ), 0 );
	glUniform1i( glGetUniformLocation( fp, "uPrev0" ), 1 );
	glUniform1i( glGetUniformLocation( fp, "uPrev1" ), 2 );
	glUniform1i( glGetUniformLocation( fp, "uPrev2" ), 3 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, atlasTexture );

	glViewport( 0, 0, targetW, targetH );
	setScissor( fieldArea, 0, 0, targetW, targetH );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glDepthMask( GL_TRUE );
	glClearDepth( 1.0 );
	if( haveText )
	{
		glUniform1f( glGetUniformLocation( fp, "uSize" ), t.sizePx );
		glUniform4f( glGetUniformLocation( fp, "uWindow" ), window.x0, window.y0, window.x1, window.y1 );
		glUniform1f( glGetUniformLocation( fp, "uWindowFade" ), windowFade );
		drawField( glyphVbo, glyphs.size(), 0, layers, alphaVaries, s.reach() + 2.0f, programs->layerFbo, programs->layerA );
	}
	if( havePlates )
	{
		glUniform1f( glGetUniformLocation( fp, "uSize" ), 1.0f );
		glUniform4f( glGetUniformLocation( fp, "uWindow" ), window.x0, window.y0, window.x1, window.y1 );
		glUniform1f( glGetUniformLocation( fp, "uWindowFade" ), windowFade );
		drawField( boxVbo, boxes.size(), 1, plateLayers, plateAlphaVaries, s.plateReach() + 2.0f, programs->plateFbo, programs->plateA );
	}
	glDisable( GL_DEPTH_TEST );

	// ---- Composite ----
	FrameSetup f;
	f.style       = &s;
	f.layers      = layers;
	f.plateLayers = plateLayers;
	f.targetW     = targetW;
	f.targetH     = targetH;
	f.originX     = originX;
	f.originY     = originY;
	f.rotation    = rotation;
	f.blockScale  = k;
	f.sizePx      = t.sizePx;
	f.boxLeft     = t.boxLeft;
	f.boxTop      = t.boxTop;
	f.boxRight    = t.boxRight;
	f.boxBottom   = t.boxBottom;
	f.plateMerge  = plateMerge;
	f.shadowDx    = shadowDx;
	f.shadowDy    = shadowDy;
	f.innerDx     = innerDx;
	f.innerDy     = innerDy;
	f.innerShadow = innerShadow;
	f.video       = &video;
	if( !threeD )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
		glViewport( (GLint)vp.x, (GLint)vp.y, w, h );
		setScissor( shadedArea, (int)vp.x, (int)vp.y, w, h );
		f.vpX = (float)vp.x;
		f.vpY = (float)vp.y;
		setCompositeUniforms( f );
		glBindVertexArray( emptyVao );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
		return;
	}

	// 3D: composite into the block-space targets, then look at them
	const float silClamp = std::max( 1.0f, s.reach() );
	glBindFramebuffer( GL_FRAMEBUFFER, programs->viewFbo );
	glViewport( 0, 0, targetW, targetH );
	glScissor( 0, 0, targetW, targetH );
	const GLfloat zero[] = { 0, 0, 0, 0 }, farSil[] = { silClamp, 0, 0, 1 };
	glClearBufferfv( GL_COLOR, 0, zero );
	glClearBufferfv( GL_COLOR, 1, zero );
	glClearBufferfv( GL_COLOR, 2, farSil );
	f.split         = true;
	f.silClamp      = silClamp;
	f.videoMapped   = true;
	f.screenOriginX = t.originX;
	f.screenOriginY = t.originY;
	f.screenRotation = t.rotation;
	f.screenW       = (float)w;
	f.screenH       = (float)h;
	setCompositeUniforms( f );
	glBindVertexArray( emptyVao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );

	// ---- View pass into the host framebuffer ----
	const Mat3 R = mul( rotZ( -t.rotation ), mul( rotY( view.tiltY ), rotX( view.tiltX ) ) );
	const float depth = view.mode == View3D::Extrude ? std::max( 0.0f, view.depth ) : 0.0f;
	// Screen bounding box of the box around the solid (full screen if a corner is behind the camera)
	Rect screen;
	bool behind = false;
	for( int c = 0; c < 8; ++c )
	{
		const float ox = ( c & 1 ) ? blockRect.x1 : blockRect.x0, oy = ( c & 2 ) ? blockRect.y1 : blockRect.y0, oz = ( c & 4 ) ? depth : 0.0f;
		const float px = ox, py = oy, pz = oz - depth;
		const float wx = R.at( 0, 0 ) * px + R.at( 0, 1 ) * py + R.at( 0, 2 ) * pz;
		const float wy = R.at( 1, 0 ) * px + R.at( 1, 1 ) * py + R.at( 1, 2 ) * pz;
		const float wz = R.at( 2, 0 ) * px + R.at( 2, 1 ) * py + R.at( 2, 2 ) * pz;
		float sx = wx, sy = wy;
		if( view.focal > 0.0f )
		{
			const float den = view.focal - wz;
			if( den < 1.0f )
			{
				behind = true;
				break;
			}
			sx = wx * view.focal / den;
			sy = wy * view.focal / den;
		}
		screen.add( sx + t.originX, sy + t.originY );
	}
	if( behind || videoFull )
		screen = Rect{ 0, 0, (float)w, (float)h };
	else
		screen = Rect{ screen.x0 - 2, screen.y0 - 2, screen.x1 + 2, screen.y1 + 2 };

	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glViewport( (GLint)vp.x, (GLint)vp.y, w, h );
	setScissor( screen, (int)vp.x, (int)vp.y, w, h );
	const GLuint vprog = programs->view;
	glUseProgram( vprog );
	const GLuint tex[ 3 ] = { programs->viewFront, programs->viewBack, programs->viewSil };
	const char* names[ 3 ] = { "uFront", "uBack", "uSil" };
	for( int i = 0; i < 3; ++i )
	{
		glActiveTexture( GL_TEXTURE0 + i );
		glBindTexture( GL_TEXTURE_2D, tex[ i ] );
		glUniform1i( glGetUniformLocation( vprog, names[ i ] ), i );
	}
	glActiveTexture( GL_TEXTURE3 );
	glBindTexture( GL_TEXTURE_2D, video.on() ? video.texture : 0 );
	glUniform1i( glGetUniformLocation( vprog, "uVideo" ), 3 );
	glUniform1i( glGetUniformLocation( vprog, "uVideoMode" ), videoFull ? (int)video.mode : 0 );
	glUniform2f( glGetUniformLocation( vprog, "uVideoMax" ), video.maxU, video.maxV );
	glUniform2f( glGetUniformLocation( vprog, "uTexSize" ), (float)programs->vw, (float)programs->vh );
	glUniform2f( glGetUniformLocation( vprog, "uUsed" ), (float)targetW, (float)targetH );
	glUniform4f( glGetUniformLocation( vprog, "uRect" ), blockRect.x0, blockRect.y0, blockRect.x1, blockRect.y1 );
	glUniform1f( glGetUniformLocation( vprog, "uScale" ), k );
	glUniform2f( glGetUniformLocation( vprog, "uViewportOrigin" ), (float)vp.x, (float)vp.y );
	glUniform2f( glGetUniformLocation( vprog, "uViewportSize" ), (float)w, (float)h );
	glUniform2f( glGetUniformLocation( vprog, "uAnchor" ), t.originX, t.originY );
	glUniformMatrix3fv( glGetUniformLocation( vprog, "uRot" ), 1, GL_FALSE, R.m );
	glUniform1f( glGetUniformLocation( vprog, "uFocal" ), view.focal );
	glUniform1i( glGetUniformLocation( vprog, "uMode" ), view.mode == View3D::Extrude ? 2 : 1 );
	glUniform1f( glGetUniformLocation( vprog, "uDepth" ), depth );
	glUniform1f( glGetUniformLocation( vprog, "uBevel" ), std::max( 0.0f, view.bevel ) );
	glUniform1f( glGetUniformLocation( vprog, "uSilClamp" ), silClamp / k );
	setColor( vprog, "uSide", view.side );
	{
		float lx = view.lightX, ly = view.lightY, lz = view.lightZ;
		const float n = std::sqrt( lx * lx + ly * ly + lz * lz );
		if( n > 1e-6f )
		{
			lx /= n;
			ly /= n;
			lz /= n;
		}
		glUniform3f( glGetUniformLocation( vprog, "uLight" ), lx, ly, lz );
	}
	glUniform1f( glGetUniformLocation( vprog, "uShading" ), std::clamp( view.shading, 0.0f, 1.0f ) );
	glUniform1f( glGetUniformLocation( vprog, "uGloss" ), std::max( 0.0f, view.gloss ) );
	glUniform1f( glGetUniformLocation( vprog, "uEdge" ), std::max( 0.25f, style.edgeSoftness ) );
	glBindVertexArray( emptyVao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
}

}//namespace sdfkit
