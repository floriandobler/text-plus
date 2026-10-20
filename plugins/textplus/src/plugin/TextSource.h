#pragma once
// TextSource: the FFGL adapter of Text+. Owns the parameters, talks to Resolume
// (dynamic dropdowns, visibility, value events, presets) and drives the core
// (fonts, layout, animators, clock, path, OSC) and the renderer (sdfkit).
//
// The same class builds two plugins: the source "Text+" and, with
// TEXTPLUS_EFFECT, the effect "Text+ Fill" that uses the clip's video.
//
// Files: TextSource.cpp (frame, fonts, text), Params.cpp (definition,
// visibility, host values, migration), Presets.cpp (animator presets, looks).

#include "ffglkit/ParamPlugin.h"
#include "sdfkit/Atlas.h"
#include "sdfkit/Renderer.h"

#include "core/Animator.h"
#include "core/Clock.h"
#include "core/FontDatabase.h"
#include "core/FontFace.h"
#include "core/Path.h"
#include "core/TextLayout.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace textplus
{
class OscReceiver;

// Parameters of one animator, in order. Animators 2-4 repeat the block. Names
// carry the "A1 " prefix (Resolume shows the internal names).
enum AnimParam : unsigned int
{
	AP_PRESET,
	AP_MODE,
	AP_ADD,     // "+ Property": shows a property row (one-shot menu)
	AP_PROGRESS,
	AP_TARGET,  // Text / Plate / Both
	AP_UNIT,
	AP_ORDER,
	AP_SPREAD,
	AP_HOLD,    // Sequence In/Out: share at rest
	AP_EASE,
	AP_RANGE_START,
	AP_RANGE_END,
	AP_OFFSET_X,
	AP_OFFSET_Y,
	AP_SCALE,
	AP_ROTATION,
	AP_SKEW,
	AP_SPACING,
	AP_OPACITY,
	AP_COLOR_H,
	AP_COLOR_S,
	AP_COLOR_B,
	AP_COLOR_A,
	AP_THICKNESS,
	AP_BLUR,
	AP_WIPE,
	AP_WIPE_FROM,
	AP_SCRAMBLE,
	AP_CHARSET,
	AP_COUNT
};

// Properties an animator shows on demand ("+ Property"); bit = 1 << value.
enum AnimProp : int
{
	PROP_OFFSET_X = 1,
	PROP_OFFSET_Y,
	PROP_SCALE,
	PROP_ROTATION,
	PROP_OPACITY,
	PROP_COLOR,
	PROP_WIPE,
	PROP_SCRAMBLE,
	PROP_SKEW,
	PROP_BLUR,
	PROP_THICKNESS,
	PROP_SPACING,
	PROP_RANGE,
	PROP_COUNT,
	PROP_SHOW_ALL   = 50,
	PROP_HIDE_UNUSED = 51
};

// Parameters of one line style ("Line 2", "Line 3+") after its Style menu.
enum LineParam : unsigned int
{
	LP_WEIGHT,  // styles of the effective family (writes LP_FONT)
	LP_FONT,    // text: "" = main font, "Bold" = style of the main family, "Family / Style" = other font
	LP_SIZE,    // % of the main size
	LP_COLOR_H, // alpha = how strongly the colour replaces the fill
	LP_COLOR_S,
	LP_COLOR_B,
	LP_COLOR_A,
	LP_TRACKING,// added to the main tracking
	LP_SPACING, // extra space above, % of the main size
	LP_COUNT
};

// Order = order in Resolume: content, type, layout, look, motion, advanced.
// Names are what Resolume stores; groups and order may change freely.
enum ParamId : unsigned int
{
	// ---- Text: where it comes from (Source) and how it is shown (Display)
	P_SOURCE,
	P_DISPLAY,
	P_TEXT,
	P_LINE_FIRST,                    // "Text 2" .. "Text 10": extra text fields, shown one by one with "+ Line"
	P_LINE_LAST = P_LINE_FIRST + 8,
	P_ADD_LINE,
	P_REMOVE_LINE,
	P_TEXT_FILE,
	P_CSV_HEADER,
	P_ROW_TEMPLATE,
	// Clock & Timer
	P_TIMER_MODE,
	P_TIMER_TARGET,  // text "20:00" or "24.12.2026 20:00"
	P_TIMER_DURATION,// text "5:00"
	P_TIMER_START,
	P_TIMER_STOP,
	P_TIMER_RESET,
	P_TIMER_AUTOSTART,
	P_TIMER_FORMAT,
	P_TIMER_AT_ZERO,
	P_COUNT_FROM,
	P_COUNT_TO,
	P_COUNT_PROGRESS,
	P_COUNT_DECIMALS,
	P_NUMBER_FORMAT,
	// Slides
	P_SLIDE,// position 0..1, split into equal shares per slide (BPM sync friendly)
	P_NEXT_SLIDE,
	P_PREV_SLIDE,
	P_SLIDE_ORDER,
	P_REPLAY,// beats; > 0 replays the sequence animators on every slide change
	P_REPLAY_MODE,// In / Out, then In
	P_REPLAY_STAGGER,// Animator k starts later by k x this share of the replay
	// Ticker / Roll
	P_SCROLL_SPEED,
	P_SCROLL_POSITION,
	P_SEPARATOR,
	P_ROLL_LOOP,
	P_EDGE_FADE,
	P_SLIDES_LEGACY,// "Slides" of 1.0, hidden: migrates old compositions to Display
	P_FORMAT,// hidden: 2 in compositions saved by 2.x (no migration then)
	// ---- Font
	P_FAMILY,
	P_STYLE,
	P_SIZE,
	P_TRACKING,
	P_CASE,
	// ---- Lines: leading and the styles of line 2 and line 3+
	P_LEADING,
	P_L2_MODE,
	P_L3_MODE,
	P_LINE_EDIT,// both own: which one the rows below show
	P_L2,
	P_L3 = P_L2 + LP_COUNT,
	// ---- Layout
	P_POS_X = P_L3 + LP_COUNT,
	P_POS_Y,
	P_ROTATION,
	P_ALIGN,
	P_WRAP,
	P_BOX_WIDTH,
	P_BOX_HEIGHT,
	P_FIT,
	P_PATH,
	P_PATH_RADIUS,
	P_WAVE_HEIGHT,
	P_WAVE_LENGTH,
	P_PATH_OFFSET,
	// ---- Look: loads a whole style
	P_LOOK,
	// ---- Fill
	P_VIDEO,// Text+ Fill only: Over / Fill / Knockout
	P_FILL_MODE,
	P_FILL_H,
	P_FILL_S,
	P_FILL_B,
	P_FILL_A,
	P_FILL2_H,
	P_FILL2_S,
	P_FILL2_B,
	P_FILL2_A,
	P_GRAD_ANGLE,
	P_PATTERN_SCALE,
	P_PATTERN_ANGLE,
	P_PATTERN_OFFSET,
	// ---- Outline (two bands)
	P_OUTLINE_MODE,
	P_OL1_WIDTH,
	P_OL1_POSITION,
	P_OL_CORNERS,
	P_OL1_H,
	P_OL1_S,
	P_OL1_B,
	P_OL1_A,
	P_OL2_WIDTH,
	P_OL2_H,
	P_OL2_S,
	P_OL2_B,
	P_OL2_A,
	// ---- Plate
	P_PLATE,
	P_PLATE_H,
	P_PLATE_S,
	P_PLATE_B,
	P_PLATE_A,
	P_PLATE_PAD_X,
	P_PLATE_PAD_Y,
	P_PLATE_RADIUS,
	P_PLATE_SLANT,
	P_PLATE_WIDTH,
	P_PLATE_BORDER,
	P_PLATE_BORDER_H,
	P_PLATE_BORDER_S,
	P_PLATE_BORDER_B,
	P_PLATE_BORDER_A,
	// ---- Shadow (drop shadow, then inner shadow; one light direction)
	P_SHADOW_MODE,
	P_SH_H,
	P_SH_S,
	P_SH_B,
	P_SH_A,
	P_SH_DISTANCE,
	P_SH_ANGLE,
	P_SH_SOFTNESS,
	P_SH_SPREAD,
	P_ISH_DISTANCE,
	P_ISH_SOFTNESS,
	P_ISH_H,
	P_ISH_S,
	P_ISH_B,
	P_ISH_A,
	// ---- Glow (outer, then inner)
	P_GLOW_MODE,
	P_GL_H,
	P_GL_S,
	P_GL_B,
	P_GL_A,
	P_GL_RADIUS,
	P_GL_STRENGTH,
	P_IGL_RADIUS,
	P_IGL_H,
	P_IGL_S,
	P_IGL_B,
	P_IGL_A,
	// ---- 3D
	P_3D,
	P_TILT_X,
	P_TILT_Y,
	P_PERSPECTIVE,
	P_DEPTH,
	P_BEVEL,
	P_SIDE_H,
	P_SIDE_S,
	P_SIDE_B,
	P_SIDE_A,
	P_LIGHT_ANGLE,
	P_LIGHT_HEIGHT,
	P_SHADING,
	P_GLOSS,
	// ---- Animators
	P_A1,
	P_A_END = P_A1 + 4 * AP_COUNT,
	// ---- Advanced (set once). The last parameter must be a slider: see SetFloatParameter.
	P_ADVANCED = P_A_END,
	P_FONT,
	P_FONT_FILE,
	P_THICKNESS,
	P_ANCHOR_H,
	P_ANCHOR_V,
	P_KERNING,
	P_LIGATURES,
	P_FEATURES,
	P_PIPE_NEWLINE,
	P_MARKUP,
	P_OSC_PORT,
	P_EDGE,

	P_COUNT
};

enum SourceMode
{
	SOURCE_INPUT,
	SOURCE_FILE,
	SOURCE_CLOCK
};
enum DisplayMode
{
	DISPLAY_STATIC,
	DISPLAY_SLIDES_LINES,
	DISPLAY_SLIDES_BLOCKS,
	DISPLAY_TICKER,
	DISPLAY_ROLL,
	DISPLAY_SLIDES_WORDS
};
enum TimerMode
{
	TIMER_CLOCK,
	TIMER_COUNTDOWN_TO,
	TIMER_COUNTDOWN,
	TIMER_STOPWATCH,
	TIMER_COUNTER
};
enum PlateMode
{
	PLATE_OFF,
	PLATE_BLOCK,
	PLATE_LINES
};
// Fill Mode menu values (stored): renderer modes plus the tints done here
enum FillMenu
{
	FILL_SOLID,
	FILL_GRADIENT_BLOCK,
	FILL_GRADIENT_CHARS,
	FILL_STRIPES,
	FILL_HALFTONE,
	FILL_CHECKER,
	FILL_NOISE,
	FILL_LED,
	FILL_ALT_CHARS,
	FILL_ALT_WORDS,
	FILL_ALT_LINES
};
enum OutlineMode
{
	OUTLINE_OFF,
	OUTLINE_ONE,
	OUTLINE_TWO
};
// Shadow and glow menus: bit 1 = outer / drop, bit 2 = inner
enum EffectMode
{
	EFFECT_OFF,
	EFFECT_OUTER,
	EFFECT_INNER,
	EFFECT_BOTH
};
enum ThreeDMode
{
	THREED_OFF,
	THREED_TILT,
	THREED_EXTRUDE
};

constexpr int kNumLineStyles = 2;// "Line 2", "Line 3+"
constexpr unsigned lineParam( int style, LineParam p ) { return ( style == 0 ? P_L2 : P_L3 ) + p; }// style 0 = Line 2
constexpr unsigned lineMode( int style ) { return style == 0 ? P_L2_MODE : P_L3_MODE; }

constexpr int kNumAnimators = 4;
constexpr unsigned animParam( int animator, AnimParam p ) { return P_A1 + (unsigned)animator * AP_COUNT + p; }

// Menus of the one-shot loaders (Presets.cpp)
extern const std::vector< ffglkit::OptionItem > kPresetItems;
extern const std::vector< ffglkit::OptionItem > kLookItems;
// Slide position 0..1 <-> slide index (TextSource.cpp)
int slideFromPosition( float position, int count );
float slidePosition( int index, int count );

class TextSource : public ffglkit::ParamPlugin
{
public:
	TextSource();
	~TextSource() override;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult DeInitGL() override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;
	char* GetParameterDisplay( unsigned int index ) override;
	FFResult SetTime( double time ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;

	static bool isEffect();// built as Text+ Fill

private:
	// ---- Params.cpp
	void defineParameters();
	void updateVisibility();
	void deriveModesFromValues();// compositions from 1.x: switch on the effects they use
	void onModePicked( unsigned index, int previous );// a mode menu went from off to on: give the effect a visible size
	bool isModeParam( unsigned index ) const;
	int source() const { return (int)values[ P_SOURCE ]; }
	int display() const { return (int)values[ P_DISPLAY ]; }
	bool slidesOn() const { return display() == DISPLAY_SLIDES_LINES || display() == DISPLAY_SLIDES_BLOCKS || display() == DISPLAY_SLIDES_WORDS; }
	bool animatorOn( int a ) const { return (int)values[ animParam( a, AP_MODE ) ] != (int)AnimMode::Off; }
	// Animator property rows: shown = used (not neutral) or added with "+ Property"
	bool propertyUsed( int a, int prop ) const;
	void refreshShownProperties( int a, bool keepAdded );
	void onAddProperty( int a, int item );
	unsigned shownProps[ kNumAnimators ] = { 0, 0, 0, 0 };

	// ---- Presets.cpp
	void applyPreset( int animator, int preset );
	void applyLook( int look );

	// Fonts
	void populateFontMenus( bool raiseEvent );
	void syncMenusToFont();
	void resolveFont();
	void onFamilyPicked( int index );
	void onStylePicked( int index );
	void setFontText( const std::string& text, bool raiseEvent );

	// Text
	void pollTextFile();
	std::string sourceText();
	std::string currentText();
	void stepSlide( int delta );
	void showLines( int count );
	std::string expandLive( const std::string& text );

	// Clock & timer
	double wallSeconds() const;
	double timerElapsed() const;
	void timerStart();
	void timerStop();
	void timerReset();
	std::string timerValueText( bool& hide );
	void onClipStart();

	// Animation
	AnimatorParams animatorParams( int animator ) const;

	LayoutParams currentLayoutParams( const std::string& text ) const;
	sdfkit::Style currentStyle() const;
	sdfkit::View3D currentView() const;
	PathParams currentPath() const;
	double currentBeats() const;

	std::string text, fontText, fontFile, textFile, rowTemplate, features, oscPortText;
	std::string timerTarget = "20:00", timerDuration = "5:00", separator = "   \xE2\x80\xA2   ";
	static constexpr int kExtraLines = P_LINE_LAST - P_LINE_FIRST + 1;
	std::string extraLines[ kExtraLines ];
	int shownLines = 0;// extra line fields currently shown
	// Slide +/- detection: Resolume's +/- buttons step a slider by 0.01
	double lastSlideHostChange = -1.0;
	bool live = false;// first frame rendered: value changes now come from the user
	bool sourceFromHost = false;// the host has set "Source" (a composition made with 1.1 or later)
	bool defaultsDone   = false;// the SDK sets every parameter to its default once, in order, after creation


	// Line styles (index 0 = "Line 2", 1 = "Line 3+")
	struct LineFont
	{
		std::string text;                 // LP_FONT
		std::shared_ptr< FontFace > face; // resolved; nullptr = main face
		bool dirty = true;
		std::vector< FontEntry > weightMenu;
		std::string weightMenuKey;
	};
	LineFont lineFonts[ kNumLineStyles ];
	void resolveLineFont( int style );
	void onWeightPicked( int style, int index );
	void setLineFontText( int style, const std::string& text, bool raiseEvent );

	// Font state
	std::shared_ptr< FontFace > face;
	bool fontDirty      = true;
	bool menusPopulated = false;
	std::vector< std::string > familyMenu;
	std::vector< FontEntry > styleMenu;
	std::string styleMenuKey;

	// Text file
	std::string textFileContent;
	int64_t textFileStamp   = -1;
	int textFilePollCounter = 0;
	bool csvActive          = false;
	bool csvWasActive       = false;

	// Slides
	int slideCount         = 1;
	int slideIndex         = 0;// shown slide (position in the shown order)
	int slideTarget        = 0;// slide the Slide position asks for; differs while the old one plays out
	bool slideOutPhase     = false;
	double slideOutStartBeat = 0.0;
	double replayOutLength() const;// beats the out phase takes (all animators, staggered)
	int lastSlideShown     = -1;
	int reportedSlideCount = -1;// slide count the host's "2 / 5" display was last refreshed for
	double slideStartBeat  = 0.0;
	std::string lastSlidesSource;
	std::vector< std::string > slides;
	std::vector< int > slideOrder;// shown position -> slide

	// Timer (wall clock, seconds)
	bool timerRunning      = false;
	double timerAccumulated = 0.0;
	double timerStartedAt   = 0.0;

	// Clip state
	double lastFrameWall = -1.0;
	bool retriggered     = false;
	double scrollTime    = 0.0;// seconds the ticker / roll has been moving
	double hostDelta     = 0.0;// host time passed since the last frame
	int hostTimeCalls    = 0;  // the host sends its time (then tickers follow it, e.g. a paused clip)

	// Live data
	std::shared_ptr< OscReceiver > osc;
	int oscPort = 0;
	bool oscWarned = false;
	float hostBpm  = 0.0f;

	// Layout cache
	LayoutParams lastLayoutParams;
	uint32_t lastLayoutFaceId = 0;
	LayoutResult layout;
	bool layoutValid = false;

	// Beat tracking
	float lastBarPhase  = -1.0f;
	double barCount     = 0.0;
	bool haveBeatInfo   = false;
	double beatsNow     = 0.0;
	double lastHostTime = -1.0;

	// GL
	sdfkit::Atlas atlas;
	sdfkit::Renderer renderer;
	std::vector< LaidGlyph > placed;// glyphs as drawn (ticker copies, roll offset)
	std::vector< GlyphState > glyphStates, plateStates;
	std::vector< sdfkit::ShapeInstance > instances, plates;
	std::vector< uint64_t > prewarmed;// face/level combinations whose Latin-1 glyphs are queued
	std::unordered_map< uint64_t, uint32_t > scrambleGlyphs;// (face, code point) -> glyph id
};

}//namespace textplus
