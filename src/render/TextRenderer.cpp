#include "TextRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <ffgl/FFGLLog.h>

namespace kontur
{
namespace
{
// ---------------------------------------------------------------------------
// Pass 1: glyph quads -> signed distance field of the union (depth = min)
// ---------------------------------------------------------------------------
const char* kFieldVS = R"GLSL(#version 410 core
layout( location = 0 ) in vec2 aCorner;       // 0..1
layout( location = 1 ) in vec4 iPlane;        // l, b, r, t  (em, y up)
layout( location = 2 ) in vec4 iUV;           // u0, v0, u1, v1 (atlas texels)
layout( location = 3 ) in vec4 iPosScaleRot;  // x, y (px, y down), scale, rotation
layout( location = 4 ) in vec4 iMisc;         // alpha, index, pxPerAtlasPx, spreadAtlasPx

uniform vec2 uViewport;    // px
uniform vec2 uOrigin;      // anchor position, px, y down
uniform vec2 uBlockRot;    // cos, sin of the block rotation
uniform float uSize;       // font size, px

out vec2 vUV;
flat out vec4 vMisc;

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
}
)GLSL";

const char* kFieldFS = R"GLSL(#version 410 core
uniform sampler2D uAtlas;
uniform vec2 uAtlasSize;
uniform float uDepthRange;
uniform float uSharpCorners;// 1 = MSDF everywhere (mitred corners), 0 = true distance away from the edge (round)

in vec2 vUV;
flat in vec4 vMisc;
layout( location = 0 ) out vec4 oField;

float median( vec3 v ) { return max( min( v.r, v.g ), min( max( v.r, v.g ), v.b ) ); }

void main()
{
	vec4 s       = texture( uAtlas, vUV / uAtlasSize );
	float range  = 2.0 * vMisc.w;
	float dMsd   = ( median( s.rgb ) - 0.5 ) * range; // sharp corners, valid near the outline
	float dTrue  = ( s.a - 0.5 ) * range;             // true distance, valid far away (round outer corners)
	// MSDF keeps the glyph's own corners sharp but is only exact close to the outline.
	// The true distance gives round offsets but is smoothed by bilinear filtering
	// within about one atlas texel of a corner, so the switch happens 1..2 texels out.
	float d      = mix( dMsd, dTrue, ( 1.0 - uSharpCorners ) * smoothstep( 1.0, 2.0, abs( dTrue ) ) );
	float distPx = -d * vMisc.z;                     // output pixels, outside positive
	gl_FragDepth = clamp( distPx / uDepthRange * 0.5 + 0.5, 0.0, 1.0 );
	oField       = vec4( distPx, vMisc.x, vMisc.y, 1.0 );
}
)GLSL";

// ---------------------------------------------------------------------------
// Pass 2: field -> colour
// ---------------------------------------------------------------------------
const char* kCompositeVS = R"GLSL(#version 410 core
void main()
{
	// full-screen triangle
	vec2 p      = vec2( ( gl_VertexID << 1 ) & 2, gl_VertexID & 2 );
	gl_Position = vec4( p * 2.0 - 1.0, 0.0, 1.0 );
}
)GLSL";

const char* kCompositeFS = R"GLSL(#version 410 core
uniform sampler2D uField;
uniform vec2 uFieldSize;
uniform vec2 uViewportOrigin;

uniform vec4 uFill;          // straight colours
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
// Coverage of the region "distance < edge"
float inside( float d, float edge ) { return clamp( 0.5 - ( d - edge ) / uEdge, 0.0, 1.0 ); }

void main()
{
	vec2 frag = gl_FragCoord.xy - uViewportOrigin;
	vec4 f    = texelFetch( uField, ivec2( frag ), 0 );
	float d   = f.r;
	float a   = f.g;

	// Text stack. Inner layers REPLACE outer ones (mix by coverage), so a
	// transparent fill really shows through (hollow text) and adjacent bands
	// have no seams.
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
	text = mix( text, premul( uFill ), inside( d, uOutline1Edges.z ) );
	text *= a;

	// Effects behind the text
	vec4 back = vec4( 0.0 );
	if( uShadow.a > 0.0 )
	{
		vec2 p = ( frag - uShadowOffset ) / uFieldSize;
		if( all( greaterThanEqual( p, vec2( 0.0 ) ) ) && all( lessThanEqual( p, vec2( 1.0 ) ) ) )
		{
			vec4 fs  = texture( uField, p );
			float w  = max( uShadowSoftness, uEdge );
			float ds = fs.r - outer - uShadowSpread;
			back     = premul( uShadow ) * ( 1.0 - smoothstep( -0.5 * w, 0.5 * w, ds ) ) * fs.g;
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

	// Knock the effects out under the text silhouette (Photoshop's default), so a
	// transparent fill shows what is behind the clip, not the shadow.
	back *= 1.0 - inside( d, outer ) * a;

	oColor = text + back * ( 1.0 - text.a );
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
		FFGLLog::LogToHost( ( std::string( "Kontur Text shader error: " ) + log ).c_str() );
		std::fprintf( stderr, "Kontur Text shader error: %s\n", log );
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
		FFGLLog::LogToHost( ( std::string( "Kontur Text link error: " ) + log ).c_str() );
		std::fprintf( stderr, "Kontur Text link error: %s\n", log );
		glDeleteProgram( p );
		return 0;
	}
	return p;
}

// Saves the GL state we touch and restores it on scope exit, so the host
// (Resolume) finds its context exactly as it left it.
struct StateGuard
{
	GLint drawFbo = 0, readFbo = 0, viewport[ 4 ]{}, program = 0, vao = 0, arrayBuffer = 0;
	GLint activeTex = 0, tex0 = 0;
	GLboolean blend = 0, depthTest = 0, scissor = 0, cull = 0, depthMask = 1;
	GLint depthFunc = GL_LESS;
	GLfloat clearColor[ 4 ]{};
	GLfloat clearDepth = 1;

	StateGuard()
	{
		glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo );
		glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &readFbo );
		glGetIntegerv( GL_VIEWPORT, viewport );
		glGetIntegerv( GL_CURRENT_PROGRAM, &program );
		glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &vao );
		glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &arrayBuffer );
		glGetIntegerv( GL_ACTIVE_TEXTURE, &activeTex );
		glActiveTexture( GL_TEXTURE0 );
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex0 );
		blend     = glIsEnabled( GL_BLEND );
		depthTest = glIsEnabled( GL_DEPTH_TEST );
		scissor   = glIsEnabled( GL_SCISSOR_TEST );
		cull      = glIsEnabled( GL_CULL_FACE );
		glGetBooleanv( GL_DEPTH_WRITEMASK, &depthMask );
		glGetIntegerv( GL_DEPTH_FUNC, &depthFunc );
		glGetFloatv( GL_COLOR_CLEAR_VALUE, clearColor );
		glGetFloatv( GL_DEPTH_CLEAR_VALUE, &clearDepth );
	}
	~StateGuard()
	{
		glBindFramebuffer( GL_DRAW_FRAMEBUFFER, (GLuint)drawFbo );
		glBindFramebuffer( GL_READ_FRAMEBUFFER, (GLuint)readFbo );
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
		glUseProgram( (GLuint)program );
		glBindVertexArray( (GLuint)vao );
		glBindBuffer( GL_ARRAY_BUFFER, (GLuint)arrayBuffer );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, (GLuint)tex0 );
		glActiveTexture( (GLenum)activeTex );
		blend ? glEnable( GL_BLEND ) : glDisable( GL_BLEND );
		depthTest ? glEnable( GL_DEPTH_TEST ) : glDisable( GL_DEPTH_TEST );
		scissor ? glEnable( GL_SCISSOR_TEST ) : glDisable( GL_SCISSOR_TEST );
		cull ? glEnable( GL_CULL_FACE ) : glDisable( GL_CULL_FACE );
		glDepthMask( depthMask );
		glDepthFunc( (GLenum)depthFunc );
		glClearColor( clearColor[ 0 ], clearColor[ 1 ], clearColor[ 2 ], clearColor[ 3 ] );
		glClearDepth( clearDepth );
	}
};

void setColor( GLuint prog, const char* name, const Color& c )
{
	glUniform4f( glGetUniformLocation( prog, name ), c.r, c.g, c.b, c.a );
}

}//namespace

// ---------------------------------------------------------------------------
float Style::reach() const
{
	float outer = outline1Position == OutlinePosition::Outside ? outline1Width : outline1Position == OutlinePosition::Center ? outline1Width * 0.5f : 0.0f;
	outer += outline2Width;
	float r = outer;
	if( glow.a > 0 && glowStrength > 0 )
		r = std::max( r, outer + glowRadius );
	if( shadow.a > 0 )
		r = std::max( r, outer + shadowSpread + shadowSoftness * 0.5f );
	if( outline1Position == OutlinePosition::Inside || outline1Position == OutlinePosition::Center )
		r = std::max( r, outline1Position == OutlinePosition::Inside ? outline1Width : outline1Width * 0.5f );// inner band needs reach too
	return r + edgeSoftness + 1.0f;
}

void Style::clampTo( float maxReachPx )
{
	float avail = std::max( 0.0f, maxReachPx - edgeSoftness - 1.0f );
	auto outerOf = [ & ] {
		return ( outline1Position == OutlinePosition::Outside ? outline1Width : outline1Position == OutlinePosition::Center ? outline1Width * 0.5f : 0.0f ) + outline2Width;
	};
	float outer = outerOf();
	if( outer > avail && outer > 0 )
	{
		float k = avail / outer;
		outline1Width *= k;
		outline2Width *= k;
		outer = outerOf();
	}
	if( outline1Position == OutlinePosition::Inside )
		outline1Width = std::min( outline1Width, avail );
	float rest   = std::max( 0.0f, avail - outer );
	glowRadius   = std::min( glowRadius, rest );
	shadowSpread = std::min( shadowSpread, rest );
	shadowSoftness = std::min( shadowSoftness, 2.0f * std::max( 0.0f, rest - shadowSpread ) );
}

// ---------------------------------------------------------------------------
bool TextRenderer::init()
{
	release();
	fieldProgram     = linkProgram( kFieldVS, kFieldFS );
	compositeProgram = linkProgram( kCompositeVS, kCompositeFS );
	if( !fieldProgram || !compositeProgram )
		return false;

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
	for( int i = 0; i < 4; ++i )
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
	if( fieldProgram )
		glDeleteProgram( fieldProgram );
	if( compositeProgram )
		glDeleteProgram( compositeProgram );
	if( glyphVao )
		glDeleteVertexArrays( 1, &glyphVao );
	if( emptyVao )
		glDeleteVertexArrays( 1, &emptyVao );
	if( cornerVbo )
		glDeleteBuffers( 1, &cornerVbo );
	if( instanceVbo )
		glDeleteBuffers( 1, &instanceVbo );
	if( fieldFbo )
		glDeleteFramebuffers( 1, &fieldFbo );
	if( fieldTex )
		glDeleteTextures( 1, &fieldTex );
	if( fieldDepth )
		glDeleteRenderbuffers( 1, &fieldDepth );
	fieldProgram = compositeProgram = glyphVao = emptyVao = cornerVbo = instanceVbo = 0;
	fieldFbo = fieldTex = fieldDepth = 0;
	fieldW = fieldH  = 0;
	instanceCapacity = 0;
}

bool TextRenderer::ensureFieldTarget( int w, int h )
{
	if( fieldFbo && w == fieldW && h == fieldH )
		return true;
	if( fieldFbo )
		glDeleteFramebuffers( 1, &fieldFbo );
	if( fieldTex )
		glDeleteTextures( 1, &fieldTex );
	if( fieldDepth )
		glDeleteRenderbuffers( 1, &fieldDepth );

	glGenTextures( 1, &fieldTex );
	glBindTexture( GL_TEXTURE_2D, fieldTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	glGenRenderbuffers( 1, &fieldDepth );
	glBindRenderbuffer( GL_RENDERBUFFER, fieldDepth );
	glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, w, h );
	glBindRenderbuffer( GL_RENDERBUFFER, 0 );

	glGenFramebuffers( 1, &fieldFbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fieldFbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fieldTex, 0 );
	glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fieldDepth );
	bool ok = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
	fieldW  = w;
	fieldH  = h;
	if( !ok )
		FFGLLog::LogToHost( "Kontur Text: field framebuffer incomplete" );
	return ok;
}

void TextRenderer::render( const FFGLViewportStruct& vp, GLuint hostFbo, const std::vector< GlyphInstance >& glyphs, const BlockTransform& t,
						   const Style& style, GLuint atlasTexture, int atlasSize )
{
	if( !isReady() || vp.width == 0 || vp.height == 0 )
		return;
	StateGuard guard;
	const int w = (int)vp.width, h = (int)vp.height;

	glDisable( GL_BLEND );
	glDisable( GL_SCISSOR_TEST );
	glDisable( GL_CULL_FACE );

	// ---- Pass 1: distance field of the union ----
	if( !ensureFieldTarget( w, h ) )
		return;
	glBindFramebuffer( GL_FRAMEBUFFER, fieldFbo );
	glViewport( 0, 0, w, h );
	glClearColor( 1.0e4f, 0.0f, 0.0f, 0.0f );// "far away", alpha 0
	glClearDepth( 1.0 );
	glDepthMask( GL_TRUE );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	if( !glyphs.empty() && atlasTexture )
	{
		glEnable( GL_DEPTH_TEST );
		glDepthFunc( GL_LESS );

		glBindVertexArray( glyphVao );
		glBindBuffer( GL_ARRAY_BUFFER, instanceVbo );
		if( glyphs.size() > instanceCapacity )
		{
			instanceCapacity = std::max( glyphs.size(), instanceCapacity * 2 );
			glBufferData( GL_ARRAY_BUFFER, instanceCapacity * sizeof( GlyphInstance ), nullptr, GL_DYNAMIC_DRAW );
		}
		glBufferSubData( GL_ARRAY_BUFFER, 0, glyphs.size() * sizeof( GlyphInstance ), glyphs.data() );

		glUseProgram( fieldProgram );
		glUniform2f( glGetUniformLocation( fieldProgram, "uViewport" ), (float)w, (float)h );
		glUniform2f( glGetUniformLocation( fieldProgram, "uOrigin" ), t.originX, t.originY );
		// Counter-clockwise on screen (y down) means a negative angle in the shader's frame.
		glUniform2f( glGetUniformLocation( fieldProgram, "uBlockRot" ), std::cos( -t.rotation ), std::sin( -t.rotation ) );
		glUniform1f( glGetUniformLocation( fieldProgram, "uSize" ), t.sizePx );
		glUniform2f( glGetUniformLocation( fieldProgram, "uAtlasSize" ), (float)atlasSize, (float)atlasSize );
		glUniform1f( glGetUniformLocation( fieldProgram, "uDepthRange" ), 16384.0f );
		glUniform1f( glGetUniformLocation( fieldProgram, "uSharpCorners" ), style.sharpCorners ? 1.0f : 0.0f );
		glUniform1i( glGetUniformLocation( fieldProgram, "uAtlas" ), 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, atlasTexture );

		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, (GLsizei)glyphs.size() );

		glDisable( GL_DEPTH_TEST );
	}

	// ---- Pass 2: composite into the host framebuffer ----
	glBindFramebuffer( GL_FRAMEBUFFER, hostFbo );
	glViewport( (GLint)vp.x, (GLint)vp.y, (GLsizei)vp.width, (GLsizei)vp.height );
	glUseProgram( compositeProgram );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, fieldTex );
	glUniform1i( glGetUniformLocation( compositeProgram, "uField" ), 0 );
	glUniform2f( glGetUniformLocation( compositeProgram, "uFieldSize" ), (float)w, (float)h );
	glUniform2f( glGetUniformLocation( compositeProgram, "uViewportOrigin" ), (float)vp.x, (float)vp.y );

	float w1 = style.outline1Width, outerEdge = 0, innerEdge = 0;
	switch( style.outline1Position )
	{
	case OutlinePosition::Outside:
		outerEdge = w1;
		innerEdge = 0;
		break;
	case OutlinePosition::Center:
		outerEdge = w1 * 0.5f;
		innerEdge = -w1 * 0.5f;
		break;
	case OutlinePosition::Inside:
		outerEdge = 0;
		innerEdge = -w1;
		break;
	}
	if( w1 <= 0 )
		outerEdge = innerEdge = 0;
	setColor( compositeProgram, "uFill", style.fill );
	setColor( compositeProgram, "uOutline1", style.outline1 );
	setColor( compositeProgram, "uOutline2", style.outline2 );
	glUniform3f( glGetUniformLocation( compositeProgram, "uOutline1Edges" ), w1, outerEdge, innerEdge );
	glUniform1f( glGetUniformLocation( compositeProgram, "uOutline2Width" ), style.outline2Width );
	setColor( compositeProgram, "uShadow", style.shadow );
	glUniform2f( glGetUniformLocation( compositeProgram, "uShadowOffset" ), style.shadowDx, -style.shadowDy );
	glUniform1f( glGetUniformLocation( compositeProgram, "uShadowSoftness" ), style.shadowSoftness );
	glUniform1f( glGetUniformLocation( compositeProgram, "uShadowSpread" ), style.shadowSpread );
	setColor( compositeProgram, "uGlow", style.glow );
	glUniform1f( glGetUniformLocation( compositeProgram, "uGlowRadius" ), style.glowRadius );
	glUniform1f( glGetUniformLocation( compositeProgram, "uGlowStrength" ), style.glowStrength );
	glUniform1f( glGetUniformLocation( compositeProgram, "uEdge" ), std::max( 0.25f, style.edgeSoftness ) );

	glBindVertexArray( emptyVao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
}

}//namespace kontur
