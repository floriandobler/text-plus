// Unit tests for the platform-neutral core: font database, layout, glyph SDFs.
// Run: ./layout_test [extra-font-file ...]
#include "core/Animator.h"
#include "core/Csv.h"
#include "core/FontDatabase.h"
#include "core/FontFace.h"
#include "core/GlyphCache.h"
#include "core/TextLayout.h"

#include <cmath>
#include <cstdio>
#include <hb.h>

using namespace textplus;

static int g_failures = 0;
#define CHECK( cond, ... )                                   \
	do                                                       \
	{                                                        \
		if( !( cond ) )                                      \
		{                                                    \
			++g_failures;                                    \
			std::printf( "FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond ); \
			std::printf( __VA_ARGS__ );                      \
			std::printf( "\n" );                             \
		}                                                    \
	} while( 0 )

static float firstGlyphX( const LayoutResult& r, int line )
{
	for( const LaidGlyph& g : r.glyphs )
		if( g.lineIndex == line )
			return g.x;
	return NAN;
}

static float distAt( const GlyphBitmap& b, int x, int y, int level )
{
	const uint16_t* p = &b.rgba[ ( (size_t)y * b.width + x ) * 4 ];
	float r = halfToFloat( p[ 0 ] ), g = halfToFloat( p[ 1 ] ), bb = halfToFloat( p[ 2 ] );
	float med = std::max( std::min( r, g ), std::min( std::max( r, g ), bb ) );
	return ( med - 0.5f ) * 2.0f * spreadLevel( level ).spreadPx;// atlas px, inside positive
}

static void testGlyphSign( const std::shared_ptr< FontFace >& face, const char* label )
{
	// 'l' / 'I' : the centre of the bitmap lies inside the stem
	hb_codepoint_t gid = 0;
	CHECK( hb_font_get_nominal_glyph( face->hbFont(), 'I', &gid ), "%s: no glyph for I", label );
	for( int level = 0; level < kNumSpreadLevels; ++level )
	{
		auto bmp = generateGlyphBitmap( face, gid, level );
		CHECK( bmp && !bmp->empty, "%s: empty bitmap", label );
		if( !bmp || bmp->empty )
			return;
		float centre = distAt( *bmp, bmp->width / 2, bmp->height / 2, level );
		float corner = distAt( *bmp, 0, 0, level );
		CHECK( centre > 0, "%s level %d: centre distance %.2f should be inside (>0)", label, level, centre );
		CHECK( corner < 0, "%s level %d: corner distance %.2f should be outside (<0)", label, level, corner );
		// The bitmap border is at least `spreadPx` away from the outline.
		CHECK( corner <= -spreadLevel( level ).spreadPx * 0.95f, "%s level %d: corner %.2f not at full spread", label, level, corner );
	}
	std::printf( "ok   glyph sign: %s\n", label );
}

int main( int argc, char** argv )
{
	FontDatabase& db = FontDatabase::instance();
	CHECK( db.waitUntilReady( 60000 ), "font scan timed out" );
	std::printf( "info %zu families\n", db.families().size() );

	// --- resolve ---
	auto dejavu = db.resolve( "DejaVu Sans / Bold" );
	CHECK( dejavu && dejavu->family == "DejaVu Sans" && dejavu->style == "Bold", "resolve exact" );
	auto fuzzy = db.resolve( "dejavusans bold" );
	CHECK( fuzzy && fuzzy->family == "DejaVu Sans" && fuzzy->style == "Bold", "resolve without slash -> %s", fuzzy ? fuzzy->displayName().c_str() : "none" );
	auto partial = db.resolve( "cantar" );
	CHECK( partial && partial->family == "Cantarell" && partial->style == "Regular", "resolve partial -> %s", partial ? partial->displayName().c_str() : "none" );
	auto interBI = db.resolve( "Inter / italic bold" );
	CHECK( interBI && interBI->weight == 700 && interBI->italic, "resolve reordered style -> %s", interBI ? interBI->displayName().c_str() : "none" );
	bool matched = true;
	auto none    = db.resolve( "this font does not exist", &matched );
	CHECK( none.has_value() && !matched, "resolve falls back to a default and says so" );
	auto cond = db.resolve( "DejaVu Sans Condensed / Bold", &matched );
	CHECK( cond && matched && cond->family == "DejaVu Sans" && cond->style == "Condensed Bold", "style part in family -> %s", cond ? cond->displayName().c_str() : "none" );
	std::printf( "ok   resolve\n" );

	auto face = FontFace::get( *db.resolve( "DejaVu Sans / Book" ) );
	CHECK( face != nullptr, "load DejaVu Sans" );
	if( !face )
		return 1;

	// --- anchoring: left aligned text keeps its left edge when a line grows ---
	LayoutParams p;
	p.sizePx  = 100;
	p.align   = Align::Left;
	p.anchorH = AnchorH::Auto;
	p.anchorV = AnchorV::FirstBaseline;
	p.text    = "Hi\nthere";
	auto a    = layoutText( *face, p );
	p.text    = "Hi\nthere is a much longer second line";
	auto b    = layoutText( *face, p );
	CHECK( std::fabs( firstGlyphX( a, 0 ) - firstGlyphX( b, 0 ) ) < 0.01f, "left edge moved %.2f -> %.2f", firstGlyphX( a, 0 ), firstGlyphX( b, 0 ) );
	CHECK( std::fabs( a.glyphs[ 0 ].y ) < 0.01f, "first baseline anchor y=%.2f", a.glyphs[ 0 ].y );
	CHECK( a.numLines == 2, "two lines" );

	// right aligned: right edge stays
	p.align = Align::Right;
	p.text  = "short\nx";
	auto c  = layoutText( *face, p );
	CHECK( std::fabs( c.right ) < 0.01f, "right aligned block ends at anchor (%.2f)", c.right );

	// --- \n escape and pipe ---
	p.text = "a\\nb|c";
	CHECK( layoutText( *face, p ).numLines == 3, "escape + pipe -> 3 lines" );
	p.pipeIsNewline = false;
	CHECK( layoutText( *face, p ).numLines == 2, "pipe disabled -> 2 lines" );
	p.pipeIsNewline = true;

	// --- spaces scale with size ---
	p.align = Align::Left;
	p.text  = "a b";
	p.sizePx = 100;
	auto s1 = layoutText( *face, p );
	p.sizePx = 200;
	auto s2 = layoutText( *face, p );
	float gap1 = s1.glyphs[ 1 ].x - s1.glyphs[ 0 ].x, gap2 = s2.glyphs[ 1 ].x - s2.glyphs[ 0 ].x;
	CHECK( std::fabs( gap2 - 2 * gap1 ) < 0.05f, "space scales: %.2f vs %.2f", gap1, gap2 );

	// --- tracking (1/1000 em) ---
	p.sizePx           = 100;
	p.trackingPermille = 100;
	auto t1            = layoutText( *face, p );
	CHECK( std::fabs( ( t1.glyphs[ 1 ].x - t1.glyphs[ 0 ].x ) - ( gap1 + 2 * 10.0f ) ) < 0.05f, "tracking adds 10px per glyph" );
	p.trackingPermille = 0;

	// --- wrapping in a box ---
	p.text     = "one two three four five six seven";
	p.boxWidth = 400;
	auto w     = layoutText( *face, p );
	CHECK( w.numLines > 1, "wraps into %d lines", w.numLines );
	for( const LaidGlyph& g : w.glyphs )
		CHECK( g.x < 400.0f, "glyph beyond box at x=%.1f", g.x );
	CHECK( !w.overflow, "no overflow for normal words" );

	// --- fit width: widest line fills the box exactly ---
	p.fit      = FitMode::FitWidth;
	p.text     = "FIT";
	p.boxWidth = 900;
	auto f     = layoutText( *face, p );
	CHECK( f.sizePx > 100, "fit width scales up (%.1f)", f.sizePx );
	p.fit = FitMode::ShrinkToBox;
	p.text      = "Supercalifragilistic";
	p.boxWidth  = 300;
	auto sh     = layoutText( *face, p );
	CHECK( !sh.overflow && sh.numLines == 1 && sh.sizePx < 100, "shrink to box: size %.1f lines %d", sh.sizePx, sh.numLines );
	p.fit      = FitMode::Off;
	p.boxWidth = 0;

	// --- word/char indices ---
	p.text = "ab cd\nef";
	auto idx = layoutText( *face, p );
	CHECK( idx.numChars == 6 && idx.numWords == 3 && idx.numLines == 2, "indices chars %d words %d lines %d", idx.numChars, idx.numWords, idx.numLines );

	// --- kerning changes advances (AV) ---
	p.text    = "AV";
	p.kerning = true;
	float kOn = layoutText( *face, p ).glyphs[ 1 ].x;
	p.kerning = false;
	float kOff = layoutText( *face, p ).glyphs[ 1 ].x;
	CHECK( kOn < kOff, "kerning tightens AV (%.2f vs %.2f)", kOn, kOff );
	std::printf( "ok   layout\n" );

	// --- inline colour markup ---
	p.text   = "a{#ff0000}bc{/}d{{e";
	auto mk  = layoutText( *face, p );
	CHECK( mk.numChars == 6 && mk.palette.size() == 1, "markup stripped: chars %d palette %zu", mk.numChars, mk.palette.size() );
	CHECK( mk.glyphs[ 0 ].colorIndex == -1 && mk.glyphs[ 1 ].colorIndex == 0 && mk.glyphs[ 2 ].colorIndex == 0 && mk.glyphs[ 3 ].colorIndex == -1, "colour spans" );
	CHECK( std::fabs( mk.palette[ 0 ].r - 1.0f ) < 1e-6 && mk.palette[ 0 ].g == 0.0f, "palette colour" );
	p.markup = false;
	CHECK( layoutText( *face, p ).numChars > 6, "markup off keeps braces" );
	p.markup = true;
	std::printf( "ok   markup\n" );

	// --- animators: pure functions of progress / beats ---
	p.text   = "abcd";
	auto lay = layoutText( *face, p );
	std::vector< GlyphState > st;
	AnimatorParams tw;
	tw.mode     = AnimMode::SequenceIn;
	tw.spread   = 0;// typewriter
	tw.opacity  = 0;
	tw.progress = 0.5f;
	initGlyphStates( lay, st );
	applyAnimator( lay, tw, st );
	CHECK( st[ 0 ].alpha == 1 && st[ 1 ].alpha == 1 && st[ 2 ].alpha == 0 && st[ 3 ].alpha == 0, "typewriter half: %g %g %g %g", st[ 0 ].alpha, st[ 1 ].alpha, st[ 2 ].alpha, st[ 3 ].alpha );
	tw.progress = 1.0f;
	initGlyphStates( lay, st );
	applyAnimator( lay, tw, st );
	CHECK( st[ 3 ].alpha == 1, "typewriter full" );
	tw.progress = 0.0f;
	initGlyphStates( lay, st );
	applyAnimator( lay, tw, st );
	CHECK( st[ 0 ].alpha == 0, "typewriter empty" );

	AnimatorParams rise;
	rise.mode     = AnimMode::SequenceIn;
	rise.spread   = 2;
	rise.offsetY  = 100;
	rise.progress = 0.0f;
	initGlyphStates( lay, st );
	applyAnimator( lay, rise, st );
	CHECK( std::fabs( st[ 0 ].dy - 100 ) < 1e-3 && std::fabs( st[ 3 ].dy - 100 ) < 1e-3, "sequence in at 0: all away" );
	rise.progress = 1.0f;
	initGlyphStates( lay, st );
	applyAnimator( lay, rise, st );
	CHECK( std::fabs( st[ 0 ].dy ) < 1e-3 && std::fabs( st[ 3 ].dy ) < 1e-3, "sequence in at 1: all at rest" );
	rise.order    = AnimOrder::Reverse;
	rise.progress = 0.3f;
	initGlyphStates( lay, st );
	applyAnimator( lay, rise, st );
	CHECK( st[ 3 ].dy < st[ 0 ].dy, "reverse order: last glyph arrives first" );

	AnimatorParams pop;
	pop.mode     = AnimMode::SequenceIn;
	pop.ease     = AnimEase::Overshoot;
	pop.scale    = 0;
	pop.spread   = 4;
	pop.progress = 0.45f;
	initGlyphStates( lay, st );
	applyAnimator( lay, pop, st );
	float maxS = 0;
	for( auto& g : st )
		maxS = std::max( maxS, g.scale );
	CHECK( maxS > 1.0f && maxS < 1.2f, "overshoot passes 100 %% (max scale %.3f)", maxS );

	AnimatorParams wave;
	wave.mode     = AnimMode::Wave;
	wave.offsetY  = 10;
	wave.spread   = 4;
	wave.progress = 0.25f;
	initGlyphStates( lay, st );
	applyAnimator( lay, wave, st );
	float a1 = st[ 0 ].dy;
	wave.progress = 1.25f;
	initGlyphStates( lay, st );
	applyAnimator( lay, wave, st );
	CHECK( std::fabs( st[ 0 ].dy - a1 ) < 1e-3 && std::fabs( a1 - 10 ) < 1e-3, "wave: progress is the phase, peak at 0.25 (%.3f)", a1 );
	wave.progress = 0.0f;
	initGlyphStates( lay, st );
	applyAnimator( lay, wave, st );
	const float w0 = st[ 2 ].dy;
	wave.progress = 1.0f;
	initGlyphStates( lay, st );
	applyAnimator( lay, wave, st );
	CHECK( std::fabs( st[ 2 ].dy - w0 ) < 1e-3, "wave loops seamlessly" );

	AnimatorParams rnd;
	rnd.mode     = AnimMode::Random;
	rnd.offsetX  = 10;
	rnd.spread   = 5;
	rnd.progress = 0.0f;
	initGlyphStates( lay, st );
	applyAnimator( lay, rnd, st );
	float r0 = st[ 0 ].dx;
	rnd.progress = 1.0f;
	initGlyphStates( lay, st );
	applyAnimator( lay, rnd, st );
	const float r1 = st[ 0 ].dx;
	rnd.progress = 0.5f;
	initGlyphStates( lay, st );
	applyAnimator( lay, rnd, st );
	CHECK( r0 == r1 && std::fabs( r0 ) <= 10.0f && st[ 0 ].dx != r0, "random loops seamlessly and moves in between" );

	// two animators stack; colour layers
	AnimatorParams col;
	col.mode        = AnimMode::Constant;
	col.progress    = 1.0f;
	col.colorAmount = 1.0f;
	col.color       = Rgb{ 0, 1, 0 };
	initGlyphStates( lay, st );
	applyAnimator( lay, rise, st );
	applyAnimator( lay, col, st );
	CHECK( st[ 1 ].tintAmount == 1.0f && st[ 1 ].tint.g == 1.0f, "colour tint" );
	std::printf( "ok   animation\n" );

	// --- glyph SDFs: TrueType and CFF outlines have opposite orientation ---
	testGlyphSign( face, "DejaVu Sans (TrueType)" );
	if( auto cff = db.resolve( "Cantarell / Regular" ) )
		testGlyphSign( FontFace::get( *cff ), "Cantarell (CFF)" );
	for( int i = 1; i < argc; ++i )
	{
		auto entries = FontDatabase::scanFile( argv[ i ] );
		CHECK( !entries.empty(), "scanFile %s", argv[ i ] );
		for( const FontEntry& e : entries )
		{
			std::printf( "info %s: %s (instance %d, weight %d)\n", argv[ i ], e.displayName().c_str(), e.instance, e.weight );
			if( auto ff = FontFace::get( e ) )
				testGlyphSign( ff, e.displayName().c_str() );
		}
	}

	// --- async cache ---
	auto& cache = GlyphCache::instance();
	hb_codepoint_t gidA = 0;
	hb_font_get_nominal_glyph( face->hbFont(), 'A', &gidA );
	auto first = cache.get( face, gidA, 0 );
	cache.waitIdle();
	auto second = cache.get( face, gidA, 0 );
	CHECK( !first && second && !second->empty, "async glyph generation" );
	std::printf( "ok   glyph cache\n" );

	// CSV: delimiter detection, quoted cells with line breaks, header row, encoding
	{
		auto rows = parseCsv( "Name;Funktion\r\nAnna Muster;\"Regie; Licht\"\r\n\"Ben \"\"B\"\" Beispiel\";\"Ton\nund Video\"\r\n;\r\n" );
		CHECK( rows.size() == 4 && rows[ 1 ].size() == 2 && rows[ 1 ][ 1 ] == "Regie; Licht", "csv: semicolon, quoted delimiter" );
		CHECK( rows.size() > 2 && rows[ 2 ][ 0 ] == "Ben \"B\" Beispiel" && rows[ 2 ][ 1 ] == "Ton\nund Video", "csv: escaped quotes, line break in cell" );
		auto slides = csvSlides( rows, true );
		CHECK( slides.size() == 2 && slides[ 0 ] == "Anna Muster\nRegie; Licht", "csv: rows -> slides, header skipped, empty row dropped" );
		CHECK( parseCsv( "a,b\tc\n" )[ 0 ].size() == 2 && parseCsv( "a\tb\tc,d\n" )[ 0 ].size() == 3, "csv: comma / tab detection" );
		CHECK( toUtf8( "M\xFCller \x80" ) == "M\xC3\xBCller \xE2\x82\xAC" && toUtf8( "\xEF\xBB\xBFM\xC3\xBCller" ) == "M\xC3\xBCller", "csv: Windows-1252 and UTF-8 BOM" );
		CHECK( isCsvPath( "/x/Namen.CSV" ) && !isCsvPath( "/x/a.txt" ), "csv: path detection" );
		std::printf( "ok   csv\n" );
	}

	std::printf( g_failures ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", g_failures );
	return g_failures ? 1 : 0;
}
