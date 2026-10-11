#include "TextSource.h"

#include "core/Csv.h"
#include "core/GlyphCache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include <hb.h>

namespace textplus
{
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
							"KTXT",                      // unique ID, kept from 0.1 so saved compositions still find the plugin
							"Text+",                     // name (sorts next to Resolume's own Text sources)
							2,                           // API major
							1,                           // API minor
							0,                           // plugin major
							2,                           // plugin minor
							FF_SOURCE,
							"Text source with distance-field outlines, shadows, glow and per-character animation",
							"Text+ 0.6",
							0, nullptr, initialiseLibrary, nullptr );

const char* kGroupText     = "Text";
const char* kGroupFont     = "Font";
const char* kGroupLayout   = "Layout";
const char* kGroupFill     = "Fill";
const char* kGroupOutline  = "Outline";
const char* kGroupOutline2 = "Outline 2";
const char* kGroupShadow   = "Shadow";
const char* kGroupGlow     = "Glow";
const char* kGroupAnim[ kNumAnimators ] = { "Animator 1", "Animator 2" };
const char* kGroupQuality  = "Quality";

enum SlidesMode
{
	SLIDES_OFF,
	SLIDES_LINES,
	SLIDES_BLOCKS
};

const std::vector< const char* > kPresetNames = { "Load preset...", "Typewriter", "Fade In", "Rise In", "Drop In", "Pop In", "Spin In",
												  "Words Slide In", "Fade Out", "Wave", "Jitter", "Color Sweep", "Reset" };

Color hsbaToColor( float h, float s, float v, float a )
{
	h = h - std::floor( h );
	float r = v, g = v, b = v;
	if( s > 0 )
	{
		float hh = h * 6.0f;
		int i    = (int)hh % 6;
		float f  = hh - std::floor( hh );
		float p = v * ( 1 - s ), q = v * ( 1 - s * f ), t = v * ( 1 - s * ( 1 - f ) );
		switch( i )
		{
		case 0: r = v; g = t; b = p; break;
		case 1: r = q; g = v; b = p; break;
		case 2: r = p; g = v; b = t; break;
		case 3: r = p; g = q; b = v; break;
		case 4: r = t; g = p; b = v; break;
		default: r = v; g = p; b = q; break;
		}
	}
	return Color{ r, g, b, a };
}

int64_t fileStamp( const std::string& path )
{
	struct stat st;
	if( stat( path.c_str(), &st ) != 0 )
		return -1;
#if defined( __APPLE__ )
	return (int64_t)st.st_mtimespec.tv_sec * 1000 + st.st_mtimespec.tv_nsec / 1000000 + (int64_t)st.st_size * 31;
#else
	return (int64_t)st.st_mtime * 1000 + (int64_t)st.st_size * 31;
#endif
}

std::string readTextFile( const std::string& path )
{
	std::ifstream in( path, std::ios::binary );
	if( !in )
		return {};
	std::stringstream ss;
	ss << in.rdbuf();
	std::string s = ss.str();
	if( s.size() >= 3 && (unsigned char)s[ 0 ] == 0xEF && (unsigned char)s[ 1 ] == 0xBB && (unsigned char)s[ 2 ] == 0xBF )
		s.erase( 0, 3 );// UTF-8 BOM
	while( !s.empty() && ( s.back() == '\n' || s.back() == '\r' ) )
		s.pop_back();
	return s;
}

std::string trimCopy( const std::string& s )
{
	size_t a = s.find_first_not_of( " \t\r\n" );
	if( a == std::string::npos )
		return {};
	size_t b = s.find_last_not_of( " \t\r\n" );
	return s.substr( a, b - a + 1 );
}

// Splits the source text into slides.
//  Each Line: every line (newline or "|" when "| = New Line" is on) is a slide.
//  Blocks:    "||" or an empty line separates slides; single breaks stay line breaks.
std::vector< std::string > splitSlides( const std::string& src, int mode, bool pipeIsNewline )
{
	std::vector< std::string > out;
	if( mode == SLIDES_LINES )
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
}//namespace

// Slide position 0..1 -> slide index: n equal shares, 1.0 belongs to the last slide.
static int slideFromPosition( float position, int count )
{
	if( count <= 1 )
		return 0;
	const float p = std::clamp( position, 0.0f, 1.0f );
	return std::min( count - 1, (int)std::floor( p * (float)count ) );
}

// Centre of a slide's share, so rounding never lands on a neighbour.
static float slidePosition( int index, int count )
{
	return count <= 1 ? 0.0f : ( (float)index + 0.5f ) / (float)count;
}

// ===========================================================================
// Parameters
// ===========================================================================
TextSource::TextSource()
{
	std::fill( std::begin( hostKnown ), std::end( hostKnown ), std::nanf( "" ) );
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
	visibility.assign( P_COUNT, -1 );

	auto named = [ & ]( unsigned id, const std::string& internal, const char* display ) {
		if( display && internal != display )
			SetParamDisplayName( id, display, false );
	};
	auto range = [ & ]( unsigned id, const std::string& name, const char* group, float def, float lo, float hi, const char* display = nullptr ) {
		values[ id ] = def;
		SetParamInfo( id, name.c_str(), FF_TYPE_STANDARD, def );
		if( ParamInfo* info = FindParamInfo( id ) )// SetParamInfo clamps defaults of standard params to 0..1
			info->defaultFloatVal = def;
		SetParamRange( id, lo, hi );
		SetParamGroup( id, group );
		named( id, name, display );
	};
	auto boolean = [ & ]( unsigned id, const std::string& name, const char* group, bool def, const char* display = nullptr ) {
		values[ id ] = def ? 1.0f : 0.0f;
		SetParamInfo( id, name.c_str(), FF_TYPE_BOOLEAN, def );
		SetParamGroup( id, group );
		named( id, name, display );
	};
	auto event = [ & ]( unsigned id, const std::string& name, const char* group ) {
		values[ id ] = 0.0f;
		SetParamInfo( id, name.c_str(), FF_TYPE_EVENT, false );
		SetParamGroup( id, group );
	};
	auto option = [ & ]( unsigned id, const std::string& name, const char* group, const std::vector< const char* >& items, int def, const char* display = nullptr ) {
		values[ id ] = (float)def;
		SetOptionParamInfo( id, name.c_str(), (unsigned)items.size(), (float)def );
		for( unsigned i = 0; i < items.size(); ++i )
			SetParamElementInfo( id, i, items[ i ], (float)i );
		SetParamGroup( id, group );
		named( id, name, display );
	};
	auto color = [ & ]( unsigned h, const std::string& name, const char* group, float hv, float sv, float bv, float av, const char* display = nullptr ) {
		const float v[ 4 ]      = { hv, sv, bv, av };
		const unsigned types[ 4 ] = { FF_TYPE_HUE, FF_TYPE_SATURATION, FF_TYPE_BRIGHTNESS, FF_TYPE_ALPHA };
		const char* suffix[ 4 ]   = { "", "_Sat", "_Bri", "_Alpha" };
		for( unsigned i = 0; i < 4; ++i )
		{
			values[ h + i ] = v[ i ];
			SetParamInfo( h + i, ( name + suffix[ i ] ).c_str(), types[ i ], v[ i ] );
			SetParamGroup( h + i, group );
		}
		named( h, name, display );// the hue parameter names the whole colour
	};

	// Text
	text = "Text+";
	SetParamInfo( P_TEXT, "Text", FF_TYPE_TEXT, text.c_str() );
	SetParamGroup( P_TEXT, kGroupText );
	// Resolume's text fields are single-line: further lines get their own fields,
	// revealed one by one with "+ Line" (hidden fields keep their saved values).
	for( int i = 0; i < kExtraLines; ++i )
	{
		const unsigned id = P_LINE_FIRST + (unsigned)i;
		SetParamInfo( id, ( "Line " + std::to_string( i + 2 ) ).c_str(), FF_TYPE_TEXT, "" );
		SetParamGroup( id, kGroupText );
	}
	event( P_ADD_LINE, "+ Line", kGroupText );
	event( P_REMOVE_LINE, "- Line", kGroupText );
	boolean( P_PIPE_NEWLINE, "| = New Line", kGroupText, true );
	boolean( P_MARKUP, "Color Tags", kGroupText, true );
	SetFileParamInfo( P_TEXT_FILE, "Text File", { "txt", "text", "md", "csv", "tsv" }, "" );
	SetParamGroup( P_TEXT_FILE, kGroupText );
	boolean( P_CSV_HEADER, "CSV: Skip First Row", kGroupText, true );
	option( P_SLIDES, "Slides", kGroupText, { "Off", "Each Line", "Blocks (|| or empty line)" }, SLIDES_OFF );
	// 0..1 split into equal shares: BPM sync on this slider cycles through all
	// slides and wraps back to the first, whatever the number of slides.
	range( P_SLIDE, "Slide", kGroupText, 0.0f, 0.0f, 1.0f );
	event( P_NEXT_SLIDE, "Next Slide", kGroupText );
	event( P_PREV_SLIDE, "Previous Slide", kGroupText );
	range( P_REPLAY, "Replay on Slide Change", kGroupText, 0.0f, 0.0f, 16.0f );

	// Font
	SetParamInfo( P_FONT, "Font", FF_TYPE_TEXT, "" );
	SetParamGroup( P_FONT, kGroupFont );
	option( P_FAMILY, "Family", kGroupFont, { "(scanning fonts...)" }, 0 );
	option( P_STYLE, "Style", kGroupFont, { "-" }, 0 );
	SetFileParamInfo( P_FONT_FILE, "Font File", { "ttf", "otf", "ttc", "otc" }, "" );
	SetParamGroup( P_FONT_FILE, kGroupFont );
	range( P_SIZE, "Size", kGroupFont, 160.0f, 4.0f, 2000.0f );

	// Layout
	range( P_POS_X, "Position X", kGroupLayout, 0.0f, -4000.0f, 4000.0f );
	range( P_POS_Y, "Position Y", kGroupLayout, 0.0f, -4000.0f, 4000.0f );
	range( P_ROTATION, "Rotation", kGroupLayout, 0.0f, -360.0f, 360.0f );
	option( P_ALIGN, "Align", kGroupLayout, { "Left", "Center", "Right", "Justify" }, 1 );
	option( P_ANCHOR_H, "Anchor H", kGroupLayout, { "Auto", "Left", "Center", "Right" }, 0 );
	option( P_ANCHOR_V, "Anchor V", kGroupLayout, { "Top", "Middle", "Bottom", "First Baseline", "Last Baseline" }, 1 );
	range( P_BOX_WIDTH, "Box Width", kGroupLayout, 0.0f, 0.0f, 8000.0f );
	range( P_BOX_HEIGHT, "Box Height", kGroupLayout, 0.0f, 0.0f, 8000.0f );
	option( P_FIT, "Auto Fit", kGroupLayout, { "Off", "Shrink to Box", "Fit Box Width" }, 0 );
	range( P_TRACKING, "Tracking", kGroupLayout, 0.0f, -300.0f, 1000.0f );
	range( P_LEADING, "Leading", kGroupLayout, 120.0f, 50.0f, 300.0f );
	boolean( P_KERNING, "Kerning", kGroupLayout, true );
	boolean( P_LIGATURES, "Ligatures", kGroupLayout, true );

	// Fill
	option( P_FILL_MODE, "Fill Mode", kGroupFill, { "Solid", "Gradient (Block)", "Gradient (Characters)" }, 0 );
	color( P_FILL_H, "Fill Color", kGroupFill, 0.0f, 0.0f, 1.0f, 1.0f );
	color( P_FILL2_H, "Fill Color 2", kGroupFill, 0.58f, 0.7f, 1.0f, 1.0f );
	range( P_GRAD_ANGLE, "Gradient Angle", kGroupFill, 0.0f, -180.0f, 180.0f );

	// Outline 1
	range( P_OL1_WIDTH, "Outline Width", kGroupOutline, 0.0f, 0.0f, 400.0f );
	option( P_OL1_POSITION, "Outline Position", kGroupOutline, { "Outside", "Center", "Inside" }, 0 );
	option( P_OL_CORNERS, "Outline Corners", kGroupOutline, { "Round", "Sharp" }, 0 );
	color( P_OL1_H, "Outline Color", kGroupOutline, 0.0f, 0.0f, 0.0f, 1.0f );

	// Outline 2
	range( P_OL2_WIDTH, "Outline 2 Width", kGroupOutline2, 0.0f, 0.0f, 400.0f );
	color( P_OL2_H, "Outline 2 Color", kGroupOutline2, 0.0f, 0.0f, 1.0f, 1.0f );

	// Shadow (Distance 0 = hidden behind the text, so the default is "off" without a toggle)
	color( P_SH_H, "Shadow Color", kGroupShadow, 0.0f, 0.0f, 0.0f, 0.6f );
	range( P_SH_DISTANCE, "Shadow Distance", kGroupShadow, 0.0f, 0.0f, 400.0f );
	range( P_SH_ANGLE, "Shadow Angle", kGroupShadow, -45.0f, -180.0f, 180.0f );
	range( P_SH_SOFTNESS, "Shadow Softness", kGroupShadow, 8.0f, 0.0f, 200.0f );
	range( P_SH_SPREAD, "Shadow Spread", kGroupShadow, 0.0f, 0.0f, 200.0f );

	// Glow (Radius 0 = off)
	color( P_GL_H, "Glow Color", kGroupGlow, 0.55f, 0.8f, 1.0f, 1.0f );
	range( P_GL_RADIUS, "Glow Radius", kGroupGlow, 0.0f, 0.0f, 400.0f );
	range( P_GL_STRENGTH, "Glow Strength", kGroupGlow, 1.0f, 0.0f, 2.0f );

	// Animators. Internal names are unique ("A1 Mode"), the UI shows short display names.
	for( int a = 0; a < kNumAnimators; ++a )
	{
		const std::string pre = "A" + std::to_string( a + 1 ) + " ";
		const char* g         = kGroupAnim[ a ];
		auto id               = [ & ]( AnimParam p ) { return animParam( a, p ); };
		option( id( AP_PRESET ), pre + "Preset", g, kPresetNames, 0, "Preset" );
		option( id( AP_MODE ), pre + "Mode", g, { "Off", "Sequence In", "Sequence Out", "Wave", "Random", "Constant" }, 0, "Mode" );
		option( id( AP_UNIT ), pre + "Unit", g, { "Character", "Word", "Line" }, 0, "Unit" );
		option( id( AP_ORDER ), pre + "Order", g, { "Forward", "Reverse", "From Center", "From Edges", "Random" }, 0, "Order" );
		range( id( AP_PROGRESS ), pre + "Progress", g, 1.0f, 0.0f, 1.0f, "Progress" );
		range( id( AP_SPREAD ), pre + "Spread", g, 3.0f, 0.0f, 50.0f, "Spread" );
		option( id( AP_EASE ), pre + "Ease", g, { "Linear", "Smooth", "Ease Out", "Overshoot" }, 1, "Ease" );
		range( id( AP_OFFSET_X ), pre + "Offset X", g, 0.0f, -2000.0f, 2000.0f, "Offset X" );
		range( id( AP_OFFSET_Y ), pre + "Offset Y", g, 0.0f, -2000.0f, 2000.0f, "Offset Y" );
		range( id( AP_SCALE ), pre + "Scale", g, 100.0f, 0.0f, 400.0f, "Scale" );
		range( id( AP_ROTATION ), pre + "Rotation", g, 0.0f, -720.0f, 720.0f, "Rotation" );
		range( id( AP_OPACITY ), pre + "Opacity", g, 100.0f, 0.0f, 100.0f, "Opacity" );
		color( id( AP_COLOR_H ), pre + "Color", g, 0.0f, 0.8f, 1.0f, 0.0f, "Color (Alpha = Amount)" );
	}

	// Quality
	range( P_EDGE, "Edge Softness", kGroupQuality, 1.0f, 0.25f, 4.0f );

	updateVisibility( false );
}

void TextSource::setValue( unsigned id, float v )
{
	if( id >= P_COUNT || values[ id ] == v )
		return;
	values[ id ] = v;
	RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
}

void TextSource::updateVisibility( bool raiseEvent )
{
	auto show = [ & ]( unsigned id, bool v ) {
		signed char want = v ? 1 : 0;
		if( visibility[ id ] == want )
			return;
		visibility[ id ] = want;
		SetParamVisibility( id, v, raiseEvent );
	};
	const bool box = values[ P_BOX_WIDTH ] > 0.0f;
	show( P_BOX_HEIGHT, box );
	show( P_FIT, box );

	for( int i = 0; i < kExtraLines; ++i )
		show( P_LINE_FIRST + (unsigned)i, i < shownLines );
	show( P_ADD_LINE, shownLines < kExtraLines );
	show( P_REMOVE_LINE, shownLines > 0 );

	const bool slidesOn = csvActive || (int)values[ P_SLIDES ] != SLIDES_OFF;
	const bool replay   = slidesOn && values[ P_REPLAY ] > 0.0f;
	show( P_CSV_HEADER, csvActive );
	show( P_SLIDES, !csvActive );// a CSV is always one slide per row
	show( P_SLIDE, slidesOn );
	show( P_NEXT_SLIDE, slidesOn );
	show( P_PREV_SLIDE, slidesOn );
	show( P_REPLAY, slidesOn );

	const int fillMode = (int)values[ P_FILL_MODE ];
	show( P_FILL2_H, fillMode != 0 );
	show( P_GRAD_ANGLE, fillMode == 1 );

	for( int a = 0; a < kNumAnimators; ++a )
	{
		const auto mode  = (AnimMode)(int)values[ animParam( a, AP_MODE ) ];
		const bool on    = mode != AnimMode::Off;
		const bool seq   = mode == AnimMode::SequenceIn || mode == AnimMode::SequenceOut;
		const bool waveR = mode == AnimMode::Wave || mode == AnimMode::Random;
		show( animParam( a, AP_UNIT ), on );
		show( animParam( a, AP_ORDER ), seq || mode == AnimMode::Wave );
		show( animParam( a, AP_PROGRESS ), on && !( seq && replay ) );// replay drives sequences itself
		show( animParam( a, AP_SPREAD ), seq || waveR );
		show( animParam( a, AP_EASE ), seq );
		for( AnimParam p : { AP_OFFSET_X, AP_OFFSET_Y, AP_SCALE, AP_ROTATION, AP_OPACITY, AP_COLOR_H } )
			show( animParam( a, p ), on );
	}
}

bool TextSource::hostChanged( unsigned index, float value )
{
	const bool changed = !( value == hostKnown[ index ] );
	hostKnown[ index ] = value;
	return changed;
}

FFResult TextSource::SetFloatParameter( unsigned int index, float value )
{
	if( index >= P_COUNT )
		return FF_FAIL;

	// Choices and positions the plugin also changes itself: only a value the host
	// did not know before is a new user action. Hosts may re-send unchanged
	// values at any time (e.g. while animating other parameters).
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
	if( index == P_ADD_LINE || index == P_REMOVE_LINE )
	{
		if( value > 0.5f && values[ index ] <= 0.5f )
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
		values[ index ] = value;
		return FF_SUCCESS;
	}
	if( index == P_NEXT_SLIDE || index == P_PREV_SLIDE )
	{
		if( value > 0.5f && values[ index ] <= 0.5f )
			stepSlide( index == P_NEXT_SLIDE ? 1 : -1 );
		values[ index ] = value;
		return FF_SUCCESS;
	}
	if( index == P_SLIDE )
	{
		if( !changed )
			return FF_SUCCESS;// keep a position set by the Next/Previous buttons
		// Resolume's own - / + buttons next to the slider step by 0.01. A single,
		// isolated step of about that size means "previous / next slide"; host
		// animations and dragging send a stream of values and are taken as they are.
		const double now    = std::chrono::duration< double >( std::chrono::steady_clock::now().time_since_epoch() ).count();
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
	}

	values[ index ] = value;
	updateVisibility( true );
	return FF_SUCCESS;
}

FFResult TextSource::SetTextParameter( unsigned int index, const char* value )
{
	std::string v = value ? value : "";
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
		}
		return FF_SUCCESS;
	default:
		return FF_FAIL;
	}
}

float TextSource::GetFloatParameter( unsigned int index )
{
	if( index >= P_COUNT )
		return 0.0f;
	hostKnown[ index ] = values[ index ];// the host now knows this value
	return values[ index ];
}

char* TextSource::GetTextParameter( unsigned int index )
{
	if( index >= P_COUNT )
		return nullptr;
	std::string& out = textBuffers[ index ];
	if( index >= P_LINE_FIRST && index <= P_LINE_LAST )
	{
		out = extraLines[ index - P_LINE_FIRST ];
		return const_cast< char* >( out.c_str() );
	}
	switch( index )
	{
	case P_TEXT:
		out = text;
		break;
	case P_FONT:
		out = fontText;
		break;
	case P_FONT_FILE:
		out = fontFile;
		break;
	case P_TEXT_FILE:
		out = textFile;
		break;
	default:
		out.clear();
		break;
	}
	return const_cast< char* >( out.c_str() );
}

char* TextSource::GetParameterDisplay( unsigned int index )
{
	char buf[ 64 ];
	const float v = index < P_COUNT ? values[ index ] : 0.0f;
	auto isAnim   = [ & ]( AnimParam p ) {
		for( int a = 0; a < kNumAnimators; ++a )
			if( index == animParam( a, p ) )
				return true;
		return false;
	};
	switch( index )
	{
	case P_TEXT:
	case P_FONT:
	case P_FONT_FILE:
	case P_TEXT_FILE:
		return GetTextParameter( index );
	case P_LINE_FIRST + 0:
	case P_LINE_FIRST + 1:
	case P_LINE_FIRST + 2:
	case P_LINE_FIRST + 3:
	case P_LINE_FIRST + 4:
	case P_LINE_FIRST + 5:
	case P_LINE_FIRST + 6:
	case P_LINE_FIRST + 7:
	case P_LINE_FIRST + 8:
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
	case P_GL_RADIUS:
		std::snprintf( buf, sizeof( buf ), "%.0f px", v );
		break;
	case P_EDGE:
		std::snprintf( buf, sizeof( buf ), "%.2f px", v );
		break;
	case P_ROTATION:
	case P_SH_ANGLE:
	case P_GRAD_ANGLE:
		std::snprintf( buf, sizeof( buf ), "%.0f\xC2\xB0", v );
		break;
	case P_TRACKING:
		std::snprintf( buf, sizeof( buf ), "%.0f /1000 em", v );
		break;
	case P_LEADING:
		std::snprintf( buf, sizeof( buf ), "%.0f %%", v );
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
		if( isAnim( AP_PROGRESS ) )
			std::snprintf( buf, sizeof( buf ), "%.0f %%", v * 100.0f );
		else if( isAnim( AP_OFFSET_X ) || isAnim( AP_OFFSET_Y ) )
			std::snprintf( buf, sizeof( buf ), "%.0f px", v );
		else if( isAnim( AP_SCALE ) || isAnim( AP_OPACITY ) )
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
	if( index >= P_COUNT )
		return nullptr;
	displayBuffers[ index ] = buf;
	return const_cast< char* >( displayBuffers[ index ].c_str() );
}

FFResult TextSource::SetTime( double time )
{
	// A jump back in host time means the clip was re-triggered or scrubbed back:
	// replay the slide-driven animations from the start.
	if( lastHostTime >= 0.0 && time + 0.05 < lastHostTime )
		slideStartBeat = currentBeats();
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
	updateVisibility( true );
}

void TextSource::stepSlide( int delta )
{
	if( (int)values[ P_SLIDES ] == SLIDES_OFF || slideCount < 1 )
		return;
	// Wraps around at both ends; the Slide slider shows the new position.
	const int next = positiveMod( slideIndex + delta, slideCount );
	setValue( P_SLIDE, slidePosition( next, slideCount ) );
}

std::string TextSource::currentText()
{
	pollTextFile();
	const bool fromFile = !textFile.empty() && textFileStamp >= 0;
	std::string typed   = text;
	int usedLines       = shownLines;// a freshly added, still empty field adds no blank line
	while( usedLines > 0 && extraLines[ usedLines - 1 ].empty() )
		--usedLines;
	for( int i = 0; i < usedLines; ++i )
		typed += "\n" + extraLines[ i ];
	const std::string& source = fromFile ? textFileContent : typed;
	const bool csv            = fromFile && isCsvPath( textFile );
	if( csv != csvActive )
	{
		csvActive = csv;
		updateVisibility( true );
	}
	const int mode = csv ? -1 : (int)values[ P_SLIDES ];
	if( mode == SLIDES_OFF )
	{
		slideCount = 1;
		slideIndex = 0;
		return source;
	}
	const std::string key = std::to_string( mode ) + ( values[ P_PIPE_NEWLINE ] > 0.5f ? "|" : "-" ) + ( values[ P_CSV_HEADER ] > 0.5f ? "h" : "-" ) + source;
	if( key != lastSlidesSource )
	{
		slides           = csv ? csvSlides( parseCsv( source ), values[ P_CSV_HEADER ] > 0.5f ) : splitSlides( source, mode, values[ P_PIPE_NEWLINE ] > 0.5f );
		lastSlidesSource = key;
	}
	slideCount = (int)slides.size();
	slideIndex = slideFromPosition( values[ P_SLIDE ], slideCount );
	return slides[ slideIndex ];
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
	};
	// Each preset describes the "away" state; Progress 0 -> 1 plays it in (or out).
	// Offsets use the UI convention: Offset Y > 0 is up, like Position Y.
	static const P presets[] = {
		/* 0 unused       */ { AnimMode::Off, AnimUnit::Character, AnimEase::Smooth, 3, 0, 0, 100, 0, 100, 0 },
		/* Typewriter     */ { AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Linear, 0, 0, 0, 100, 0, 0, 0 },
		/* Fade In        */ { AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Smooth, 4, 0, 0, 100, 0, 0, 0 },
		/* Rise In        */ { AnimMode::SequenceIn, AnimUnit::Character, AnimEase::EaseOut, 4, 0, -120, 100, 0, 0, 0 },
		/* Drop In        */ { AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Overshoot, 3, 0, 160, 100, 0, 0, 0 },
		/* Pop In         */ { AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Overshoot, 3, 0, 0, 0, 0, 0, 0 },
		/* Spin In        */ { AnimMode::SequenceIn, AnimUnit::Character, AnimEase::EaseOut, 4, 0, 0, 0, 180, 0, 0 },
		/* Words Slide In */ { AnimMode::SequenceIn, AnimUnit::Word, AnimEase::EaseOut, 1.5f, -220, 0, 100, 0, 0, 0 },
		/* Fade Out       */ { AnimMode::SequenceOut, AnimUnit::Character, AnimEase::Smooth, 4, 0, 0, 100, 0, 0, 0 },
		/* Wave           */ { AnimMode::Wave, AnimUnit::Character, AnimEase::Smooth, 8, 0, 30, 100, 0, 100, 0 },
		/* Jitter         */ { AnimMode::Random, AnimUnit::Character, AnimEase::Smooth, 8, 6, 6, 100, 8, 100, 0 },
		/* Color Sweep    */ { AnimMode::Wave, AnimUnit::Character, AnimEase::Smooth, 12, 0, 0, 100, 0, 100, 1 },
		/* Reset          */ { AnimMode::Off, AnimUnit::Character, AnimEase::Smooth, 3, 0, 0, 100, 0, 100, 0 },
	};
	if( preset <= 0 || preset >= (int)( sizeof( presets ) / sizeof( presets[ 0 ] ) ) )
		return;
	const P& p  = presets[ preset ];
	auto id     = [ & ]( AnimParam ap ) { return animParam( a, ap ); };
	setValue( id( AP_MODE ), (float)p.mode );
	setValue( id( AP_UNIT ), (float)p.unit );
	setValue( id( AP_ORDER ), 0.0f );
	setValue( id( AP_EASE ), (float)p.ease );
	setValue( id( AP_SPREAD ), p.spread );
	setValue( id( AP_OFFSET_X ), p.offX );
	setValue( id( AP_OFFSET_Y ), p.offY );
	setValue( id( AP_SCALE ), p.scale );
	setValue( id( AP_ROTATION ), p.rot );
	setValue( id( AP_OPACITY ), p.opacity );
	setValue( id( AP_COLOR_A ), p.colorAlpha );
	if( p.mode == AnimMode::Wave || p.mode == AnimMode::Random )
		setValue( id( AP_PROGRESS ), 0.0f );// the phase; animate it (e.g. BPM sync) to make it move
	else if( p.mode == AnimMode::Off )
		setValue( id( AP_PROGRESS ), 1.0f );
	// The menu is a one-shot loader: back to "Load preset..." so loading a
	// composition never re-applies a preset over tweaked values.
	setValue( id( AP_PRESET ), 0.0f );
	updateVisibility( true );
}

AnimatorParams TextSource::animatorParams( int a ) const
{
	auto v = [ & ]( AnimParam p ) { return values[ animParam( a, p ) ]; };
	AnimatorParams ap;
	ap.mode        = (AnimMode)std::clamp( (int)v( AP_MODE ), 0, 5 );
	ap.unit        = (AnimUnit)std::clamp( (int)v( AP_UNIT ), 0, 2 );
	ap.order       = (AnimOrder)std::clamp( (int)v( AP_ORDER ), 0, 4 );
	ap.ease        = (AnimEase)std::clamp( (int)v( AP_EASE ), 0, 3 );
	ap.progress    = v( AP_PROGRESS );
	const bool seq = ap.mode == AnimMode::SequenceIn || ap.mode == AnimMode::SequenceOut;
	const bool slidesOn = csvActive || (int)values[ P_SLIDES ] != SLIDES_OFF;
	if( seq && slidesOn && values[ P_REPLAY ] > 0.0f )// played by the plugin on every slide change
		ap.progress = (float)std::clamp( ( currentBeats() - slideStartBeat ) / std::max( 0.05f, values[ P_REPLAY ] ), 0.0, 1.0 );
	ap.spread      = v( AP_SPREAD );
	ap.offsetX     = v( AP_OFFSET_X );
	ap.offsetY     = -v( AP_OFFSET_Y );// UI: Y up, like Position Y
	ap.scale       = v( AP_SCALE ) / 100.0f;
	ap.rotationDeg = v( AP_ROTATION );
	ap.opacity     = v( AP_OPACITY ) / 100.0f;
	Color c        = hsbaToColor( v( AP_COLOR_H ), v( AP_COLOR_S ), v( AP_COLOR_B ), 1.0f );
	ap.color       = Rgb{ c.r, c.g, c.b };
	ap.colorAmount = v( AP_COLOR_A );
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
	p.boxWidth         = values[ P_BOX_WIDTH ];
	p.boxHeight        = values[ P_BOX_WIDTH ] > 0 ? values[ P_BOX_HEIGHT ] : 0.0f;
	p.align            = (Align)std::clamp( (int)values[ P_ALIGN ], 0, 3 );
	p.anchorH          = (AnchorH)std::clamp( (int)values[ P_ANCHOR_H ], 0, 3 );
	p.anchorV          = (AnchorV)std::clamp( (int)values[ P_ANCHOR_V ], 0, 4 );
	p.fit              = values[ P_BOX_WIDTH ] > 0 ? (FitMode)std::clamp( (int)values[ P_FIT ], 0, 2 ) : FitMode::Off;
	p.kerning          = values[ P_KERNING ] > 0.5f;
	p.ligatures        = values[ P_LIGATURES ] > 0.5f;
	// With slides the pipe already separated the slides; inside a block it still breaks lines.
	p.pipeIsNewline = values[ P_PIPE_NEWLINE ] > 0.5f;
	p.markup        = values[ P_MARKUP ] > 0.5f;
	return p;
}

Style TextSource::currentStyle() const
{
	auto col = [ & ]( unsigned h ) { return hsbaToColor( values[ h ], values[ h + 1 ], values[ h + 2 ], values[ h + 3 ] ); };
	Style s;
	s.fill             = col( P_FILL_H );
	s.fill2            = col( P_FILL2_H );
	s.fillMode         = (FillMode)std::clamp( (int)values[ P_FILL_MODE ], 0, 2 );
	s.gradientAngle    = values[ P_GRAD_ANGLE ] * 3.14159265f / 180.0f;
	s.outline1Width    = values[ P_OL1_WIDTH ];
	s.outline1Position = (OutlinePosition)std::clamp( (int)values[ P_OL1_POSITION ], 0, 2 );
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
	s.glow                = col( P_GL_H );
	s.glowRadius          = values[ P_GL_RADIUS ];
	s.glowStrength        = values[ P_GL_STRENGTH ];
	s.edgeSoftness        = values[ P_EDGE ];
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

	instances.clear();
	BlockTransform transform;
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

		LayoutParams lp = currentLayoutParams( shownText );
		if( !layoutValid || lp != lastLayoutParams || face->id() != lastLayoutFaceId )
		{
			layout           = layoutText( *face, lp );
			lastLayoutParams = lp;
			lastLayoutFaceId = face->id();
			layoutValid      = true;
		}

		// Per-glyph state: inline colours, character gradient, then the animators.
		initGlyphStates( layout, glyphStates );
		const float lastIdx = (float)std::max( 1, layout.numChars - 1 );
		if( style.fillMode == FillMode::GradientChars )
			for( size_t i = 0; i < glyphStates.size(); ++i )
				if( glyphStates[ i ].tintAmount <= 0.0f )// inline colours win
				{
					const float t = layout.glyphs[ i ].charIndex / lastIdx;
					layerTint( glyphStates[ i ], Rgb{ style.fill.r + ( style.fill2.r - style.fill.r ) * t, style.fill.g + ( style.fill2.g - style.fill.g ) * t,
													 style.fill.b + ( style.fill2.b - style.fill.b ) * t },
							   1.0f );
				}
		float maxScale = 1.0f;
		for( int a = 0; a < kNumAnimators; ++a )
		{
			const AnimatorParams ap = animatorParams( a );
			applyAnimator( layout, ap, glyphStates );
			maxScale *= maxAnimatedScale( ap );
		}

		const float size    = layout.sizePx;
		const int level     = spreadLevelFor( style.reach() / std::max( 1.0f, size ) );
		int minLevelUsed    = level;
		bool atlasFull      = false;
		GlyphCache& cache   = GlyphCache::instance();
		float minGlyphScale = 1.0f;

		instances.reserve( layout.glyphs.size() );
		for( size_t i = 0; i < layout.glyphs.size(); ++i )
		{
			const LaidGlyph& g   = layout.glyphs[ i ];
			const GlyphState& st = glyphStates[ i ];
			if( st.alpha <= 0.001f || st.scale <= 0.001f )
				continue;

			const AtlasSlot* slot = atlas.find( atlasKey( face->id(), g.glyphId, level ) );
			if( !slot )
			{
				if( auto bmp = cache.get( face, g.glyphId, level ) )
				{
					if( bmp->empty )
						continue;
					slot = atlas.add( atlasKey( face->id(), g.glyphId, level ), *bmp );
					if( !slot )
						atlasFull = true;
				}
			}
			// While the wanted spread level is being generated, use any level we already have.
			for( int l = kNumSpreadLevels - 1; !slot && l >= 0; --l )
				slot = atlas.find( atlasKey( face->id(), g.glyphId, l ) );
			if( !slot )
				continue;

			minLevelUsed         = std::min( minLevelUsed, slot->level );
			minGlyphScale        = std::min( minGlyphScale, st.scale );
			const SpreadLevel& L = spreadLevel( slot->level );
			GlyphInstance inst;
			inst.planeL        = slot->planeLeft;
			inst.planeB        = slot->planeBottom;
			inst.planeR        = slot->planeRight;
			inst.planeT        = slot->planeTop;
			inst.u0            = slot->u0;
			inst.v0            = slot->v0;
			inst.u1            = slot->u1;
			inst.v1            = slot->v1;
			inst.x             = g.x + st.dx;
			inst.y             = g.y + st.dy;
			inst.scale         = st.scale;
			inst.rotation      = st.rotation;
			inst.alpha         = st.alpha;
			inst.index         = g.charIndex / lastIdx;
			inst.pxPerAtlasPx  = size * st.scale / L.emPx;
			inst.spreadAtlasPx = (float)L.spreadPx;
			inst.tintR         = st.tint.r;
			inst.tintG         = st.tint.g;
			inst.tintB         = st.tint.b;
			inst.tintAmount    = st.tintAmount;
			instances.push_back( inst );
		}

		// Queue the rest of Latin-1 behind the visible glyphs, so typing new text
		// rarely has to wait for glyph generation.
		const uint64_t warmKey = ( (uint64_t)face->id() << 8 ) | (uint64_t)level;
		if( warmKey != prewarmedKey )
		{
			prewarmedKey = warmKey;
			for( hb_codepoint_t cp = 0x21; cp <= 0xFF; ++cp )
			{
				hb_codepoint_t gid = 0;
				if( ( cp < 0x7F || cp > 0xA0 ) && hb_font_get_nominal_glyph( face->hbFont(), cp, &gid ) )
					cache.get( face, gid, level, false );
			}
		}
		if( atlasFull )
			atlas.clear();// rebuilt from the CPU cache over the next frames

		// The field reaches spreadEm * glyph size; effects beyond that are clamped.
		style.clampTo( spreadLevel( minLevelUsed ).spreadEm * size * std::max( 0.05f, minGlyphScale ) );
		(void)maxScale;
		transform.sizePx    = size;
		transform.boxLeft   = layout.left;
		transform.boxTop    = layout.top;
		transform.boxRight  = layout.right;
		transform.boxBottom = layout.bottom;
	}

	transform.originX  = currentViewport.width * 0.5f + values[ P_POS_X ];
	transform.originY  = currentViewport.height * 0.5f - values[ P_POS_Y ];// Position Y up
	transform.rotation = values[ P_ROTATION ] * 3.14159265f / 180.0f;

	renderer.render( currentViewport, pGL->HostFBO, instances, transform, style, atlas.texture(), atlas.size() );

	if( !live )
	{
		live = true;
		updateVisibility( true );
	}
	return FF_SUCCESS;
}

}//namespace textplus
