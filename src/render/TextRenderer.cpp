#include "TextRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include <ffgl/FFGLLog.h>

#if defined( __APPLE__ )
#include <OpenGL/OpenGL.h>// CGLGetCurrentContext
#elif defined( _WIN32 )
#include <windows.h>
#endif

namespace textplus
{
namespace
{
// ---------------------------------------------------------------------------
// Shared vertex shader + glyph distance function (field passes)
// ---------------------------------------------------------------------------
const char* kGlyphVS = R"GLSL(#version 410 core
layout( location = 0 ) in vec2 aCorner;       // 0..1
layout( location = 1 ) in vec4 iPlane;        // l, b, r, t  (em, y up)
layout( location = 2 ) in vec4 iUV;           // u0, v0, u1, v1 (atlas texels)
layout( location = 3 ) in vec4 iPosScaleRot;  // x, y (px, y down), scale, rotation
layout( location = 4 ) in vec4 iMisc;         // alpha, index, pxPerAtlasPx, spreadAtlasPx
layout( location = 5 ) in vec4 iTint;         // rgb, amount

uniform vec2 uViewport;    // px
uniform vec2 uOrigin;      // anchor position, px, y down
uniform vec2 uBlockRot;    // cos, sin of the block rotation (screen frame)
uniform float uSize;       // font size, px

out vec2 vUV;
flat out vec4 vMisc;
flat out vec4 vTint;
flat out float vId;

vec2 rotate( vec2 p, vec2 cs ) { return vec2( cs.x * p.x - cs.y * p.y, cs.y * p.x + cs.x * p.y ); }

void main()
{
	vec2 em    = mix( iPlane.xy, iPlane.zw, aCorner );
	vec2 local = vec2( em.x, -em.y ) * uSize * iPosScaleRot.z;
	local      = rotate( local, vec2( cos( iPosScaleRot.w ), sin( iPosScaleRot.w ) ) );
	vec2 p     = rotate( iPosScaleRot.xy + local, uBlockRot ) + uOrigin;
	gl_Position = vec4( p.x / uViewport.x * 2.0 - 1.0, 1.0 - p.y / uViewport.y * 2.0, 0.0, 1.0 );
	vUV   = mix( iUV.xy, iUV.zw, aCorner );
	vMisc = iMisc;
	vTint = iTint;
	vId   = float( gl_InstanceID % 2048 );// exact in half float
}
)GLSL";

const char* kGlyphDistance = R"GLSL(
uniform sampler2D uAtlas;
uniform vec2 uAtlasSize;
uniform float uSharpCorners;// 1 = MSDF everywhere (mitred corners), 0 = true distance away from the edge (round)

in vec2 vUV;
flat in vec4 vMisc;
flat in vec4 vTint;
flat in float vId;

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
)GLSL";

// Layer k keeps, per pixel, the k-th nearest glyph: the depth test keeps the
// smallest distance, glyphs already stored in layers 0..k-1 are skipped.
const std::string kFieldFS = std::string( "#version 410 core\n" ) + kGlyphDistance + R"GLSL(
uniform float uDepthRange;
uniform int uLayer;
uniform sampler2D uPrev0;
uniform sampler2D uPrev1;
uniform sampler2D uPrev2;
layout( location = 0 ) out vec4 oA;// distance, glyph id, opacity, tint amount
layout( location = 1 ) out vec4 oB;// tint colour
void main()
{
	if( uLayer > 0 )
	{
		ivec2 ip = ivec2( gl_FragCoord.xy );
		if( texelFetch( uPrev0, ip, 0 ).g == vId )
			discard;
		if( uLayer > 1 && texelFetch( uPrev1, ip, 0 ).g == vId )
			discard;
		if( uLayer > 2 && texelFetch( uPrev2, ip, 0 ).g == vId )
			discard;
	}
	float dist   = glyphDistancePx();
	gl_FragDepth = clamp( dist / uDepthRange * 0.5 + 0.5, 0.0, 1.0 );
	oA           = vec4( dist, vId, vMisc.x, vTint.a );
	oB           = vec4( vTint.rgb, 1.0 );
}
)GLSL";

// ---------------------------------------------------------------------------
// Composite: layers -> colour
//
// Opacity per glyph is treated as a threshold: the picture is the integral
// over t in 0..1 of "the text made of all glyphs with opacity >= t". For one
// fading glyph that is a cross-fade between the text with and without it, so
// merged outlines never show a hole where a nearly invisible glyph sits, and
// nothing jumps when it disappears. A uniform opacity a gives a * text, as
// before. Only the nearest kLayers glyphs per pixel take part.
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
uniform int uLayers;
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
uniform float uEdge;         // anti-aliasing width in px

out vec4 oColor;

const float kFar = 1.0e4;

vec4 premul( vec4 c ) { return vec4( c.rgb * c.a, c.a ); }
float inside( float d, float edge ) { return clamp( 0.5 - ( d - edge ) / uEdge, 0.0, 1.0 ); }// coverage of "d < edge"

// Nearest glyphs at one pixel: distance, opacity, tint amount, tint colour
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

Entries load( ivec2 ip )
{
	Entries e;
	e.n = 0;
	if( any( lessThan( ip, ivec2( 0 ) ) ) || any( greaterThanEqual( ip, ivec2( uFieldSize ) ) ) )
		return e;
	for( int k = 0; k < 4; ++k )
	{
		if( k >= uLayers )
			break;
		vec4 a;
		vec3 b;
		fetchLayer( k, ip, a, b );
		if( a.r >= 0.5 * kFar )
			break;// no further glyph near this pixel
		e.a[ e.n ]    = a;
		e.tint[ e.n ] = b;
		e.n++;
	}
	return e;
}

// Union distance of the glyphs with opacity >= t
float unionDistance( Entries e, float t )
{
	float d = kFar;
	for( int i = 0; i < 4; ++i )
		if( i < e.n && e.a[ i ].b >= t )
			d = min( d, e.a[ i ].r );
	return d;
}

// Tint of the glyphs with opacity >= t, blended smoothly where they meet
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

void main()
{
	vec2 frag = gl_FragCoord.xy - uViewportOrigin;
	ivec2 ip  = ivec2( frag );
	Entries ep = load( ip );
	bool shadowOn = uShadow.a > 0.0;
	Entries eq;
	eq.n = 0;
	if( shadowOn )
		eq = load( ip - uShadowOffset );
	if( ep.n == 0 && eq.n == 0 )
	{
		oColor = vec4( 0.0 );
		return;
	}

	// Opacity thresholds of all glyphs involved, highest first
	float th[ 8 ];
	int nt = 0;
	for( int i = 0; i < 4; ++i )
		if( i < ep.n )
			th[ nt++ ] = ep.a[ i ].b;
	for( int i = 0; i < 4; ++i )
		if( i < eq.n )
			th[ nt++ ] = eq.a[ i ].b;
	for( int i = 1; i < 8; ++i )
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
	for( int k = 0; k < 8; ++k )
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

		// Text stack: inner layers REPLACE outer ones (mix by coverage), so a
		// transparent fill shows through (hollow text) and bands have no seams.
		vec4 text = vec4( 0.0 );
		if( uOutline2Width > 0.0 )
			text = mix( text, premul( uOutline2 ), inside( d, outer ) );
		if( uOutline1Edges.x > 0.0 )
			text = mix( text, premul( uOutline1 ), inside( d, uOutline1Edges.y ) );
		text = mix( text, premul( fill ), inside( d, uOutline1Edges.z ) );

		// Effects behind the text
		vec4 back = vec4( 0.0 );
		if( shadowOn )
		{
			float w  = max( uShadowSoftness, uEdge );
			float ds = unionDistance( eq, t ) - outer - uShadowSpread;
			back     = premul( uShadow ) * ( 1.0 - smoothstep( -0.5 * w, 0.5 * w, ds ) );
		}
		if( uGlowRadius > 0.0 && uGlowStrength > 0.0 && uGlow.a > 0.0 )
		{
			float g  = max( d - outer, 0.0 );
			float kk = clamp( 1.0 - g / uGlowRadius, 0.0, 1.0 );
			vec4 gc  = premul( uGlow ) * clamp( uGlowStrength * kk * kk, 0.0, 1.0 );
			back     = gc + back * ( 1.0 - gc.a );
		}
		// Effects are knocked out under the text silhouette (Photoshop's default).
		back *= 1.0 - inside( d, outer );

		result += weight * ( text + back * ( 1.0 - text.a ) );
	}
	if( result.a > 0.0 )
		result.rgb /= result.a;// Resolume composites sources as straight alpha
	oColor = result;
}
)GLSL";

GLuint compileStage( GLenum type, const char* src )
{
	GLuint s = glCreateShader( type );
	glShaderSource( s, 1, &src, nullptr );
	glCompileShader( s );
	GLint ok = 0;
	glGetShaderiv( s, GL_COMPILE_STATUS, &ok );
	if( !ok )
	{
		char log[ 2048 ];
		glGetShaderInfoLog( s, sizeof( log ), nullptr, log );
		FFGLLog::LogToHost( ( std::string( "Text+ shader error: " ) + log ).c_str() );
		std::fprintf( stderr, "Text+ shader error: %s\n", log );
		glDeleteShader( s );
		return 0;
	}
	return s;
}

GLuint linkProgram( const char* vs, const char* fs )
{
	GLuint v = compileStage( GL_VERTEX_SHADER, vs );
	GLuint f = compileStage( GL_FRAGMENT_SHADER, fs );
	if( !v || !f )
	{
		if( v )
			glDeleteShader( v );
		if( f )
			glDeleteShader( f );
		return 0;
	}
	GLuint p = glCreateProgram();
	glAttachShader( p, v );
	glAttachShader( p, f );
	glLinkProgram( p );
	glDeleteShader( v );
	glDeleteShader( f );
	GLint ok = 0;
	glGetProgramiv( p, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		char log[ 2048 ];
		glGetProgramInfoLog( p, sizeof( log ), nullptr, log );
		FFGLLog::LogToHost( ( std::string( "Text+ link error: " ) + log ).c_str() );
		std::fprintf( stderr, "Text+ link error: %s\n", log );
		glDeleteProgram( p );
		return 0;
	}
	return p;
}

void* currentContextKey()
{
#if defined( __APPLE__ )
	return (void*)CGLGetCurrentContext();
#elif defined( _WIN32 )
	return (void*)wglGetCurrentContext();
#else
	return nullptr;// Linux is only used by the single-context test host
#endif
}

// Saves the GL state we touch and restores it on scope exit, so the host
// (Resolume) finds its context exactly as it left it.
struct StateGuard
{
	GLint drawFbo = 0, readFbo = 0, viewport[ 4 ]{}, scissorBox[ 4 ]{}, program = 0, vao = 0, arrayBuffer = 0;
	GLint activeTex = 0, tex[ 8 ]{};
	GLboolean blend = 0, depthTest = 0, scissor = 0, cull = 0, depthMask = 1;
	GLint depthFunc = GL_LESS, blendSrcRGB = GL_ONE, blendDstRGB = GL_ZERO, blendSrcA = GL_ONE, blendDstA = GL_ZERO, blendEqRGB = GL_FUNC_ADD, blendEqA = GL_FUNC_ADD;
	GLfloat clearColor[ 4 ]{};
	GLfloat clearDepth = 1;

	StateGuard()
	{
		glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo );
		glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &readFbo );
		glGetIntegerv( GL_VIEWPORT, viewport );
		glGetIntegerv( GL_SCISSOR_BOX, scissorBox );
		glGetIntegerv( GL_CURRENT_PROGRAM, &program );
		glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &vao );
		glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &arrayBuffer );
		glGetIntegerv( GL_ACTIVE_TEXTURE, &activeTex );
		for( int i = 0; i < 8; ++i )
		{
			glActiveTexture( GL_TEXTURE0 + i );
			glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex[ i ] );
		}
		blend     = glIsEnabled( GL_BLEND );
		depthTest = glIsEnabled( GL_DEPTH_TEST );
		scissor   = glIsEnabled( GL_SCISSOR_TEST );
		cull      = glIsEnabled( GL_CULL_FACE );
		glGetBooleanv( GL_DEPTH_WRITEMASK, &depthMask );
		glGetIntegerv( GL_DEPTH_FUNC, &depthFunc );
		glGetIntegerv( GL_BLEND_SRC_RGB, &blendSrcRGB );
		glGetIntegerv( GL_BLEND_DST_RGB, &blendDstRGB );
		glGetIntegerv( GL_BLEND_SRC_ALPHA, &blendSrcA );
		glGetIntegerv( GL_BLEND_DST_ALPHA, &blendDstA );
		glGetIntegerv( GL_BLEND_EQUATION_RGB, &blendEqRGB );
		glGetIntegerv( GL_BLEND_EQUATION_ALPHA, &blendEqA );
		glGetFloatv( GL_COLOR_CLEAR_VALUE, clearColor );
		glGetFloatv( GL_DEPTH_CLEAR_VALUE, &clearDepth );
	}
	~StateGuard()
	{
		glBindFramebuffer( GL_DRAW_FRAMEBUFFER, (GLuint)drawFbo );
		glBindFramebuffer( GL_READ_FRAMEBUFFER, (GLuint)readFbo );
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
		glScissor( scissorBox[ 0 ], scissorBox[ 1 ], scissorBox[ 2 ], scissorBox[ 3 ] );
		glUseProgram( (GLuint)program );
		glBindVertexArray( (GLuint)vao );
		glBindBuffer( GL_ARRAY_BUFFER, (GLuint)arrayBuffer );
		for( int i = 7; i >= 0; --i )
		{
			glActiveTexture( GL_TEXTURE0 + i );
			glBindTexture( GL_TEXTURE_2D, (GLuint)tex[ i ] );
		}
		glActiveTexture( (GLenum)activeTex );
		blend ? glEnable( GL_BLEND ) : glDisable( GL_BLEND );
		depthTest ? glEnable( GL_DEPTH_TEST ) : glDisable( GL_DEPTH_TEST );
		scissor ? glEnable( GL_SCISSOR_TEST ) : glDisable( GL_SCISSOR_TEST );
		cull ? glEnable( GL_CULL_FACE ) : glDisable( GL_CULL_FACE );
		glDepthMask( depthMask );
		glDepthFunc( (GLenum)depthFunc );
		glBlendFuncSeparate( (GLenum)blendSrcRGB, (GLenum)blendDstRGB, (GLenum)blendSrcA, (GLenum)blendDstA );
		glBlendEquationSeparate( (GLenum)blendEqRGB, (GLenum)blendEqA );
		glClearColor( clearColor[ 0 ], clearColor[ 1 ], clearColor[ 2 ], clearColor[ 3 ] );
		glClearDepth( clearDepth );
	}
};

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

}//namespace

// ---------------------------------------------------------------------------
// Programs and scratch render targets exist once per GL context and are shared
// by all plugin instances: instances in one context render one after another,
// and nothing in the targets survives a render() call.
// ---------------------------------------------------------------------------
struct TextRenderer::SharedPrograms
{
	void* context = nullptr;
	GLuint field = 0, composite = 0;
	int refs = 0;

	// Layer k: A = distance, glyph id, opacity, tint amount (RGBA16F); B = tint colour (RGBA8)
	GLuint layerFbo[ kLayers ]{}, layerA[ kLayers ]{}, layerB[ kLayers ]{};
	GLuint depth = 0;
	int w = 0, h = 0;

	void releaseTargets()
	{
		glDeleteFramebuffers( kLayers, layerFbo );
		glDeleteTextures( kLayers, layerA );
		glDeleteTextures( kLayers, layerB );
		if( depth )
			glDeleteRenderbuffers( 1, &depth );
		for( int k = 0; k < kLayers; ++k )
			layerFbo[ k ] = layerA[ k ] = layerB[ k ] = 0;
		depth = 0;
		w = h = 0;
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
		bool ok = true;
		glGenFramebuffers( kLayers, layerFbo );
		for( int k = 0; k < kLayers; ++k )
		{
			layerA[ k ] = makeTexture( GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, nw, nh, GL_NEAREST );
			layerB[ k ] = makeTexture( GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nw, nh, GL_NEAREST );
			glBindFramebuffer( GL_FRAMEBUFFER, layerFbo[ k ] );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, layerA[ k ], 0 );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, layerB[ k ], 0 );
			glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth );
			const GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
			glDrawBuffers( 2, bufs );
			ok = ok && glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
		}
		w = nw;
		h = nh;
		if( !ok )
			FFGLLog::LogToHost( "Text+: offscreen framebuffer incomplete" );
		return ok;
	}
};

namespace
{
std::mutex g_programMutex;
std::vector< TextRenderer::SharedPrograms* > g_programs;
}//namespace

// ---------------------------------------------------------------------------
float Style::outerEdge() const
{
	return ( outline1Position == OutlinePosition::Outside ? outline1Width : outline1Position == OutlinePosition::Center ? outline1Width * 0.5f : 0.0f ) + outline2Width;
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
}

// ---------------------------------------------------------------------------
bool TextRenderer::init()
{
	release();
	{
		std::lock_guard< std::mutex > lock( g_programMutex );
		void* ctx = currentContextKey();
		for( SharedPrograms* sp : g_programs )
			if( sp->context == ctx )
				programs = sp;
		if( !programs )
		{
			auto* sp      = new SharedPrograms();
			sp->context   = ctx;
			sp->field     = linkProgram( kGlyphVS, kFieldFS.c_str() );
			sp->composite = linkProgram( kCompositeVS, kCompositeFS );
			if( !sp->field || !sp->composite )
			{
				if( sp->field )
					glDeleteProgram( sp->field );
				if( sp->composite )
					glDeleteProgram( sp->composite );
				delete sp;
				return false;
			}
			g_programs.push_back( sp );
			programs = sp;
		}
		++programs->refs;
	}

	GLint prevVao = 0, prevBuf = 0;
	glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &prevVao );
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &prevBuf );

	glGenVertexArrays( 1, &glyphVao );
	glGenVertexArrays( 1, &emptyVao );
	glGenBuffers( 1, &cornerVbo );
	glGenBuffers( 1, &instanceVbo );

	glBindVertexArray( glyphVao );
	const float corners[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	glBindBuffer( GL_ARRAY_BUFFER, cornerVbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof( corners ), corners, GL_STATIC_DRAW );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 0, nullptr );

	glBindBuffer( GL_ARRAY_BUFFER, instanceVbo );
	glBufferData( GL_ARRAY_BUFFER, 64 * sizeof( GlyphInstance ), nullptr, GL_DYNAMIC_DRAW );
	instanceCapacity = 64;
	for( int i = 0; i < 5; ++i )
	{
		glEnableVertexAttribArray( 1 + i );
		glVertexAttribPointer( 1 + i, 4, GL_FLOAT, GL_FALSE, sizeof( GlyphInstance ), (const void*)( sizeof( float ) * 4 * i ) );
		glVertexAttribDivisor( 1 + i, 1 );
	}

	glBindVertexArray( (GLuint)prevVao );
	glBindBuffer( GL_ARRAY_BUFFER, (GLuint)prevBuf );
	return true;
}

void TextRenderer::release()
{
	if( programs )
	{
		std::lock_guard< std::mutex > lock( g_programMutex );
		if( --programs->refs == 0 )
		{
			glDeleteProgram( programs->field );
			glDeleteProgram( programs->composite );
			programs->releaseTargets();
			g_programs.erase( std::remove( g_programs.begin(), g_programs.end(), programs ), g_programs.end() );
			delete programs;
		}
		programs = nullptr;
	}
	if( glyphVao )
		glDeleteVertexArrays( 1, &glyphVao );
	if( emptyVao )
		glDeleteVertexArrays( 1, &emptyVao );
	if( cornerVbo )
		glDeleteBuffers( 1, &cornerVbo );
	if( instanceVbo )
		glDeleteBuffers( 1, &instanceVbo );
	glyphVao = emptyVao = cornerVbo = instanceVbo = 0;
	instanceCapacity = 0;
}

void TextRenderer::render( const FFGLViewportStruct& vp, GLuint hostFbo, const std::vector< GlyphInstance >& glyphs, const BlockTransform& t,
						   const Style& style, GLuint atlasTexture, int atlasSize )
{
	if( !isReady() || vp.width == 0 || vp.height == 0 )
		return;
	StateGuard guard;
	const int w = (int)vp.width, h = (int)vp.height;
	const float blockCos = std::cos( -t.rotation ), blockSin = std::sin( -t.rotation );

	glDisable( GL_BLEND );
	glDisable( GL_CULL_FACE );
	glEnable( GL_SCISSOR_TEST );

	// Clear the whole output; only the text's bounding box gets shaded afterwards.
	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glViewport( (GLint)vp.x, (GLint)vp.y, w, h );
	glScissor( (GLint)vp.x, (GLint)vp.y, w, h );
	glClearColor( 0, 0, 0, 0 );
	glClear( GL_COLOR_BUFFER_BIT );
	if( glyphs.empty() || !atlasTexture || !programs->ensureTargets( w, h ) )
		return;

	// Screen-space bounding box of all quads (they already include the effect reach).
	Rect bbox;
	bool alphaVaries = false, tintVaries = false;
	const GlyphInstance& g0 = glyphs.front();
	for( const GlyphInstance& g : glyphs )
	{
		const float c = std::cos( g.rotation ), s = std::sin( g.rotation );
		const float k = t.sizePx * g.scale;
		const float ex[ 2 ] = { g.planeL, g.planeR }, ey[ 2 ] = { g.planeB, g.planeT };
		for( float emx : ex )
			for( float emy : ey )
			{
				float lx = emx * k, ly = -emy * k;
				float rx = c * lx - s * ly + g.x, ry = s * lx + c * ly + g.y;
				bbox.add( blockCos * rx - blockSin * ry + t.originX, blockSin * rx + blockCos * ry + t.originY );
			}
		alphaVaries = alphaVaries || std::fabs( g.alpha - g0.alpha ) > 1e-4f;
		tintVaries  = tintVaries || g.tintAmount != g0.tintAmount || g.tintR != g0.tintR || g.tintG != g0.tintG || g.tintB != g0.tintB;
	}
	if( bbox.empty() )
		return;
	// Glyphs of different opacity need the nearest few glyphs per pixel; different
	// tints need two for smooth colour transitions; otherwise the nearest is enough.
	const int layers = alphaVaries ? kLayers : tintVaries ? 2 : 1;

	bbox = Rect{ bbox.x0 - 2, bbox.y0 - 2, bbox.x1 + 2, bbox.y1 + 2 };
	const bool shadow = style.shadow.a > 0;
	// Whole pixels, so the shadow reads the layers texel-exact.
	const int shadowDx = (int)std::lround( style.shadowDx ), shadowDy = (int)std::lround( style.shadowDy );
	Rect shadedArea = bbox;// what the composite writes
	if( shadow )
		shadedArea.add( bbox.shifted( (float)shadowDx, (float)shadowDy ) );
	Rect fieldArea = shadedArea;// what the composite reads
	if( shadow )
		fieldArea.add( shadedArea.shifted( (float)-shadowDx, (float)-shadowDy ) );

	// ---- Field passes: per pixel the nearest `layers` glyphs ----
	glBindVertexArray( glyphVao );
	glBindBuffer( GL_ARRAY_BUFFER, instanceVbo );
	if( glyphs.size() > instanceCapacity )
	{
		instanceCapacity = std::max( glyphs.size(), instanceCapacity * 2 );
		glBufferData( GL_ARRAY_BUFFER, instanceCapacity * sizeof( GlyphInstance ), nullptr, GL_DYNAMIC_DRAW );
	}
	glBufferSubData( GL_ARRAY_BUFFER, 0, glyphs.size() * sizeof( GlyphInstance ), glyphs.data() );

	const GLuint fp = programs->field;
	glUseProgram( fp );
	glUniform2f( glGetUniformLocation( fp, "uViewport" ), (float)w, (float)h );
	glUniform2f( glGetUniformLocation( fp, "uOrigin" ), t.originX, t.originY );
	glUniform2f( glGetUniformLocation( fp, "uBlockRot" ), blockCos, blockSin );
	glUniform1f( glGetUniformLocation( fp, "uSize" ), t.sizePx );
	glUniform2f( glGetUniformLocation( fp, "uAtlasSize" ), (float)atlasSize, (float)atlasSize );
	glUniform1f( glGetUniformLocation( fp, "uSharpCorners" ), style.sharpCorners ? 1.0f : 0.0f );
	glUniform1f( glGetUniformLocation( fp, "uDepthRange" ), 16384.0f );
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
	for( int k = 0; k < layers; ++k )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, programs->layerFbo[ k ] );
		const GLfloat farA[] = { 1.0e4f, -1.0f, 0.0f, 0.0f }, zeroB[] = { 0, 0, 0, 0 };
		glClearBufferfv( GL_COLOR, 0, farA );
		glClearBufferfv( GL_COLOR, 1, zeroB );
		glClear( GL_DEPTH_BUFFER_BIT );
		for( int j = 0; j < 3; ++j )
		{
			glActiveTexture( GL_TEXTURE1 + j );
			glBindTexture( GL_TEXTURE_2D, j < k ? programs->layerA[ j ] : 0 );
		}
		glUniform1i( glGetUniformLocation( fp, "uLayer" ), k );
		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, (GLsizei)glyphs.size() );
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
	glUniform1i( glGetUniformLocation( prog, "uLayers" ), layers );
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
	glUniform1f( glGetUniformLocation( prog, "uEdge" ), std::max( 0.25f, style.edgeSoftness ) );

	glBindVertexArray( emptyVao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
}

}//namespace textplus
