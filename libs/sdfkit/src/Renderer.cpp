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

uniform vec2 uViewport;    // px
uniform vec2 uOrigin;      // anchor position, px, y down
uniform vec2 uBlockRot;    // cos, sin of the block rotation (screen frame)
uniform float uSize;       // px per shape unit (font size for glyphs, 1 for boxes)

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

vec2 rotate( vec2 p, vec2 cs ) { return vec2( cs.x * p.x - cs.y * p.y, cs.y * p.x + cs.x * p.y ); }

void main()
{
	vec2 q        = mix( iQuad.xy, iQuad.zw, aCorner );
	vec2 unscaled = vec2( q.x, -q.y ) * uSize;
	vec2 local    = unscaled * iPosScaleRot.z;
	local         = rotate( local, vec2( cos( iPosScaleRot.w ), sin( iPosScaleRot.w ) ) );
	vec2 pb       = iPosScaleRot.xy + local;
	vec2 p        = rotate( pb, uBlockRot ) + uOrigin;
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
flat in float vId;
flat in float vScale;

layout( location = 0 ) out vec4 oA;// distance, shape id, opacity, tint amount
layout( location = 1 ) out vec4 oB;// tint colour

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

float rectDistance( vec2 p, vec4 rc )
{
	vec2 c = 0.5 * ( rc.xy + rc.zw ), h = 0.5 * ( rc.zw - rc.xy );
	vec2 d = abs( p - c ) - h;
	return length( max( d, 0.0 ) ) + min( max( d.x, d.y ), 0.0 );
}

void main()
{
	float dist  = uKind == 0 ? glyphDistancePx() : boxDistancePx();
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
	oB = vec4( vTint.rgb, 1.0 );
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
uniform int uLayers;         // text layers in use (0 = no text)
uniform int uPlateLayers;    // plate layers in use (0 = no plates)
uniform vec2 uFieldSize;
uniform vec2 uViewportOrigin;
uniform float uAttrSoftness; // px: colour transition width where glyphs merge

uniform vec4 uFill;          // straight colours
uniform vec4 uFill2;
uniform int uFillMode;       // 0 solid, 1 box gradient
uniform vec2 uBlockOrigin;   // anchor, px, y down
uniform vec2 uBlockRotInv;   // cos, sin to go from screen back into block space
uniform vec4 uBox;           // block metric box l, t, r, b (px, y down, block space)
uniform vec2 uGradDir;       // gradient direction in block space

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
uniform float uEdge;         // anti-aliasing width in px

out vec4 oColor;

const float kFar = 1.0e4;

vec4 premul( vec4 c ) { return vec4( c.rgb * c.a, c.a ); }
float inside( float d, float edge ) { return clamp( 0.5 - ( d - edge ) / uEdge, 0.0, 1.0 ); }// coverage of "d < edge"

// Nearest shapes at one pixel: distance, opacity, tint amount, tint colour
struct Entries
{
	int n;
	vec4 a[ 4 ];// distance, id, opacity, tint amount
	vec3 tint[ 4 ];
};

void fetchLayer( int k, ivec2 ip, out vec4 a, out vec3 b )
{
	if( k == 0 ) { a = texelFetch( uA0, ip, 0 ); b = texelFetch( uB0, ip, 0 ).rgb; }
	else if( k == 1 ) { a = texelFetch( uA1, ip, 0 ); b = texelFetch( uB1, ip, 0 ).rgb; }
	else if( k == 2 ) { a = texelFetch( uA2, ip, 0 ); b = texelFetch( uB2, ip, 0 ).rgb; }
	else { a = texelFetch( uA3, ip, 0 ); b = texelFetch( uB3, ip, 0 ).rgb; }
}

void fetchPlate( int k, ivec2 ip, out vec4 a, out vec3 b )
{
	if( k == 0 ) { a = texelFetch( uPA0, ip, 0 ); b = texelFetch( uPB0, ip, 0 ).rgb; }
	else { a = texelFetch( uPA1, ip, 0 ); b = texelFetch( uPB1, ip, 0 ).rgb; }
}

Entries none()
{
	Entries e;
	e.n = 0;
	return e;
}

bool outsideField( ivec2 ip ) { return any( lessThan( ip, ivec2( 0 ) ) ) || any( greaterThanEqual( ip, ivec2( uFieldSize ) ) ); }

Entries loadText( ivec2 ip )
{
	Entries e = none();
	if( outsideField( ip ) )
		return e;
	for( int k = 0; k < 4; ++k )
	{
		if( k >= uLayers )
			break;
		vec4 a;
		vec3 b;
		fetchLayer( k, ip, a, b );
		if( a.r >= 0.5 * kFar )
			break;// no further shape near this pixel
		e.a[ e.n ]    = a;
		e.tint[ e.n ] = b;
		e.n++;
	}
	return e;
}

Entries loadPlates( ivec2 ip )
{
	Entries e = none();
	if( outsideField( ip ) )
		return e;
	for( int k = 0; k < 2; ++k )
	{
		if( k >= uPlateLayers )
			break;
		vec4 a;
		vec3 b;
		fetchPlate( k, ip, a, b );
		if( a.r >= 0.5 * kFar )
			break;
		e.a[ e.n ]    = a;
		e.tint[ e.n ] = b;
		e.n++;
	}
	return e;
}

// Union distance of the shapes with opacity >= t
float unionDistance( Entries e, float t )
{
	float d = kFar;
	for( int i = 0; i < 4; ++i )
		if( i < e.n && e.a[ i ].b >= t )
			d = min( d, e.a[ i ].r );
	return d;
}

// Tint of the shapes with opacity >= t, blended smoothly where they meet
vec4 unionTint( Entries e, float t )
{
	float wSum = 0.0, wtSum = 0.0;
	vec3 c     = vec3( 0.0 );
	for( int i = 0; i < 4; ++i )
		if( i < e.n && e.a[ i ].b >= t )
		{
			float w  = exp( clamp( -e.a[ i ].r / uAttrSoftness, -40.0, 40.0 ) );
			float wt = w * e.a[ i ].a;
			wSum += w;
			wtSum += wt;
			c += wt * e.tint[ i ];
		}
	if( wtSum <= 0.0 || wSum <= 0.0 )
		return vec4( 0.0 );
	return vec4( c / wtSum, clamp( wtSum / wSum, 0.0, 1.0 ) );
}

float th[ 16 ];
int nt = 0;
void addThresholds( Entries e )
{
	for( int i = 0; i < 4; ++i )
		if( i < e.n && nt < 16 )
			th[ nt++ ] = e.a[ i ].b;
}

void main()
{
	vec2 frag = gl_FragCoord.xy - uViewportOrigin;
	ivec2 ip  = ivec2( frag );
	Entries ep = loadText( ip );
	bool shadowOn = uShadow.a > 0.0;
	bool platesOn = uPlateLayers > 0;
	Entries eq = none();
	if( shadowOn )
		eq = loadText( ip - uShadowOffset );
	Entries er = none();
	if( uInnerShadowOn == 1 )
		er = loadText( ip - uInnerShadowOffset );
	Entries pp = none(), pq = none();
	if( platesOn )
	{
		pp = loadPlates( ip );
		if( shadowOn )
			pq = loadPlates( ip - uShadowOffset );
	}
	if( ep.n == 0 && eq.n == 0 && pp.n == 0 && pq.n == 0 )
	{
		oColor = vec4( 0.0 );
		return;
	}

	// Opacity thresholds of all shapes involved, highest first
	addThresholds( ep );
	addThresholds( eq );
	addThresholds( er );
	addThresholds( pp );
	addThresholds( pq );
	for( int i = 1; i < 16; ++i )
	{
		if( i >= nt )
			break;
		float v = th[ i ];
		int j   = i - 1;
		while( j >= 0 && th[ j ] < v )
		{
			th[ j + 1 ] = th[ j ];
			--j;
		}
		th[ j + 1 ] = v;
	}

	// Fill colour: solid or box gradient (position based, the same for every threshold)
	vec4 baseFill = uFill;
	if( uFillMode == 1 )
	{
		vec2 s     = vec2( frag.x, uFieldSize.y - frag.y ) - uBlockOrigin;// screen px, y down, relative to anchor
		vec2 local = vec2( uBlockRotInv.x * s.x - uBlockRotInv.y * s.y, uBlockRotInv.y * s.x + uBlockRotInv.x * s.y );
		vec2 c     = 0.5 * ( uBox.xy + uBox.zw );
		vec2 halfs = 0.5 * ( uBox.zw - uBox.xy );
		float ext  = max( abs( halfs.x * uGradDir.x ) + abs( halfs.y * uGradDir.y ), 1e-3 );
		float t    = clamp( dot( local - c, uGradDir ) / ext * 0.5 + 0.5, 0.0, 1.0 );
		baseFill   = mix( uFill, uFill2, t );
	}

	float outer = uOutline1Edges.y + ( uOutline2Width > 0.0 ? uOutline2Width : 0.0 );
	vec4 result = vec4( 0.0 );
	for( int k = 0; k < 16; ++k )
	{
		if( k >= nt )
			break;
		float t      = th[ k ];
		float weight = t - ( k + 1 < nt ? th[ k + 1 ] : 0.0 );
		if( weight <= 0.0 )
			continue;
		float d = unionDistance( ep, t );

		vec4 fill = baseFill;
		vec4 tint = unionTint( ep, t );
		fill.rgb  = mix( fill.rgb, tint.rgb, tint.a );
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
			float dr = unionDistance( er, t ) - uOutline1Edges.z;// the shape moved by the offset
			vec4 is  = premul( uInnerShadow ) * smoothstep( -0.5 * w, 0.5 * w, dr );
			fillP    = is + fillP * ( 1.0 - is.a );
		}

		// Text stack: inner layers REPLACE outer ones (mix by coverage), so a
		// transparent fill shows through (hollow text) and bands have no seams.
		vec4 text = vec4( 0.0 );
		if( uOutline2Width > 0.0 )
			text = mix( text, premul( uOutline2 ), inside( d, outer ) );
		if( uOutline1Edges.x > 0.0 )
			text = mix( text, premul( uOutline1 ), inside( d, uOutline1Edges.y ) );
		text = mix( text, fillP, inside( d, uOutline1Edges.z ) );

		float textShape = inside( d, outer );
		vec4 glowC      = vec4( 0.0 );
		if( uGlowRadius > 0.0 && uGlowStrength > 0.0 && uGlow.a > 0.0 )
		{
			float g  = max( d - outer, 0.0 );
			float kk = clamp( 1.0 - g / uGlowRadius, 0.0, 1.0 );
			glowC    = premul( uGlow ) * clamp( uGlowStrength * kk * kk, 0.0, 1.0 );
		}

		if( !platesOn )
		{
			// Effects behind the text
			vec4 back = vec4( 0.0 );
			if( shadowOn )
			{
				float w  = max( uShadowSoftness, uEdge );
				float ds = unionDistance( eq, t ) - outer - uShadowSpread;
				back     = premul( uShadow ) * ( 1.0 - smoothstep( -0.5 * w, 0.5 * w, ds ) );
			}
			back = glowC + back * ( 1.0 - glowC.a );
			// Effects are knocked out under the text silhouette (Photoshop's default).
			back *= 1.0 - textShape;
			result += weight * ( text + back * ( 1.0 - text.a ) );
			continue;
		}

		// Plates
		float dP    = unionDistance( pp, t );
		vec4 pc     = uPlate;
		vec4 ptint  = unionTint( pp, t );
		pc.rgb      = mix( pc.rgb, ptint.rgb, ptint.a );
		float plateShape = inside( dP, 0.0 );
		vec4 plateC = vec4( 0.0 );
		if( uPlateBorderWidth > 0.0 )
			plateC = mix( plateC, premul( uPlateBorder ), plateShape );
		plateC = mix( plateC, premul( pc ), inside( dP, -uPlateBorderWidth ) );

		vec4 shadowC = vec4( 0.0 );
		if( shadowOn )
		{
			float w  = max( uShadowSoftness, uEdge );
			float ds = min( unionDistance( eq, t ) - outer, unionDistance( pq, t ) ) - uShadowSpread;
			shadowC  = premul( uShadow ) * ( 1.0 - smoothstep( -0.5 * w, 0.5 * w, ds ) ) * ( 1.0 - plateShape ) * ( 1.0 - textShape );
		}
		glowC *= 1.0 - textShape;
		vec4 c = plateC + shadowC * ( 1.0 - plateC.a );
		c      = glowC + c * ( 1.0 - glowC.a );
		result += weight * ( text + c * ( 1.0 - text.a ) );
	}
	if( result.a > 0.0 )
		result.rgb /= result.a;// Resolume composites sources as straight alpha
	oColor = result;
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

// Screen bounding box of instanced quads (they already include the effect reach).
void addQuads( Rect& bbox, const std::vector< ShapeInstance >& items, float unitPx, const BlockTransform& t, float blockCos, float blockSin )
{
	for( const ShapeInstance& g : items )
	{
		const float c = std::cos( g.rotation ), s = std::sin( g.rotation );
		const float k = unitPx * g.scale;
		const float ex[ 2 ] = { g.quadL, g.quadR }, ey[ 2 ] = { g.quadB, g.quadT };
		for( float emx : ex )
			for( float emy : ey )
			{
				float lx = emx * k, ly = -emy * k;
				float rx = c * lx - s * ly + g.x, ry = s * lx + c * ly + g.y;
				bbox.add( blockCos * rx - blockSin * ry + t.originX, blockSin * rx + blockCos * ry + t.originY );
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
	GLuint field = 0, composite = 0;

	// Layer k: A = distance, shape id, opacity, tint amount (RGBA16F); B = tint colour (RGBA8)
	GLuint layerFbo[ kLayers ]{}, layerA[ kLayers ]{}, layerB[ kLayers ]{};
	GLuint plateFbo[ kPlateLayers ]{}, plateA[ kPlateLayers ]{}, plateB[ kPlateLayers ]{};
	GLuint depth = 0;
	int w = 0, h = 0;

	void destroy()
	{
		glDeleteProgram( field );
		glDeleteProgram( composite );
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
	return r + edgeSoftness + 1.0f;
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
	const float in   = std::max( 0.0f, avail + innerEdgeOf( *this ) );
	innerGlowRadius  = std::min( innerGlowRadius, in );
	innerShadowSoftness = std::min( innerShadowSoftness, 2.0f * in );
}

// ---------------------------------------------------------------------------
bool Renderer::init()
{
	release();
	programs = g_programs.acquire( []() -> SharedPrograms* {
		auto* sp      = new SharedPrograms();
		sp->field     = ffglkit::linkProgram( kShapeVS, kFieldFS, "sdfkit field" );
		sp->composite = ffglkit::linkProgram( kCompositeVS, kCompositeFS, "sdfkit composite" );
		if( !sp->field || !sp->composite )
		{
			if( sp->field )
				glDeleteProgram( sp->field );
			if( sp->composite )
				glDeleteProgram( sp->composite );
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
	for( int i = 0; i < 8; ++i )
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
	for( int i = 0; i < 8; ++i )
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

void Renderer::render( const FFGLViewportStruct& vp, GLuint hostFbo, const std::vector< ShapeInstance >& glyphs, const std::vector< ShapeInstance >& boxes,
					   const BlockTransform& t, const Style& style, const Window& window, GLuint atlasTexture, int atlasSize )
{
	if( !isReady() || vp.width == 0 || vp.height == 0 )
		return;
	ffglkit::StateGuard guard;
	const int w = (int)vp.width, h = (int)vp.height;
	const float blockCos = std::cos( -t.rotation ), blockSin = std::sin( -t.rotation );

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
	if( ( !haveText && !havePlates ) || !programs->ensureTargets( w, h ) )
		return;

	Rect bbox;
	if( haveText )
		addQuads( bbox, glyphs, t.sizePx, t, blockCos, blockSin );
	if( havePlates )
		addQuads( bbox, boxes, 1.0f, t, blockCos, blockSin );
	if( bbox.empty() )
		return;

	// Shapes of different opacity need several shapes per pixel; different tints
	// need two for smooth colour transitions; otherwise the nearest is enough.
	bool alphaVaries = false, tintVaries = false, plateAlphaVaries = false, plateTintVaries = false;
	varies( glyphs, alphaVaries, tintVaries );
	varies( boxes, plateAlphaVaries, plateTintVaries );
	if( window.on )
		alphaVaries = true;
	const int layers      = !haveText ? 0 : alphaVaries ? kLayers : tintVaries ? 2 : 1;
	const int plateLayers = !havePlates ? 0 : plateAlphaVaries || plateTintVaries ? kPlateLayers : 1;

	bbox = Rect{ bbox.x0 - 2, bbox.y0 - 2, bbox.x1 + 2, bbox.y1 + 2 };
	const bool shadow = style.shadow.a > 0;
	// Whole pixels, so the shadows read the layers texel-exact.
	const int shadowDx = (int)std::lround( style.shadowDx ), shadowDy = (int)std::lround( style.shadowDy );
	const bool innerShadow = haveText && style.innerShadowOn();
	const int innerDx = (int)std::lround( style.innerShadowDx ), innerDy = (int)std::lround( style.innerShadowDy );
	Rect shadedArea = bbox;// what the composite writes
	if( shadow )
		shadedArea.add( bbox.shifted( (float)shadowDx, (float)shadowDy ) );
	Rect fieldArea = shadedArea;// what the composite reads
	if( shadow )
		fieldArea.add( shadedArea.shifted( (float)-shadowDx, (float)-shadowDy ) );
	if( innerShadow )
		fieldArea.add( shadedArea.shifted( (float)-innerDx, (float)-innerDy ) );

	// ---- Field passes ----
	glBindVertexArray( shapeVao );
	uploadInstances( glyphVbo, glyphCapacity, glyphs );
	uploadInstances( boxVbo, boxCapacity, boxes );

	const GLuint fp = programs->field;
	glUseProgram( fp );
	glUniform2f( glGetUniformLocation( fp, "uViewport" ), (float)w, (float)h );
	glUniform2f( glGetUniformLocation( fp, "uOrigin" ), t.originX, t.originY );
	glUniform2f( glGetUniformLocation( fp, "uBlockRot" ), blockCos, blockSin );
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
		glUniform1f( glGetUniformLocation( fp, "uWindowFade" ), window.on ? std::max( 0.0f, window.fade ) : -1.0f );
		drawField( glyphVbo, glyphs.size(), 0, layers, alphaVaries, style.reach() + 2.0f, programs->layerFbo, programs->layerA );
	}
	if( havePlates )
	{
		glUniform1f( glGetUniformLocation( fp, "uSize" ), 1.0f );
		glUniform1f( glGetUniformLocation( fp, "uWindowFade" ), -1.0f );
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
	glUniform1i( glGetUniformLocation( prog, "uLayers" ), layers );
	glUniform1i( glGetUniformLocation( prog, "uPlateLayers" ), plateLayers );
	glUniform2f( glGetUniformLocation( prog, "uFieldSize" ), (float)w, (float)h );
	glUniform2f( glGetUniformLocation( prog, "uViewportOrigin" ), (float)vp.x, (float)vp.y );
	// Transition width between neighbouring glyphs' colours: grows with the outline
	// so merged outlines change colour smoothly from one glyph to the next.
	glUniform1f( glGetUniformLocation( prog, "uAttrSoftness" ), std::max( 1.0f, 0.3f * style.outerEdge() + 0.01f * t.sizePx ) );

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

	setColor( prog, "uFill", style.fill );
	setColor( prog, "uFill2", style.fill2 );
	glUniform1i( glGetUniformLocation( prog, "uFillMode" ), style.fillMode == FillMode::GradientBox ? 1 : 0 );
	glUniform2f( glGetUniformLocation( prog, "uBlockOrigin" ), t.originX, t.originY );
	glUniform2f( glGetUniformLocation( prog, "uBlockRotInv" ), std::cos( t.rotation ), std::sin( t.rotation ) );
	glUniform4f( glGetUniformLocation( prog, "uBox" ), t.boxLeft, t.boxTop, t.boxRight, t.boxBottom );
	glUniform2f( glGetUniformLocation( prog, "uGradDir" ), std::cos( style.gradientAngle ), -std::sin( style.gradientAngle ) );
	setColor( prog, "uOutline1", style.outline1 );
	setColor( prog, "uOutline2", style.outline2 );
	glUniform3f( glGetUniformLocation( prog, "uOutline1Edges" ), w1, outerEdge, innerEdge );
	glUniform1f( glGetUniformLocation( prog, "uOutline2Width" ), style.outline2Width );
	setColor( prog, "uShadow", style.shadow );
	glUniform2i( glGetUniformLocation( prog, "uShadowOffset" ), shadowDx, -shadowDy );
	glUniform1f( glGetUniformLocation( prog, "uShadowSoftness" ), style.shadowSoftness );
	glUniform1f( glGetUniformLocation( prog, "uShadowSpread" ), style.shadowSpread );
	setColor( prog, "uGlow", style.glow );
	glUniform1f( glGetUniformLocation( prog, "uGlowRadius" ), style.glowRadius );
	glUniform1f( glGetUniformLocation( prog, "uGlowStrength" ), style.glowStrength );
	setColor( prog, "uInnerGlow", style.innerGlow );
	glUniform1f( glGetUniformLocation( prog, "uInnerGlowRadius" ), style.innerGlowOn() ? style.innerGlowRadius : 0.0f );
	setColor( prog, "uInnerShadow", style.innerShadow );
	glUniform2i( glGetUniformLocation( prog, "uInnerShadowOffset" ), innerDx, -innerDy );
	glUniform1f( glGetUniformLocation( prog, "uInnerShadowSoftness" ), style.innerShadowSoftness );
	glUniform1i( glGetUniformLocation( prog, "uInnerShadowOn" ), innerShadow ? 1 : 0 );
	setColor( prog, "uPlate", style.plate );
	setColor( prog, "uPlateBorder", style.plateBorderColor );
	glUniform1f( glGetUniformLocation( prog, "uPlateBorderWidth" ), style.plateBorderWidth );
	glUniform1f( glGetUniformLocation( prog, "uEdge" ), std::max( 0.25f, style.edgeSoftness ) );

	glBindVertexArray( emptyVao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
}

}//namespace sdfkit
