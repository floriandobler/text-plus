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
// Shared vertex shader + glyph distance function (passes 1 and 1b)
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
}
)GLSL";

const char* kGlyphDistance = R"GLSL(
uniform sampler2D uAtlas;
uniform vec2 uAtlasSize;
uniform float uSharpCorners;// 1 = MSDF everywhere (mitred corners), 0 = true distance away from the edge (round)

in vec2 vUV;
flat in vec4 vMisc;
flat in vec4 vTint;

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

const std::string kFieldFS = std::string( "#version 410 core\n" ) + kGlyphDistance + R"GLSL(
uniform float uDepthRange;
layout( location = 0 ) out vec4 oField;
void main()
{
	float dist   = glyphDistancePx();
	gl_FragDepth = clamp( dist / uDepthRange * 0.5 + 0.5, 0.0, 1.0 );
	oField       = vec4( dist, vMisc.x, vMisc.y, 1.0 );
}
)GLSL";

const std::string kAccumFS = std::string( "#version 410 core\n" ) + kGlyphDistance + R"GLSL(
uniform float uAttrSoftness;// px
layout( location = 0 ) out vec4 oAcc0;// w*alpha, w, w*tintAmount, -
layout( location = 1 ) out vec4 oAcc1;// w*tintAmount*tint.rgb, -
void main()
{
	float dist = glyphDistancePx();
	float w    = exp( clamp( -dist / uAttrSoftness, -40.0, 40.0 ) );
	float wt   = w * vTint.a;
	oAcc0      = vec4( w * vMisc.x, w, wt, 0.0 );
	oAcc1      = vec4( wt * vTint.rgb, 0.0 );
}
)GLSL";

// ---------------------------------------------------------------------------
// Pass 2: field -> colour
// ---------------------------------------------------------------------------
const char* kCompositeVS = R"GLSL(#version 410 core
void main()
{
	vec2 p      = vec2( ( gl_VertexID << 1 ) & 2, gl_VertexID & 2 );// full-screen triangle
	gl_Position = vec4( p * 2.0 - 1.0, 0.0, 1.0 );
}
)GLSL";

const char* kCompositeFS = R"GLSL(#version 410 core
uniform sampler2D uField;
uniform sampler2D uAcc0;
uniform sampler2D uAcc1;
uniform float uUseAccum;
uniform vec2 uFieldSize;
uniform vec2 uViewportOrigin;

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
uniform vec2 uShadowOffset;  // px, GL orientation (y up)
uniform float uShadowSoftness;
uniform float uShadowSpread;
uniform vec4 uGlow;
uniform float uGlowRadius;
uniform float uGlowStrength;
uniform float uEdge;         // anti-aliasing width in px

out vec4 oColor;

vec4 premul( vec4 c ) { return vec4( c.rgb * c.a, c.a ); }
float inside( float d, float edge ) { return clamp( 0.5 - ( d - edge ) / uEdge, 0.0, 1.0 ); }// coverage of "d < edge"

float alphaAt( vec4 field, vec4 acc0 )
{
	return ( uUseAccum > 0.5 && acc0.y > 0.0 ) ? acc0.x / acc0.y : field.g;
}

void main()
{
	vec2 frag  = gl_FragCoord.xy - uViewportOrigin;
	ivec2 ip   = ivec2( frag );
	vec4 f     = texelFetch( uField, ip, 0 );
	vec4 acc0  = uUseAccum > 0.5 ? texelFetch( uAcc0, ip, 0 ) : vec4( 0.0 );
	float d    = f.r;
	float a    = alphaAt( f, acc0 );

	// Fill colour: solid or box gradient, then per-glyph tint
	vec4 fill = uFill;
	if( uFillMode == 1 )
	{
		vec2 s     = vec2( frag.x, uFieldSize.y - frag.y ) - uBlockOrigin;// screen px, y down, relative to anchor
		vec2 local = vec2( uBlockRotInv.x * s.x - uBlockRotInv.y * s.y, uBlockRotInv.y * s.x + uBlockRotInv.x * s.y );
		vec2 c     = 0.5 * ( uBox.xy + uBox.zw );
		vec2 halfs = 0.5 * ( uBox.zw - uBox.xy );
		float ext  = max( abs( halfs.x * uGradDir.x ) + abs( halfs.y * uGradDir.y ), 1e-3 );
		float t    = clamp( dot( local - c, uGradDir ) / ext * 0.5 + 0.5, 0.0, 1.0 );
		fill       = mix( uFill, uFill2, t );
	}
	if( uUseAccum > 0.5 && acc0.z > 0.0 )
	{
		vec3 tint  = texelFetch( uAcc1, ip, 0 ).rgb / acc0.z;
		float amt  = clamp( acc0.z / acc0.y, 0.0, 1.0 );
		fill.rgb   = mix( fill.rgb, tint, amt );
	}

	// Text stack: inner layers REPLACE outer ones (mix by coverage), so a
	// transparent fill shows through (hollow text) and bands have no seams.
	vec4 text   = vec4( 0.0 );
	float outer = uOutline1Edges.y;
	if( uOutline2Width > 0.0 )
	{
		float e = uOutline1Edges.y + uOutline2Width;
		text    = mix( text, premul( uOutline2 ), inside( d, e ) );
		outer   = e;
	}
	if( uOutline1Edges.x > 0.0 )
		text = mix( text, premul( uOutline1 ), inside( d, uOutline1Edges.y ) );
	text = mix( text, premul( fill ), inside( d, uOutline1Edges.z ) );
	text *= a;

	// Effects behind the text
	vec4 back = vec4( 0.0 );
	if( uShadow.a > 0.0 )
	{
		vec2 p = ( frag - uShadowOffset ) / uFieldSize;
		if( all( greaterThanEqual( p, vec2( 0.0 ) ) ) && all( lessThanEqual( p, vec2( 1.0 ) ) ) )
		{
			vec4 fs   = texture( uField, p );
			vec4 as0  = uUseAccum > 0.5 ? texture( uAcc0, p ) : vec4( 0.0 );
			float w   = max( uShadowSoftness, uEdge );
			float ds  = fs.r - outer - uShadowSpread;
			back      = premul( uShadow ) * ( 1.0 - smoothstep( -0.5 * w, 0.5 * w, ds ) ) * alphaAt( fs, as0 );
		}
	}
	if( uGlowRadius > 0.0 && uGlowStrength > 0.0 && uGlow.a > 0.0 )
	{
		float g  = max( d - outer, 0.0 );
		float k  = clamp( 1.0 - g / uGlowRadius, 0.0, 1.0 );
		float ga = clamp( uGlowStrength * k * k, 0.0, 1.0 ) * a;
		vec4 gc  = premul( uGlow ) * ga;
		back     = gc + back * ( 1.0 - gc.a );
	}
	// Effects are knocked out under the text silhouette (Photoshop's default).
	back *= 1.0 - inside( d, outer ) * a;

	vec4 result = text + back * ( 1.0 - text.a );
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
	GLint activeTex = 0, tex[ 3 ]{};
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
		for( int i = 0; i < 3; ++i )
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
		for( int i = 2; i >= 0; --i )
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
// Programs are compiled once per GL context and shared by all plugin
// instances (compiling on every new clip caused a visible hitch).
// ---------------------------------------------------------------------------
struct TextRenderer::SharedPrograms
{
	void* context = nullptr;
	GLuint field = 0, accum = 0, composite = 0;
	int refs     = 0;
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
			sp->accum     = linkProgram( kGlyphVS, kAccumFS.c_str() );
			sp->composite = linkProgram( kCompositeVS, kCompositeFS );
			if( !sp->field || !sp->accum || !sp->composite )
			{
				if( sp->field )
					glDeleteProgram( sp->field );
				if( sp->accum )
					glDeleteProgram( sp->accum );
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
			glDeleteProgram( programs->accum );
			glDeleteProgram( programs->composite );
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
	GLuint fbos[] = { fieldFbo, accumFbo };
	glDeleteFramebuffers( 2, fbos );
	GLuint texs[] = { fieldTex, accumTex0, accumTex1 };
	glDeleteTextures( 3, texs );
	if( fieldDepth )
		glDeleteRenderbuffers( 1, &fieldDepth );
	glyphVao = emptyVao = cornerVbo = instanceVbo = 0;
	fieldFbo = fieldTex = fieldDepth = accumFbo = accumTex0 = accumTex1 = 0;
	targetW = targetH = 0;
	instanceCapacity  = 0;
}

bool TextRenderer::ensureTargets( int w, int h )
{
	if( fieldFbo && w == targetW && h == targetH )
		return true;
	GLuint fbos[] = { fieldFbo, accumFbo };
	glDeleteFramebuffers( 2, fbos );
	GLuint texs[] = { fieldTex, accumTex0, accumTex1 };
	glDeleteTextures( 3, texs );
	if( fieldDepth )
		glDeleteRenderbuffers( 1, &fieldDepth );

	fieldTex  = makeTexture( GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, w, h, GL_LINEAR );
	accumTex0 = makeTexture( GL_RGBA32F, GL_RGBA, GL_FLOAT, w, h, GL_NEAREST );
	accumTex1 = makeTexture( GL_RGBA32F, GL_RGBA, GL_FLOAT, w, h, GL_NEAREST );

	glGenRenderbuffers( 1, &fieldDepth );
	glBindRenderbuffer( GL_RENDERBUFFER, fieldDepth );
	glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, w, h );
	glBindRenderbuffer( GL_RENDERBUFFER, 0 );

	glGenFramebuffers( 1, &fieldFbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fieldFbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fieldTex, 0 );
	glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fieldDepth );
	bool ok = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;

	glGenFramebuffers( 1, &accumFbo );
	glBindFramebuffer( GL_FRAMEBUFFER, accumFbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, accumTex0, 0 );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, accumTex1, 0 );
	const GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	glDrawBuffers( 2, bufs );
	ok = ok && glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;

	targetW = w;
	targetH = h;
	if( !ok )
		FFGLLog::LogToHost( "Text+: offscreen framebuffer incomplete" );
	return ok;
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
	if( glyphs.empty() || !atlasTexture || !ensureTargets( w, h ) )
		return;

	// Screen-space bounding box of all quads (they already include the effect reach).
	Rect bbox;
	bool needAccum = false;
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
		needAccum = needAccum || g.alpha < 0.999f || g.tintAmount > 0.0f;
	}
	if( bbox.empty() )
		return;
	bbox = Rect{ bbox.x0 - 2, bbox.y0 - 2, bbox.x1 + 2, bbox.y1 + 2 };
	const bool shadow = style.shadow.a > 0;
	Rect shadedArea = bbox;// what pass 2 writes
	if( shadow )
		shadedArea.add( bbox.shifted( style.shadowDx, style.shadowDy ) );
	Rect fieldArea = shadedArea;// what pass 2 reads
	if( shadow )
		fieldArea.add( shadedArea.shifted( -style.shadowDx, -style.shadowDy ) );

	// ---- Pass 1: distance field of the union ----
	glBindVertexArray( glyphVao );
	glBindBuffer( GL_ARRAY_BUFFER, instanceVbo );
	if( glyphs.size() > instanceCapacity )
	{
		instanceCapacity = std::max( glyphs.size(), instanceCapacity * 2 );
		glBufferData( GL_ARRAY_BUFFER, instanceCapacity * sizeof( GlyphInstance ), nullptr, GL_DYNAMIC_DRAW );
	}
	glBufferSubData( GL_ARRAY_BUFFER, 0, glyphs.size() * sizeof( GlyphInstance ), glyphs.data() );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, atlasTexture );

	auto setGlyphUniforms = [ & ]( GLuint prog ) {
		glUseProgram( prog );
		glUniform2f( glGetUniformLocation( prog, "uViewport" ), (float)w, (float)h );
		glUniform2f( glGetUniformLocation( prog, "uOrigin" ), t.originX, t.originY );
		glUniform2f( glGetUniformLocation( prog, "uBlockRot" ), blockCos, blockSin );
		glUniform1f( glGetUniformLocation( prog, "uSize" ), t.sizePx );
		glUniform2f( glGetUniformLocation( prog, "uAtlasSize" ), (float)atlasSize, (float)atlasSize );
		glUniform1f( glGetUniformLocation( prog, "uSharpCorners" ), style.sharpCorners ? 1.0f : 0.0f );
		glUniform1i( glGetUniformLocation( prog, "uAtlas" ), 0 );
	};

	glBindFramebuffer( GL_FRAMEBUFFER, fieldFbo );
	glViewport( 0, 0, w, h );
	setScissor( fieldArea, 0, 0, w, h );
	glClearColor( 1.0e4f, 0.0f, 0.0f, 0.0f );// "far away", alpha 0
	glClearDepth( 1.0 );
	glDepthMask( GL_TRUE );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	setGlyphUniforms( programs->field );
	glUniform1f( glGetUniformLocation( programs->field, "uDepthRange" ), 16384.0f );
	glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, (GLsizei)glyphs.size() );
	glDisable( GL_DEPTH_TEST );

	// ---- Pass 1b: smooth per-glyph attributes (only when they differ) ----
	if( needAccum )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, accumFbo );
		glClearColor( 0, 0, 0, 0 );
		glClear( GL_COLOR_BUFFER_BIT );
		glEnable( GL_BLEND );
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ONE, GL_ONE );
		setGlyphUniforms( programs->accum );
		// Transition width between neighbouring glyphs: grows with the outline so
		// merged outlines fade smoothly from one glyph's opacity to the next.
		const float soft = std::max( 1.0f, 0.3f * style.outerEdge() + 0.01f * t.sizePx );
		glUniform1f( glGetUniformLocation( programs->accum, "uAttrSoftness" ), soft );
		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, (GLsizei)glyphs.size() );
		glDisable( GL_BLEND );
	}

	// ---- Pass 2: composite into the host framebuffer ----
	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glViewport( (GLint)vp.x, (GLint)vp.y, w, h );
	setScissor( shadedArea, (int)vp.x, (int)vp.y, w, h );
	const GLuint prog = programs->composite;
	glUseProgram( prog );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, fieldTex );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, accumTex0 );
	glActiveTexture( GL_TEXTURE2 );
	glBindTexture( GL_TEXTURE_2D, accumTex1 );
	glUniform1i( glGetUniformLocation( prog, "uField" ), 0 );
	glUniform1i( glGetUniformLocation( prog, "uAcc0" ), 1 );
	glUniform1i( glGetUniformLocation( prog, "uAcc1" ), 2 );
	glUniform1f( glGetUniformLocation( prog, "uUseAccum" ), needAccum ? 1.0f : 0.0f );
	glUniform2f( glGetUniformLocation( prog, "uFieldSize" ), (float)w, (float)h );
	glUniform2f( glGetUniformLocation( prog, "uViewportOrigin" ), (float)vp.x, (float)vp.y );

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
	glUniform2f( glGetUniformLocation( prog, "uShadowOffset" ), style.shadowDx, -style.shadowDy );
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
