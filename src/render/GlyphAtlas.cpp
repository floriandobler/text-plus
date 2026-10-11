#include "GlyphAtlas.h"

#include <algorithm>

namespace textplus
{
namespace
{
constexpr int kPadding = 1;// gap between glyphs so bilinear filtering never mixes neighbours

struct SavedUnpackState
{
	GLint alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0;
	GLint unpackBuffer = 0;
	SavedUnpackState()
	{
		glGetIntegerv( GL_UNPACK_ALIGNMENT, &alignment );
		glGetIntegerv( GL_UNPACK_ROW_LENGTH, &rowLength );
		glGetIntegerv( GL_UNPACK_SKIP_ROWS, &skipRows );
		glGetIntegerv( GL_UNPACK_SKIP_PIXELS, &skipPixels );
		glGetIntegerv( GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer );
		glBindBuffer( GL_PIXEL_UNPACK_BUFFER, 0 );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
		glPixelStorei( GL_UNPACK_ROW_LENGTH, 0 );
		glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );
		glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
	}
	~SavedUnpackState()
	{
		glPixelStorei( GL_UNPACK_ALIGNMENT, alignment );
		glPixelStorei( GL_UNPACK_ROW_LENGTH, rowLength );
		glPixelStorei( GL_UNPACK_SKIP_ROWS, skipRows );
		glPixelStorei( GL_UNPACK_SKIP_PIXELS, skipPixels );
		glBindBuffer( GL_PIXEL_UNPACK_BUFFER, (GLuint)unpackBuffer );
	}
};

struct SavedTextureBinding
{
	GLint active = 0, bound = 0;
	SavedTextureBinding()
	{
		glGetIntegerv( GL_ACTIVE_TEXTURE, &active );
		glActiveTexture( GL_TEXTURE0 );
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
	}
	~SavedTextureBinding()
	{
		glBindTexture( GL_TEXTURE_2D, (GLuint)bound );
		glActiveTexture( (GLenum)active );
	}
};

GLuint createTexture( int size )
{
	GLuint t = 0;
	glGenTextures( 1, &t );
	glBindTexture( GL_TEXTURE_2D, t );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, size, size, 0, GL_RGBA, GL_HALF_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	return t;
}
}//namespace

uint64_t atlasKey( uint32_t faceId, uint32_t glyph, int level )
{
	return ( (uint64_t)( faceId & 0xFFFFFF ) << 40 ) | ( (uint64_t)( level & 0xFF ) << 32 ) | glyph;
}

bool GlyphAtlas::init( int initialSize )
{
	release();
	SavedTextureBinding saved;
	texSize = initialSize;
	tex     = createTexture( texSize );
	clear();
	return tex != 0;
}

void GlyphAtlas::release()
{
	if( tex )
		glDeleteTextures( 1, &tex );
	tex     = 0;
	texSize = 0;
	slots.clear();
	shelves.clear();
	nextShelfY = 0;
}

void GlyphAtlas::clear()
{
	slots.clear();
	shelves.clear();
	nextShelfY = 0;
}

const AtlasSlot* GlyphAtlas::find( uint64_t key ) const
{
	auto it = slots.find( key );
	return it == slots.end() ? nullptr : &it->second;
}

bool GlyphAtlas::allocate( int, int, int w, int h, int& outX, int& outY )
{
	const int pw = w + kPadding, ph = h + kPadding;
	// Best-fitting existing shelf
	Shelf* best = nullptr;
	for( Shelf& s : shelves )
		if( s.height >= ph && s.x + pw <= texSize && ( !best || s.height < best->height ) )
			best = &s;
	if( !best )
	{
		if( nextShelfY + ph > texSize || pw > texSize )
			return false;
		shelves.push_back( Shelf{ nextShelfY, ph, 0 } );
		nextShelfY += ph;
		best = &shelves.back();
	}
	outX = best->x;
	outY = best->y;
	best->x += pw;
	return true;
}

bool GlyphAtlas::grow()
{
	if( texSize >= kMaxSize )
		return false;
	const int newSize = texSize * 2;
	GLuint newTex     = createTexture( newSize );

	// Copy the old content into the lower-left corner; texel coordinates stay valid.
	GLint prevRead = 0, prevDraw = 0;
	glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &prevRead );
	glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw );
	GLuint fbos[ 2 ];
	glGenFramebuffers( 2, fbos );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, fbos[ 0 ] );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0 );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, fbos[ 1 ] );
	glFramebufferTexture2D( GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, newTex, 0 );
	glBlitFramebuffer( 0, 0, texSize, texSize, 0, 0, texSize, texSize, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, (GLuint)prevRead );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, (GLuint)prevDraw );
	glDeleteFramebuffers( 2, fbos );

	glDeleteTextures( 1, &tex );
	tex     = newTex;
	texSize = newSize;
	return true;
}

const AtlasSlot* GlyphAtlas::add( uint64_t key, const GlyphBitmap& bmp )
{
	if( !tex || bmp.empty || bmp.width <= 0 || bmp.height <= 0 )
		return nullptr;
	if( auto existing = find( key ) )
		return existing;

	SavedTextureBinding savedTex;
	int x = 0, y = 0;
	while( !allocate( 0, 0, bmp.width, bmp.height, x, y ) )
	{
		glBindTexture( GL_TEXTURE_2D, tex );
		if( !grow() )
			return nullptr;
	}

	{
		SavedUnpackState unpack;
		glBindTexture( GL_TEXTURE_2D, tex );
		glTexSubImage2D( GL_TEXTURE_2D, 0, x, y, bmp.width, bmp.height, GL_RGBA, GL_HALF_FLOAT, bmp.rgba.data() );
	}

	// The quad spans the bitmap from texel centre to texel centre, so linear
	// filtering never reads outside the glyph's own pixels.
	const SpreadLevel& L = spreadLevel( bmp.level );
	const float half     = 0.5f / L.emPx;
	AtlasSlot s;
	s.u0          = x + 0.5f;
	s.v0          = y + 0.5f;
	s.u1          = x + bmp.width - 0.5f;
	s.v1          = y + bmp.height - 0.5f;
	s.planeLeft   = bmp.planeLeft + half;
	s.planeBottom = bmp.planeBottom + half;
	s.planeRight  = bmp.planeRight - half;
	s.planeTop    = bmp.planeTop - half;
	s.level       = bmp.level;
	return &( slots[ key ] = s );
}

}//namespace textplus
