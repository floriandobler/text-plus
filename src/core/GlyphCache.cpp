#include "GlyphCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <msdfgen.h>

namespace kontur
{
namespace
{
const SpreadLevel kLevels[ kNumSpreadLevels ] = {
	{ 0.125f, 64, 8 },
	{ 0.25f, 64, 16 },
	{ 0.5f, 56, 28 },
	{ 1.0f, 48, 48 },
};

uint64_t makeKey( uint32_t faceId, uint32_t glyph, int level )
{
	return ( (uint64_t)( faceId & 0xFFFFFF ) << 40 ) | ( (uint64_t)( level & 0xFF ) << 32 ) | glyph;
}

// --- FreeType per thread ----------------------------------------------------
// FT_Library / FT_Face must not be shared between threads, so each thread keeps
// its own, created from the shared, immutable file bytes of the FontFace.
struct ThreadFreeType
{
	FT_Library lib = nullptr;
	struct Face
	{
		std::shared_ptr< FontFace > keepAlive;// FT_Face reads directly from the FontFace's bytes
		FT_Face face = nullptr;
	};
	std::map< uint32_t, Face > faces;

	ThreadFreeType() { FT_Init_FreeType( &lib ); }
	~ThreadFreeType()
	{
		for( auto& kv : faces )
			if( kv.second.face )
				FT_Done_Face( kv.second.face );
		if( lib )
			FT_Done_FreeType( lib );
	}

	FT_Face faceFor( const std::shared_ptr< FontFace >& f )
	{
		auto it = faces.find( f->id() );
		if( it != faces.end() )
			return it->second.face;
		// Release faces nobody else uses any more.
		if( faces.size() > 16 )
		{
			for( auto i = faces.begin(); i != faces.end(); )
			{
				if( i->second.keepAlive.use_count() == 1 )
				{
					if( i->second.face )
						FT_Done_Face( i->second.face );
					i = faces.erase( i );
				}
				else
				{
					++i;
				}
			}
		}
		Face entry;
		entry.keepAlive = f;
		const auto& bytes = f->data();
		if( FT_New_Memory_Face( lib, bytes.data(), (FT_Long)bytes.size(), f->ftFaceIndex(), &entry.face ) != 0 )
			entry.face = nullptr;
		if( entry.face )
			FT_Set_Char_Size( entry.face, 0, (FT_F26Dot6)f->unitsPerEm() * 64, 72, 72 );// 1 px == 1 font unit
		faces[ f->id() ] = entry;
		return entry.face;
	}
};

ThreadFreeType& threadFreeType()
{
	thread_local ThreadFreeType tft;
	return tft;
}

// --- FreeType outline -> msdfgen shape ---------------------------------------
struct OutlineContext
{
	double scale;
	msdfgen::Point2 position;
	msdfgen::Shape* shape;
	msdfgen::Contour* contour;
};
msdfgen::Point2 pt( const FT_Vector& v, double scale ) { return msdfgen::Point2( v.x * scale, v.y * scale ); }

int moveTo( const FT_Vector* to, void* user )
{
	auto* c = static_cast< OutlineContext* >( user );
	if( !( c->contour && c->contour->edges.empty() ) )
		c->contour = &c->shape->addContour();
	c->position = pt( *to, c->scale );
	return 0;
}
int lineTo( const FT_Vector* to, void* user )
{
	auto* c                 = static_cast< OutlineContext* >( user );
	msdfgen::Point2 end     = pt( *to, c->scale );
	if( end != c->position )
	{
		c->contour->addEdge( msdfgen::EdgeHolder( c->position, end ) );
		c->position = end;
	}
	return 0;
}
int conicTo( const FT_Vector* ctrl, const FT_Vector* to, void* user )
{
	auto* c             = static_cast< OutlineContext* >( user );
	msdfgen::Point2 end = pt( *to, c->scale );
	if( end != c->position )
	{
		c->contour->addEdge( msdfgen::EdgeHolder( c->position, pt( *ctrl, c->scale ), end ) );
		c->position = end;
	}
	return 0;
}
int cubicTo( const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user )
{
	auto* c             = static_cast< OutlineContext* >( user );
	msdfgen::Point2 end = pt( *to, c->scale );
	if( end != c->position || msdfgen::crossProduct( pt( *c1, c->scale ) - end, pt( *c2, c->scale ) - end ) )
	{
		c->contour->addEdge( msdfgen::EdgeHolder( c->position, pt( *c1, c->scale ), pt( *c2, c->scale ), end ) );
		c->position = end;
	}
	return 0;
}

}//namespace

const SpreadLevel& spreadLevel( int level )
{
	return kLevels[ std::clamp( level, 0, kNumSpreadLevels - 1 ) ];
}

int spreadLevelFor( float requiredEm )
{
	for( int i = 0; i < kNumSpreadLevels; ++i )
		if( kLevels[ i ].spreadEm >= requiredEm )
			return i;
	return kNumSpreadLevels - 1;
}

std::shared_ptr< GlyphBitmap > generateGlyphBitmap( const std::shared_ptr< FontFace >& facePtr, uint32_t glyph, int level )
{
	auto out          = std::make_shared< GlyphBitmap >();
	out->level        = level;
	const SpreadLevel& L = spreadLevel( level );

	const FontFace& face = *facePtr;
	FT_Face ft           = threadFreeType().faceFor( facePtr );
	if( !ft || FT_Load_Glyph( ft, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP ) != 0 || ft->glyph->format != FT_GLYPH_FORMAT_OUTLINE )
		return out;
	FT_Outline* outline = &ft->glyph->outline;
	if( outline->n_contours <= 0 )
		return out;

	// Outline is in 26.6 with 1 px == 1 font unit. Scale to atlas pixels.
	const double scale = (double)L.emPx / face.unitsPerEm() / 64.0;
	msdfgen::Shape shape;
	OutlineContext ctx{ scale, msdfgen::Point2(), &shape, nullptr };
	FT_Outline_Funcs funcs{};
	funcs.move_to  = moveTo;
	funcs.line_to  = lineTo;
	funcs.conic_to = conicTo;
	funcs.cubic_to = cubicTo;
	if( FT_Outline_Decompose( outline, &funcs, &ctx ) != 0 )
		return out;
	if( !shape.contours.empty() && shape.contours.back().edges.empty() )
		shape.contours.pop_back();
	if( shape.contours.empty() )
		return out;

	// TrueType fills clockwise contours, PostScript/CFF counter-clockwise.
	// msdfgen expects the TrueType convention.
	if( FT_Outline_Get_Orientation( outline ) == FT_ORIENTATION_POSTSCRIPT )
		for( auto& contour : shape.contours )
			contour.reverse();

	shape.normalize();
	msdfgen::edgeColoringInkTrap( shape, 3.0 );

	msdfgen::Shape::Bounds b = shape.getBounds();
	if( b.l > b.r || b.b > b.t )
		return out;
	const int x0 = (int)std::floor( b.l ) - L.spreadPx - 1;
	const int y0 = (int)std::floor( b.b ) - L.spreadPx - 1;
	const int x1 = (int)std::ceil( b.r ) + L.spreadPx + 1;
	const int y1 = (int)std::ceil( b.t ) + L.spreadPx + 1;
	const int w = x1 - x0, h = y1 - y0;
	if( w <= 0 || h <= 0 || w > 4096 || h > 4096 )
		return out;

	msdfgen::Bitmap< float, 4 > bmp( w, h, msdfgen::Y_UPWARD );
	msdfgen::Projection projection( msdfgen::Vector2( 1.0, 1.0 ), msdfgen::Vector2( -x0, -y0 ) );
	msdfgen::Range range( 2.0 * L.spreadPx );
	msdfgen::MSDFGeneratorConfig config;
	config.overlapSupport = true;
	msdfgen::generateMTSDF( bmp, shape, projection, range, config );
	// Robust sign for overlapping contours (common in variable fonts).
	msdfgen::distanceSignCorrection( bmp, shape, projection, 0.5f, msdfgen::FILL_NONZERO );

	out->width  = w;
	out->height = h;
	out->rgba.resize( (size_t)w * h * 4 );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			const float* px = bmp( x, y );
			uint16_t* dst   = &out->rgba[ ( (size_t)y * w + x ) * 4 ];
			for( int c = 0; c < 4; ++c )
				dst[ c ] = floatToHalf( px[ c ] );
		}
	out->planeLeft   = (float)x0 / L.emPx;
	out->planeBottom = (float)y0 / L.emPx;
	out->planeRight  = (float)x1 / L.emPx;
	out->planeTop    = (float)y1 / L.emPx;
	out->empty       = false;
	return out;
}

GlyphCache& GlyphCache::instance()
{
	static GlyphCache cache;
	return cache;
}

GlyphCache::GlyphCache()
{
	unsigned hw = std::thread::hardware_concurrency();
	int n       = (int)std::clamp( hw / 2u, 1u, 4u );
	for( int i = 0; i < n; ++i )
		workers.emplace_back( [ this ] { workerMain(); } );
}

GlyphCache::~GlyphCache()
{
	{
		std::lock_guard< std::mutex > lock( mutex );
		quit = true;
		queue.clear();
	}
	cv.notify_all();
	for( auto& t : workers )
		if( t.joinable() )
			t.join();
}

std::shared_ptr< const GlyphBitmap > GlyphCache::get( const std::shared_ptr< FontFace >& face, uint32_t glyph, int level )
{
	if( !face )
		return nullptr;
	const uint64_t key = makeKey( face->id(), glyph, level );
	std::lock_guard< std::mutex > lock( mutex );
	auto it = entries.find( key );
	if( it != entries.end() )
	{
		it->second.lastUse = ++useCounter;
		return it->second.bitmap;// may be null while pending
	}
	entries[ key ].lastUse = ++useCounter;
	queue.push_back( Job{ face, glyph, level, key } );
	cv.notify_one();
	return nullptr;
}

void GlyphCache::waitIdle()
{
	std::unique_lock< std::mutex > lock( mutex );
	idleCv.wait( lock, [ this ] { return queue.empty() && busy == 0; } );
}

void GlyphCache::workerMain()
{
	for( ;; )
	{
		Job job;
		{
			std::unique_lock< std::mutex > lock( mutex );
			cv.wait( lock, [ this ] { return quit || !queue.empty(); } );
			if( quit )
				return;
			job = std::move( queue.front() );
			queue.pop_front();
			++busy;
		}

		std::shared_ptr< GlyphBitmap > bmp = generateGlyphBitmap( job.face, job.glyph, job.level );

		{
			std::lock_guard< std::mutex > lock( mutex );
			auto it = entries.find( job.key );
			if( it != entries.end() )
			{
				it->second.bitmap = bmp;
				usedBytes += bmp->bytes();
				evictIfNeeded();
			}
			--busy;
			completed.fetch_add( 1 );
			if( queue.empty() && busy == 0 )
				idleCv.notify_all();
		}
	}
}

void GlyphCache::evictIfNeeded()
{
	if( usedBytes <= budget )
		return;
	std::vector< std::pair< uint64_t, uint64_t > > byAge;// lastUse, key
	byAge.reserve( entries.size() );
	for( auto& kv : entries )
		if( kv.second.bitmap )
			byAge.emplace_back( kv.second.lastUse, kv.first );
	std::sort( byAge.begin(), byAge.end() );
	const size_t target = budget * 3 / 4;
	for( auto& item : byAge )
	{
		if( usedBytes <= target )
			break;
		auto it = entries.find( item.second );
		usedBytes -= it->second.bitmap->bytes();
		entries.erase( it );
	}
}

// --- half floats ----------------------------------------------------------------
uint16_t floatToHalf( float f )
{
	uint32_t x;
	std::memcpy( &x, &f, 4 );
	uint32_t sign = ( x >> 16 ) & 0x8000;
	int32_t exp   = (int32_t)( ( x >> 23 ) & 0xFF ) - 127 + 15;
	uint32_t mant = x & 0x7FFFFF;
	if( ( ( x >> 23 ) & 0xFF ) == 0xFF )
		return (uint16_t)( sign | 0x7C00 | ( mant ? 0x200 : 0 ) );
	if( exp >= 31 )
		return (uint16_t)( sign | 0x7BFF );// clamp to max finite
	if( exp <= 0 )
	{
		if( exp < -10 )
			return (uint16_t)sign;
		mant = ( mant | 0x800000 ) >> ( 1 - exp );
		return (uint16_t)( sign | ( ( mant + 0x1000 ) >> 13 ) );
	}
	uint32_t h = sign | ( (uint32_t)exp << 10 ) | ( mant >> 13 );
	if( mant & 0x1000 )
		++h;// round
	return (uint16_t)h;
}

float halfToFloat( uint16_t h )
{
	uint32_t sign = ( h & 0x8000u ) << 16;
	uint32_t exp  = ( h >> 10 ) & 0x1F;
	uint32_t mant = h & 0x3FF;
	uint32_t x;
	if( exp == 0 )
	{
		if( mant == 0 )
			x = sign;
		else
		{
			exp = 127 - 15 + 1;
			while( !( mant & 0x400 ) )
			{
				mant <<= 1;
				--exp;
			}
			mant &= 0x3FF;
			x = sign | ( exp << 23 ) | ( mant << 13 );
		}
	}
	else if( exp == 31 )
		x = sign | 0x7F800000 | ( mant << 13 );
	else
		x = sign | ( ( exp - 15 + 127 ) << 23 ) | ( mant << 13 );
	float f;
	std::memcpy( &f, &x, 4 );
	return f;
}

}//namespace kontur
