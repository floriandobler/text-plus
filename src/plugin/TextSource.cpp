#include "TextSource.h"

#include "core/GlyphCache.h"

#include <hb.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace kontur
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

CFFGLPluginInfo PluginInfo( PluginFactory< TextSource >,                            // create method
							"KTXT",                                                // unique ID (4 chars)
							"Kontur Text",                                         // name
							2,                                                     // API major
							1,                                                     // API minor
							0,                                                     // plugin major
							1,                                                     // plugin minor
							FF_SOURCE,                                             // type
							"Text source with distance-field outlines, shadows and glow",
							"Kontur Text 0.1",
							0, nullptr, initialiseLibrary, nullptr );

const char* kGroupText      = "Text";
const char* kGroupFont      = "Font";
const char* kGroupLayout    = "Layout";
const char* kGroupFill      = "Fill";
const char* kGroupOutline   = "Outline";
const char* kGroupOutline2  = "Outline 2";
const char* kGroupShadow    = "Shadow";
const char* kGroupGlow      = "Glow";
const char* kGroupAnimation = "Animation";
const char* kGroupQuality   = "Quality";

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

bool familyMatchesQuery( const std::string& family, const std::string& query )
{
	std::string f = normalizeFontName( family );
	std::string q = normalizeFontName( query.substr( 0, query.find( '/' ) ) );
	return !q.empty() && ( f.find( q ) != std::string::npos || q.find( f ) != std::string::npos );
}
}//namespace

TextSource::TextSource()
{
	SetMinInputs( 0 );
	SetMaxInputs( 0 );
	defineParameters();
	FontDatabase::instance().ensureScanning();
}

TextSource::~TextSource() = default;

void TextSource::defineParameters()
{
	auto range = [ & ]( ParamId id, const char* name, const char* group, float def, float lo, float hi ) {
		values[ id ] = def;
		SetParamInfo( id, name, FF_TYPE_STANDARD, def );
		// SetParamInfo clamps defaults of standard params to 0..1; store the real default.
		if( ParamInfo* info = FindParamInfo( id ) )
			info->defaultFloatVal = def;
		SetParamRange( id, lo, hi );
		SetParamGroup( id, group );
	};
	auto boolean = [ & ]( ParamId id, const char* name, const char* group, bool def ) {
		values[ id ] = def ? 1.0f : 0.0f;
		SetParamInfo( id, name, FF_TYPE_BOOLEAN, def );
		SetParamGroup( id, group );
	};
	auto option = [ & ]( ParamId id, const char* name, const char* group, std::vector< const char* > items, int def ) {
		values[ id ] = (float)def;
		SetOptionParamInfo( id, name, (unsigned)items.size(), (float)def );
		for( unsigned i = 0; i < items.size(); ++i )
			SetParamElementInfo( id, i, items[ i ], (float)i );
		SetParamGroup( id, group );
	};
	auto color = [ & ]( ParamId h, const char* name, const char* group, float hv, float sv, float bv, float av ) {
		std::string n = name;
		values[ h ] = hv;
		values[ h + 1 ] = sv;
		values[ h + 2 ] = bv;
		values[ h + 3 ] = av;
		SetParamInfo( h, n.c_str(), FF_TYPE_HUE, hv );
		SetParamInfo( h + 1, ( n + "_Sat" ).c_str(), FF_TYPE_SATURATION, sv );
		SetParamInfo( h + 2, ( n + "_Bri" ).c_str(), FF_TYPE_BRIGHTNESS, bv );
		SetParamInfo( h + 3, ( n + "_Alpha" ).c_str(), FF_TYPE_ALPHA, av );
		for( unsigned i = 0; i < 4; ++i )
			SetParamGroup( h + i, group );
	};

	// Text
	text = "Kontur";
	SetParamInfo( P_TEXT, "Text", FF_TYPE_TEXT, text.c_str() );
	SetParamGroup( P_TEXT, kGroupText );
	boolean( P_PIPE_NEWLINE, "| = New Line", kGroupText, true );

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
	option( P_ANCHOR_H, "Anchor H", kGroupLayout, { "Auto (follows Align)", "Left", "Center", "Right" }, 0 );
	option( P_ANCHOR_V, "Anchor V", kGroupLayout, { "Top", "Middle", "Bottom", "First Baseline", "Last Baseline" }, 1 );
	range( P_BOX_WIDTH, "Box Width", kGroupLayout, 0.0f, 0.0f, 8000.0f );
	range( P_BOX_HEIGHT, "Box Height", kGroupLayout, 0.0f, 0.0f, 8000.0f );
	option( P_FIT, "Auto Fit", kGroupLayout, { "Off", "Shrink to Box", "Fit Box Width" }, 0 );
	range( P_TRACKING, "Tracking", kGroupLayout, 0.0f, -300.0f, 1000.0f );
	range( P_LEADING, "Leading", kGroupLayout, 120.0f, 50.0f, 300.0f );
	boolean( P_KERNING, "Kerning", kGroupLayout, true );
	boolean( P_LIGATURES, "Ligatures", kGroupLayout, true );

	// Fill
	color( P_FILL_H, "Fill Color", kGroupFill, 0.0f, 0.0f, 1.0f, 1.0f );

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

	// Animation
	option( P_AN_MODE, "Animation", kGroupAnimation, { "Off", "Typewriter", "Fade", "Rise", "Wave" }, 0 );
	option( P_AN_UNIT, "Unit", kGroupAnimation, { "Character", "Word", "Line" }, 0 );
	range( P_AN_PROGRESS, "Progress", kGroupAnimation, 1.0f, 0.0f, 1.0f );
	range( P_AN_SPREAD, "Spread", kGroupAnimation, 3.0f, 0.0f, 50.0f );
	range( P_AN_AMOUNT, "Amount", kGroupAnimation, 40.0f, -1000.0f, 1000.0f );
	range( P_AN_SPEED, "Speed (cycles/beat)", kGroupAnimation, 1.0f, -8.0f, 8.0f );

	// Quality
	range( P_EDGE, "Edge Softness", kGroupQuality, 1.0f, 0.25f, 4.0f );

	updateVisibility( false );
}

void TextSource::updateVisibility( bool raiseEvent )
{
	const bool box      = values[ P_BOX_WIDTH ] > 0.0f;
	const bool animated = (int)values[ P_AN_MODE ] != 0;
	if( visibilityInitialised && box == lastVisibilityBox && animated == lastVisibilityOutline2 )
		return;
	SetParamVisibility( P_BOX_HEIGHT, box, raiseEvent );
	SetParamVisibility( P_FIT, box, raiseEvent );
	for( ParamId id : { P_AN_UNIT, P_AN_PROGRESS, P_AN_SPREAD, P_AN_AMOUNT, P_AN_SPEED } )
		SetParamVisibility( id, animated, raiseEvent );
	lastVisibilityBox      = box;
	lastVisibilityOutline2 = animated;
	visibilityInitialised  = true;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
FFResult TextSource::SetFloatParameter( unsigned int index, float value )
{
	if( index >= P_COUNT )
		return FF_FAIL;
	switch( index )
	{
	case P_FAMILY:
		if( menusPopulated && (int)value != (int)values[ P_FAMILY ] )
		{
			values[ index ] = value;
			onFamilyPicked( (int)value );
		}
		else if( menusPopulated )
			values[ index ] = value;
		return FF_SUCCESS;
	case P_STYLE:
		if( menusPopulated && (int)value != (int)values[ P_STYLE ] )
		{
			values[ index ] = value;
			onStylePicked( (int)value );
		}
		else if( menusPopulated )
			values[ index ] = value;
		return FF_SUCCESS;
	default:
		values[ index ] = value;
		break;
	}
	if( index == P_BOX_WIDTH || index == P_AN_MODE )
		updateVisibility( true );
	return FF_SUCCESS;
}

FFResult TextSource::SetTextParameter( unsigned int index, const char* value )
{
	std::string v = value ? value : "";
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
	default:
		return FF_FAIL;
	}
}

float TextSource::GetFloatParameter( unsigned int index )
{
	return index < P_COUNT ? values[ index ] : 0.0f;
}

char* TextSource::GetTextParameter( unsigned int index )
{
	switch( index )
	{
	case P_TEXT:
		textReturnBuffer = text;
		break;
	case P_FONT:
		textReturnBuffer = fontText;
		break;
	case P_FONT_FILE:
		textReturnBuffer = fontFile;
		break;
	default:
		textReturnBuffer.clear();
		break;
	}
	return const_cast< char* >( textReturnBuffer.c_str() );
}

char* TextSource::GetParameterDisplay( unsigned int index )
{
	char buf[ 64 ];
	const float v = index < P_COUNT ? values[ index ] : 0.0f;
	switch( index )
	{
	case P_TEXT:
	case P_FONT:
	case P_FONT_FILE:
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
	case P_AN_AMOUNT:
		std::snprintf( buf, sizeof( buf ), "%.0f px", v );
		break;
	case P_EDGE:
		std::snprintf( buf, sizeof( buf ), "%.2f px", v );
		break;
	case P_ROTATION:
	case P_SH_ANGLE:
		std::snprintf( buf, sizeof( buf ), "%.0f\xC2\xB0", v );
		break;
	case P_TRACKING:
		std::snprintf( buf, sizeof( buf ), "%.0f /1000 em", v );
		break;
	case P_LEADING:
		std::snprintf( buf, sizeof( buf ), "%.0f %%", v );
		break;
	case P_AN_PROGRESS:
		std::snprintf( buf, sizeof( buf ), "%.0f %%", v * 100.0f );
		break;
	default:
		return CFFGLPlugin::GetParameterDisplay( index );
	}
	displayBuffer = buf;
	return const_cast< char* >( displayBuffer.c_str() );
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
	beats        = 4.0 * ( barCount + barPhase );
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------
void TextSource::setFontText( const std::string& t, bool raiseEvent )
{
	if( t == fontText )
		return;
	fontText  = t;
	fontDirty = true;
	if( raiseEvent )
		RaiseParamEvent( P_FONT, FF_EVENT_FLAG_VALUE );
}

void TextSource::populateFontMenus()
{
	familyMenu = FontDatabase::instance().families();
	if( familyMenu.empty() )
		familyMenu.push_back( "(no fonts found)" );
	std::vector< float > vals( familyMenu.size() );
	for( size_t i = 0; i < vals.size(); ++i )
		vals[ i ] = (float)i;
	SetParamElements( P_FAMILY, familyMenu, vals, true );
	menusPopulated = true;
	styleMenuFamily.clear();
}

void TextSource::syncMenusToFont()
{
	if( !menusPopulated || !face )
		return;
	const FontEntry& e = face->entry();

	auto famIt = std::find( familyMenu.begin(), familyMenu.end(), e.family );
	if( famIt != familyMenu.end() )
	{
		float idx = (float)( famIt - familyMenu.begin() );
		if( values[ P_FAMILY ] != idx )
		{
			values[ P_FAMILY ] = idx;
			RaiseParamEvent( P_FAMILY, FF_EVENT_FLAG_VALUE );
		}
	}

	const std::string menuKey = ( fontFile.empty() ? "db:" : "file:" + fontFile + ":" ) + e.family;
	if( menuKey != styleMenuFamily )
	{
		styleMenu        = fontFile.empty() ? FontDatabase::instance().stylesOf( e.family ) : FontDatabase::scanFile( fontFile );
		fileStylesActive = !fontFile.empty();
		std::vector< std::string > names;
		std::vector< float > vals;
		for( size_t i = 0; i < styleMenu.size(); ++i )
		{
			names.push_back( fileStylesActive && styleMenu[ i ].family != e.family ? styleMenu[ i ].displayName() : styleMenu[ i ].style );
			vals.push_back( (float)i );
		}
		if( names.empty() )
		{
			names.push_back( e.style );
			vals.push_back( 0.0f );
		}
		SetParamElements( P_STYLE, names, vals, true );
		styleMenuFamily = menuKey;
	}
	for( size_t i = 0; i < styleMenu.size(); ++i )
		if( styleMenu[ i ].sameFace( e ) )
		{
			if( values[ P_STYLE ] != (float)i )
			{
				values[ P_STYLE ] = (float)i;
				RaiseParamEvent( P_STYLE, FF_EVENT_FLAG_VALUE );
			}
			break;
		}
}

void TextSource::onFamilyPicked( int index )
{
	if( index < 0 || index >= (int)familyMenu.size() )
		return;
	if( !fontFile.empty() )
		return;// the file decides; the family menu is informational then
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
			entry = faces.front();
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
		}
		else
		{
			FFGLLog::LogToHost( ( "Kontur Text: cannot read font file " + fontFile ).c_str() );
		}
	}
	if( !entry )
	{
		if( !db.isReady() )
			return;// try again next frame
		entry = db.resolve( fontText );
		if( !entry )
		{
			fontDirty = false;
			face      = nullptr;
			return;
		}
		// Write the canonical "Family / Style" back, but only when the query really
		// matched. A font missing on this machine must not overwrite the saved name.
		if( fontText.empty() || familyMatchesQuery( entry->family, fontText ) )
			setFontText( entry->displayName(), true );
		else
			FFGLLog::LogToHost( ( "Kontur Text: font '" + fontText + "' not found, using " + entry->displayName() ).c_str() );
	}
	else if( fontText.empty() || fontText.find( '/' ) == std::string::npos )
	{
		setFontText( entry->displayName(), true );
	}

	auto newFace = FontFace::get( *entry );
	if( newFace )
		face = newFace;
	fontDirty = false;
	syncMenusToFont();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
LayoutParams TextSource::currentLayoutParams() const
{
	LayoutParams p;
	p.text             = text;
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
	p.pipeIsNewline    = values[ P_PIPE_NEWLINE ] > 0.5f;
	return p;
}

Style TextSource::currentStyle() const
{
	auto col = [ & ]( ParamId h ) { return hsbaToColor( values[ h ], values[ h + 1 ], values[ h + 2 ], values[ h + 3 ] ); };
	Style s;
	s.fill             = col( P_FILL_H );
	s.outline1Width    = values[ P_OL1_WIDTH ];
	s.outline1Position = (OutlinePosition)std::clamp( (int)values[ P_OL1_POSITION ], 0, 2 );
	s.outline1         = col( P_OL1_H );
	s.sharpCorners     = (int)values[ P_OL_CORNERS ] == 1;
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
	s.glow         = col( P_GL_H );
	s.glowRadius   = values[ P_GL_RADIUS ];
	s.glowStrength = values[ P_GL_STRENGTH ];
	s.edgeSoftness = values[ P_EDGE ];
	return s;
}

AnimParams TextSource::currentAnimParams() const
{
	AnimParams a;
	a.mode     = (AnimMode)std::clamp( (int)values[ P_AN_MODE ], 0, 4 );
	a.unit     = (AnimUnit)std::clamp( (int)values[ P_AN_UNIT ], 0, 2 );
	a.progress = values[ P_AN_PROGRESS ];
	a.spread   = values[ P_AN_SPREAD ];
	a.amount   = values[ P_AN_AMOUNT ];
	a.speed    = values[ P_AN_SPEED ];
	a.beats    = haveBeatInfo ? beats : hostTime * 2.0;// 120 BPM when the host sends no beat info
	return a;
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
		populateFontMenus();
		fontDirty = true;
	}
	if( fontDirty )
		resolveFont();

	instances.clear();
	pendingGlyphs = !face;
	BlockTransform transform;
	Style style = currentStyle();

	if( face )
	{
		LayoutParams lp = currentLayoutParams();
		if( !layoutValid || lp != lastLayoutParams || face->id() != lastLayoutFaceId )
		{
			layout           = layoutText( *face, lp );
			lastLayoutParams = lp;
			lastLayoutFaceId = face->id();
			layoutValid      = true;
		}

		const AnimParams anim = currentAnimParams();
		animateGlyphs( layout, anim, glyphStates );

		const float size     = layout.sizePx;
		const float maxScale = std::max( 1.0f, maxAnimatedScale( anim ) );
		const int level      = spreadLevelFor( style.reach() / std::max( 1.0f, size ) );
		int minLevelUsed     = level;
		bool atlasFull       = false;
		GlyphCache& cache    = GlyphCache::instance();
		const float lastIdx  = (float)std::max( 1, layout.numChars - 1 );

		instances.reserve( layout.glyphs.size() );
		for( size_t i = 0; i < layout.glyphs.size(); ++i )
		{
			const LaidGlyph& g    = layout.glyphs[ i ];
			const GlyphState& st  = glyphStates[ i ];
			if( st.alpha <= 0.001f )
				continue;

			const AtlasSlot* slot = atlas.find( atlasKey( face->id(), g.glyphId, level ) );
			if( !slot )
			{
				auto bmp = cache.get( face, g.glyphId, level );
				if( bmp )
				{
					if( bmp->empty )
						continue;
					slot = atlas.add( atlasKey( face->id(), g.glyphId, level ), *bmp );
					if( !slot )
						atlasFull = true;
				}
				else
					pendingGlyphs = true;
			}
			// While the wanted spread level is being generated, use any level we already have.
			for( int l = kNumSpreadLevels - 1; !slot && l >= 0; --l )
				slot = atlas.find( atlasKey( face->id(), g.glyphId, l ) );
			if( !slot )
				continue;

			minLevelUsed         = std::min( minLevelUsed, slot->level );
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
					cache.get( face, gid, level );
			}
		}

		if( atlasFull )
		{
			atlas.clear();// rebuilt from the CPU cache over the next frames
			pendingGlyphs = true;
		}

		// The field reaches spreadEm * (glyph size in px); effects beyond that are clamped.
		(void)maxScale;
		style.clampTo( spreadLevel( minLevelUsed ).spreadEm * size );
		transform.sizePx = size;
	}

	transform.originX  = currentViewport.width * 0.5f + values[ P_POS_X ];
	transform.originY  = currentViewport.height * 0.5f - values[ P_POS_Y ];// Position Y up
	transform.rotation = values[ P_ROTATION ] * 3.14159265f / 180.0f;

	renderer.render( currentViewport, pGL->HostFBO, instances, transform, style, atlas.texture(), atlas.size() );
	return FF_SUCCESS;
}

}//namespace kontur
