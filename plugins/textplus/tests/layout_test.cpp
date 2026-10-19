// Unit tests for the platform-neutral core: font database, layout, glyph SDFs.
// Run: ./layout_test [extra-font-file ...]
#include "core/Animator.h"
#include "core/LiveData.h"
#include "core/Osc.h"
#include "core/WebFetch.h"
#include <cstring>
#include "core/Clock.h"
#include "core/FontFallback.h"
#include "core/Utf8.h"
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

static bool r_lines_ok( const LayoutResult& r )
{
	for( const LineBox& lb : r.lines )
		if( !( lb.top < lb.baseline && lb.baseline < lb.bottom && lb.left <= lb.right && lb.right <= lb.penEnd + 0.01f ) )
			return false;
	return !r.lines.empty();
}

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

	// --- line styles: line 2 own (50 %, +20 % spacing), line 3+ repeats line 2 until it is own ---
	{
		LayoutParams q;
		q.text           = "Aa|Bb|Cc|Dd";
		q.sizePx         = 100;
		q.leadingPercent = 120;
		q.anchorV        = AnchorV::FirstBaseline;
		q.align          = Align::Left;
		auto plain       = layoutText( *face, q );
		CHECK( plain.glyphs[ 2 ].style == 0 && plain.glyphs[ 2 ].scale == 1.0f, "no own styles: all lines main style" );
		q.lines[ 1 ].own            = true;
		q.lines[ 1 ].sizePercent    = 50;
		q.lines[ 1 ].spacingPercent = 20;
		auto two = layoutText( *face, q );
		// baselines: 0, 0.5*120 + 20 = 80, +60 = 140, +60 = 200
		CHECK( two.glyphs[ 2 ].style == 1 && std::fabs( two.glyphs[ 2 ].scale - 0.5f ) < 1e-6, "line 2 styled" );
		CHECK( two.glyphs[ 4 ].style == 1 && two.glyphs[ 6 ].style == 1, "line 3+ repeats line 2" );
		CHECK( std::fabs( two.glyphs[ 2 ].y - 80.0f ) < 0.01f && std::fabs( two.glyphs[ 4 ].y - 140.0f ) < 0.01f, "line 2 baseline %.2f, line 3 %.2f",
			   two.glyphs[ 2 ].y, two.glyphs[ 4 ].y );
		q.lines[ 2 ].own         = true;
		q.lines[ 2 ].sizePercent = 25;
		auto three               = layoutText( *face, q );
		CHECK( three.glyphs[ 4 ].style == 2 && three.glyphs[ 6 ].style == 2 && std::fabs( three.glyphs[ 4 ].scale - 0.25f ) < 1e-6, "line 3+ own" );
		CHECK( std::fabs( three.glyphs[ 4 ].y - ( 80.0f + 30.0f ) ) < 0.01f, "line 3 baseline from its own size (%.2f)", three.glyphs[ 4 ].y );
		// widths: a 50 % line is half as wide as the same text at 100 %
		q.text      = "Hello|Hello";
		q.lines[ 1 ].spacingPercent = 0;
		auto widths = layoutText( *face, q );
		const float w1 = widths.glyphs[ 4 ].x - widths.glyphs[ 0 ].x, w2 = widths.glyphs[ 9 ].x - widths.glyphs[ 5 ].x;
		CHECK( std::fabs( w2 - 0.5f * w1 ) < 0.05f, "line 2 advances scale with its size (%.2f vs %.2f)", w1, w2 );
		std::printf( "ok   line styles\n" );
	}

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

	// Clock & timer formatting
	{
		CHECK( formatDuration( 90, TimeFormat::Auto, true ) == "1:30", "duration auto -> %s", formatDuration( 90, TimeFormat::Auto, true ).c_str() );
		CHECK( formatDuration( 3725, TimeFormat::Auto, true ) == "1:02:05", "duration auto with hours" );
		CHECK( formatDuration( 89.2, TimeFormat::MMSS, true ) == "01:30", "countdown rounds up" );
		CHECK( formatDuration( 89.8, TimeFormat::MMSS, false ) == "01:29", "stopwatch rounds down" );
		CHECK( formatDuration( 0.04, TimeFormat::HHMMSS, true ) == "00:00:01", "last fraction of a second still shows 1" );
		CHECK( formatDuration( -12, TimeFormat::Auto, false ) == "-0:12", "overtime -> %s", formatDuration( -12, TimeFormat::Auto, false ).c_str() );
		CHECK( formatDuration( 7384, TimeFormat::MMSS, false ) == "123:04", "minutes beyond 59" );
		CHECK( formatDuration( 65.45, TimeFormat::MMSSTenths, false ) == "01:05.4", "tenths -> %s", formatDuration( 65.45, TimeFormat::MMSSTenths, false ).c_str() );
		CHECK( formatDuration( 303, TimeFormat::Seconds, false ) == "303", "seconds" );
		CHECK( formatTimeOfDay( 9 * 3600 + 5 * 60 + 3, TimeFormat::HHMMSS ) == "09:05:03" && formatTimeOfDay( 9 * 3600 + 5 * 60, TimeFormat::Auto ) == "09:05", "time of day" );
		CHECK( formatNumber( 12840.5, 1, NumberStyle::Dot ) == "12.840,5" && formatNumber( 1234567, 0, NumberStyle::Comma ) == "1,234,567", "numbers -> %s", formatNumber( 12840.5, 1, NumberStyle::Dot ).c_str() );
		CHECK( formatNumber( -0.001, 1, NumberStyle::Plain ) == "0.0" && formatNumber( -5, 0, NumberStyle::Plain ) == "-5", "number sign" );
		double v = 0;
		CHECK( parseDuration( "5:00", v ) && v == 300, "duration m:ss" );
		CHECK( parseDuration( "1:30:00", v ) && v == 5400, "duration h:mm:ss" );
		CHECK( parseDuration( "90", v ) && v == 90 && parseDuration( "1h30m", v ) && v == 5400 && parseDuration( "2,5m", v ) && v == 150, "duration units" );
		CHECK( !parseDuration( "abc", v ) && !parseDuration( "", v ), "duration rejects garbage" );
		CHECK( parseTimeOfDay( "20:00", v ) && v == 72000 && parseTimeOfDay( "8:15:30", v ) && v == 29730 && !parseTimeOfDay( "25:00", v ), "time of day parse" );
		LocalTime lt;
		lt.secondsOfDay = 23 * 3600.0;
		CHECK( secondsUntil( 3600.0, lt ) == 7200.0, "target after midnight is tonight" );
		lt.secondsOfDay = 3600.0;
		CHECK( secondsUntil( 23 * 3600.0, lt ) == -7200.0, "target late last night is over" );
		lt.day = 3;
		lt.month = 4;
		lt.year = 2026;
		lt.secondsOfDay = 13 * 3600 + 7 * 60;
		CHECK( expandTimeTemplate( "Doors {timer} at {time}, {date}", "5:00", lt ) == "Doors 5:00 at 13:07, 03.04.2026", "template -> %s",
			   expandTimeTemplate( "Doors {timer} at {time}, {date}", "5:00", lt ).c_str() );
		CHECK( expandTimeTemplate( "Text+", "5:00", lt ) == "5:00", "no placeholder: timer alone" );
		std::printf( "ok   clock\n" );
	}

	// Case
	{
		CHECK( applyCase( "Straße ärger ÿ", TextCase::Upper ) == "STRASSE ÄRGER Ÿ", "upper -> %s", applyCase( "Straße ärger ÿ", TextCase::Upper ).c_str() );
		CHECK( applyCase( "ÉCOLE ΩMEGA ПРИВЕТ İ", TextCase::Lower ) == "école ωmega привет i", "lower -> %s", applyCase( "ÉCOLE ΩMEGA ПРИВЕТ İ", TextCase::Lower ).c_str() );
		CHECK( applyCase( "a\\nb {#ff00aa}x{/}", TextCase::Upper ) == "A\\nB {#ff00aa}X{/}", "escapes and tags kept -> %s", applyCase( "a\\nb {#ff00aa}x{/}", TextCase::Upper ).c_str() );
		std::printf( "ok   case\n" );
	}

	// Animator: Sequence In/Out, wipe, scramble
	{
		LayoutParams ap;
		ap.sizePx = 100;
		ap.text   = "ABCD";
		LayoutResult r = layoutText( *face, ap );
		auto run = [ & ]( AnimatorParams a ) {
			std::vector< GlyphState > st;
			initGlyphStates( r, st );
			applyAnimator( r, a, st );
			return st;
		};
		AnimatorParams a;
		a.mode    = AnimMode::SequenceInOut;
		a.opacity = 0.0f;
		a.spread  = 1.0f;
		a.ease    = AnimEase::Linear;
		a.hold    = 0.5f;
		a.progress = 0.0f;
		CHECK( run( a )[ 0 ].alpha < 0.01f, "in/out starts away" );
		a.progress = 0.5f;
		bool allIn = true;
		for( auto& s : run( a ) )
			allIn = allIn && s.alpha > 0.99f;
		CHECK( allIn, "in/out at rest in the middle" );
		a.progress = 1.0f;
		CHECK( run( a )[ 3 ].alpha < 0.01f, "in/out ends away" );
		a.progress = 0.85f;
		auto mid = run( a );
		CHECK( mid[ 0 ].alpha < mid[ 3 ].alpha, "first in, first out" );

		AnimatorParams w;
		w.mode     = AnimMode::Constant;
		w.progress = 0.5f;
		w.wipe     = 1.0f;
		w.unit     = AnimUnit::Line;
		w.wipeFrom = WipeFrom::Left;
		auto ws    = run( w );
		const float lineL = r.lines[ 0 ].left, lineR = r.glyphs.back().x + r.glyphs.back().advance;
		CHECK( std::fabs( ws[ 0 ].clipX1 - ( lineL + 0.5f * ( lineR - lineL ) ) ) < 1.0f && ws[ 0 ].clipX0 < -1e8f, "wipe from left: half the line -> %f", ws[ 0 ].clipX1 );
		w.wipeFrom = WipeFrom::Center;
		ws         = run( w );
		CHECK( ws[ 0 ].clipX0 > lineL && ws[ 0 ].clipX1 < lineR, "wipe from centre" );
		w.progress = 0.0f;
		CHECK( run( w )[ 0 ].clipX1 > 1e8f, "no wipe at rest" );

		AnimatorParams sc;
		sc.mode     = AnimMode::Constant;
		sc.progress = 1.0f;
		sc.scramble = 1.0f;
		bool allScrambled = true;
		for( auto& s : run( sc ) )
			allScrambled = allScrambled && s.scramble != 0;
		CHECK( allScrambled, "scramble 100%% away" );
		sc.progress = 0.0f;
		CHECK( run( sc )[ 0 ].scramble == 0, "no scramble at rest" );
		std::printf( "ok   animator in/out, wipe, scramble\n" );
	}

	// Font fallback: emoji from the embedded font, missing scripts from other fonts
	{
		auto inter = db.resolve( "Inter / Regular" );
		auto iface = inter ? FontFace::get( *inter ) : nullptr;
		CHECK( emojiFace() && emojiFace()->hasGlyph( 0x1F600 ), "embedded emoji font" );
		if( iface )
		{
			LayoutParams fp;
			fp.sizePx = 100;
			fp.text   = "A\xF0\x9F\x98\x80\xD7\xA9";// A, grinning face, Hebrew shin
			LayoutResult fr = layoutText( *iface, fp );
			CHECK( fr.glyphs.size() == 3 && fr.glyphs[ 0 ].face == nullptr && fr.glyphs[ 1 ].face == emojiFace().get() && fr.glyphs[ 2 ].face != nullptr &&
					   fr.glyphs[ 2 ].glyphId != 0,
				   "fallback faces (%zu glyphs)", fr.glyphs.size() );
			fp.text = "1\xEF\xB8\x8F\xE2\x83\xA3";// keycap 1
			fr      = layoutText( *iface, fp );
			CHECK( !fr.glyphs.empty() && fr.glyphs[ 0 ].face == emojiFace().get(), "keycap from the emoji font" );
			CHECK( r_lines_ok( fr ), "line boxes" );
		}
		std::printf( "ok   font fallback\n" );
	}

	// v2: live data (JSON, placeholders, OSC decoding), easings, 3D properties
	{
		std::string err;
		Json j = Json::parse( R"({"main":{"temp":21.5,"name":"Ravensburg"},"items":[{"t":"a"},{"t":"b"}],"ok":true,"n":3,"s":"\u00e4\ud83d\ude00"})", &err );
		CHECK( err.empty() && j.type() == Json::Type::Object, "json parse (%s)", err.c_str() );
		CHECK( j.find( "main.temp" ) && j.find( "main.temp" )->text() == "21.5", "json number path" );
		CHECK( j.find( "items[1].t" ) && j.find( "items[1].t" )->text() == "b", "json array path" );
		CHECK( j.find( "items.0.t" ) && j.find( "items.0.t" )->text() == "a", "json dotted index" );
		CHECK( j.find( "items[-1].t" ) && j.find( "items[-1].t" )->text() == "b", "json negative index" );
		CHECK( j.find( "n" )->text() == "3" && j.find( "ok" )->text() == "true", "json whole number and bool" );
		CHECK( j.find( "s" )->text() == "\xC3\xA4\xF0\x9F\x98\x80", "json unicode escapes" );
		CHECK( !j.find( "main.nope" ) && !j.find( "items[5]" ), "json missing paths" );
		Json bad = Json::parse( "{\"a\":", &err );
		CHECK( bad.isNull() && !err.empty(), "json error reported" );
		CHECK( j.outline().find( "main.temp: 21.5" ) != std::string::npos && j.outline().find( "items[1].t: b" ) != std::string::npos, "json outline: %s", j.outline().c_str() );

		PlaceholderContext ctx;
		ctx.bpm    = 128;
		ctx.beats  = 9.5;// bar 3, beat 2
		ctx.slide  = 2;
		ctx.slides = 5;
		ctx.json   = &j;
		bool found = false;
		std::string e = expandPlaceholders( "{bpm} BPM, bar {bar} beat {beat}, {slide}/{slides}, {main.name} {main.temp}\u00b0", ctx, &found );
		CHECK( found && e == "128 BPM, bar 3 beat 2, 2/5, Ravensburg 21.5\u00b0", "placeholders: %s", e.c_str() );
		e = expandPlaceholders( "{#ff0000}red{/} {{literal}} {unknown}", ctx, &found );
		CHECK( !found && e == "{#ff0000}red{/} {{literal}} {unknown}", "colour tags and unknown stay: %s", e.c_str() );
		Variables::instance().set( "/score/home", "3" );
		e = expandPlaceholders( "Home {/score/home}", ctx, &found );
		CHECK( found && e == "Home 3", "osc variable: %s", e.c_str() );

		// OSC message "/score/away" ",isf" 7 "Gast" 0.5 and a bundle around it
		std::vector< uint8_t > msg;
		auto pad  = [ & ]() { while( msg.size() % 4 ) msg.push_back( 0 ); };
		auto str  = [ & ]( const char* t ) { for( const char* c = t; *c; ++c ) msg.push_back( (uint8_t)*c ); msg.push_back( 0 ); pad(); };
		auto i32  = [ & ]( uint32_t v ) { msg.push_back( v >> 24 ); msg.push_back( v >> 16 ); msg.push_back( v >> 8 ); msg.push_back( v ); };
		str( "/score/away" );
		str( ",isf" );
		i32( 7 );
		str( "Gast" );
		float half = 0.5f;
		uint32_t bits;
		std::memcpy( &bits, &half, 4 );
		i32( bits );
		auto dec = OscReceiver::decode( msg.data(), msg.size() );
		CHECK( dec.size() == 1 && dec[ 0 ].first == "/score/away" && dec[ 0 ].second == "7 Gast 0.5", "osc decode: %s",
			   dec.empty() ? "-" : dec[ 0 ].second.c_str() );
		std::vector< uint8_t > inner = msg;
		msg.clear();
		str( "#bundle" );
		i32( 0 );
		i32( 1 );
		i32( (uint32_t)inner.size() );
		msg.insert( msg.end(), inner.begin(), inner.end() );
		dec = OscReceiver::decode( msg.data(), msg.size() );
		CHECK( dec.size() == 1 && dec[ 0 ].second == "7 Gast 0.5", "osc bundle" );
		msg.resize( 10 );
		dec = OscReceiver::decode( msg.data(), msg.size() );
		CHECK( dec.empty(), "osc truncated packet ignored" );

		CHECK( detectWebFormat( "https://x/pub?output=csv", "text/plain", "a,b" ) == WebFormat::Csv, "csv by url" );
		CHECK( detectWebFormat( "https://x/api", "application/json; charset=utf-8", "" ) == WebFormat::Json, "json by type" );
		CHECK( detectWebFormat( "https://x/raw", "text/plain", "  [1,2]" ) == WebFormat::Json, "json by body" );

		for( AnimEase ez : { AnimEase::Bounce, AnimEase::Elastic, AnimEase::EaseIn } )
			CHECK( std::fabs( applyEase( ez, 0.0f ) ) < 1e-4f && std::fabs( applyEase( ez, 1.0f ) - 1.0f ) < 1e-4f, "ease %d ends", (int)ez );
		CHECK( applyEase( AnimEase::Elastic, 0.15f ) > 1.2f, "elastic overshoots" );

		std::vector< AnimItem > items( 3 );
		for( int k = 0; k < 3; ++k )
			items[ k ].charIndex = items[ k ].wordIndex = k;
		AnimatorParams ap;
		ap.mode         = AnimMode::Constant;
		ap.progress     = 1.0f;
		ap.rotationXDeg = 90.0f;
		ap.depth        = 100.0f;
		ap.trackingPx   = 10.0f;
		ap.blurPx       = 4.0f;
		std::vector< GlyphState > st( 3 );
		applyAnimator( items, UnitCounts{ 3, 3, 1 }, ap, st );
		CHECK( std::fabs( st[ 0 ].rotationX - 1.5708f ) < 1e-3f && st[ 1 ].depth == 100.0f && st[ 2 ].blur == 4.0f, "3D properties" );
		CHECK( std::fabs( st[ 0 ].dx + 10.0f ) < 1e-3f && std::fabs( st[ 1 ].dx ) < 1e-3f && std::fabs( st[ 2 ].dx - 10.0f ) < 1e-3f, "tracking spreads from the centre" );
		CHECK( usesProjection( ap ), "projection detected" );
		std::printf( "ok   live data, osc, easing, 3D properties\n" );
	}

	std::printf( g_failures ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", g_failures );
	return g_failures ? 1 : 0;
}
