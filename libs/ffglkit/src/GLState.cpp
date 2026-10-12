#include "ffglkit/GLState.h"

#include <cstdio>
#include <string>

#include <ffgl/FFGLLog.h>

#if defined( __APPLE__ )
#include <OpenGL/OpenGL.h>// CGLGetCurrentContext
#elif defined( _WIN32 )
#include <windows.h>
#endif

namespace ffglkit
{

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

StateGuard::StateGuard()
{
	GLint maxUnits = 8;
	glGetIntegerv( GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxUnits );
	units = std::min( kTextureUnits, std::max( 1, (int)maxUnits ) );
	glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo );
	glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &readFbo );
	glGetIntegerv( GL_VIEWPORT, viewport );
	glGetIntegerv( GL_SCISSOR_BOX, scissorBox );
	glGetIntegerv( GL_CURRENT_PROGRAM, &program );
	glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &vao );
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &arrayBuffer );
	glGetIntegerv( GL_ACTIVE_TEXTURE, &activeTex );
	for( int i = 0; i < units; ++i )
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

StateGuard::~StateGuard()
{
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, (GLuint)drawFbo );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, (GLuint)readFbo );
	glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
	glScissor( scissorBox[ 0 ], scissorBox[ 1 ], scissorBox[ 2 ], scissorBox[ 3 ] );
	glUseProgram( (GLuint)program );
	glBindVertexArray( (GLuint)vao );
	glBindBuffer( GL_ARRAY_BUFFER, (GLuint)arrayBuffer );
	for( int i = units - 1; i >= 0; --i )
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

namespace
{
GLuint compileStage( GLenum type, const char* src, const char* prefix )
{
	GLuint s = glCreateShader( type );
	glShaderSource( s, 1, &src, nullptr );
	glCompileShader( s );
	GLint ok = 0;
	glGetShaderiv( s, GL_COMPILE_STATUS, &ok );
	if( !ok )
	{
		char log[ 4096 ];
		glGetShaderInfoLog( s, sizeof( log ), nullptr, log );
		FFGLLog::LogToHost( ( std::string( prefix ) + " shader error: " + log ).c_str() );
		std::fprintf( stderr, "%s shader error: %s\n", prefix, log );
		glDeleteShader( s );
		return 0;
	}
	return s;
}
}//namespace

GLuint linkProgram( const char* vs, const char* fs, const char* prefix )
{
	GLuint v = compileStage( GL_VERTEX_SHADER, vs, prefix );
	GLuint f = compileStage( GL_FRAGMENT_SHADER, fs, prefix );
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
		char log[ 4096 ];
		glGetProgramInfoLog( p, sizeof( log ), nullptr, log );
		FFGLLog::LogToHost( ( std::string( prefix ) + " link error: " + log ).c_str() );
		std::fprintf( stderr, "%s link error: %s\n", prefix, log );
		glDeleteProgram( p );
		return 0;
	}
	return p;
}

}//namespace ffglkit
