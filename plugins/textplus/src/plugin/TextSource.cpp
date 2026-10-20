#include "TextSource.h"

#include "core/Csv.h"
#include "core/FileUtil.h"
#include "core/GlyphCache.h"
#include "core/Osc.h"
#include "core/Placeholders.h"
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

#if defined( TEXTPLUS_EFFECT )
CFFGLPluginInfo PluginInfo( PluginFactory< TextSource >,// create method
							"KTXF",                      // unique FFGL plugin ID
							"Text+ Fill",                // name
							2,                           // API major
							1,                           // API minor
							2,                           // plugin major
							0,                           // plugin minor
							FF_EFFECT,
							"Text+ as an effect: text over the clip, text filled with the clip, or the clip with the text cut out",
							"Text+ Fill " TEXTPLUS_VERSION,
							0, nullptr, initialiseLibrary, nullptr );
#else
CFFGLPluginInfo PluginInfo( PluginFactory< TextSource >,// create method
							"KTXT",                      // unique FFGL plugin ID
							"Text+",                     // name (sorts next to Resolume's own Text sources)
							2,                           // API major
							1,                           // API minor
							2,                           // plugin major
							0,                           // plugin minor
							FF_SOURCE,
							"Text source with distance-field outlines, plates, 3D, patterns, paths, live data and per-character animation",
							"Text+ " TEXTPLUS_VERSION,
							0, nullptr, initialiseLibrary, nullptr );
#endif

// Splits the source text into slides.
//  Lines:  every line (newline or "|" when "| = New Line" is on) is a slide.
//  Blocks: "||" or an empty line separates slides; single breaks stay line breaks.
//  Words:  every word is a slide.
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
	if( mode == DISPLAY_SLIDES_LINES || mode == DISPLAY_SLIDES_WORDS )
	{
		const bool words = mode == DISPLAY_SLIDES_WORDS;
		std::string cur;
		for( size_t i = 0; i < src.size(); ++i )
		{
			char c       = src[ i ];
			bool isBreak = c == '\n' || c == '\r' || ( c == '|' && pipeIsNewline ) || ( c == '\\' && i + 1 < src.size() && src[ i + 1 ] == 'n' ) ||
				( words && ( c == ' ' || c == '\t' ) );
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

Color toColor( const ffglkit::Rgba& c )
{
	return Color{ c.r, c.g, c.b, c.a };
}

bool isSequence( AnimMode m )
{
	return m == AnimMode::SequenceIn || m == AnimMode::SequenceOut || m == AnimMode::SequenceInOut;
}

uint32_t mix32( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

// A fixed shuffle of n slides, the same on every platform for the same texts.
std::vector< int > shuffledOrder( const std::vector< std::string >& slides )
{
	uint32_t seed = 0x9E3779B9u;
	for( const std::string& s : slides )
		for( unsigned char c : s )
			seed = mix32( seed ^ c ) + 0x632BE5ABu;
	std::vector< int > order( slides.size() );
	for( size_t i = 0; i < order.size(); ++i )
		order[ i ] = (int)i;
	for( size_t i = order.size(); i > 1; --i )
	{
		seed         = mix32( seed + (uint32_t)i );
		const size_t j = seed % i;
		std::swap( order[ i - 1 ], order[ j ] );
	}
	return order;
}
}//namespace

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

#if defined( TEXTPLUS_EFFECT )
bool TextSource::isEffect() { return true; }
#else
bool TextSource::isEffect() { return false; }
#endif

// ===========================================================================
// Construction, host time
// ===========================================================================
TextSource::TextSource() :
	ParamPlugin( P_COUNT )
{
	m_isConnected = false;// left uninitialised by the SDK
	SetMinInputs( isEffect() ? 1 : 0 );
	SetMaxInputs( isEffect() ? 1 : 0 );
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

FFResult TextSource::SetTime( double time )
{
	// A jump back in host time means the clip was re-triggered or scrubbed back:
	// replay slide animations, restart tickers and timers that start with the clip.
	if( lastHostTime >= 0.0 && time + 0.05 < lastHostTime )
		retriggered = true;
	else if( lastHostTime >= 0.0 )
		hostDelta += time - lastHostTime;
	lastHostTime = time;
	++hostTimeCalls;
	return CFFGLPlugin::SetTime( time );
}

void TextSource::SetBeatInfo( float bpm, float barPhase )
{
	CFFGLPlugin::SetBeatInfo( bpm, barPhase );
	if( bpm <= 0.0f )
		return;
	hostBpm = bpm;
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
// Text, slides, live values
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
	const int next = positiveMod( slideTarget + delta, slideCount );
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

std::string TextSource::expandLive( const std::string& t )
{
	if( t.find( '{' ) == std::string::npos )
		return t;
	PlaceholderContext ctx;
	ctx.now    = localNow();
	ctx.slide  = slideIndex + 1;
	ctx.slides = slideCount;
	ctx.bpm    = hostBpm > 0.0f ? hostBpm : 120.0f;
	ctx.osc    = osc.get();
	return expandPlaceholders( t, ctx );
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
		const bool shuffle    = (int)values[ P_SLIDE_ORDER ] == 1;
		const std::string key = std::to_string( csv ? -1 : mode ) + ( values[ P_PIPE_NEWLINE ] > 0.5f ? "|" : "-" ) + ( values[ P_CSV_HEADER ] > 0.5f ? "h" : "-" ) +
			( shuffle ? "s" : "o" ) + ( csv ? rowTemplate : std::string() ) + "\x1f" + src;
		if( key != lastSlidesSource )
		{
			slides = csv ? csvSlides( parseCsv( src ), values[ P_CSV_HEADER ] > 0.5f, rowTemplate ) : splitSlides( src, mode, values[ P_PIPE_NEWLINE ] > 0.5f );
			if( csv && mode == DISPLAY_SLIDES_WORDS )
			{
				// Words of every row, one after another
				std::vector< std::string > words;
				for( const std::string& s : slides )
					for( const std::string& w : splitSlides( s, DISPLAY_SLIDES_WORDS, true ) )
						if( !w.empty() )
							words.push_back( w );
				slides = words;
			}
			lastSlidesSource = key;
			if( slides.empty() )
				slides.push_back( std::string() );
			if( shuffle )
				slideOrder = shuffledOrder( slides );
			else
			{
				slideOrder.resize( slides.size() );
				for( size_t i = 0; i < slideOrder.size(); ++i )
					slideOrder[ i ] = (int)i;
			}
		}
		if( slidesOn() )
		{
			slideCount  = (int)slides.size();
			slideTarget = slideFromPosition( values[ P_SLIDE ], slideCount );
			// "Out, then In": a new slide first lets the shown one play its animation
			// backwards, then appears and plays in.
			const bool outFirst = live && values[ P_REPLAY ] > 0.0f && (int)values[ P_REPLAY_MODE ] == 1 && lastSlideShown >= 0;
			if( !outFirst || slideIndex >= slideCount )
			{
				slideIndex    = slideTarget;
				slideOutPhase = false;
			}
			else if( slideTarget != slideIndex )
			{
				if( !slideOutPhase )
				{
					slideOutPhase     = true;
					slideOutStartBeat = currentBeats();
				}
				else if( currentBeats() - slideOutStartBeat >= replayOutLength() )
				{
					slideOutPhase = false;
					slideIndex    = slideTarget;
				}
			}
			else
				slideOutPhase = false;// back to the shown slide before it was gone
			const int which = slideIndex < (int)slideOrder.size() ? slideOrder[ (size_t)slideIndex ] : slideIndex;
			shown           = slides[ (size_t)std::clamp( which, 0, slideCount - 1 ) ];
		}
		else
		{
			// A CSV shown statically, as ticker or roll: all rows, a blank line between them
			slideCount = 1;
			slideIndex = slideTarget = 0;
			for( size_t i = 0; i < slides.size(); ++i )
				shown += ( i ? "\n\n" : "" ) + slides[ i ];
		}
	}
	else
	{
		slideCount = 1;
		slideIndex = slideTarget = 0;
		shown      = src;
	}

	if( source() == SOURCE_CLOCK )
	{
		bool hide               = false;
		const std::string value = timerValueText( hide );
		shown                   = hide ? std::string() : expandTimeTemplate( shown, value, localNow() );
	}
	shown = expandLive( shown );
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
	const auto format       = (TimeFormat)std::clamp( (int)values[ P_TIMER_FORMAT ], 0, 6 );
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
		const LocalTime now = localNow();
		double epoch        = 0;
		if( parseDateTime( timerTarget, now, epoch ) )// a date: days, hours, minutes
			return remaining( epoch - now.epoch );
		double target = 0;
		if( !parseTimeOfDay( timerTarget, target ) )
			return "--:--";
		return remaining( secondsUntil( target, now ) );
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
// Animation, layout, style, 3D, path
// ===========================================================================
AnimatorParams TextSource::animatorParams( int a ) const
{
	auto v = [ & ]( AnimParam p ) { return values[ animParam( a, p ) ]; };
	AnimatorParams ap;
	const int mode = (int)v( AP_MODE );
	ap.mode        = mode >= 0 && mode <= (int)AnimMode::SequenceInOut ? (AnimMode)mode : AnimMode::Off;
	ap.unit        = (AnimUnit)std::clamp( (int)v( AP_UNIT ), 0, 2 );
	ap.order       = (AnimOrder)std::clamp( (int)v( AP_ORDER ), 0, kNumOrders - 1 );
	ap.ease        = (AnimEase)std::clamp( (int)v( AP_EASE ), 0, kNumEases - 1 );
	ap.progress    = v( AP_PROGRESS );
	if( isSequence( ap.mode ) && slidesOn() && values[ P_REPLAY ] > 0.0f )
	{
		// Played by the plugin on every slide change; animator k starts k x Stagger later.
		const double length = std::max( 0.05f, values[ P_REPLAY ] );
		const double step   = length * std::clamp( values[ P_REPLAY_STAGGER ], 0.0f, 100.0f ) / 100.0;
		if( slideOutPhase )
		{
			// Backwards, the last animator first (text out, then the plate)
			int last = 0;
			for( int k = 0; k < kNumAnimators; ++k )
				if( isSequence( (AnimMode)(int)values[ animParam( k, AP_MODE ) ] ) )
					last = k;
			const double delay = step * ( last - a );
			ap.progress        = (float)( 1.0 - std::clamp( ( currentBeats() - slideOutStartBeat - delay ) / length, 0.0, 1.0 ) );
		}
		else
			ap.progress = (float)std::clamp( ( currentBeats() - slideStartBeat - step * a ) / length, 0.0, 1.0 );
	}
	const float size = values[ P_SIZE ];
	ap.spread      = v( AP_SPREAD );
	ap.hold        = v( AP_HOLD ) / 100.0f;
	ap.rangeStart  = v( AP_RANGE_START ) / 100.0f;
	ap.rangeEnd    = v( AP_RANGE_END ) / 100.0f;
	ap.offsetX     = v( AP_OFFSET_X );
	ap.offsetY     = -v( AP_OFFSET_Y );// UI: Y up, like Position Y
	ap.scale       = v( AP_SCALE ) / 100.0f;
	ap.rotationDeg = v( AP_ROTATION );
	ap.skew        = std::tan( std::clamp( v( AP_SKEW ), -80.0f, 80.0f ) * 3.14159265f / 180.0f );
	ap.spacing     = v( AP_SPACING );
	ap.opacity     = v( AP_OPACITY ) / 100.0f;
	const ffglkit::Rgba c = ffglkit::hsbaToRgba( v( AP_COLOR_H ), v( AP_COLOR_S ), v( AP_COLOR_B ), 1.0f );
	ap.color       = Rgb{ c.r, c.g, c.b };
	ap.colorAmount = v( AP_COLOR_A );
	ap.thickness   = v( AP_THICKNESS ) / 100.0f * size;
	ap.blur        = std::max( 0.0f, v( AP_BLUR ) );
	ap.wipe        = v( AP_WIPE ) / 100.0f;
	ap.wipeFrom    = (WipeFrom)std::clamp( (int)v( AP_WIPE_FROM ), 0, 4 );
	ap.scramble    = v( AP_SCRAMBLE ) / 100.0f;
	ap.charset     = (Charset)std::clamp( (int)v( AP_CHARSET ), 0, 4 );
	ap.beats       = currentBeats();
	ap.seed        = 0x1234u + (unsigned)a * 7919u;
	return ap;
}

double TextSource::replayOutLength() const
{
	const double length = std::max( 0.05f, values[ P_REPLAY ] );
	int last            = 0;
	for( int k = 0; k < kNumAnimators; ++k )
		if( isSequence( (AnimMode)(int)values[ animParam( k, AP_MODE ) ] ) )
			last = k;
	return length + length * std::clamp( values[ P_REPLAY_STAGGER ], 0.0f, 100.0f ) / 100.0 * last;
}

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
	p.fit              = box ? (FitMode)std::clamp( (int)values[ P_FIT ], 0, 3 ) : FitMode::Off;
	for( int l = 0; l < kNumLineStyles; ++l )
	{
		LineStyle& ls       = p.lines[ l + 1 ];
		ls.own              = (int)values[ lineMode( l ) ] == 1;
		ls.face             = lineFonts[ l ].face.get();
		ls.sizePercent      = values[ lineParam( l, LP_SIZE ) ];
		ls.trackingPermille = values[ lineParam( l, LP_TRACKING ) ];
		ls.spacingPercent   = values[ lineParam( l, LP_SPACING ) ];
	}
	p.kerning   = values[ P_KERNING ] > 0.5f;
	p.tabularNumbers = source() == SOURCE_CLOCK;// running digits must not make the text jitter
	p.ligatures = values[ P_LIGATURES ] > 0.5f;
	p.features  = features;
	// With slides the pipe already separated the slides; inside a block it still breaks lines.
	p.pipeIsNewline = values[ P_PIPE_NEWLINE ] > 0.5f;
	p.markup        = values[ P_MARKUP ] > 0.5f;
	return p;
}

Style TextSource::currentStyle() const
{
	auto col = [ & ]( unsigned h ) { return toColor( colorOf( h ) ); };
	Style s;
	s.fill  = col( P_FILL_H );
	s.fill2 = col( P_FILL2_H );
	switch( (int)values[ P_FILL_MODE ] )
	{
	case FILL_GRADIENT_BLOCK: s.fillMode = sdfkit::FillMode::GradientBox; break;
	case FILL_GRADIENT_CHARS: s.fillMode = sdfkit::FillMode::GradientChars; break;
	case FILL_STRIPES: s.fillMode = sdfkit::FillMode::Stripes; break;
	case FILL_HALFTONE: s.fillMode = sdfkit::FillMode::Halftone; break;
	case FILL_CHECKER: s.fillMode = sdfkit::FillMode::Checker; break;
	case FILL_NOISE: s.fillMode = sdfkit::FillMode::Noise; break;
	case FILL_LED: s.fillMode = sdfkit::FillMode::Led; break;
	default: s.fillMode = sdfkit::FillMode::Solid; break;// alternates are tints
	}
	s.gradientAngle = values[ P_GRAD_ANGLE ] * 3.14159265f / 180.0f;
	s.patternScale  = std::max( 2.0f, values[ P_PATTERN_SCALE ] );
	s.patternAngle  = values[ P_PATTERN_ANGLE ] * 3.14159265f / 180.0f;
	s.patternOffset = values[ P_PATTERN_OFFSET ];

	const int outline  = (int)values[ P_OUTLINE_MODE ];
	s.outline1Width    = outline >= OUTLINE_ONE ? values[ P_OL1_WIDTH ] : 0.0f;
	s.outline1Position = (sdfkit::OutlinePosition)std::clamp( (int)values[ P_OL1_POSITION ], 0, 2 );
	s.sharpCorners     = (int)values[ P_OL_CORNERS ] == 1;
	s.outline1         = col( P_OL1_H );
	s.outline2Width    = outline == OUTLINE_TWO ? values[ P_OL2_WIDTH ] : 0.0f;
	s.outline2         = col( P_OL2_H );

	const int shadow   = (int)values[ P_SHADOW_MODE ];
	s.shadow           = col( P_SH_H );
	const float dist   = values[ P_SH_DISTANCE ];
	const float ang    = values[ P_SH_ANGLE ] * 3.14159265f / 180.0f;
	s.shadowDx         = dist * std::cos( ang );
	s.shadowDy         = -dist * std::sin( ang );// angle counter-clockwise, screen y down
	s.shadowSoftness   = values[ P_SH_SOFTNESS ];
	s.shadowSpread     = values[ P_SH_SPREAD ];
	if( !( shadow & EFFECT_OUTER ) || ( dist <= 0.0f && s.shadowSpread <= 0.0f ) )
		s.shadow.a = 0.0f;// off, or fully hidden behind the text: skip the work
	// Inner shadow: same light direction as the drop shadow
	const float iDist     = values[ P_ISH_DISTANCE ];
	s.innerShadow         = col( P_ISH_H );
	s.innerShadowDx       = iDist * std::cos( ang );
	s.innerShadowDy       = -iDist * std::sin( ang );
	s.innerShadowSoftness = values[ P_ISH_SOFTNESS ];
	if( !( shadow & EFFECT_INNER ) || iDist <= 0.0f )
		s.innerShadow.a = 0.0f;

	const int glow    = (int)values[ P_GLOW_MODE ];
	s.glow            = col( P_GL_H );
	s.glowRadius      = glow & EFFECT_OUTER ? values[ P_GL_RADIUS ] : 0.0f;
	s.glowStrength    = values[ P_GL_STRENGTH ];
	s.innerGlow       = col( P_IGL_H );
	s.innerGlowRadius = glow & EFFECT_INNER ? values[ P_IGL_RADIUS ] : 0.0f;

	s.plate            = col( P_PLATE_H );
	s.plateBorderWidth = values[ P_PLATE_BORDER ];
	s.plateBorderColor = col( P_PLATE_BORDER_H );
	s.plateMerge       = values[ P_PLATE_RADIUS ] / 100.0f * values[ P_SIZE ];// fillets as round as the corners
	s.edgeSoftness     = values[ P_EDGE ];
	return s;
}

sdfkit::View3D TextSource::currentView() const
{
	sdfkit::View3D v;
	const int mode = (int)values[ P_3D ];
	v.mode         = mode == THREED_TILT ? sdfkit::View3D::Tilt : mode == THREED_EXTRUDE ? sdfkit::View3D::Extrude : sdfkit::View3D::Off;
	const float d2r = 3.14159265f / 180.0f;
	v.tiltX         = values[ P_TILT_X ] * d2r;
	v.tiltY         = values[ P_TILT_Y ] * d2r;
	// Perspective 0..100 % = field of view 0..90 degrees over the output height
	const float fov = std::clamp( values[ P_PERSPECTIVE ], 0.0f, 100.0f ) * 0.9f * d2r;
	v.focal         = fov > 0.01f ? 0.5f * (float)currentViewport.height / std::tan( 0.5f * fov ) : 0.0f;
	v.depth         = std::max( 0.0f, values[ P_DEPTH ] );
	v.bevel         = std::max( 0.0f, values[ P_BEVEL ] );
	v.side          = toColor( colorOf( P_SIDE_H ) );
	// Light: the angle it comes from (counter-clockwise, 0 = from the right) and its height
	const float la  = values[ P_LIGHT_ANGLE ] * d2r, lh = std::clamp( values[ P_LIGHT_HEIGHT ], 0.0f, 90.0f ) * d2r;
	v.lightX        = std::cos( lh ) * std::cos( la );
	v.lightY        = -std::cos( lh ) * std::sin( la );
	v.lightZ        = std::sin( lh );
	v.shading       = values[ P_SHADING ] / 100.0f;
	v.gloss         = values[ P_GLOSS ] / 100.0f;
	return v;
}

PathParams TextSource::currentPath() const
{
	PathParams p;
	p.mode       = (PathMode)std::clamp( (int)values[ P_PATH ], 0, 3 );
	p.radius     = values[ P_PATH_RADIUS ];
	p.offset     = values[ P_PATH_OFFSET ];
	p.waveHeight = values[ P_WAVE_HEIGHT ];
	p.waveLength = values[ P_WAVE_LENGTH ];
	return p;
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

// ===========================================================================
// Frame
// ===========================================================================
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
	if( !live )
	{
		// First frame: the host has sent the composition. 1.x compositions get their
		// effect menus from their values; animators show the properties they use.
		deriveModesFromValues();
		for( int a = 0; a < kNumAnimators; ++a )
			refreshShownProperties( a, true );
	}

	// Live data: one OSC receiver per port, shared by all instances
	const int port = parseOscPort( oscPortText );
	if( port != oscPort )
	{
		osc       = OscReceiver::forPort( port );
		oscPort   = port;
		oscWarned = false;
		if( port > 0 )
			FFGLLog::LogToHost( ( "Text+: listening for OSC on port " + std::to_string( port ) ).c_str() );
	}

	// Clip start (first frame, or the host time jumped back = re-triggered).
	// Tickers and rolls move with the host's time when it sends one (a paused
	// clip pauses them), else with the wall clock; never while not rendered.
	const double now = wallSeconds();
	if( lastFrameWall < 0.0 || retriggered )
		onClipStart();
	else
		scrollTime += std::clamp( hostTimeCalls > 2 ? hostDelta : now - lastFrameWall, 0.0, 0.1 );
	lastFrameWall = now;
	retriggered   = false;
	hostDelta     = 0.0;

	instances.clear();
	plates.clear();
	placed.clear();
	sdfkit::BlockTransform transform;
	sdfkit::Window window;
	Style style                = currentStyle();
	const sdfkit::View3D view  = currentView();
	sdfkit::VideoInput video;
	if( isEffect() && pGL && pGL->numInputTextures > 0 && pGL->inputTextures && pGL->inputTextures[ 0 ] )
	{
		const FFGLTextureStruct& in = *pGL->inputTextures[ 0 ];
		video.mode                  = (sdfkit::VideoInput::Mode)std::clamp( (int)values[ P_VIDEO ], 0, 3 );
		video.texture               = in.Handle;
		video.maxU                  = in.HardwareWidth > 0 ? (float)in.Width / (float)in.HardwareWidth : 1.0f;
		video.maxV                  = in.HardwareHeight > 0 ? (float)in.Height / (float)in.HardwareHeight : 1.0f;
	}

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
		const PathParams path = ticker || roll ? PathParams() : currentPath();// tickers and rolls run straight
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

		// Shapes that grow or soften (animated thickness and blur, the 3D raymarch)
		// need a wider distance field around them.
		{
			float grow = std::max( 0.0f, values[ P_THICKNESS ] / 100.0f * size ), blur = 0.0f;
			for( int a = 0; a < kNumAnimators; ++a )
				if( animatorOn( a ) )
				{
					grow = std::max( grow, values[ P_THICKNESS ] / 100.0f * size + values[ animParam( a, AP_THICKNESS ) ] / 100.0f * size );
					blur = std::max( blur, values[ animParam( a, AP_BLUR ) ] );
				}
			float march = 0.0f;
			if( view.mode == sdfkit::View3D::Extrude )
				march = std::clamp( 0.5f * view.depth, 4.0f, 0.1f * size );
			style.extraReach = std::max( grow + 0.5f * blur, march );
		}

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

		// Per-glyph state: line colours, inline colours (win), character gradient,
		// alternating colours, animators.
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
		const int fillMenu  = (int)values[ P_FILL_MODE ];
		if( style.fillMode == sdfkit::FillMode::GradientChars )
			for( size_t i = 0; i < glyphStates.size(); ++i )
				if( glyphStates[ i ].tintAmount <= 0.0f )// inline colours win
				{
					const float t = placed[ i ].charIndex / lastIdx;
					layerTint( glyphStates[ i ], Rgb{ style.fill.r + ( style.fill2.r - style.fill.r ) * t, style.fill.g + ( style.fill2.g - style.fill.g ) * t,
													 style.fill.b + ( style.fill2.b - style.fill.b ) * t },
							   1.0f );
				}
		if( fillMenu >= FILL_ALT_CHARS && fillMenu <= FILL_ALT_LINES )
			for( size_t i = 0; i < glyphStates.size(); ++i )
				if( glyphStates[ i ].tintAmount <= 0.0f )
				{
					const LaidGlyph& g = placed[ i ];
					const int unit     = fillMenu == FILL_ALT_CHARS ? g.charIndex : fillMenu == FILL_ALT_WORDS ? g.wordIndex : g.lineIndex;
					if( unit & 1 )
						layerTint( glyphStates[ i ], Rgb{ style.fill2.r, style.fill2.g, style.fill2.b }, 1.0f );
				}

		// ---- Plates (not on a path: they would not follow it) ----
		struct PlateRect
		{
			float x0, y0, x1, y1;
		};
		std::vector< PlateRect > plateRects;
		const int plateMode = path.on() ? PLATE_OFF : (int)values[ P_PLATE ];
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
			if( !animatorOn( a ) )
				continue;
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
				pp.spacing        = 0.0f;
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

		// Line extents for the circle path: the line plus one average glyph of gap
		std::vector< float > lineLength( layout.lines.size(), 0.0f );
		if( path.mode == PathMode::Circle )
			for( size_t li = 0; li < layout.lines.size(); ++li )
			{
				const LineBox& lb = layout.lines[ li ];
				const float len   = lb.penEnd - lb.left;
				lineLength[ li ]  = len + ( lb.glyphCount > 0 ? len / (float)lb.glyphCount : 0.0f );
			}
		const float globalWeight = values[ P_THICKNESS ] / 100.0f * size;

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

			// Where it sits: on the layout, or on the path (animator offsets and turns
			// then work in the glyph's turned frame)
			float baseX = g.x, baseY = g.y, turn = 0.0f, dx = st.dx, dy = st.dy;
			if( path.on() )
			{
				const LineBox* lb = g.lineIndex >= 0 && g.lineIndex < (int)layout.lines.size() ? &layout.lines[ (size_t)g.lineIndex ] : nullptr;
				const PathPlacement pl = placeOnPath( path, g.x + 0.5f * g.advance, g.y, g.advance, lb ? lb->left : 0.0f,
													  lb && (size_t)g.lineIndex < lineLength.size() ? lineLength[ (size_t)g.lineIndex ] : 0.0f,
													  layout.lines.empty() ? 0.0f : layout.lines.front().baseline );
				baseX = pl.x;
				baseY = pl.y;
				turn  = pl.angle;
				const float ct = std::cos( turn ), sn = std::sin( turn );
				dx    = ct * st.dx - sn * st.dy;
				dy    = sn * st.dx + ct * st.dy;
			}
			const float rot = st.rotation + turn;
			const float c = std::cos( rot ), s = std::sin( rot );
			ShapeInstance inst;
			inst.quadL         = slot->planeLeft;
			inst.quadB         = slot->planeBottom;
			inst.quadR         = slot->planeRight;
			inst.quadT         = slot->planeTop;
			inst.u0            = slot->u0;
			inst.v0            = slot->v0;
			inst.u1            = slot->u1;
			inst.v1            = slot->v1;
			inst.x             = baseX + dx + shiftX * st.scale * c;
			inst.y             = baseY + dy + shiftX * st.scale * s;
			inst.scale         = glyphScale;
			inst.rotation      = rot;
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
			inst.skew          = st.skew;
			inst.weightPx      = st.weight + globalWeight * g.scale;
			inst.blurPx        = std::clamp( st.blur, 0.0f, 64.0f );
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
			b.skew       = st.skew;
			b.blurPx     = std::clamp( st.blur, 0.0f, 64.0f );
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

	renderer.render( currentViewport, pGL->HostFBO, instances, plates, transform, style, window, atlas.texture(), atlas.size(), view, video );

	if( !live )
	{
		live = true;
		updateVisibility();
	}
	return FF_SUCCESS;
}

}//namespace textplus
