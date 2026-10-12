#pragma once
// GL state helpers every FFGL plugin needs.
//
//  StateGuard        saves the GL state a renderer touches and restores it on
//                    scope exit, so the host finds its context exactly as it
//                    left it.
//  currentContextKey identifies the current GL context, so programs and
//                    scratch targets can be shared by all instances in one
//                    context (see PerContext).

#include "GL.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <vector>

namespace ffglkit
{

void* currentContextKey();

struct StateGuard
{
	static constexpr int kTextureUnits = 16;

	StateGuard();
	~StateGuard();
	StateGuard( const StateGuard& )            = delete;
	StateGuard& operator=( const StateGuard& ) = delete;

private:
	GLint drawFbo = 0, readFbo = 0, viewport[ 4 ]{}, scissorBox[ 4 ]{}, program = 0, vao = 0, arrayBuffer = 0;
	GLint activeTex = 0, tex[ kTextureUnits ]{};
	GLboolean blend = 0, depthTest = 0, scissor = 0, cull = 0, depthMask = 1;
	GLint depthFunc = GL_LESS, blendSrcRGB = GL_ONE, blendDstRGB = GL_ZERO, blendSrcA = GL_ONE, blendDstA = GL_ZERO, blendEqRGB = GL_FUNC_ADD,
		  blendEqA = GL_FUNC_ADD;
	GLfloat clearColor[ 4 ]{};
	GLfloat clearDepth = 1;
	int units          = 8;
};

// Compiles and links a program; logs errors to the host. 0 on failure.
GLuint linkProgram( const char* vertexSource, const char* fragmentSource, const char* logPrefix );

// One T per GL context, reference counted by the instances that use it.
// T needs a member `void* context` and `void destroy()` (called with the context current).
template < typename T >
class PerContext
{
public:
	// Returns the object of the current context, creating it with `make` if needed (nullptr if make fails).
	T* acquire( const std::function< T*() >& make )
	{
		std::lock_guard< std::mutex > lock( mutex );
		void* ctx = currentContextKey();
		for( Item& it : items )
			if( it.object->context == ctx )
			{
				++it.refs;
				return it.object;
			}
		T* obj = make();
		if( !obj )
			return nullptr;
		obj->context = ctx;
		items.push_back( Item{ obj, 1 } );
		return obj;
	}
	void release( T* obj )
	{
		if( !obj )
			return;
		std::lock_guard< std::mutex > lock( mutex );
		for( size_t i = 0; i < items.size(); ++i )
			if( items[ i ].object == obj && --items[ i ].refs == 0 )
			{
				obj->destroy();
				delete obj;
				items.erase( items.begin() + (long)i );
				return;
			}
	}

private:
	struct Item
	{
		T* object;
		int refs;
	};
	std::mutex mutex;
	std::vector< Item > items;
};

}//namespace ffglkit
