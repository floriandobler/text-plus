// Headless FFGL host for Linux (EGL + Mesa) and macOS (CGL). Loads the plugin
// exactly like a host does (dlopen + plugMain), renders frames into an FBO and
// writes a PNG.
//
//   ffgl_test_host <plugin.so | TextPlus.bundle> --list
//   ffgl_test_host <plugin.so> <out.png> <width> <height> [Param=value ...] [@frames=N] [@time=seconds] [@bg=checker|black|white]
//
// Param names are the plugin's parameter names ("Outline Width=24").
// Option parameters take the element index, text parameters the string.
// It also checks that the plugin leaves the GL state the way it found it.

#if defined( __APPLE__ )
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#else
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glew.h>
#endif
#include <dlfcn.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <ffgl/FFGL.h>
#include "stb_image_write.h"

typedef FFMixed ( *PlugMainFn )( FFUInt32, FFMixed, FFInstanceID );
static PlugMainFn g_plugMain;

static FFMixed call( FFUInt32 code, FFMixed in, FFInstanceID inst = nullptr ) { return g_plugMain( code, in, inst ); }
static FFMixed u( FFUInt32 v )
{
	FFMixed m;
	m.UIntValue = v;
	return m;
}
static FFMixed ptr( void* p )
{
	FFMixed m;
	m.PointerValue = p;
	return m;
}

#if defined( __APPLE__ )
static bool createContext()
{
	// Same profile as Resolume: OpenGL 4.1 core. Fall back to Apple's software
	// renderer where there is no GPU (CI machines).
	CGLPixelFormatAttribute hw[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core, kCGLPFAAllowOfflineRenderers,
									 (CGLPixelFormatAttribute)0 };
	CGLPixelFormatAttribute sw[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core, kCGLPFARendererID,
									 (CGLPixelFormatAttribute)kCGLRendererGenericFloatID, (CGLPixelFormatAttribute)0 };
	CGLPixelFormatObj pf = nullptr;
	GLint n              = 0;
	if( CGLChoosePixelFormat( hw, &pf, &n ) != kCGLNoError || !pf )
		if( CGLChoosePixelFormat( sw, &pf, &n ) != kCGLNoError || !pf )
			return false;
	CGLContextObj ctx = nullptr;
	if( CGLCreateContext( pf, nullptr, &ctx ) != kCGLNoError || !ctx )
		return false;
	CGLDestroyPixelFormat( pf );
	if( CGLSetCurrentContext( ctx ) != kCGLNoError )
		return false;
	std::printf( "GL: %s / %s\n", (const char*)glGetString( GL_RENDERER ), (const char*)glGetString( GL_VERSION ) );
	return true;
}
#else
static bool createContext()
{
	auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress( "eglGetPlatformDisplayEXT" );
	EGLDisplay dpy          = getPlatformDisplay ? getPlatformDisplay( EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr ) : eglGetDisplay( EGL_DEFAULT_DISPLAY );
	if( !eglInitialize( dpy, nullptr, nullptr ) )
		return false;
	EGLint cfgAttrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
	EGLConfig cfg;
	EGLint n = 0;
	eglChooseConfig( dpy, cfgAttrs, &cfg, 1, &n );
	eglBindAPI( EGL_OPENGL_API );
	// Same profile as Resolume on macOS: 4.1 core.
	EGLint ctxAttrs[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 1, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
	EGLContext ctx    = eglCreateContext( dpy, n ? cfg : nullptr, EGL_NO_CONTEXT, ctxAttrs );
	if( ctx == EGL_NO_CONTEXT || !eglMakeCurrent( dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx ) )
		return false;
	glewExperimental = GL_TRUE;
	glewInit();// may report "no GLX display" but loads the core entry points
	glGetError();
	return glGenFramebuffers != nullptr;
}
#endif

struct GLSnapshot
{
	GLint fbo, program, vao, buffer, activeTex, tex0, viewport[ 4 ];
	GLboolean blend, depth, scissor;
	static GLSnapshot take()
	{
		GLSnapshot s{};
		glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &s.fbo );
		glGetIntegerv( GL_CURRENT_PROGRAM, &s.program );
		glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &s.vao );
		glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &s.buffer );
		glGetIntegerv( GL_ACTIVE_TEXTURE, &s.activeTex );
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &s.tex0 );
		glGetIntegerv( GL_VIEWPORT, s.viewport );
		s.blend   = glIsEnabled( GL_BLEND );
		s.depth   = glIsEnabled( GL_DEPTH_TEST );
		s.scissor = glIsEnabled( GL_SCISSOR_TEST );
		return s;
	}
	bool operator==( const GLSnapshot& o ) const { return std::memcmp( this, &o, sizeof( *this ) ) == 0; }
};

int main( int argc, char** argv )
{
	if( argc < 3 )
	{
		std::fprintf( stderr, "usage: %s plugin.so (--list | out.png w h [Param=value ...])\n", argv[ 0 ] );
		return 2;
	}
	if( !createContext() )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}
	std::string pluginPath = argv[ 1 ];
	if( pluginPath.size() > 7 && pluginPath.compare( pluginPath.size() - 7, 7, ".bundle" ) == 0 )
		pluginPath += "/Contents/MacOS/TextPlus";// macOS bundle: load its executable
	void* lib = dlopen( pluginPath.c_str(), RTLD_NOW | RTLD_LOCAL );
	if( !lib )
	{
		std::fprintf( stderr, "dlopen: %s\n", dlerror() );
		return 1;
	}
	g_plugMain = (PlugMainFn)dlsym( lib, "plugMain" );
	if( !g_plugMain || call( FF_INITIALISE_V2, u( 0 ) ).UIntValue != FF_SUCCESS )
	{
		std::fprintf( stderr, "plugin init failed\n" );
		return 1;
	}

	// Parameter table
	const unsigned numParams = call( FF_GET_NUM_PARAMETERS, u( 0 ) ).UIntValue;
	std::map< std::string, unsigned > byName;
	std::vector< unsigned > types( numParams );
	for( unsigned i = 0; i < numParams; ++i )
	{
		byName[ (const char*)call( FF_GET_PARAMETER_NAME, u( i ) ).PointerValue ] = i;
		types[ i ] = call( FF_GET_PARAMETER_TYPE, u( i ) ).UIntValue;
	}

	if( std::string( argv[ 2 ] ) == "--list" )
	{
		for( unsigned i = 0; i < numParams; ++i )
		{
			const char* name = (const char*)call( FF_GET_PARAMETER_NAME, u( i ) ).PointerValue;
			char group[ 256 ] = "";
			GetStringStruct gs{ i, { group, (FFUInt32)sizeof( group ) - 1 } };
			call( FF_GET_PARAM_GROUP, ptr( &gs ) );
			FFMixed def      = call( FF_GET_PARAMETER_DEFAULT, u( i ) );
			GetRangeStruct rs{ i, { 0, 1 } };
			call( FF_GET_RANGE, ptr( &rs ) );
			std::printf( "%2u  %-12s %-24s type %-4u", i, group, name, types[ i ] );
			if( types[ i ] == FF_TYPE_TEXT || types[ i ] == FF_TYPE_FILE )
				std::printf( " default \"%s\"\n", def.PointerValue ? (const char*)def.PointerValue : "" );
			else
				std::printf( " default %-8g range [%g, %g]\n", *(float*)&def.UIntValue, rs.range.min, rs.range.max );
		}
		return 0;
	}

	if( argc < 5 )
		return 2;
	const char* outPath = argv[ 2 ];
	const int W = std::atoi( argv[ 3 ] ), H = std::atoi( argv[ 4 ] );
	int frames      = 40;
	int extraInstances = 0;
	for( int a = 5; a < argc; ++a )
		if( std::strncmp( argv[ a ], "@instances=", 11 ) == 0 )
			extraInstances = std::atoi( argv[ a ] + 11 ) - 1;
	double timeSec  = 0.0;
	std::vector< std::string > printParams;// "@print=A1 Mode,A1 Preset"
	double dt       = 0.0;// seconds per frame; > 0 plays time forward and renders exactly @frames frames
	std::string bg  = "checker";
	bool straight   = true;// Text+ outputs straight alpha by default (Resolume's convention)
	// "@resend=stale|sync": like a host that sends all its parameter values every frame.
	// stale = the host ignores value events from the plugin, sync = it reads them back first.
	bool expectInk = false;// "@expectInk": fail (exit 4) if the output is completely transparent
	std::string resend;
	int deferAt = 8;// frame at which "+Param=value" is applied ("@deferAt=N"); "++" one frame later
	std::vector< std::string > resendOnly;// "@resend=stale:A1 Preset;A1 Mode" limits the re-sent parameters
	// "@ramp=Param:from:to": animates a parameter linearly over all frames (host-side animation); repeatable
	struct Ramp
	{
		std::string name;
		float from, to;
	};
	std::vector< Ramp > ramps;
	// "@dump=dir": writes every frame as dir/fNNN.png
	std::string dumpDir;
	std::vector< std::string > tracePrint;// "@trace=A1 Spread,...": print values every frame
	std::map< unsigned, float > hostModel;

	FFGLViewportStruct vp{ 0, 0, (GLuint)W, (GLuint)H };
	auto tInst        = std::chrono::steady_clock::now();
	FFInstanceID inst = (FFInstanceID)call( FF_INSTANTIATE_GL, ptr( &vp ) ).PointerValue;
	const double instantiateMs = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - tInst ).count();
	// "@instances=N": N-1 more instances with different text, rendered (into a
	// scratch framebuffer) before the main instance every frame. They share the
	// GL programs and scratch targets, so the main output must not change.
	std::vector< FFInstanceID > others;
	for( int k = 0; k < extraInstances; ++k )
	{
		auto t = std::chrono::steady_clock::now();
		others.push_back( (FFInstanceID)call( FF_INSTANTIATE_GL, ptr( &vp ) ).PointerValue );
		std::printf( "  extra instance %d: %.2f ms\n", k + 1, std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t ).count() );
	}
	if( !inst || inst == (FFInstanceID)(uintptr_t)FF_FAIL )
	{
		std::fprintf( stderr, "instantiate failed\n" );
		return 1;
	}

	// "Name=value": the first '=' after which the name is a known parameter
	// (names like "| = New Line" contain '=' themselves).
	auto splitArg = [ & ]( const std::string& a ) -> size_t {
		for( size_t eq = a.find( '=' ); eq != std::string::npos; eq = a.find( '=', eq + 1 ) )
			if( byName.count( a.substr( 0, eq ) ) )
				return eq;
		return a.find( '=' );
	};
	std::vector< std::string > keep;// text params must outlive the call
	keep.reserve( 256 );
	std::vector< std::string > deferred;// "+Param=value": applied after the first frames (user interaction)
	std::vector< std::string > deferred2;// "++Param=value": applied one frame later (a second user action)
	auto setParam = [ & ]( const std::string& key, const std::string& val ) -> bool {
		auto it = byName.find( key );
		if( it == byName.end() )
		{
			std::fprintf( stderr, "unknown parameter '%s'\n", key.c_str() );
			return false;
		}
		SetParameterStruct sp{};
		sp.ParameterNumber = it->second;
		if( types[ it->second ] == FF_TYPE_TEXT || types[ it->second ] == FF_TYPE_FILE )
		{
			keep.push_back( val );
			sp.NewParameterValue.PointerValue = (void*)keep.back().c_str();
		}
		else
		{
			float f = (float)std::atof( val.c_str() );
			std::memcpy( &sp.NewParameterValue.UIntValue, &f, 4 );
			hostModel[ it->second ] = f;
		}
		call( FF_SET_PARAMETER, ptr( &sp ), inst );
		return true;
	};
	for( int a = 5; a < argc; ++a )
	{
		std::string arg = argv[ a ];
		// A parameter whose own name starts with '+' ("+ Line") is set directly.
		const bool literalPlus = !arg.empty() && arg[ 0 ] == '+' && byName.count( arg.substr( 0, splitArg( arg ) ) );
		if( !literalPlus && arg.size() > 1 && arg[ 0 ] == '+' && arg[ 1 ] == '+' )
		{
			deferred2.push_back( arg.substr( 2 ) );
			continue;
		}
		if( !literalPlus && !arg.empty() && arg[ 0 ] == '+' )
		{
			deferred.push_back( arg.substr( 1 ) );
			continue;
		}
		size_t eq       = arg[ 0 ] == '@' ? arg.find( '=' ) : splitArg( arg );
		if( eq == std::string::npos )
			continue;
		std::string key = arg.substr( 0, eq ), val = arg.substr( eq + 1 );
		if( key == "@instances" )
			continue;
		if( key == "@expectInk" )
		{
			expectInk = true;
			continue;
		}
		if( key == "@deferAt" )
		{
			deferAt = std::atoi( val.c_str() );
			continue;
		}
		if( key == "@resend" )
		{
			size_t colon = val.find( ':' );
			resend       = val.substr( 0, colon );
			while( colon != std::string::npos )
			{
				size_t next = val.find( ';', colon + 1 );
				resendOnly.push_back( val.substr( colon + 1, next == std::string::npos ? std::string::npos : next - colon - 1 ) );
				colon = next;
			}
			continue;
		}
		if( key == "@dump" )
		{
			dumpDir = val;
			continue;
		}
		if( key == "@trace" )
		{
			size_t start = 0;
			while( true )
			{
				size_t comma = val.find( ',', start );
				tracePrint.push_back( val.substr( start, comma == std::string::npos ? std::string::npos : comma - start ) );
				if( comma == std::string::npos )
					break;
				start = comma + 1;
			}
			continue;
		}
		if( key == "@ramp" )
		{
			size_t c1 = val.find( ':' ), c2 = val.rfind( ':' );
			ramps.push_back( Ramp{ val.substr( 0, c1 ), (float)std::atof( val.substr( c1 + 1, c2 - c1 - 1 ).c_str() ), (float)std::atof( val.substr( c2 + 1 ).c_str() ) } );
			continue;
		}
		if( key == "@frames" )
		{
			frames = std::atoi( val.c_str() );
			continue;
		}
		if( key == "@print" )
		{
			size_t start = 0;
			while( start <= val.size() )
			{
				size_t comma = val.find( ',', start );
				printParams.push_back( val.substr( start, comma == std::string::npos ? std::string::npos : comma - start ) );
				if( comma == std::string::npos )
					break;
				start = comma + 1;
			}
			continue;
		}
		if( key == "@dt" )
		{
			dt = std::atof( val.c_str() );
			continue;
		}
		if( key == "@time" )
		{
			timeSec = std::atof( val.c_str() );
			continue;
		}
		if( key == "@bg" )
		{
			bg = val;
			continue;
		}
		if( key == "@alpha" )
		{
			straight = val != "premul";
			continue;
		}
		if( !setParam( key, val ) )
			return 1;
	}

	// Host framebuffer
	GLuint tex, fbo;
	glGenTextures( 1, &tex );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0 );
	glViewport( 0, 0, W, H );
	GLuint hostVao;
	glGenVertexArrays( 1, &hostVao );
	glBindVertexArray( hostVao );

	std::vector< unsigned char > pixels( (size_t)W * H * 4 ), previous;
	int stable = 0, stateErrors = 0;
	double frameMs = 0, firstFrameMs = -1;
	for( int f = 0; f < frames; ++f )
	{
		double t = timeSec + f * dt;
		call( FF_SET_TIME, ptr( &t ), inst );
		SetBeatinfoStruct beat{ 120.0f, (float)std::fmod( t * 0.5, 1.0 ) };
		call( FF_SET_BEATINFO, ptr( &beat ), inst );

		for( const Ramp& r : ramps )
			if( f >= 10 )
			{
				const float k = frames > 11 ? float( f - 10 ) / float( frames - 11 ) : 1.0f;
				char v[ 32 ];
				std::snprintf( v, sizeof( v ), "%g", r.from + ( r.to - r.from ) * k );
				setParam( r.name, v );
			}
		if( !resend.empty() )
		{
			if( resend == "sync" )
			{
				ParamEventStruct evs[ 64 ];
				GetParamEventsStruct ge{ 64, evs };
				call( FF_GET_PARAMETER_EVENTS, ptr( &ge ), inst );
				for( unsigned e = 0; e < ge.numEvents; ++e )
					if( ( evs[ e ].eventFlags & FF_EVENT_FLAG_VALUE ) && hostModel.count( evs[ e ].ParameterNumber ) )
					{
						FFMixed v = call( FF_GET_PARAMETER, u( evs[ e ].ParameterNumber ), inst );
						std::memcpy( &hostModel[ evs[ e ].ParameterNumber ], &v.UIntValue, 4 );
					}
			}
			for( auto& [ idx, val ] : hostModel )
			{
				if( !resendOnly.empty() )
				{
					bool listed = false;
					for( const std::string& n : resendOnly )
						listed = listed || ( byName.count( n ) && byName[ n ] == idx );
					if( !listed )
						continue;
				}
				SetParameterStruct sp{};
				sp.ParameterNumber = idx;
				std::memcpy( &sp.NewParameterValue.UIntValue, &val, 4 );
				call( FF_SET_PARAMETER, ptr( &sp ), inst );
			}
		}

		glClearColor( 0, 0, 0, 0 );
		glClear( GL_COLOR_BUFFER_BIT );
		GLSnapshot before = GLSnapshot::take();
		for( size_t k = 0; k < others.size(); ++k )
		{
			static std::string otherText;
			otherText = "Other " + std::to_string( k ) + " WWWWWW|second line";
			SetParameterStruct sp{};
			sp.ParameterNumber = byName[ "Text" ];
			sp.NewParameterValue.PointerValue = (void*)otherText.c_str();
			call( FF_SET_PARAMETER, ptr( &sp ), others[ k ] );
			float glow = 40.0f;
			sp.ParameterNumber = byName[ "Glow Radius" ];
			std::memcpy( &sp.NewParameterValue.UIntValue, &glow, 4 );
			call( FF_SET_PARAMETER, ptr( &sp ), others[ k ] );
			static GLuint scratchTex = 0, scratchFbo = 0;
			if( !scratchFbo )
			{
				glGenTextures( 1, &scratchTex );
				glBindTexture( GL_TEXTURE_2D, scratchTex );
				glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
				glBindTexture( GL_TEXTURE_2D, 0 );
				glGenFramebuffers( 1, &scratchFbo );
				glBindFramebuffer( GL_FRAMEBUFFER, scratchFbo );
				glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scratchTex, 0 );
				glBindFramebuffer( GL_FRAMEBUFFER, fbo );
			}
			call( FF_SET_TIME, ptr( &t ), others[ k ] );
			ProcessOpenGLStruct opgl{ 0, nullptr, scratchFbo };
			glBindFramebuffer( GL_FRAMEBUFFER, scratchFbo );
			call( FF_PROCESS_OPENGL, ptr( &opgl ), others[ k ] );
			glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		}
		ProcessOpenGLStruct pgl{ 0, nullptr, fbo };
		auto t0 = std::chrono::steady_clock::now();
		call( FF_PROCESS_OPENGL, ptr( &pgl ), inst );
		glFinish();
		frameMs = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
		if( firstFrameMs < 0 )
			firstFrameMs = frameMs;
		GLSnapshot after = GLSnapshot::take();
		if( !( before == after ) )
			++stateErrors;
		GLenum err = glGetError();
		if( err != GL_NO_ERROR )
			std::fprintf( stderr, "GL error 0x%x in frame %d\n", err, f );

		if( f == deferAt && !deferred.empty() )
		{
			for( const std::string& d : deferred )
				setParam( d.substr( 0, splitArg( d ) ), d.substr( splitArg( d ) + 1 ) );
			deferred.clear();
			stable = 0;
		}
		if( f == deferAt + 1 && !deferred2.empty() )
		{
			for( const std::string& d : deferred2 )
				setParam( d.substr( 0, splitArg( d ) ), d.substr( splitArg( d ) + 1 ) );
			deferred2.clear();
			stable = 0;
		}
		glReadPixels( 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		if( !tracePrint.empty() )
		{
			std::printf( "  frame %3d:", f );
			for( const std::string& name : tracePrint )
			{
				auto it = byName.find( name );
				if( it == byName.end() )
					continue;
				FFMixed v = call( FF_GET_PARAMETER, u( it->second ), inst );
				std::printf( "  %s=%g", name.c_str(), *(float*)&v.UIntValue );
			}
			std::printf( "\n" );
		}
		if( !dumpDir.empty() && bg == "none" )
		{
			std::vector< unsigned char > rgba( (size_t)W * H * 4 );
			for( int y = 0; y < H; ++y )
				std::memcpy( &rgba[ (size_t)y * W * 4 ], &pixels[ (size_t)( H - 1 - y ) * W * 4 ], (size_t)W * 4 );
			char name[ 512 ];
			std::snprintf( name, sizeof( name ), "%s/f%03d.png", dumpDir.c_str(), f );
			stbi_write_png( name, W, H, 4, rgba.data(), W * 4 );
		}
		else if( !dumpDir.empty() )
		{
			std::vector< unsigned char > rgb( (size_t)W * H * 3 );
			for( int y = 0; y < H; ++y )
				for( int x = 0; x < W; ++x )
				{
					const unsigned char* px = &pixels[ ( (size_t)( H - 1 - y ) * W + x ) * 4 ];
					for( int c = 0; c < 3; ++c )
						rgb[ ( (size_t)y * W + x ) * 3 + c ] = (unsigned char)( px[ c ] * px[ 3 ] / 255 );
				}
			char name[ 512 ];
			std::snprintf( name, sizeof( name ), "%s/f%03d.png", dumpDir.c_str(), f );
			stbi_write_png( name, W, H, 3, rgb.data(), W * 3 );
		}
		if( dt <= 0.0 && !previous.empty() && previous == pixels && f > 10 )
		{
			if( ++stable >= 8 && f > 5 )
				break;
		}
		else
			stable = 0;
		previous = pixels;
		std::this_thread::sleep_for( std::chrono::milliseconds( dt > 0.0 ? 5 : 15 ) );
	}

	// "@bg=none": write the plugin's output as RGBA (straight alpha), top-down.
	if( bg == "none" )
	{
		std::vector< unsigned char > rgba( (size_t)W * H * 4 );
		for( int y = 0; y < H; ++y )
			std::memcpy( &rgba[ (size_t)y * W * 4 ], &pixels[ (size_t)( H - 1 - y ) * W * 4 ], (size_t)W * 4 );
		stbi_write_png( outPath, W, H, 4, rgba.data(), W * 4 );
	}
	// Composite the premultiplied output over a background for viewing; flip to top-down.
	std::vector< unsigned char > out( (size_t)W * H * 3 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const unsigned char* p = &pixels[ ( (size_t)( H - 1 - y ) * W + x ) * 4 ];
			float back;
			if( bg.find( ',' ) != std::string::npos )
				back = -1.0f;// solid colour, handled per channel below
			else if( bg == "black" )
				back = 0.0f;
			else if( bg == "white" )
				back = 1.0f;
			else
				back = ( ( x / 16 + y / 16 ) & 1 ) ? 0.36f : 0.28f;
			float a = p[ 3 ] / 255.0f;
			for( int c = 0; c < 3; ++c )
			{
				float bgc = back;
				if( back < 0.0f )
				{
					float rgb[ 3 ] = { 0, 0, 0 };
					std::sscanf( bg.c_str(), "%f,%f,%f", &rgb[ 0 ], &rgb[ 1 ], &rgb[ 2 ] );
					bgc = rgb[ c ];
				}
				float src = p[ c ] / 255.0f * ( straight ? a : 1.0f );
				float v   = src + bgc * ( 1.0f - a );// "over"
				out[ ( (size_t)y * W + x ) * 3 + c ] = (unsigned char)std::lround( std::fmin( 1.0f, v ) * 255.0f );
			}
		}
	if( bg != "none" )
		stbi_write_png( outPath, W, H, 3, out.data(), W * 3 );

	// Premultiplication sanity check: colour never exceeds alpha
	int premulViolations = 0;
	for( size_t i = 0; !straight && i < pixels.size(); i += 4 )
		for( int c = 0; c < 3; ++c )
			if( pixels[ i + c ] > pixels[ i + 3 ] + 1 )
				++premulViolations;

	// Report the font state as a host would see it
	{
		auto report = [ & ]( const char* name ) {
			unsigned idx  = byName[ name ];
			unsigned n    = call( FF_GET_NUM_PARAMETER_ELEMENTS, u( idx ), inst ).UIntValue;
			FFMixed v     = call( FF_GET_PARAMETER, u( idx ), inst );
			float value   = *(float*)&v.UIntValue;
			GetParameterElementNameStruct en{ idx, (FFUInt32)value };
			const char* elementName = (const char*)call( FF_GET_PARAMETER_ELEMENT_NAME, ptr( &en ), inst ).PointerValue;
			std::printf( "  %-7s %u entries, selected #%d \"%s\"\n", name, n, (int)value, elementName ? elementName : "?" );
		};
		FFMixed font = call( FF_GET_PARAMETER, u( byName[ "Font" ] ), inst );
		std::printf( "  Font    \"%s\"\n", (const char*)font.PointerValue );
		report( "Family" );
		report( "Style" );
		ParamEventStruct events[ 64 ];
		GetParamEventsStruct ge{ 64, events };
		call( FF_GET_PARAMETER_EVENTS, ptr( &ge ), inst );
		std::printf( "  pending host events: %u\n", ge.numEvents );
		for( const std::string& name : printParams )
		{
			auto it = byName.find( name );
			if( it == byName.end() )
				continue;
			FFMixed v = call( FF_GET_PARAMETER, u( it->second ), inst );
			char disp[ 128 ];
			const char* d = (const char*)call( FF_GET_PARAMETER_DISPLAY, u( it->second ), inst ).PointerValue;
			std::snprintf( disp, sizeof( disp ), "%s", d ? d : "" );
			std::printf( "  %-14s = %g  (%s)\n", name.c_str(), *(float*)&v.UIntValue, disp );
		}
	}

	for( FFInstanceID o : others )
		call( FF_DEINSTANTIATE_GL, u( 0 ), o );
	call( FF_DEINSTANTIATE_GL, u( 0 ), inst );
	call( FF_DEINITIALISE, u( 0 ) );
	std::printf( "%s  instantiate %.1f ms, first frame %.1f ms, last frame %.1f ms  state-leaks %d  premul-violations %d\n", outPath, instantiateMs,
				 firstFrameMs, frameMs, stateErrors, premulViolations );
	if( expectInk )
	{
		size_t ink = 0;
		for( size_t i = 3; i < pixels.size(); i += 4 )
			ink += pixels[ i ] > 0;
		std::printf( "  pixels with ink: %zu\n", ink );
		if( ink == 0 )
			return 4;
	}
	return stateErrors || premulViolations ? 3 : 0;
}
