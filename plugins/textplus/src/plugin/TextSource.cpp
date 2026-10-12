#include "TextSource.h"

#include "core/Csv.h"
#include "core/FileUtil.h"
#include "core/GlyphCache.h"
#include "core/Utf8.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <hb.h>

#ifndef TEXTPLUS_VERSION
#define TEXTPLUS_VERSION "dev"
#endif

namespace textplus
{
using sdfkit::Color;
using sdfkit::ShapeInstance;
using sdfkit::Style;

namespace
{
FFResult initialiseLibrary()
{
	// Start scanning fonts as soon as Resolume loads the plugin, long before the
	// first clip is triggered.
	FontDatabase::instance().ensureScanning();
	return FF_SUCCESS;
}

CFFGLPluginInfo PluginInfo( PluginFactory< TextSource >,// create method
							"KTXT",                      // unique FFGL plugin ID
							"Text+",                     // name (sorts next to Resolume's own Text sources)
							2,                           // API major
							1,                           // API minor
							1,                           // plugin major
							1,                           // plugin minor
							FF_SOURCE,
							"Text source with distance-field outlines, plates, shadows, glow, clocks, tickers and per-character animation",
							"Text+ " TEXTPLUS_VERSION,
							0, nullptr, initialiseLibrary, nullptr );

const char* kGroupText     = "Text";
const char* kGroupFont     = "Font";
const char* kGroupLayout   = "Layout";
const char* kGroupFill     = "Fill";
const char* kGroupOutline  = "Outline";
const char* kGroupPlate    = "Plate";
const char* kGroupShadow   = "Shadow";
const char* kGroupGlow     = "Glow";
const char* kGroupAnim[ kNumAnimators ] = { "Animator 1", "Animator 2" };
const char* kGroupLine[ kNumLineStyles ] = { "Line 2", "Line 3+" };
const char* kGroupOptions  = "Options";

// Preset menu: values are stable (scripts and saved compositions), the order is for reading.
enum Preset
{
	PRESET_NONE           = 0,
	PRESET_TYPEWRITER     = 1,
	PRESET_FADE_IN        = 2,
	PRESET_RISE_IN        = 3,
	PRESET_DROP_IN        = 4,
	PRESET_POP_IN         = 5,
	PRESET_SPIN_IN        = 6,
	PRESET_WORDS_SLIDE_IN = 7,
	PRESET_FADE_OUT       = 8,
	PRESET_WAVE           = 9,
	PRESET_JITTER         = 10,
	PRESET_COLOR_SWEEP    = 11,
	PRESET_RESET          = 12,
	PRESET_WIPE_IN        = 13,
	PRESET_DECODE         = 14,
	PRESET_LOWER_THIRD_IN = 15,
	PRESET_LOWER_THIRD_OUT = 16,
	PRESET_GLITCH         = 17,
	PRESET_IN_OUT         = 18,
};

const std::vector< ffglkit::OptionItem > kPresetItems = {
	{ "Load preset...", PRESET_NONE },
	{ "Typewriter", PRESET_TYPEWRITER, true },
	{ "Fade In", PRESET_FADE_IN },
	{ "Rise In", PRESET_RISE_IN },
	{ "Drop In", PRESET_DROP_IN },
	{ "Pop In", PRESET_POP_IN },
	{ "Spin In", PRESET_SPIN_IN },
	{ "Words Slide In", PRESET_WORDS_SLIDE_IN },
	{ "Wipe In", PRESET_WIPE_IN },
	{ "Decode", PRESET_DECODE },
	{ "Fade Out", PRESET_FADE_OUT },
	{ "Fade In, Hold, Out", PRESET_IN_OUT },
	{ "Lower Third In (both animators)", PRESET_LOWER_THIRD_IN, true },
	{ "Lower Third Out (both animators)", PRESET_LOWER_THIRD_OUT },
	{ "Wave", PRESET_WAVE, true },
	{ "Jitter", PRESET_JITTER },
	{ "Glitch", PRESET_GLITCH },
	{ "Color Sweep", PRESET_COLOR_SWEEP },
	{ "Reset", PRESET_RESET, true },
};

// Splits the source text into slides.
//  Lines:  every line (newline or "|" when "| = New Line" is on) is a slide.
//  Blocks: "||" or an empty line separates slides; single breaks stay line breaks.
std::string trimCopy( const std::string& s )
{
	size_t a = s.find_first_not_of( " \t\r\n" );
	if( a == std::string::npos )
		return {};
	size_t b = s.find_last_not_of( " \t\r\n" );
	return s.substr( a, b - a + 1 );
}

std::vector< std::string > splitSlides( const std::string& src, int mode, bool pipeIsNewline )
{
	std::vector< std::string > out;
	if( mode == DISPLAY_SLIDES_LINES )
	{
		std::string cur;
		for( size_t i = 0; i < src.size(); ++i )
		{
			char c       = src[ i ];
			bool isBreak = c == '\n' || c == '\r' || ( c == '|' && pipeIsNewline ) || ( c == '\\' && i + 1 < src.size() && src[ i + 1 ] == 'n' );
			if( isBreak )
			{
				if( c == '\\' || ( c == '\r' && i + 1 < src.size() && src[ i + 1 ] == '\n' ) )
					++i;
				if( !trimCopy( cur ).empty() )
					out.push_back( cur );
				cur.clear();
			}
			else
				cur.push_back( c );
		}
		if( !trimCopy( cur ).empty() )
			out.push_back( cur );
	}
	else
	{
		// Normalise line breaks, then cut at "||" and at blank lines.
		std::string norm;
		for( size_t i = 0; i < src.size(); ++i )
		{
			if( src[ i ] == '\r' )
			{
				if( i + 1 < src.size() && src[ i + 1 ] == '\n' )
					++i;
				norm.push_back( '\n' );
			}
			else
				norm.push_back( src[ i ] );
		}
		std::string cur;
		size_t i = 0;
		while( i < norm.size() )
		{
			if( norm.compare( i, 2, "||" ) == 0 )
			{
				out.push_back( cur );
				cur.clear();
				i += 2;
				continue;
			}
			if( norm[ i ] == '\n' )
			{
				size_t j = i + 1;
				while( j < norm.size() && ( norm[ j ] == ' ' || norm[ j ] == '\t' ) )
					++j;
				if( j < norm.size() && norm[ j ] == '\n' )// blank line
				{
					out.push_back( cur );
					cur.clear();
					while( j < norm.size() && ( norm[ j ] == '\n' || norm[ j ] == ' ' || norm[ j ] == '\t' ) )
						++j;
					i = j;
					continue;
				}
			}
			cur.push_back( norm[ i ] );
			++i;
		}
		out.push_back( cur );
		out.erase( std::remove_if( out.begin(), out.end(), []( const std::string& s ) { return trimCopy( s ).empty(); } ), out.end() );
	}
	if( out.empty() )
		out.push_back( std::string() );
	return out;
}

int positiveMod( int a, int n )
{
	return n <= 0 ? 0 : ( ( a % n ) + n ) % n;
}

double positiveFmod( double a, double n )
{
	if( n <= 0.0 )
		return 0.0;
	double r = std::fmod( a, n );
	return r < 0.0 ? r + n : r;
}

// Slide position 0..1 -> slide index: n equal shares, 1.0 belongs to the last slide.
int slideFromPosition( float position, int count )
{
	if( count <= 1 )
		return 0;
	const float p = std::clamp( position, 0.0f, 1.0f );
	return std::min( count - 1, (int)std::floor( p * (float)count ) );
}

// Centre of a slide's share, so rounding never lands on a neighbour.
float slidePosition( int index, int count )
{
	return count <= 1 ? 0.0f : ( (float)index + 0.5f ) / (float)count;
}

Color toColor( const ffglkit::Rgba& c )
{
	return Color{ c.r, c.g, c.b, c.a };
}

bool isSequence( AnimMode m )
{
	return m == AnimMode::SequenceIn || m == AnimMode::SequenceOut || m == AnimMode::SequenceInOut;
}
}//namespace

// ===========================================================================
// Parameters
// ===========================================================================
TextSource::TextSource() :
	ParamPlugin( P_COUNT )
{
	m_isConnected = false;// left uninitialised by the SDK
	SetMinInputs( 0 );
	SetMaxInputs( 0 );
	defineParameters();
	FontDatabase& db = FontDatabase::instance();
	db.ensureScanning();
	// If the fonts are known already (every clip after the first), fill the menus
	// now: they are then part of the initial parameter info instead of a
	// "list changed" event the host has to process later.
	if( db.isReady() )
		populateFontMenus( false );
}

TextSource::~TextSource() = default;

void TextSource::defineParameters()
{
	using ffglkit::OptionItem;

	// ---- Text: where it comes from, how it is shown ----
	defOption( P_SOURCE, "Source", kGroupText, std::vector< OptionItem >{ { "Input", SOURCE_INPUT }, { "File", SOURCE_FILE }, { "Clock & Timer", SOURCE_CLOCK } },
			   SOURCE_INPUT );
	defOption( P_DISPLAY, "Display", kGroupText,
			   std::vector< OptionItem >{ { "Static", DISPLAY_STATIC },
										  { "Slides: Lines", DISPLAY_SLIDES_LINES, true },
										  { "Slides: Blocks (|| or empty line)", DISPLAY_SLIDES_BLOCKS },
										  { "Ticker", DISPLAY_TICKER, true },
										  { "Roll", DISPLAY_ROLL } },
			   DISPLAY_STATIC );
	text = "Text+";
	defText( P_TEXT, "Text", kGroupText, text );
	// Resolume's text fields are single-line: further lines get their own fields,
	// revealed one by one with "+ Line" (hidden fields keep their saved values).
	for( int i = 0; i < kExtraLines; ++i )
		defText( P_LINE_FIRST + (unsigned)i, "Text " + std::to_string( i + 2 ), kGroupText, "" );
	defEvent( P_ADD_LINE, "+ Line", kGroupText );
	defEvent( P_REMOVE_LINE, "- Line", kGroupText );
	defFile( P_TEXT_FILE, "Text File", kGroupText, { "txt", "text", "md", "csv", "tsv" } );
	defBool( P_CSV_HEADER, "CSV: Skip First Row", kGroupText, true );

	// Clock & Timer: the text fields are the template ("Doors open in {timer}")
	defOption( P_TIMER_MODE, "Timer Mode", kGroupText,
			   std::vector< OptionItem >{ { "Clock", TIMER_CLOCK },
										  { "Countdown to Time", TIMER_COUNTDOWN_TO },
										  { "Countdown", TIMER_COUNTDOWN },
										  { "Stopwatch", TIMER_STOPWATCH },
										  { "Counter", TIMER_COUNTER, true } },
			   TIMER_CLOCK );
	defText( P_TIMER_TARGET, "Target Time", kGroupText, timerTarget );
	defText( P_TIMER_DURATION, "Duration", kGroupText, timerDuration );
	defEvent( P_TIMER_START, "Start", kGroupText );
	defEvent( P_TIMER_STOP, "Stop", kGroupText );
	defEvent( P_TIMER_RESET, "Reset", kGroupText );
	defBool( P_TIMER_AUTOSTART, "Start with Clip", kGroupText, true );
	defOption( P_TIMER_FORMAT, "Time Format", kGroupText, { "Auto", "HH:MM:SS", "HH:MM", "MM:SS", "MM:SS.f", "Seconds" }, 0 );
	defOption( P_TIMER_AT_ZERO, "At Zero", kGroupText, { "Hold 0", "Hide", "Count Up" }, 0 );
	defRange( P_COUNT_FROM, "Count From", kGroupText, 0.0f, -1000000.0f, 1000000.0f );
	defRange( P_COUNT_TO, "Count To", kGroupText, 1000.0f, -1000000.0f, 1000000.0f );
	defRange( P_COUNT_PROGRESS, "Count", kGroupText, 1.0f, 0.0f, 1.0f );
	defOption( P_COUNT_DECIMALS, "Decimals", kGroupText, { "0", "1", "2", "3" }, 0 );
	defOption( P_NUMBER_FORMAT, "Number Format", kGroupText, { "1234.5", "1,234.5", "1.234,5", "1 234,5", "1'234.5" }, 1 );

	// Slides: 0..1 split into equal shares, so BPM sync on the slider cycles through
	// all slides and wraps back to the first, whatever the number of slides.
	defRange( P_SLIDE, "Slide", kGroupText, 0.0f, 0.0f, 1.0f );
	defEvent( P_NEXT_SLIDE, "Next Slide", kGroupText );
	defEvent( P_PREV_SLIDE, "Previous Slide", kGroupText );
	defRange( P_REPLAY, "Replay on Slide Change", kGroupText, 0.0f, 0.0f, 16.0f );
	defRange( P_REPLAY_STAGGER, "Replay Stagger", kGroupText, 0.0f, 0.0f, 100.0f );

	// Ticker / Roll
	defRange( P_SCROLL_SPEED, "Scroll Speed", kGroupText, 150.0f, -2000.0f, 2000.0f );
	defRange( P_SCROLL_POSITION, "Scroll Position", kGroupText, 0.0f, 0.0f, 1.0f );
	defText( P_SEPARATOR, "Separator", kGroupText, separator );
	defBool( P_ROLL_LOOP, "Roll Loop", kGroupText, true );
	defRange( P_EDGE_FADE, "Edge Fade", kGroupText, 60.0f, 0.0f, 1000.0f );
	// Text+ 1.0 had "Slides" (Off / Each Line / Blocks); kept hidden so old
	// compositions open with the matching Display.
	defOption( P_SLIDES_LEGACY, "Slides", kGroupText, { "Off", "Each Line", "Blocks" }, 0 );

	// ---- Font: the menus first; "Font" is the stored, typeable name the menus write into ----
	defOption( P_FAMILY, "Family", kGroupFont, std::vector< const char* >{ "(scanning fonts...)" }, 0 );
	defOption( P_STYLE, "Style", kGroupFont, std::vector< const char* >{ "-" }, 0 );
	defRange( P_SIZE, "Size", kGroupFont, 160.0f, 4.0f, 2000.0f );
	defText( P_FONT, "Font", kGroupFont, "" );
	defFile( P_FONT_FILE, "Font File", kGroupFont, { "ttf", "otf", "ttc", "otc" } );

	// ---- Line styles: "Line 2" and "Line 3+" (every line from the third on) ----
	for( int l = 0; l < kNumLineStyles; ++l )
	{
		const std::string pre = l == 0 ? "L2 " : "L3 ";
		const char* g         = kGroupLine[ l ];
		auto id               = [ & ]( LineParam p ) { return lineParam( l, p ); };
		defOption( id( LP_MODE ), pre + "Style", g, std::vector< const char* >{ l == 0 ? "Same as Line 1" : "Same as Line 2", "Own" }, 0 );
		defOption( id( LP_WEIGHT ), pre + "Weight", g, std::vector< const char* >{ "-" }, 0 );
		defText( id( LP_FONT ), pre + "Font", g, "" );
		defRange( id( LP_SIZE ), pre + "Size", g, 100.0f, 5.0f, 400.0f );
		defColor( id( LP_COLOR_H ), pre + "Color", g, 0.0f, 0.0f, 1.0f, 0.0f );
		defRange( id( LP_TRACKING ), pre + "Tracking", g, 0.0f, -300.0f, 1000.0f );
		defRange( id( LP_SPACING ), pre + "Spacing", g, 0.0f, -100.0f, 200.0f );
	}

	// ---- Layout ----
	defRange( P_POS_X, "Position X", kGroupLayout, 0.0f, -4000.0f, 4000.0f );
	defRange( P_POS_Y, "Position Y", kGroupLayout, 0.0f, -4000.0f, 4000.0f );
	defRange( P_ROTATION, "Rotation", kGroupLayout, 0.0f, -360.0f, 360.0f );
	defOption( P_ALIGN, "Align", kGroupLayout, { "Left", "Center", "Right", "Justify" }, 1 );
	defOption( P_CASE, "Case", kGroupLayout, { "Tt", "TT", "tt" }, 0 );
	defOption( P_ANCHOR_H, "Anchor H", kGroupLayout, { "Auto", "Left", "Center", "Right" }, 0 );
	defOption( P_ANCHOR_V, "Anchor V", kGroupLayout, { "Top", "Middle", "Bottom", "First Baseline", "Last Baseline" }, 1 );
	defOption( P_WRAP, "Wrap", kGroupLayout, { "Off", "Box" }, 0 );
	defRange( P_BOX_WIDTH, "Box Width", kGroupLayout, 1000.0f, 10.0f, 8000.0f );
	defRange( P_BOX_HEIGHT, "Box Height", kGroupLayout, 0.0f, 0.0f, 8000.0f );
	defOption( P_FIT, "Auto Fit", kGroupLayout, { "Off", "Shrink to Box", "Fit Box Width" }, 0 );
	defRange( P_TRACKING, "Tracking", kGroupLayout, 0.0f, -300.0f, 1000.0f );
	defRange( P_LEADING, "Leading", kGroupLayout, 120.0f, 50.0f, 300.0f );

	// ---- Fill ----
	defOption( P_FILL_MODE, "Fill Mode", kGroupFill, { "Solid", "Gradient (Block)", "Gradient (Characters)" }, 0 );
	defColor( P_FILL_H, "Fill Color", kGroupFill, 0.0f, 0.0f, 1.0f, 1.0f );
	defColor( P_FILL2_H, "Fill Color 2", kGroupFill, 0.58f, 0.7f, 1.0f, 1.0f );
	defRange( P_GRAD_ANGLE, "Gradient Angle", kGroupFill, 0.0f, -180.0f, 180.0f );

	// ---- Outline: band 1 and, outside it, band 2 ----
	defRange( P_OL1_WIDTH, "Outline Width", kGroupOutline, 0.0f, 0.0f, 400.0f );
	defOption( P_OL1_POSITION, "Outline Position", kGroupOutline, { "Outside", "Center", "Inside" }, 0 );
	defOption( P_OL_CORNERS, "Outline Corners", kGroupOutline, { "Round", "Sharp" }, 0 );
	defColor( P_OL1_H, "Outline Color", kGroupOutline, 0.0f, 0.0f, 0.0f, 1.0f );
	defRange( P_OL2_WIDTH, "Outline 2 Width", kGroupOutline, 0.0f, 0.0f, 400.0f );
	defColor( P_OL2_H, "Outline 2 Color", kGroupOutline, 0.0f, 0.0f, 1.0f, 1.0f );

	// ---- Plate: areas behind the block or behind each line ----
	defOption( P_PLATE, "Plate", kGroupPlate, { "Off", "Block", "Lines" }, PLATE_OFF );
	defColor( P_PLATE_H, "Plate Color", kGroupPlate, 0.0f, 0.0f, 0.08f, 0.85f );
	defRange( P_PLATE_PAD_X, "Plate Padding X", kGroupPlate, 35.0f, -50.0f, 300.0f );
	defRange( P_PLATE_PAD_Y, "Plate Padding Y", kGroupPlate, 10.0f, -50.0f, 300.0f );
	defRange( P_PLATE_RADIUS, "Plate Radius", kGroupPlate, 0.0f, 0.0f, 100.0f );
	defRange( P_PLATE_SLANT, "Plate Slant", kGroupPlate, 0.0f, -45.0f, 45.0f );
	defOption( P_PLATE_WIDTH, "Plate Width", kGroupPlate, { "Fit Text", "Equal", "Box" }, 0 );
	defRange( P_PLATE_BORDER, "Plate Border", kGroupPlate, 0.0f, 0.0f, 200.0f );
	defColor( P_PLATE_BORDER_H, "Plate Border Color", kGroupPlate, 0.0f, 0.0f, 1.0f, 1.0f );

	// ---- Shadow (Distance 0 = hidden behind the text, so the default is "off" without a toggle) ----
	defColor( P_SH_H, "Shadow Color", kGroupShadow, 0.0f, 0.0f, 0.0f, 0.6f );
	defRange( P_SH_DISTANCE, "Shadow Distance", kGroupShadow, 0.0f, 0.0f, 400.0f );
	defRange( P_SH_ANGLE, "Shadow Angle", kGroupShadow, -45.0f, -180.0f, 180.0f );
	defRange( P_SH_SOFTNESS, "Shadow Softness", kGroupShadow, 8.0f, 0.0f, 200.0f );
	defRange( P_SH_SPREAD, "Shadow Spread", kGroupShadow, 0.0f, 0.0f, 200.0f );
	defRange( P_ISH_DISTANCE, "Inner Shadow Distance", kGroupShadow, 0.0f, 0.0f, 200.0f );
	defRange( P_ISH_SOFTNESS, "Inner Shadow Softness", kGroupShadow, 6.0f, 0.0f, 200.0f );
	defColor( P_ISH_H, "Inner Shadow Color", kGroupShadow, 0.0f, 0.0f, 0.0f, 0.6f );

	// ---- Glow (Radius 0 = off) ----
	defColor( P_GL_H, "Glow Color", kGroupGlow, 0.55f, 0.8f, 1.0f, 1.0f );
	defRange( P_GL_RADIUS, "Glow Radius", kGroupGlow, 0.0f, 0.0f, 400.0f );
	defRange( P_GL_STRENGTH, "Glow Strength", kGroupGlow, 1.0f, 0.0f, 2.0f );
	defRange( P_IGL_RADIUS, "Inner Glow Radius", kGroupGlow, 0.0f, 0.0f, 200.0f );
	defColor( P_IGL_H, "Inner Glow Color", kGroupGlow, 0.14f, 0.6f, 1.0f, 1.0f );

	// ---- Animators. Resolume shows the internal names, so they carry the "A1 " prefix. ----
	for( int a = 0; a < kNumAnimators; ++a )
	{
		const std::string pre = "A" + std::to_string( a + 1 ) + " ";
		const char* g         = kGroupAnim[ a ];
		auto id               = [ & ]( AnimParam p ) { return animParam( a, p ); };
		defOption( id( AP_PRESET ), pre + "Preset", g, kPresetItems, PRESET_NONE );
		defOption( id( AP_MODE ), pre + "Mode", g,
				   std::vector< OptionItem >{ { "Off", (float)AnimMode::Off },
											  { "Sequence In", (float)AnimMode::SequenceIn, true },
											  { "Sequence Out", (float)AnimMode::SequenceOut },
											  { "Sequence In/Out", (float)AnimMode::SequenceInOut },
											  { "Wave", (float)AnimMode::Wave, true },
											  { "Random", (float)AnimMode::Random },
											  { "Constant", (float)AnimMode::Constant } },
				   (float)AnimMode::Off );
		defOption( id( AP_TARGET ), pre + "Target", g, { "Text", "Plate", "Text + Plate" }, 0 );
		defOption( id( AP_UNIT ), pre + "Unit", g, { "Character", "Word", "Line" }, 0 );
		defOption( id( AP_ORDER ), pre + "Order", g, { "Forward", "Reverse", "From Center", "From Edges", "Random" }, 0 );
		defRange( id( AP_PROGRESS ), pre + "Progress", g, 1.0f, 0.0f, 1.0f );
		defRange( id( AP_SPREAD ), pre + "Spread", g, 3.0f, 0.0f, 50.0f );
		defRange( id( AP_HOLD ), pre + "Hold", g, 50.0f, 0.0f, 100.0f );
		defOption( id( AP_EASE ), pre + "Ease", g, { "Linear", "Smooth", "Ease Out", "Overshoot" }, 1 );
		defRange( id( AP_OFFSET_X ), pre + "Offset X", g, 0.0f, -2000.0f, 2000.0f );
		defRange( id( AP_OFFSET_Y ), pre + "Offset Y", g, 0.0f, -2000.0f, 2000.0f );
		defRange( id( AP_SCALE ), pre + "Scale", g, 100.0f, 0.0f, 400.0f );
		defRange( id( AP_ROTATION ), pre + "Rotation", g, 0.0f, -720.0f, 720.0f );
		defRange( id( AP_OPACITY ), pre + "Opacity", g, 100.0f, 0.0f, 100.0f );
		defRange( id( AP_WIPE ), pre + "Wipe", g, 0.0f, 0.0f, 100.0f );
		defOption( id( AP_WIPE_FROM ), pre + "Wipe From", g, { "Left", "Right", "Center", "Top", "Bottom" }, 0 );
		defRange( id( AP_SCRAMBLE ), pre + "Scramble", g, 0.0f, 0.0f, 100.0f );
		defOption( id( AP_CHARSET ), pre + "Characters", g, { "Letters", "Digits", "Symbols", "Binary", "Same as Text" }, 0 );
		defColor( id( AP_COLOR_H ), pre + "Color", g, 0.0f, 0.8f, 1.0f, 0.0f );
	}

	// ---- Options: set once, rarely touched ----
	defBool( P_PIPE_NEWLINE, "| = New Line", kGroupOptions, true );
	defBool( P_MARKUP, "Color Tags", kGroupOptions, true );
	defBool( P_KERNING, "Kerning", kGroupOptions, true );
	defBool( P_LIGATURES, "Ligatures", kGroupOptions, true );
	defRange( P_EDGE, "Edge Softness", kGroupOptions, 1.0f, 0.25f, 4.0f );

	raiseVisibilityEvents = false;
	updateVisibility();// part of the initial parameter info
	raiseVisibilityEvents = true;
}

void TextSource::updateVisibility()
{
	// Rows appear only through explicit switches (menus, toggles), never through
	// slider values, so the list never jumps under the mouse while dragging.
	const int src       = source();
	const int disp      = display();
	const bool typed    = src == SOURCE_INPUT || src == SOURCE_CLOCK;// the clock uses the fields as template
	const bool fromFile = src == SOURCE_FILE;
	show( P_TEXT, typed );
	for( int i = 0; i < kExtraLines; ++i )
		show( P_LINE_FIRST + (unsigned)i, typed && i < shownLines );
	show( P_ADD_LINE, typed && shownLines < kExtraLines );
	show( P_REMOVE_LINE, typed && shownLines > 0 );
	show( P_TEXT_FILE, fromFile );
	show( P_CSV_HEADER, fromFile && csvActive );

	const bool clock = src == SOURCE_CLOCK;
	const int tm     = (int)values[ P_TIMER_MODE ];
	const bool timed = tm == TIMER_COUNTDOWN || tm == TIMER_STOPWATCH;
	show( P_TIMER_MODE, clock );
	show( P_TIMER_TARGET, clock && tm == TIMER_COUNTDOWN_TO );
	show( P_TIMER_DURATION, clock && tm == TIMER_COUNTDOWN );
	show( P_TIMER_START, clock && timed );
	show( P_TIMER_STOP, clock && timed );
	show( P_TIMER_RESET, clock && timed );
	show( P_TIMER_AUTOSTART, clock && timed );
	show( P_TIMER_FORMAT, clock && tm != TIMER_COUNTER );
	show( P_TIMER_AT_ZERO, clock && ( tm == TIMER_COUNTDOWN || tm == TIMER_COUNTDOWN_TO ) );
	for( unsigned id : { P_COUNT_FROM, P_COUNT_TO, P_COUNT_PROGRESS, P_COUNT_DECIMALS, P_NUMBER_FORMAT } )
		show( id, clock && tm == TIMER_COUNTER );

	const bool slides = slidesOn();
	const bool replay = slides && values[ P_REPLAY ] > 0.0f;
	show( P_SLIDE, slides );
	show( P_NEXT_SLIDE, slides );
	show( P_PREV_SLIDE, slides );
	show( P_REPLAY, slides );
	show( P_REPLAY_STAGGER, slides );

	const bool ticker = disp == DISPLAY_TICKER, roll = disp == DISPLAY_ROLL;
	show( P_SCROLL_SPEED, ticker || roll );
	show( P_SCROLL_POSITION, ticker || roll );
	show( P_SEPARATOR, ticker );
	show( P_ROLL_LOOP, roll );
	show( P_EDGE_FADE, ticker || roll );
	show( P_SLIDES_LEGACY, false );

	const bool box = (int)values[ P_WRAP ] == 1;
	show( P_BOX_WIDTH, box );
	show( P_BOX_HEIGHT, box );
	show( P_FIT, box );

	for( int l = 0; l < kNumLineStyles; ++l )
	{
		const bool own = (int)values[ lineParam( l, LP_MODE ) ] == 1;
		for( LineParam p : { LP_WEIGHT, LP_FONT, LP_SIZE, LP_COLOR_H, LP_TRACKING, LP_SPACING } )
			show( lineParam( l, p ), own );
	}

	const int fillMode = (int)values[ P_FILL_MODE ];
	show( P_FILL2_H, fillMode != 0 );
	show( P_GRAD_ANGLE, fillMode == 1 );

	const bool plate = (int)values[ P_PLATE ] != PLATE_OFF;
	for( unsigned id : { P_PLATE_H, P_PLATE_PAD_X, P_PLATE_PAD_Y, P_PLATE_RADIUS, P_PLATE_SLANT, P_PLATE_WIDTH, P_PLATE_BORDER, P_PLATE_BORDER_H } )
		show( id, plate );

	for( int a = 0; a < kNumAnimators; ++a )
	{
		const auto mode  = (AnimMode)(int)values[ animParam( a, AP_MODE ) ];
		const bool on    = mode != AnimMode::Off;
		const bool seq   = isSequence( mode );
		const bool waveR = mode == AnimMode::Wave || mode == AnimMode::Random;
		show( animParam( a, AP_TARGET ), on && plate );
		show( animParam( a, AP_UNIT ), on );
		show( animParam( a, AP_ORDER ), seq || mode == AnimMode::Wave );
		show( animParam( a, AP_PROGRESS ), on && !( seq && replay ) );// replay drives sequences itself
		show( animParam( a, AP_SPREAD ), seq || waveR );
		show( animParam( a, AP_HOLD ), mode == AnimMode::SequenceInOut );
		show( animParam( a, AP_EASE ), seq );
		for( AnimParam p : { AP_OFFSET_X, AP_OFFSET_Y, AP_SCALE, AP_ROTATION, AP_OPACITY, AP_WIPE, AP_WIPE_FROM, AP_SCRAMBLE, AP_CHARSET, AP_COLOR_H } )
			show( animParam( a, p ), on );
	}
}

FFResult TextSource::SetFloatParameter( unsigned int index, float value )
{
	if( index >= P_COUNT )
		return FF_FAIL;

	// Choices and positions the plugin also changes itself: only a value the host
	// did not know before is a new user action. Hosts may re-send unchanged
	// values at any time (e.g. while animating other parameters).
	if( index == P_COUNT - 1 && !defaultsDone )
	{
		defaultsDone = true;// the last parameter of the SDK's default pass
		values[ index ] = value;
		hostKnown[ index ] = value;
		return FF_SUCCESS;
	}
	const float previousHost = hostKnown[ index ];
	const bool changed       = hostChanged( index, value );

	if( index == P_FAMILY || index == P_STYLE )
	{
		// Before the menus are filled the stored index means nothing (the font list
		// may have changed since the composition was saved); "Font" decides.
		if( !menusPopulated || !changed )
			return FF_SUCCESS;
		values[ index ] = value;
		if( live )
			index == P_FAMILY ? onFamilyPicked( (int)value ) : onStylePicked( (int)value );
		return FF_SUCCESS;
	}
	for( int l = 0; l < kNumLineStyles; ++l )
		if( index == lineParam( l, LP_WEIGHT ) )
		{
			if( !menusPopulated || !changed )
				return FF_SUCCESS;
			values[ index ] = value;
			if( live )
				onWeightPicked( l, (int)value );
			return FF_SUCCESS;
		}
	for( int a = 0; a < kNumAnimators; ++a )
		if( index == animParam( a, AP_PRESET ) )
		{
			if( !changed )
				return FF_SUCCESS;
			values[ index ] = value;
			if( live && (int)value > 0 )
				applyPreset( a, (int)value );
			return FF_SUCCESS;
		}
	switch( index )
	{
	case P_ADD_LINE:
	case P_REMOVE_LINE:
		if( pressed( index, value ) )
		{
			if( index == P_ADD_LINE )
				showLines( shownLines + 1 );
			else if( shownLines > 0 )
			{
				// The removed field is emptied, so it stays hidden after reloading.
				extraLines[ shownLines - 1 ].clear();
				RaiseParamEvent( P_LINE_FIRST + (unsigned)( shownLines - 1 ), FF_EVENT_FLAG_VALUE );
				showLines( shownLines - 1 );
			}
		}
		return FF_SUCCESS;
	case P_NEXT_SLIDE:
	case P_PREV_SLIDE:
		if( pressed( index, value ) )
			stepSlide( index == P_NEXT_SLIDE ? 1 : -1 );
		return FF_SUCCESS;
	case P_TIMER_START:
		if( pressed( index, value ) )
			timerStart();
		return FF_SUCCESS;
	case P_TIMER_STOP:
		if( pressed( index, value ) )
			timerStop();
		return FF_SUCCESS;
	case P_TIMER_RESET:
		if( pressed( index, value ) )
			timerReset();
		return FF_SUCCESS;
	case P_SLIDE: {
		if( !changed )
			return FF_SUCCESS;// keep a position set by the Next/Previous buttons
		// Resolume's own - / + buttons next to the slider step by 0.01. A single,
		// isolated step of about that size means "previous / next slide"; host
		// animations and dragging send a stream of values and are taken as they are.
		const double now    = wallSeconds();
		const bool isolated = lastSlideHostChange < 0.0 || std::fabs( now - lastSlideHostChange ) > 0.3;
		lastSlideHostChange = now;
		const float delta   = value - previousHost;
		if( isolated && std::isfinite( previousHost ) && std::fabs( delta ) > 0.004f && std::fabs( delta ) < 0.016f && slideCount > 1 )
		{
			values[ P_SLIDE ] = previousHost;
			slideIndex        = slideFromPosition( previousHost, slideCount );
			stepSlide( delta > 0.0f ? 1 : -1 );
			return FF_SUCCESS;
		}
		break;
	}
	case P_SOURCE:
		sourceFromHost = sourceFromHost || defaultsDone;
		break;
	case P_SLIDES_LEGACY:
		// A composition from Text+ 1.0: "Slides" becomes the matching Display.
		values[ index ] = value;
		if( changed && (int)value > 0 && display() == DISPLAY_STATIC )
		{
			setValue( P_DISPLAY, (int)value == 1 ? (float)DISPLAY_SLIDES_LINES : (float)DISPLAY_SLIDES_BLOCKS );
			updateVisibility();
		}
		return FF_SUCCESS;
	default:
		break;
	}

	values[ index ] = value;
	updateVisibility();
	return FF_SUCCESS;
}

FFResult TextSource::SetTextParameter( unsigned int index, const char* value )
{
	std::string v = value ? value : "";
	for( int l = 0; l < kNumLineStyles; ++l )
		if( index == lineParam( l, LP_FONT ) )
		{
			setLineFontText( l, v, false );
			return FF_SUCCESS;
		}
	if( index >= P_LINE_FIRST && index <= P_LINE_LAST )
	{
		const int i     = (int)( index - P_LINE_FIRST );
		extraLines[ i ] = v;
		if( !v.empty() && i + 1 > shownLines )
			showLines( i + 1 );// a saved composition brings its lines back
		return FF_SUCCESS;
	}
	switch( index )
	{
	case P_TEXT:
		text = v;
		return FF_SUCCESS;
	case P_FONT:
		if( v != fontText )
		{
			fontText  = v;
			fontDirty = true;
		}
		return FF_SUCCESS;
	case P_FONT_FILE:
		if( v != fontFile )
		{
			fontFile  = v;
			fontDirty = true;
		}
		return FF_SUCCESS;
	case P_TEXT_FILE:
		if( v != textFile )
		{
			textFile            = v;
			textFileStamp       = -1;
			textFilePollCounter = 0;
			textFileContent.clear();
			// Text+ 1.0 compositions have no "Source": a file there means "show the file".
			if( !v.empty() && !live && !sourceFromHost && source() == SOURCE_INPUT )
			{
				values[ P_SOURCE ] = (float)SOURCE_FILE;
				RaiseParamEvent( P_SOURCE, FF_EVENT_FLAG_VALUE );
				updateVisibility();
			}
		}
		return FF_SUCCESS;
	case P_TIMER_TARGET:
		timerTarget = v;
		return FF_SUCCESS;
	case P_TIMER_DURATION:
		timerDuration = v;
		return FF_SUCCESS;
	case P_SEPARATOR:
		separator = v;
		return FF_SUCCESS;
	default:
		return FF_FAIL;
	}
}

char* TextSource::GetTextParameter( unsigned int index )
{
	if( index >= P_COUNT )
		return nullptr;
	if( index >= P_LINE_FIRST && index <= P_LINE_LAST )
		return returnText( index, extraLines[ index - P_LINE_FIRST ] );
	for( int l = 0; l < kNumLineStyles; ++l )
		if( index == lineParam( l, LP_FONT ) )
			return returnText( index, lineFonts[ l ].text );
	switch( index )
	{
	case P_TEXT: return returnText( index, text );
	case P_FONT: return returnText( index, fontText );
	case P_FONT_FILE: return returnText( index, fontFile );
	case P_TEXT_FILE: return returnText( index, textFile );
	case P_TIMER_TARGET: return returnText( index, timerTarget );
	case P_TIMER_DURATION: return returnText( index, timerDuration );
	case P_SEPARATOR: return returnText( index, separator );
	default: return returnText( index, "" );
	}
}

char* TextSource::GetParameterDisplay( unsigned int index )
{
	if( index >= P_COUNT )
		return nullptr;
	char buf[ 96 ];
	const float v = values[ index ];
	auto isAnim   = [ & ]( AnimParam p ) {
		for( int a = 0; a < kNumAnimators; ++a )
			if( index == animParam( a, p ) )
				return true;
		return false;
	};
	auto isLine = [ & ]( LineParam p ) { return index == lineParam( 0, p ) || index == lineParam( 1, p ); };
	if( ( index >= P_LINE_FIRST && index <= P_LINE_LAST ) || isLine( LP_FONT ) )
		return GetTextParameter( index );
	switch( index )
	{
	case P_TEXT:
	case P_FONT:
	case P_FONT_FILE:
	case P_TEXT_FILE:
	case P_TIMER_TARGET:
	case P_TIMER_DURATION:
	case P_SEPARATOR:
		return GetTextParameter( index );
	case P_SIZE:
	case P_POS_X:
	case P_POS_Y:
	case P_BOX_WIDTH:
	case P_BOX_HEIGHT:
	case P_OL1_WIDTH:
	case P_OL2_WIDTH:
	case P_SH_DISTANCE:
	case P_SH_SOFTNESS:
	case P_SH_SPREAD:
	case P_ISH_DISTANCE:
	case P_ISH_SOFTNESS:
	case P_GL_RADIUS:
	case P_IGL_RADIUS:
	case P_PLATE_BORDER:
	case P_EDGE_FADE:
		std::snprintf( buf, sizeof( buf ), "%.0f px", v );
		break;
	case P_SCROLL_SPEED:
		std::snprintf( buf, sizeof( buf ), "%.0f px/s", v );
		break;
	case P_EDGE:
		std::snprintf( buf, sizeof( buf ), "%.2f px", v );
		break;
	case P_ROTATION:
	case P_SH_ANGLE:
	case P_GRAD_ANGLE:
	case P_PLATE_SLANT:
		std::snprintf( buf, sizeof( buf ), "%.0f\xC2\xB0", v );
		break;
	case P_TRACKING:
		std::snprintf( buf, sizeof( buf ), "%.0f /1000 em", v );
		break;
	case P_LEADING:
	case P_PLATE_PAD_X:
	case P_PLATE_PAD_Y:
	case P_PLATE_RADIUS:
	case P_REPLAY_STAGGER:
		std::snprintf( buf, sizeof( buf ), "%.0f %%", v );
		break;
	case P_SCROLL_POSITION:
	case P_COUNT_PROGRESS:
		std::snprintf( buf, sizeof( buf ), "%.0f %%", v * 100.0f );
		break;
	case P_COUNT_FROM:
	case P_COUNT_TO:
		std::snprintf( buf, sizeof( buf ), "%s", formatNumber( v, (int)values[ P_COUNT_DECIMALS ], (NumberStyle)(int)values[ P_NUMBER_FORMAT ] ).c_str() );
		break;
	case P_SLIDE:
		std::snprintf( buf, sizeof( buf ), "%d / %d", slideFromPosition( v, slideCount ) + 1, slideCount );
		break;
	case P_REPLAY:
		if( v <= 0.0f )
			std::snprintf( buf, sizeof( buf ), "Off" );
		else
			std::snprintf( buf, sizeof( buf ), "%.2g beats", v );
		break;
	default:
		if( isLine( LP_SIZE ) || isLine( LP_SPACING ) )
			std::snprintf( buf, sizeof( buf ), "%.0f %%", v );
		else if( isLine( LP_TRACKING ) )
			std::snprintf( buf, sizeof( buf ), "%+.0f /1000 em", v );
		else if( isAnim( AP_PROGRESS ) )
			std::snprintf( buf, sizeof( buf ), "%.0f %%", v * 100.0f );
		else if( isAnim( AP_OFFSET_X ) || isAnim( AP_OFFSET_Y ) )
			std::snprintf( buf, sizeof( buf ), "%.0f px", v );
		else if( isAnim( AP_SCALE ) || isAnim( AP_OPACITY ) || isAnim( AP_WIPE ) || isAnim( AP_SCRAMBLE ) || isAnim( AP_HOLD ) )
			std::snprintf( buf, sizeof( buf ), "%.0f %%", v );
		else if( isAnim( AP_ROTATION ) )
			std::snprintf( buf, sizeof( buf ), "%.0f\xC2\xB0", v );
		else if( isAnim( AP_SPREAD ) )
		{
			const int a     = index == animParam( 0, AP_SPREAD ) ? 0 : 1;
			const auto mode = (AnimMode)(int)values[ animParam( a, AP_MODE ) ];
			if( mode == AnimMode::Random )
				std::snprintf( buf, sizeof( buf ), "%d states", std::max( 2, (int)std::lround( v ) ) );
			else if( mode == AnimMode::Wave )
				std::snprintf( buf, sizeof( buf ), "%.1f units / wave", v );
			else
				std::snprintf( buf, sizeof( buf ), "%.1f units", v );
		}
		else
		{
			const char* d = CFFGLPlugin::GetParameterDisplay( index );
			std::snprintf( buf, sizeof( buf ), "%s", d ? d : "" );
		}
	}
	return returnDisplay( index, buf );
}

FFResult TextSource::SetTime( double time )
{
	// A jump back in host time means the clip was re-triggered or scrubbed back:
	// replay slide animations, restart tickers and timers that start with the clip.
	if( lastHostTime >= 0.0 && time + 0.05 < lastHostTime )
		retriggered = true;
	lastHostTime = time;
	return CFFGLPlugin::SetTime( time );
}

void TextSource::SetBeatInfo( float bpm, float barPhase )
{
	CFFGLPlugin::SetBeatInfo( bpm, barPhase );
	if( bpm <= 0.0f )
		return;
	if( lastBarPhase >= 0.0f && barPhase + 0.5f < lastBarPhase )
		barCount += 1.0;// bar wrapped
	lastBarPhase = barPhase;
	haveBeatInfo = true;
	beatsNow     = 4.0 * ( barCount + barPhase );
}

double TextSource::currentBeats() const
{
	return haveBeatInfo ? beatsNow : hostTime * 2.0;// 120 BPM when the host sends no beat info
}

double TextSource::wallSeconds() const
{
	return std::chrono::duration< double >( std::chrono::steady_clock::now().time_since_epoch() ).count();
}

// ===========================================================================
// Fonts
// ===========================================================================
void TextSource::setFontText( const std::string& t, bool raiseEvent )
{
	if( t == fontText )
		return;
	fontText  = t;
	fontDirty = true;
	if( raiseEvent )
		RaiseParamEvent( P_FONT, FF_EVENT_FLAG_VALUE );
}

void TextSource::populateFontMenus( bool raiseEvent )
{
	familyMenu = FontDatabase::instance().families();
	if( familyMenu.empty() )
		familyMenu.push_back( "(no fonts found)" );
	std::vector< float > vals( familyMenu.size() );
	for( size_t i = 0; i < vals.size(); ++i )
		vals[ i ] = (float)i;
	SetParamElements( P_FAMILY, familyMenu, vals, raiseEvent );
	menusPopulated = true;
	styleMenuKey.clear();
}

void TextSource::syncMenusToFont()
{
	if( !menusPopulated || !face )
		return;
	const FontEntry& e = face->entry();

	auto famIt = std::find( familyMenu.begin(), familyMenu.end(), e.family );
	if( famIt != familyMenu.end() )
		setValue( P_FAMILY, (float)( famIt - familyMenu.begin() ) );

	const bool fromFile       = !fontFile.empty();
	const std::string menuKey = ( fromFile ? "file:" + fontFile + ":" : "db:" ) + e.family;
	if( menuKey != styleMenuKey )
	{
		styleMenu = fromFile ? FontDatabase::scanFile( fontFile ) : FontDatabase::instance().stylesOf( e.family );
		std::vector< std::string > names;
		std::vector< float > vals;
		for( size_t i = 0; i < styleMenu.size(); ++i )
		{
			names.push_back( fromFile && styleMenu[ i ].family != e.family ? styleMenu[ i ].displayName() : styleMenu[ i ].style );
			vals.push_back( (float)i );
		}
		if( names.empty() )
		{
			names.push_back( e.style );
			vals.push_back( 0.0f );
		}
		SetParamElements( P_STYLE, names, vals, true );
		styleMenuKey = menuKey;
	}
	for( size_t i = 0; i < styleMenu.size(); ++i )
		if( styleMenu[ i ].sameFace( e ) )
		{
			setValue( P_STYLE, (float)i );
			break;
		}
}

void TextSource::onFamilyPicked( int index )
{
	if( index < 0 || index >= (int)familyMenu.size() || !fontFile.empty() )
		return;// with a Font File the file decides; the family menu is informational
	setFontText( familyMenu[ index ], true );// resolve() picks the regular style
}

void TextSource::onStylePicked( int index )
{
	if( index < 0 || index >= (int)styleMenu.size() )
		return;
	setFontText( styleMenu[ index ].displayName(), true );
}

void TextSource::resolveFont()
{
	FontDatabase& db = FontDatabase::instance();
	std::optional< FontEntry > entry;

	if( !fontFile.empty() )
	{
		std::vector< FontEntry > faces = FontDatabase::scanFile( fontFile );
		if( !faces.empty() )
		{
			entry                  = faces.front();
			std::string styleQuery = fontText.find( '/' ) != std::string::npos ? fontText.substr( fontText.find( '/' ) + 1 ) : std::string();
			std::string sn         = normalizeFontName( styleQuery );
			bool found             = false;
			for( const FontEntry& f : faces )
				if( !sn.empty() && normalizeFontName( f.style ) == sn )
				{
					entry = f;
					found = true;
					break;
				}
			if( !found )
				for( const FontEntry& f : faces )
					if( !f.italic && f.weight == 400 )
					{
						entry = f;
						break;
					}
			if( fontText.empty() || fontText.find( '/' ) == std::string::npos )
				setFontText( entry->displayName(), true );
		}
		else
			FFGLLog::LogToHost( ( "Text+: cannot read font file " + fontFile ).c_str() );
	}
	if( !entry )
	{
		if( !db.isReady() )
			return;// try again next frame
		bool matched = false;
		entry        = db.resolve( fontText, &matched );
		if( !entry )
		{
			fontDirty = false;
			face      = nullptr;
			return;
		}
		// Write the canonical "Family / Style" back, but only when the query really
		// matched. A font missing on this machine must not overwrite the saved name.
		if( fontText.empty() || matched )
			setFontText( entry->displayName(), true );
		else
			FFGLLog::LogToHost( ( "Text+: font '" + fontText + "' not found, using " + entry->displayName() ).c_str() );
	}

	if( auto newFace = FontFace::get( *entry ) )
		face = newFace;
	fontDirty = false;
	syncMenusToFont();
	for( LineFont& lf : lineFonts )
		lf.dirty = true;// style names are relative to the main family
}

// ---------------------------------------------------------------------------
// Line styles. "L2 Font" holds "" (main font), a style of the main family
// ("Bold") or a full "Family / Style". A style name follows the main family,
// so switching the main font keeps "name bold, function regular".
// ---------------------------------------------------------------------------
void TextSource::setLineFontText( int l, const std::string& t, bool raiseEvent )
{
	if( t == lineFonts[ l ].text )
		return;
	lineFonts[ l ].text  = t;
	lineFonts[ l ].dirty = true;
	if( raiseEvent )
		RaiseParamEvent( lineParam( l, LP_FONT ), FF_EVENT_FLAG_VALUE );
}

void TextSource::resolveLineFont( int l )
{
	LineFont& lf = lineFonts[ l ];
	if( !face )
		return;
	FontDatabase& db      = FontDatabase::instance();
	const FontEntry& main = face->entry();
	const std::string q   = trimCopy( lf.text );
	const bool fromFile   = !fontFile.empty();
	std::optional< FontEntry > entry;
	std::vector< FontEntry > fileFaces;
	if( fromFile )
		fileFaces = FontDatabase::scanFile( fontFile );

	if( q.empty() )
		entry = main;
	else if( q.find( '/' ) == std::string::npos && fromFile )
	{
		for( const FontEntry& f : fileFaces )
			if( normalizeFontName( f.style ) == normalizeFontName( q ) )
				entry = f;
	}
	if( !entry )
	{
		if( !db.isReady() )
			return;// try again next frame
		const std::string query = q.find( '/' ) == std::string::npos ? main.family + " / " + q : q;
		bool matched            = false;
		auto found              = db.resolve( query, &matched );
		if( found && matched )
			entry = *found;
		else
		{
			entry = main;
			FFGLLog::LogToHost( ( "Text+: line font '" + q + "' not found, using the main font" ).c_str() );
		}
	}
	lf.dirty = false;
	lf.face  = entry->sameFace( main ) ? nullptr : FontFace::get( *entry );

	// "Weight" lists the styles of the line's family.
	if( !menusPopulated )
		return;
	const bool sameFile       = fromFile && entry->path == main.path;
	const std::string menuKey = ( sameFile ? "file:" + fontFile + ":" : "db:" ) + entry->family;
	if( menuKey != lf.weightMenuKey )
	{
		lf.weightMenu = sameFile ? fileFaces : db.stylesOf( entry->family );
		std::vector< std::string > names;
		std::vector< float > vals;
		for( size_t i = 0; i < lf.weightMenu.size(); ++i )
		{
			names.push_back( lf.weightMenu[ i ].style );
			vals.push_back( (float)i );
		}
		if( names.empty() )
		{
			names.push_back( entry->style );
			vals.push_back( 0.0f );
		}
		SetParamElements( lineParam( l, LP_WEIGHT ), names, vals, true );
		lf.weightMenuKey = menuKey;
	}
	for( size_t i = 0; i < lf.weightMenu.size(); ++i )
		if( lf.weightMenu[ i ].sameFace( *entry ) )
		{
			setValue( lineParam( l, LP_WEIGHT ), (float)i );
			break;
		}
}

void TextSource::onWeightPicked( int l, int index )
{
	LineFont& lf = lineFonts[ l ];
	if( index < 0 || index >= (int)lf.weightMenu.size() || !face )
		return;
	const FontEntry& e = lf.weightMenu[ index ];
	// Same family as the main font: store only the style, so it follows the main family.
	setLineFontText( l, e.family == face->entry().family ? e.style : e.displayName(), true );
}

// ===========================================================================
// Text, slides
// ===========================================================================
void TextSource::pollTextFile()
{
	if( textFile.empty() )
		return;
	if( textFilePollCounter-- > 0 )
		return;
	textFilePollCounter = 20;// about three times a second at 60 fps
	const int64_t stamp = fileStamp( textFile );
	if( stamp == textFileStamp )
		return;
	textFileStamp   = stamp;
	textFileContent = stamp >= 0 ? toUtf8( readTextFile( textFile ) ) : std::string();
	if( stamp < 0 )
		FFGLLog::LogToHost( ( "Text+: cannot read text file " + textFile ).c_str() );
}

void TextSource::showLines( int count )
{
	shownLines = std::clamp( count, 0, kExtraLines );
	updateVisibility();
}

void TextSource::stepSlide( int delta )
{
	if( !slidesOn() || slideCount < 1 )
		return;
	// Wraps around at both ends; the Slide slider shows the new position.
	const int next = positiveMod( slideIndex + delta, slideCount );
	setValue( P_SLIDE, slidePosition( next, slideCount ) );
}

std::string TextSource::sourceText()
{
	if( source() == SOURCE_FILE )
	{
		pollTextFile();
		return !textFile.empty() && textFileStamp >= 0 ? textFileContent : std::string();
	}
	std::string typed = text;
	int usedLines     = shownLines;// a freshly added, still empty field adds no blank line
	while( usedLines > 0 && extraLines[ usedLines - 1 ].empty() )
		--usedLines;
	for( int i = 0; i < usedLines; ++i )
		typed += "\n" + extraLines[ i ];
	return typed;
}

std::string TextSource::currentText()
{
	std::string src = sourceText();
	const bool csv  = source() == SOURCE_FILE && !textFile.empty() && textFileStamp >= 0 && isCsvPath( textFile );
	if( csv != csvActive )
	{
		csvActive = csv;
		// A spreadsheet is a list of slides: switch a static display to slides once.
		if( csv && !csvWasActive && display() == DISPLAY_STATIC )
			setValue( P_DISPLAY, (float)DISPLAY_SLIDES_LINES );
		csvWasActive = csvWasActive || csv;
		updateVisibility();
	}

	std::string shown;
	if( csv || slidesOn() )
	{
		const int mode        = display();
		const std::string key = std::to_string( csv ? -1 : mode ) + ( values[ P_PIPE_NEWLINE ] > 0.5f ? "|" : "-" ) + ( values[ P_CSV_HEADER ] > 0.5f ? "h" : "-" ) + src;
		if( key != lastSlidesSource )
		{
			slides           = csv ? csvSlides( parseCsv( src ), values[ P_CSV_HEADER ] > 0.5f ) : splitSlides( src, mode, values[ P_PIPE_NEWLINE ] > 0.5f );
			lastSlidesSource = key;
			if( slides.empty() )
				slides.push_back( std::string() );
		}
		if( slidesOn() )
		{
			slideCount = (int)slides.size();
			slideIndex = slideFromPosition( values[ P_SLIDE ], slideCount );
			shown      = slides[ slideIndex ];
		}
		else
		{
			// A CSV shown statically, as ticker or roll: all rows, a blank line between them
			slideCount = 1;
			slideIndex = 0;
			for( size_t i = 0; i < slides.size(); ++i )
				shown += ( i ? "\n\n" : "" ) + slides[ i ];
		}
	}
	else
	{
		slideCount = 1;
		slideIndex = 0;
		shown      = src;
	}

	if( source() == SOURCE_CLOCK )
	{
		bool hide               = false;
		const std::string value = timerValueText( hide );
		shown                   = hide ? std::string() : expandTimeTemplate( shown, value, localNow() );
	}
	return applyCase( shown, (TextCase)std::clamp( (int)values[ P_CASE ], 0, 2 ) );
}

// ===========================================================================
// Clock & timer
// ===========================================================================
double TextSource::timerElapsed() const
{
	return timerAccumulated + ( timerRunning ? wallSeconds() - timerStartedAt : 0.0 );
}

void TextSource::timerStart()
{
	if( timerRunning )
		return;
	timerRunning   = true;
	timerStartedAt = wallSeconds();
}

void TextSource::timerStop()
{
	if( !timerRunning )
		return;
	timerAccumulated += wallSeconds() - timerStartedAt;
	timerRunning = false;
}

void TextSource::timerReset()
{
	timerAccumulated = 0.0;
	timerStartedAt   = wallSeconds();
}

std::string TextSource::timerValueText( bool& hide )
{
	hide                    = false;
	const auto format       = (TimeFormat)std::clamp( (int)values[ P_TIMER_FORMAT ], 0, 5 );
	const int atZero        = (int)values[ P_TIMER_AT_ZERO ];
	auto remaining          = [ & ]( double rem ) -> std::string {
		if( rem <= 0.0 )
		{
			if( atZero == 1 )
			{
				hide = true;
				return {};
			}
			if( atZero == 2 )
				return formatDuration( rem, format, false );// overtime: "-0:12"
			rem = 0.0;
		}
		return formatDuration( rem, format, true );
	};
	switch( (int)values[ P_TIMER_MODE ] )
	{
	case TIMER_CLOCK:
		return formatTimeOfDay( localNow().secondsOfDay, format );
	case TIMER_COUNTDOWN_TO: {
		double target = 0;
		if( !parseTimeOfDay( timerTarget, target ) )
			return "--:--";
		return remaining( secondsUntil( target, localNow() ) );
	}
	case TIMER_COUNTDOWN: {
		double duration = 0;
		if( !parseDuration( timerDuration, duration ) )
			return "--:--";
		return remaining( duration - timerElapsed() );
	}
	case TIMER_STOPWATCH:
		return formatDuration( timerElapsed(), format, false );
	case TIMER_COUNTER: {
		const double v = values[ P_COUNT_FROM ] + ( (double)values[ P_COUNT_TO ] - values[ P_COUNT_FROM ] ) * std::clamp( values[ P_COUNT_PROGRESS ], 0.0f, 1.0f );
		return formatNumber( v, (int)values[ P_COUNT_DECIMALS ], (NumberStyle)std::clamp( (int)values[ P_NUMBER_FORMAT ], 0, 4 ) );
	}
	default:
		return {};
	}
}

void TextSource::onClipStart()
{
	scrollTime     = 0.0;
	slideStartBeat = currentBeats();
	const int tm   = (int)values[ P_TIMER_MODE ];
	if( values[ P_TIMER_AUTOSTART ] > 0.5f && ( tm == TIMER_COUNTDOWN || tm == TIMER_STOPWATCH ) )
	{
		timerRunning = false;
		timerReset();
		timerStart();
	}
}

// ===========================================================================
// Animation
// ===========================================================================
void TextSource::applyPreset( int a, int preset )
{
	struct P
	{
		AnimMode mode;
		AnimUnit unit;
		AnimEase ease;
		float spread, offX, offY, scale, rot, opacity, colorAlpha;
		float wipe       = 0;
		WipeFrom wipeFrom = WipeFrom::Left;
		float scramble   = 0;
		Charset charset  = Charset::Letters;
		AnimTarget target = AnimTarget::Text;
		float hold       = 50;
	};
	// Each preset describes the "away" state; Progress 0 -> 1 plays it in (or out).
	// Offsets use the UI convention: Offset Y > 0 is up, like Position Y.
	auto make = [ & ]( int id ) -> P {
		switch( id )
		{
		case PRESET_TYPEWRITER: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Linear, 0, 0, 0, 100, 0, 0, 0 };
		case PRESET_FADE_IN: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Smooth, 4, 0, 0, 100, 0, 0, 0 };
		case PRESET_RISE_IN: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::EaseOut, 4, 0, -120, 100, 0, 0, 0 };
		case PRESET_DROP_IN: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Overshoot, 3, 0, 160, 100, 0, 0, 0 };
		case PRESET_POP_IN: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Overshoot, 3, 0, 0, 0, 0, 0, 0 };
		case PRESET_SPIN_IN: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::EaseOut, 4, 0, 0, 0, 180, 0, 0 };
		case PRESET_WORDS_SLIDE_IN: return P{ AnimMode::SequenceIn, AnimUnit::Word, AnimEase::EaseOut, 1.5f, -220, 0, 100, 0, 0, 0 };
		case PRESET_WIPE_IN: {
			P p{ AnimMode::SequenceIn, AnimUnit::Line, AnimEase::EaseOut, 1.5f, 0, 0, 100, 0, 100, 0 };
			p.wipe = 100;
			return p;
		}
		case PRESET_DECODE: {
			P p{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Linear, 8, 0, 0, 100, 0, 100, 0 };
			p.scramble = 100;
			return p;
		}
		case PRESET_FADE_OUT: return P{ AnimMode::SequenceOut, AnimUnit::Character, AnimEase::Smooth, 4, 0, 0, 100, 0, 0, 0 };
		case PRESET_IN_OUT: {
			P p{ AnimMode::SequenceInOut, AnimUnit::Character, AnimEase::Smooth, 4, 0, -40, 100, 0, 0, 0 };
			p.hold = 60;
			return p;
		}
		case PRESET_WAVE: return P{ AnimMode::Wave, AnimUnit::Character, AnimEase::Smooth, 8, 0, 30, 100, 0, 100, 0 };
		case PRESET_JITTER: return P{ AnimMode::Random, AnimUnit::Character, AnimEase::Smooth, 8, 6, 6, 100, 8, 100, 0 };
		case PRESET_GLITCH: {
			P p{ AnimMode::Constant, AnimUnit::Character, AnimEase::Smooth, 3, 0, 0, 100, 0, 100, 0 };
			p.scramble = 12;
			p.charset  = Charset::Symbols;
			return p;
		}
		case PRESET_COLOR_SWEEP: return P{ AnimMode::Wave, AnimUnit::Character, AnimEase::Smooth, 12, 0, 0, 100, 0, 100, 1 };
		default: return P{ AnimMode::Off, AnimUnit::Character, AnimEase::Smooth, 3, 0, 0, 100, 0, 100, 0 };
		}
	};
	auto write = [ & ]( int animator, const P& p ) {
		auto id = [ & ]( AnimParam ap ) { return animParam( animator, ap ); };
		setValue( id( AP_MODE ), (float)p.mode );
		setValue( id( AP_TARGET ), (float)p.target );
		setValue( id( AP_UNIT ), (float)p.unit );
		setValue( id( AP_ORDER ), 0.0f );
		setValue( id( AP_EASE ), (float)p.ease );
		setValue( id( AP_SPREAD ), p.spread );
		setValue( id( AP_HOLD ), p.hold );
		setValue( id( AP_OFFSET_X ), p.offX );
		setValue( id( AP_OFFSET_Y ), p.offY );
		setValue( id( AP_SCALE ), p.scale );
		setValue( id( AP_ROTATION ), p.rot );
		setValue( id( AP_OPACITY ), p.opacity );
		setValue( id( AP_WIPE ), p.wipe );
		setValue( id( AP_WIPE_FROM ), (float)p.wipeFrom );
		setValue( id( AP_SCRAMBLE ), p.scramble );
		setValue( id( AP_CHARSET ), (float)p.charset );
		setValue( id( AP_COLOR_A ), p.colorAlpha );
		if( p.mode == AnimMode::Wave || p.mode == AnimMode::Random )
			setValue( id( AP_PROGRESS ), 0.0f );// the phase; animate it (e.g. BPM sync) to make it move
		else if( p.mode == AnimMode::Constant )
			setValue( id( AP_PROGRESS ), 1.0f );
		else if( p.mode == AnimMode::Off )
			setValue( id( AP_PROGRESS ), 1.0f );
	};

	if( preset == PRESET_LOWER_THIRD_IN || preset == PRESET_LOWER_THIRD_OUT )
	{
		// Plate wipes open from the left, the text follows character by character
		// (and on the way out: text first, then the plate). Sets both animators.
		const bool in = preset == PRESET_LOWER_THIRD_IN;
		P plateP{ in ? AnimMode::SequenceIn : AnimMode::SequenceOut, AnimUnit::Line, AnimEase::EaseOut, 1.5f, 0, 0, 100, 0, 100, 0 };
		plateP.wipe   = 100;
		plateP.target = AnimTarget::Plate;
		P textP{ in ? AnimMode::SequenceIn : AnimMode::SequenceOut, AnimUnit::Character, AnimEase::EaseOut, 5, -30, 0, 100, 0, 0, 0 };
		write( in ? 0 : 1, plateP );
		write( in ? 1 : 0, textP );
		if( (int)values[ P_PLATE ] == PLATE_OFF )
			setValue( P_PLATE, (float)PLATE_LINES );
		if( values[ P_REPLAY_STAGGER ] <= 0.0f )
			setValue( P_REPLAY_STAGGER, 35.0f );
		if( !in )
			for( int k = 0; k < kNumAnimators; ++k )
				setValue( animParam( k, AP_PROGRESS ), 0.0f );
		for( int k = 0; k < kNumAnimators; ++k )
			setValue( animParam( k, AP_PRESET ), 0.0f );
		updateVisibility();
		return;
	}
	if( preset <= PRESET_NONE || preset > PRESET_IN_OUT )
		return;
	write( a, make( preset ) );
	// The menu is a one-shot loader: back to "Load preset..." so loading a
	// composition never re-applies a preset over tweaked values.
	setValue( animParam( a, AP_PRESET ), 0.0f );
	updateVisibility();
}

AnimatorParams TextSource::animatorParams( int a ) const
{
	auto v = [ & ]( AnimParam p ) { return values[ animParam( a, p ) ]; };
	AnimatorParams ap;
	const int mode = (int)v( AP_MODE );
	ap.mode        = mode >= 0 && mode <= (int)AnimMode::SequenceInOut ? (AnimMode)mode : AnimMode::Off;
	ap.unit        = (AnimUnit)std::clamp( (int)v( AP_UNIT ), 0, 2 );
	ap.order       = (AnimOrder)std::clamp( (int)v( AP_ORDER ), 0, 4 );
	ap.ease        = (AnimEase)std::clamp( (int)v( AP_EASE ), 0, 3 );
	ap.progress    = v( AP_PROGRESS );
	if( isSequence( ap.mode ) && slidesOn() && values[ P_REPLAY ] > 0.0f )
	{
		// Played by the plugin on every slide change; Animator 2 may start later.
		const double length = std::max( 0.05f, values[ P_REPLAY ] );
		const double delay  = a == 1 ? length * std::clamp( values[ P_REPLAY_STAGGER ], 0.0f, 100.0f ) / 100.0 : 0.0;
		ap.progress         = (float)std::clamp( ( currentBeats() - slideStartBeat - delay ) / length, 0.0, 1.0 );
	}
	ap.spread      = v( AP_SPREAD );
	ap.hold        = v( AP_HOLD ) / 100.0f;
	ap.offsetX     = v( AP_OFFSET_X );
	ap.offsetY     = -v( AP_OFFSET_Y );// UI: Y up, like Position Y
	ap.scale       = v( AP_SCALE ) / 100.0f;
	ap.rotationDeg = v( AP_ROTATION );
	ap.opacity     = v( AP_OPACITY ) / 100.0f;
	const ffglkit::Rgba c = ffglkit::hsbaToRgba( v( AP_COLOR_H ), v( AP_COLOR_S ), v( AP_COLOR_B ), 1.0f );
	ap.color       = Rgb{ c.r, c.g, c.b };
	ap.colorAmount = v( AP_COLOR_A );
	ap.wipe        = v( AP_WIPE ) / 100.0f;
	ap.wipeFrom    = (WipeFrom)std::clamp( (int)v( AP_WIPE_FROM ), 0, 4 );
	ap.scramble    = v( AP_SCRAMBLE ) / 100.0f;
	ap.charset     = (Charset)std::clamp( (int)v( AP_CHARSET ), 0, 4 );
	ap.beats       = currentBeats();
	ap.seed        = 0x1234u + (unsigned)a * 7919u;
	return ap;
}

// ===========================================================================
// Frame
// ===========================================================================
LayoutParams TextSource::currentLayoutParams( const std::string& t ) const
{
	LayoutParams p;
	p.text             = t;
	p.sizePx           = values[ P_SIZE ];
	p.trackingPermille = values[ P_TRACKING ];
	p.leadingPercent   = values[ P_LEADING ];
	const bool box     = (int)values[ P_WRAP ] == 1;
	p.boxWidth         = box ? std::max( 1.0f, values[ P_BOX_WIDTH ] ) : 0.0f;
	p.boxHeight        = box ? values[ P_BOX_HEIGHT ] : 0.0f;
	p.align            = (Align)std::clamp( (int)values[ P_ALIGN ], 0, 3 );
	p.anchorH          = (AnchorH)std::clamp( (int)values[ P_ANCHOR_H ], 0, 3 );
	p.anchorV          = (AnchorV)std::clamp( (int)values[ P_ANCHOR_V ], 0, 4 );
	p.fit              = box ? (FitMode)std::clamp( (int)values[ P_FIT ], 0, 2 ) : FitMode::Off;
	for( int l = 0; l < kNumLineStyles; ++l )
	{
		LineStyle& ls       = p.lines[ l + 1 ];
		ls.own              = (int)values[ lineParam( l, LP_MODE ) ] == 1;
		ls.face             = lineFonts[ l ].face.get();
		ls.sizePercent      = values[ lineParam( l, LP_SIZE ) ];
		ls.trackingPermille = values[ lineParam( l, LP_TRACKING ) ];
		ls.spacingPercent   = values[ lineParam( l, LP_SPACING ) ];
	}
	p.kerning   = values[ P_KERNING ] > 0.5f;
	p.ligatures = values[ P_LIGATURES ] > 0.5f;
	// With slides the pipe already separated the slides; inside a block it still breaks lines.
	p.pipeIsNewline = values[ P_PIPE_NEWLINE ] > 0.5f;
	p.markup        = values[ P_MARKUP ] > 0.5f;
	return p;
}

Style TextSource::currentStyle() const
{
	auto col = [ & ]( unsigned h ) { return toColor( colorOf( h ) ); };
	Style s;
	s.fill             = col( P_FILL_H );
	s.fill2            = col( P_FILL2_H );
	s.fillMode         = (sdfkit::FillMode)std::clamp( (int)values[ P_FILL_MODE ], 0, 2 );
	s.gradientAngle    = values[ P_GRAD_ANGLE ] * 3.14159265f / 180.0f;
	s.outline1Width    = values[ P_OL1_WIDTH ];
	s.outline1Position = (sdfkit::OutlinePosition)std::clamp( (int)values[ P_OL1_POSITION ], 0, 2 );
	s.sharpCorners     = (int)values[ P_OL_CORNERS ] == 1;
	s.outline1         = col( P_OL1_H );
	s.outline2Width    = values[ P_OL2_WIDTH ];
	s.outline2         = col( P_OL2_H );
	s.shadow           = col( P_SH_H );
	const float dist   = values[ P_SH_DISTANCE ];
	const float ang    = values[ P_SH_ANGLE ] * 3.14159265f / 180.0f;
	s.shadowDx         = dist * std::cos( ang );
	s.shadowDy         = -dist * std::sin( ang );// angle counter-clockwise, screen y down
	s.shadowSoftness   = values[ P_SH_SOFTNESS ];
	s.shadowSpread     = values[ P_SH_SPREAD ];
	if( dist <= 0.0f && s.shadowSpread <= 0.0f )
		s.shadow.a = 0.0f;// fully hidden behind the text: skip the work
	// Inner shadow: same light direction as the drop shadow
	const float iDist     = values[ P_ISH_DISTANCE ];
	s.innerShadow         = col( P_ISH_H );
	s.innerShadowDx       = iDist * std::cos( ang );
	s.innerShadowDy       = -iDist * std::sin( ang );
	s.innerShadowSoftness = values[ P_ISH_SOFTNESS ];
	if( iDist <= 0.0f )
		s.innerShadow.a = 0.0f;// Distance 0 = off
	s.glow            = col( P_GL_H );
	s.glowRadius      = values[ P_GL_RADIUS ];
	s.glowStrength    = values[ P_GL_STRENGTH ];
	s.innerGlow       = col( P_IGL_H );
	s.innerGlowRadius = values[ P_IGL_RADIUS ];
	s.plate           = col( P_PLATE_H );
	s.plateBorderWidth = values[ P_PLATE_BORDER ];
	s.plateBorderColor = col( P_PLATE_BORDER_H );
	s.edgeSoftness    = values[ P_EDGE ];
	return s;
}

FFResult TextSource::InitGL( const FFGLViewportStruct* vp )
{
	CFFGLPlugin::InitGL( vp );
	if( !renderer.init() || !atlas.init() )
	{
		DeInitGL();
		return FF_FAIL;
	}
	return FF_SUCCESS;
}

FFResult TextSource::DeInitGL()
{
	renderer.release();
	atlas.release();
	return FF_SUCCESS;
}

FFResult TextSource::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	FontDatabase& db = FontDatabase::instance();
	db.ensureScanning();
	if( !menusPopulated && db.isReady() )
	{
		populateFontMenus( true );
		fontDirty = true;
	}
	if( fontDirty )
		resolveFont();
	for( int l = 0; l < kNumLineStyles; ++l )
		if( lineFonts[ l ].dirty )
			resolveLineFont( l );

	// Clip start (first frame, or the host time jumped back = re-triggered)
	const double now = wallSeconds();
	if( lastFrameWall < 0.0 || retriggered )
		onClipStart();
	else
		scrollTime += std::clamp( now - lastFrameWall, 0.0, 0.1 );// paused while the clip is not rendered
	lastFrameWall = now;
	retriggered   = false;

	instances.clear();
	plates.clear();
	placed.clear();
	sdfkit::BlockTransform transform;
	sdfkit::Window window;
	Style style = currentStyle();

	if( face )
	{
		const std::string shownText = currentText();
		if( slideCount != reportedSlideCount )
		{
			// The "2 / 5" display depends on the number of slides: let the host re-read it.
			reportedSlideCount = slideCount;
			RaiseParamEvent( P_SLIDE, FF_EVENT_FLAG_VALUE );
		}
		if( slideIndex != lastSlideShown )
		{
			slideStartBeat = currentBeats();
			lastSlideShown = slideIndex;
		}

		const int disp    = display();
		const bool ticker = disp == DISPLAY_TICKER, roll = disp == DISPLAY_ROLL;
		LayoutParams lp   = currentLayoutParams( shownText );
		if( ticker )
		{
			// One line: the lines joined by the separator, plus one separator before the repeat
			std::string joined;
			for( const std::string& para : splitParagraphs( shownText, lp.pipeIsNewline ) )
				if( !trimCopy( para ).empty() )
					joined += ( joined.empty() ? "" : separator ) + para;
			lp.text          = joined.empty() ? joined : joined + separator;
			lp.pipeIsNewline = false;
			lp.boxWidth      = 0.0f;
			lp.fit           = FitMode::Off;
			lp.align         = Align::Left;
			lp.anchorH       = AnchorH::Left;
		}
		if( !layoutValid || lp != lastLayoutParams || face->id() != lastLayoutFaceId )
		{
			layout           = layoutText( *face, lp );
			lastLayoutParams = lp;
			lastLayoutFaceId = face->id();
			layoutValid      = true;
		}
		const float size  = layout.sizePx;
		const float vpW   = (float)currentViewport.width, vpH = (float)currentViewport.height;
		const bool boxOn  = (int)values[ P_WRAP ] == 1;

		// ---- Copies of the laid-out block (ticker repeats, roll loop) ----
		std::vector< std::pair< float, float > > copies;
		if( ticker )
		{
			const float winW = boxOn ? std::max( 1.0f, values[ P_BOX_WIDTH ] ) : vpW;
			window.on        = true;
			window.x0        = -winW * 0.5f;
			window.x1        = winW * 0.5f;
			window.y0        = -1e6f;
			window.y1        = 1e6f;
			window.fade      = values[ P_EDGE_FADE ];
			if( !layout.lines.empty() )
			{
				const LineBox& lb = layout.lines.front();
				const float cycle = lb.penEnd - lb.left;
				if( cycle > 1.0f )
				{
					const double travel = scrollTime * values[ P_SCROLL_SPEED ] + (double)values[ P_SCROLL_POSITION ] * cycle;
					const float o       = (float)positiveFmod( travel, cycle );
					const float start   = window.x0 - o - lb.left;
					const float reach   = style.reach() + size;
					for( int k = 0; k < 400; ++k )
					{
						const float shift = start + k * cycle;
						if( shift + lb.left > window.x1 + reach )
							break;
						copies.emplace_back( shift, 0.0f );
					}
				}
			}
		}
		else if( roll )
		{
			const float winH = boxOn && values[ P_BOX_HEIGHT ] > 0.0f ? values[ P_BOX_HEIGHT ] : vpH;
			window.on        = true;
			window.x0        = -1e6f;
			window.x1        = 1e6f;
			window.y0        = -winH * 0.5f;
			window.y1        = winH * 0.5f;
			window.fade      = values[ P_EDGE_FADE ];
			const float H    = layout.bottom - layout.top;
			const bool loop  = values[ P_ROLL_LOOP ] > 0.5f;
			const float gap  = size * lp.leadingPercent / 100.0f;
			const float cycle = loop ? H + gap : H + winH;
			const double travel = scrollTime * values[ P_SCROLL_SPEED ] + (double)values[ P_SCROLL_POSITION ] * cycle;
			const float reach   = style.reach() + size;
			if( H > 0.0f && cycle > 1.0f )
			{
				if( loop )
				{
					const float o = (float)positiveFmod( travel, cycle );
					// Block tops at window bottom - o - k * cycle, for every copy that can show
					for( int k = -1; k < 400; ++k )
					{
						const float top = window.y1 - o - k * cycle;
						if( top + H < window.y0 - reach )
							break;
						if( top > window.y1 + reach )
							continue;
						copies.emplace_back( 0.0f, top - layout.top );
					}
				}
				else
				{
					const float o   = (float)std::clamp( travel, 0.0, (double)cycle );
					const float top = window.y1 - o;
					if( top + H > window.y0 - reach )
						copies.emplace_back( 0.0f, top - layout.top );
				}
			}
		}
		else
			copies.emplace_back( 0.0f, 0.0f );

		// ---- Glyphs as drawn, with the items the animators move ----
		const float textMargin = style.reach();
		std::vector< AnimItem > items;
		for( const auto& c : copies )
		{
			const size_t first = placed.size();
			for( LaidGlyph g : layout.glyphs )
			{
				g.x += c.first;
				g.y += c.second;
				placed.push_back( g );
			}
			std::vector< LaidGlyph > copyGlyphs( placed.begin() + (long)first, placed.end() );
			std::vector< AnimItem > copyItems = glyphItems( copyGlyphs, layout.lines, textMargin );
			items.insert( items.end(), copyItems.begin(), copyItems.end() );
		}

		// Per-glyph state: line colours, inline colours (win), character gradient, animators.
		glyphStates.assign( placed.size(), GlyphState{} );
		Rgb lineColor[ kLineStyles ];
		float lineColorAmount[ kLineStyles ] = { 0, 0, 0 };
		for( int l = 0; l < kNumLineStyles; ++l )
		{
			const unsigned h            = lineParam( l, LP_COLOR_H );
			const ffglkit::Rgba c       = ffglkit::hsbaToRgba( values[ h ], values[ h + 1 ], values[ h + 2 ], 1.0f );
			lineColor[ l + 1 ]          = Rgb{ c.r, c.g, c.b };
			lineColorAmount[ l + 1 ]    = values[ h + 3 ];
		}
		for( size_t i = 0; i < placed.size(); ++i )
		{
			const LaidGlyph& g = placed[ i ];
			if( g.style > 0 && lineColorAmount[ g.style ] > 0.0f )
				layerTint( glyphStates[ i ], lineColor[ g.style ], lineColorAmount[ g.style ] );
			if( g.colorIndex >= 0 && g.colorIndex < (int)layout.palette.size() )
				layerTint( glyphStates[ i ], layout.palette[ g.colorIndex ], 1.0f );
		}
		const float lastIdx = (float)std::max( 1, layout.numChars - 1 );
		if( style.fillMode == sdfkit::FillMode::GradientChars )
			for( size_t i = 0; i < glyphStates.size(); ++i )
				if( glyphStates[ i ].tintAmount <= 0.0f )// inline colours win
				{
					const float t = placed[ i ].charIndex / lastIdx;
					layerTint( glyphStates[ i ], Rgb{ style.fill.r + ( style.fill2.r - style.fill.r ) * t, style.fill.g + ( style.fill2.g - style.fill.g ) * t,
													 style.fill.b + ( style.fill2.b - style.fill.b ) * t },
							   1.0f );
				}

		// ---- Plates ----
		struct PlateRect
		{
			float x0, y0, x1, y1;
		};
		std::vector< PlateRect > plateRects;
		const int plateMode = (int)values[ P_PLATE ];
		if( plateMode != PLATE_OFF && !layout.lines.empty() )
		{
			const float padX  = values[ P_PLATE_PAD_X ] / 100.0f * size;
			const float padY  = values[ P_PLATE_PAD_Y ] / 100.0f * size;
			const int widthMode = (int)values[ P_PLATE_WIDTH ];
			float inkL = 1e30f, inkR = -1e30f;
			const LineBox* firstLine = nullptr;
			const LineBox* lastLine  = nullptr;
			for( const LineBox& lb : layout.lines )
				if( !lb.empty() )
				{
					inkL      = std::min( inkL, lb.left );
					inkR      = std::max( inkR, lb.right );
					firstLine = firstLine ? firstLine : &lb;
					lastLine  = &lb;
				}
			if( firstLine )
			{
				const bool boxWidth = widthMode == 2 && ( boxOn || ticker );
				const float fullL   = boxWidth ? ( ticker ? window.x0 : layout.left ) : inkL;
				const float fullR   = boxWidth ? ( ticker ? window.x1 : layout.right ) : inkR;
				if( ticker )
				{
					// One band across the ticker window
					plateRects.push_back( PlateRect{ window.x0, firstLine->top - padY, window.x1, firstLine->bottom + padY } );
				}
				else
					for( const auto& c : copies )
					{
						if( plateMode == PLATE_BLOCK )
						{
							const float l = widthMode == 0 ? inkL : fullL, r = widthMode == 0 ? inkR : fullR;
							plateRects.push_back( PlateRect{ l - padX + c.first, firstLine->top - padY + c.second, r + padX + c.first, lastLine->bottom + padY + c.second } );
						}
						else
							for( const LineBox& lb : layout.lines )
							{
								if( lb.empty() )
									continue;
								const float l = widthMode == 0 ? lb.left : widthMode == 1 ? inkL : fullL;
								const float r = widthMode == 0 ? lb.right : widthMode == 1 ? inkR : fullR;
								plateRects.push_back( PlateRect{ l - padX + c.first, lb.top - padY + c.second, r + padX + c.first, lb.bottom + padY + c.second } );
							}
					}
			}
		}
		std::vector< AnimItem > plateItems( plateRects.size() );
		for( size_t i = 0; i < plateRects.size(); ++i )
		{
			AnimItem& it = plateItems[ i ];
			it.charIndex = it.wordIndex = it.lineIndex = (int)i;
			it.charBox = it.wordBox = it.lineBox = UnitBox{ plateRects[ i ].x0, plateRects[ i ].y0, plateRects[ i ].x1, plateRects[ i ].y1 };
		}
		plateStates.assign( plateRects.size(), GlyphState{} );

		// ---- Animators ----
		const UnitCounts counts = unitCounts( layout );
		const UnitCounts plateCounts{ (int)plateRects.size(), (int)plateRects.size(), (int)plateRects.size() };
		for( int a = 0; a < kNumAnimators; ++a )
		{
			const AnimatorParams ap = animatorParams( a );
			const auto target       = plateRects.empty() ? AnimTarget::Text : (AnimTarget)std::clamp( (int)values[ animParam( a, AP_TARGET ) ], 0, 2 );
			if( target != AnimTarget::Plate )
			{
				applyAnimator( items, counts, ap, glyphStates );
			}
			if( target != AnimTarget::Text && !plateRects.empty() )
			{
				AnimatorParams pp = ap;
				pp.scramble       = 0.0f;
				applyAnimator( plateItems, plateCounts, pp, plateStates );
			}
		}

		// Faces and distance-field levels per line style: smaller lines need a wider
		// spread (in em) for the same effect width in pixels.
		std::shared_ptr< FontFace > styleFace[ kLineStyles ] = { face, face, face };
		int styleLevel[ kLineStyles ];
		for( int st = 0; st < kLineStyles; ++st )
		{
			if( st > 0 && lineFonts[ st - 1 ].face )
				styleFace[ st ] = lineFonts[ st - 1 ].face;
			const float scale = st == 0 ? 1.0f : std::max( 0.05f, values[ lineParam( st - 1, LP_SIZE ) ] / 100.0f );
			styleLevel[ st ]  = spreadLevelFor( style.reach() / std::max( 1.0f, size * scale ) );
		}
		bool atlasFull    = false;
		GlyphCache& cache = GlyphCache::instance();
		float minReachPx  = 1.0e9f;

		auto slotFor = [ & ]( const std::shared_ptr< FontFace >& gf, uint32_t gid, int level ) -> const sdfkit::AtlasSlot* {
			const sdfkit::AtlasSlot* slot = atlas.find( atlasKey( gf->id(), gid, level ) );
			if( !slot )
			{
				if( auto bmp = cache.get( gf, gid, level ) )
				{
					if( bmp->empty )
						return nullptr;
					sdfkit::FieldBitmapView view;
					view.width       = bmp->width;
					view.height      = bmp->height;
					view.rgba        = bmp->rgba.data();
					view.planeLeft   = bmp->planeLeft;
					view.planeBottom = bmp->planeBottom;
					view.planeRight  = bmp->planeRight;
					view.planeTop    = bmp->planeTop;
					view.unitPx      = (float)spreadLevel( bmp->level ).emPx;
					view.level       = bmp->level;
					slot             = atlas.add( atlasKey( gf->id(), gid, level ), view );
					if( !slot )
						atlasFull = true;
				}
			}
			// While the wanted spread level is being generated, use any level we already have.
			for( int l = kNumSpreadLevels - 1; !slot && l >= 0; --l )
				slot = atlas.find( atlasKey( gf->id(), gid, l ) );
			return slot;
		};
		auto faceOf = [ & ]( const LaidGlyph& g ) -> std::shared_ptr< FontFace > {
			if( g.face )
				return const_cast< FontFace* >( g.face )->shared_from_this();
			return styleFace[ std::clamp( g.style, 0, kLineStyles - 1 ) ];
		};
		auto scrambleGlyph = [ & ]( const std::shared_ptr< FontFace >& gf, uint32_t cp ) -> uint32_t {
			const uint64_t key = ( (uint64_t)gf->id() << 32 ) | cp;
			auto it            = scrambleGlyphs.find( key );
			if( it != scrambleGlyphs.end() )
				return it->second;
			hb_codepoint_t gid = 0;
			hb_font_get_nominal_glyph( gf->hbFont(), cp, &gid );
			scrambleGlyphs[ key ] = (uint32_t)gid;
			return (uint32_t)gid;
		};

		instances.reserve( placed.size() );
		for( size_t i = 0; i < placed.size(); ++i )
		{
			const LaidGlyph& g   = placed[ i ];
			const GlyphState& st = glyphStates[ i ];
			if( st.alpha <= 0.001f || st.scale <= 0.001f )
				continue;
			if( st.clipX0 >= st.clipX1 || st.clipY0 >= st.clipY1 )
				continue;// wiped away completely
			const int gs                       = std::clamp( g.style, 0, kLineStyles - 1 );
			const std::shared_ptr< FontFace > realFace = faceOf( g );
			const int level                    = styleLevel[ gs ];
			const float glyphScale             = st.scale * g.scale;

			// Scramble: a random character centred on the real one's advance
			std::shared_ptr< FontFace > gf = realFace;
			uint32_t gid                   = g.glyphId;
			float shiftX                   = 0.0f;
			if( st.scramble != 0 )
			{
				if( st.charset == Charset::SameAsText )
				{
					const LaidGlyph& pick = layout.glyphs[ st.scramble % layout.glyphs.size() ];
					gf                    = faceOf( pick );
					gid                   = pick.glyphId;
				}
				else
				{
					const std::vector< uint32_t >& cps = charsetCodepoints( st.charset );
					const uint32_t cand                = cps.empty() ? 0 : scrambleGlyph( realFace, cps[ st.scramble % cps.size() ] );
					if( cand )
						gid = cand;
				}
				if( gid != g.glyphId || gf != realFace )
				{
					const float adv = hb_font_get_glyph_h_advance( gf->hbFont(), gid ) / (float)gf->unitsPerEm() * size * g.scale;
					shiftX          = ( g.advance - adv ) * 0.5f;
				}
			}
			const sdfkit::AtlasSlot* slot = slotFor( gf, gid, level );
			if( !slot && ( gid != g.glyphId || gf != realFace ) )
			{
				// The random character is not ready yet: show the real one this frame
				shiftX = 0.0f;
				slot   = slotFor( realFace, g.glyphId, level );
			}
			if( !slot )
				continue;

			// The field of this glyph reaches spreadEm * its size in px.
			const SpreadLevel& L = spreadLevel( slot->level );
			minReachPx           = std::min( minReachPx, L.spreadEm * size * std::max( 0.05f, glyphScale ) );
			ShapeInstance inst;
			inst.quadL         = slot->planeLeft;
			inst.quadB         = slot->planeBottom;
			inst.quadR         = slot->planeRight;
			inst.quadT         = slot->planeTop;
			inst.u0            = slot->u0;
			inst.v0            = slot->v0;
			inst.u1            = slot->u1;
			inst.v1            = slot->v1;
			const float c = std::cos( st.rotation ), s = std::sin( st.rotation );
			inst.x             = g.x + st.dx + shiftX * st.scale * c;
			inst.y             = g.y + st.dy + shiftX * st.scale * s;
			inst.scale         = glyphScale;
			inst.rotation      = st.rotation;
			inst.alpha         = st.alpha;
			inst.index         = g.charIndex / lastIdx;
			inst.pxPerAtlasPx  = size * glyphScale / L.emPx;
			inst.spreadAtlasPx = (float)L.spreadPx;
			inst.tintR         = st.tint.r;
			inst.tintG         = st.tint.g;
			inst.tintB         = st.tint.b;
			inst.tintAmount    = st.tintAmount;
			inst.restX         = g.x + shiftX;
			inst.restY         = g.y;
			inst.restScale     = g.scale;
			inst.clipX0        = st.clipX0;
			inst.clipY0        = st.clipY0;
			inst.clipX1        = st.clipX1;
			inst.clipY1        = st.clipY1;
			instances.push_back( inst );
		}

		// Plates as boxes
		const float radius = values[ P_PLATE_RADIUS ] / 100.0f * size;
		const float slant  = std::tan( std::clamp( values[ P_PLATE_SLANT ], -60.0f, 60.0f ) * 3.14159265f / 180.0f );
		for( size_t i = 0; i < plateRects.size(); ++i )
		{
			const PlateRect& r   = plateRects[ i ];
			const GlyphState& st = plateStates[ i ];
			if( st.alpha <= 0.001f || st.scale <= 0.001f || st.clipX0 >= st.clipX1 || st.clipY0 >= st.clipY1 )
				continue;
			const float hw = 0.5f * ( r.x1 - r.x0 ), hh = 0.5f * ( r.y1 - r.y0 );
			if( hw <= 0.0f || hh <= 0.0f )
				continue;
			const float cx = 0.5f * ( r.x0 + r.x1 ), cy = 0.5f * ( r.y0 + r.y1 );
			const float margin = style.plateReach() + 2.0f;
			ShapeInstance b;
			b.quadL      = -( hw + std::fabs( slant ) * hh + margin );
			b.quadR      = hw + std::fabs( slant ) * hh + margin;
			b.quadB      = -( hh + margin );
			b.quadT      = hh + margin;
			b.x          = cx + st.dx;
			b.y          = cy + st.dy;
			b.scale      = st.scale;
			b.rotation   = st.rotation;
			b.alpha      = st.alpha;
			b.tintR      = st.tint.r;
			b.tintG      = st.tint.g;
			b.tintB      = st.tint.b;
			b.tintAmount = st.tintAmount;
			b.restX      = cx;
			b.restY      = cy;
			b.restScale  = 1.0f;
			b.clipX0     = st.clipX0;
			b.clipY0     = st.clipY0;
			b.clipX1     = st.clipX1;
			b.clipY1     = st.clipY1;
			b.boxHalfW   = hw;
			b.boxHalfH   = hh;
			b.boxRadius  = radius;
			b.boxSlant   = slant;
			plates.push_back( b );
		}

		// Queue the rest of Latin-1 behind the visible glyphs, so typing new text
		// (and scrambling, whose characters are ASCII) rarely waits for glyph generation.
		for( int st = 0; st < kLineStyles; ++st )
		{
			const std::shared_ptr< FontFace >& wf = styleFace[ st ];
			const uint64_t warmKey                = ( (uint64_t)wf->id() << 8 ) | (uint64_t)styleLevel[ st ];
			if( std::find( prewarmed.begin(), prewarmed.end(), warmKey ) != prewarmed.end() )
				continue;
			prewarmed.push_back( warmKey );
			if( prewarmed.size() > 16 )
				prewarmed.erase( prewarmed.begin() );
			for( hb_codepoint_t cp = 0x21; cp <= 0xFF; ++cp )
			{
				hb_codepoint_t gid = 0;
				if( ( cp < 0x7F || cp > 0xA0 ) && hb_font_get_nominal_glyph( wf->hbFont(), cp, &gid ) )
					cache.get( wf, gid, styleLevel[ st ], false );
			}
		}
		if( atlasFull )
			atlas.clear();// rebuilt from the CPU cache over the next frames

		// The field reaches spreadEm * glyph size; effects beyond that are clamped.
		if( minReachPx < 1.0e8f )
			style.clampTo( minReachPx );
		transform.sizePx    = size;
		transform.boxLeft   = layout.left;
		transform.boxTop    = layout.top;
		transform.boxRight  = layout.right;
		transform.boxBottom = layout.bottom;
	}

	transform.originX  = currentViewport.width * 0.5f + values[ P_POS_X ];
	transform.originY  = currentViewport.height * 0.5f - values[ P_POS_Y ];// Position Y up
	transform.rotation = values[ P_ROTATION ] * 3.14159265f / 180.0f;

	renderer.render( currentViewport, pGL->HostFBO, instances, plates, transform, style, window, atlas.texture(), atlas.size() );

	if( !live )
	{
		live = true;
		updateVisibility();
	}
	return FF_SUCCESS;
}

}//namespace textplus
