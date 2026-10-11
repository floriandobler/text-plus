#pragma once
// GlyphAtlas: GPU texture (RGBA16F) holding MTSDF glyph bitmaps for one plugin
// instance. Shelf packing; grows by doubling (old content is blitted over)
// up to 4096x4096. When even that is full the caller clears it.

#include "GL.h"
#include "core/GlyphCache.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace textplus
{

struct AtlasSlot
{
	float u0, v0, u1, v1;// texel coordinates of the bitmap
	float planeLeft, planeBottom, planeRight, planeTop;// em, relative to glyph origin, y up
	int level;
};

class GlyphAtlas
{
public:
	bool init( int initialSize = 512 );
	void release();

	const AtlasSlot* find( uint64_t key ) const;
	// Uploads the bitmap. Returns nullptr when the atlas is full at maximum size.
	const AtlasSlot* add( uint64_t key, const GlyphBitmap& bitmap );
	void clear();

	GLuint texture() const { return tex; }
	int size() const { return texSize; }
	size_t glyphCount() const { return slots.size(); }

	static constexpr int kMaxSize = 4096;

private:
	bool allocate( int x, int y, int w, int h, int& outX, int& outY );
	bool grow();

	struct Shelf
	{
		int y, height, x;
	};
	GLuint tex  = 0;
	int texSize = 0;
	std::vector< Shelf > shelves;
	int nextShelfY = 0;
	std::unordered_map< uint64_t, AtlasSlot > slots;
};

uint64_t atlasKey( uint32_t faceId, uint32_t glyph, int level );

}//namespace textplus
