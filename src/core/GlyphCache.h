#pragma once
// GlyphCache: generates multi-channel + true signed distance fields (MTSDF)
// for glyphs on worker threads and keeps them in CPU memory, shared by all
// plugin instances. The render thread never waits for it: get() returns
// nullptr until the glyph is ready, and the glyph appears a frame later.
//
// Spread levels: the distance field only reaches a limited distance around the
// glyph. Thick outlines, glows and soft shadows need a wider reach, so glyphs
// exist in four spread levels (1/8, 1/4, 1/2, 1 em). The renderer picks the
// smallest level that covers its widest effect.

#include "FontFace.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace textplus
{

struct SpreadLevel
{
	float spreadEm;// distance reach in em on each side of the outline
	int emPx;      // atlas pixels per em
	int spreadPx;  // = spreadEm * emPx
};
constexpr int kNumSpreadLevels = 4;
const SpreadLevel& spreadLevel( int level );
// Smallest level whose reach covers `requiredEm`; the largest level if none does.
int spreadLevelFor( float requiredEm );

struct GlyphBitmap
{
	int width = 0, height = 0;
	std::vector< uint16_t > rgba;// half floats, 4 channels, rows bottom-up (y up)
	// Quad of the bitmap relative to the glyph origin, em units, y up.
	float planeLeft = 0, planeBottom = 0, planeRight = 0, planeTop = 0;
	int level   = 0;
	bool empty  = true;// glyph has no outline (space, control glyph)
	size_t bytes() const { return rgba.size() * sizeof( uint16_t ); }
};

class GlyphCache
{
public:
	static GlyphCache& instance();

	// Ready bitmap or nullptr (then generation is scheduled). Urgent requests
	// (glyphs on screen) are generated before background ones (prefetching).
	std::shared_ptr< const GlyphBitmap > get( const std::shared_ptr< FontFace >& face, uint32_t glyph, int level, bool urgent = true );
	// Increments whenever a glyph finishes. Lets renderers know when to retry.
	uint64_t generation() const { return completed.load(); }
	// Blocks until the queue is empty (tests only).
	void waitIdle();

	void setMemoryBudget( size_t bytes ) { budget = bytes; }

private:
	GlyphCache();
	~GlyphCache();

	struct Job
	{
		std::shared_ptr< FontFace > face;
		uint32_t glyph;
		int level;
		uint64_t key;
	};
	struct Entry
	{
		std::shared_ptr< const GlyphBitmap > bitmap;// null while pending
		uint64_t lastUse = 0;
	};

	void workerMain();
	void evictIfNeeded();// mutex held

	std::mutex mutex;
	std::condition_variable cv;
	std::condition_variable idleCv;
	std::deque< Job > urgentQueue, backgroundQueue;
	std::unordered_map< uint64_t, Entry > entries;
	std::vector< std::thread > workers;
	int busy             = 0;
	bool quit            = false;
	size_t usedBytes     = 0;
	size_t budget        = 256u << 20;
	uint64_t useCounter  = 0;
	std::atomic< uint64_t > completed{ 0 };
};

// Exposed for tests: synchronous generation.
std::shared_ptr< GlyphBitmap > generateGlyphBitmap( const std::shared_ptr< FontFace >& face, uint32_t glyph, int level );

// IEEE 754 half <-> float
uint16_t floatToHalf( float f );
float halfToFloat( uint16_t h );

}//namespace textplus
