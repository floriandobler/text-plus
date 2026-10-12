#pragma once
// Atlas: GPU texture (RGBA16F) holding multi-channel + true distance fields
// (MTSDF) of shapes such as glyphs. Shelf packing; grows by doubling (old
// content is blitted over) up to 4096x4096. When even that is full the caller
// clears it.

#include "ffglkit/GL.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sdfkit
{

struct AtlasSlot
{
	float u0, v0, u1, v1;                              // texel coordinates of the bitmap
	float planeLeft, planeBottom, planeRight, planeTop;// shape units (em), relative to the origin, y up
	int level;
};

// A distance-field bitmap to upload.
struct FieldBitmapView
{
	int width = 0, height = 0;
	const uint16_t* rgba = nullptr;// half floats, 4 channels, rows bottom-up
	float planeLeft = 0, planeBottom = 0, planeRight = 0, planeTop = 0;// shape units, y up
	float unitPx = 1;              // bitmap pixels per shape unit
	int level    = 0;              // caller's tag (spread level)
};

class Atlas
{
public:
	bool init( int initialSize = 512 );
	void release();

	const AtlasSlot* find( uint64_t key ) const;
	// Uploads the bitmap. Returns nullptr when the atlas is full at maximum size.
	const AtlasSlot* add( uint64_t key, const FieldBitmapView& bitmap );
	void clear();

	GLuint texture() const { return tex; }
	int size() const { return texSize; }
	size_t count() const { return slots.size(); }

	static constexpr int kMaxSize = 4096;

private:
	bool allocate( int w, int h, int& outX, int& outY );
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

}//namespace sdfkit
