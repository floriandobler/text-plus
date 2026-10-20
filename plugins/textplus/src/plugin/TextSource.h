#pragma once
// TextSource: the FFGL adapter of Text+. Owns the parameters, talks to Resolume
// (dynamic dropdowns, visibility, value events, presets) and drives the core
// (fonts, layout, animators, clock) and the renderer (sdfkit).

#include "ffglkit/ParamPlugin.h"
#include "sdfkit/Atlas.h"
#include "sdfkit/Renderer.h"

#include "core/Animator.h"
#include "core/Clock.h"
#include "core/FontDatabase.h"
#include "core/FontFace.h"
#include "core/LiveData.h"
#include "core/TextLayout.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace textplus
{

// Parameters of one animator, in order. Animators 2 and 3 repeat the block.
// The stored names keep the 1.1 names ("A1 Offset X"), so compositions and
// OSC addresses stay valid; Resolume shows display names without the prefix.
enum AnimParam : unsigned int
{
	AP_PRESET,
	AP_MODE,
	AP_TARGET,  // Text / Plate / Both
	AP_UNIT,
	AP_ORDER,
	AP_PROGRESS,
	AP_SPREAD,
	AP_HOLD,    // Sequence In/Out: share at rest
	AP_EASE,
	AP_PROPERTY,// menu: add or remove a property row
	// Properties: the "away" state. A row is shown when added, or when its value is not neutral.
	AP_OFFSET_X,
	AP_OFFSET_Y,
	AP_DEPTH,
	AP_SCALE,
	AP_ROTATION,
	AP_ROTATION_X,
	AP_ROTATION_Y,
	AP_SKEW,
	AP_TRACKING,
	AP_OPACITY,
	AP_BLUR,
	AP_WEIGHT,
	AP_WIPE,
	AP_WIPE_FROM,
	AP_SCRAMBLE,
	AP_CHARSET,
	AP_COLOR_H,
	AP_COLOR_S,
	AP_COLOR_B,
	AP_COLOR_A,
	AP_COUNT
};

// Properties offered by an animator's "+ Property" menu (menu values: add = id, remove = 100 + id).
enum AnimProp : int
{
	PROP_NONE = 0,
	PROP_OFFSET_X,
	PROP_OFFSET_Y,
	PROP_DEPTH,
	PROP_SCALE,
	PROP_ROTATION,
	PROP_ROTATION_X,
	PROP_ROTATION_Y,
	PROP_SKEW,
	PROP_TRACKING,
	PROP_OPACITY,
	PROP_BLUR,
	PROP_WEIGHT,
	PROP_WIPE,
	PROP_SCRAMBLE,
	PROP_COLOR,
	PROP_COUNT
};

// Parameters of one line style ("Line 2", "Line 3+"), in order.
enum LineParam : unsigned int
{
	LP_MODE,    // Same as the line above / Own
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

// Parameters of one effect slot of the style stack. The type decides what the
// generic values mean and how Resolume labels them (see sdfkit::Effect).
enum FxParam : unsigned int
{
	FX_TYPE,
	FX_COLOR_H,
	FX_COLOR_S,
	FX_COLOR_B,
	FX_COLOR_A,
	FX_COLOR2_H,
	FX_COLOR2_S,
	FX_COLOR2_B,
	FX_COLOR2_A,
	FX_SIZE,
	FX_SOFTNESS,
	FX_DISTANCE,
	FX_ANGLE,
	FX_AMOUNT,
	FX_MODE,
	FX_COUNT
};
constexpr int kNumFx = sdfkit::kMaxEffects;

// Order = order in Resolume: content, look, motion, layout, set-once options.
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
	P_CASE,
	P_TEXT_FILE,
	P_CSV_HEADER,
	// Web
	P_URL,
	P_URL_REFRESH,
	// Clock & Timer
	P_TIMER_MODE,
	P_TIMER_TARGET,  // text "20:00"
	P_TIMER_DURATION,// text "5:00"
	P_TIMER_START,
	P_TIMER_STOP,
	P_TIMER_RESET,
	P_TIMER_AUTOSTART,
	P_TIMER_FORMAT,
	P_TIMER_AT_ZERO,
	P_WARN_AT,       // seconds left at which a countdown turns to the warning colour (0 = off)
	P_WARN_H,
	P_WARN_S,
	P_WARN_B,
	P_WARN_A,
	P_WARN_BLINK,
	P_COUNT_FROM,
	P_COUNT_TO,
	P_COUNT_PROGRESS,
	P_COUNT_DECIMALS,
	P_NUMBER_FORMAT,
	// Slides
	P_SLIDE,// position 0..1, split into equal shares per slide (BPM sync friendly)
	P_NEXT_SLIDE,
	P_PREV_SLIDE,
	P_REPLAY,// beats; > 0 replays the sequence animators on every slide change
	P_REPLAY_STAGGER,// later animators start later by this share of the replay
	// Ticker / Roll / Pattern
	P_SCROLL_SPEED,
	P_SCROLL_POSITION,
	P_SEPARATOR,
	P_ROLL_LOOP,
	P_PATTERN_ROWS,
	P_PATTERN_SPACING,
	P_PATTERN_SHIFT,
	P_PATTERN_ALTERNATE,
	P_EDGE_FADE,
	P_SLIDES_LEGACY,// "Slides" of 1.0, hidden: migrates old compositions to Display
	// ---- Font
	P_FAMILY,
	P_STYLE,
	P_SIZE,
	P_FONT,
	P_WEIGHT,
	P_TRACKING,
	P_LEADING,
	// ---- Line styles
	P_L2,
	P_L3 = P_L2 + LP_COUNT,
	// ---- Style: look presets, fill, effect slots
	P_LOOK = P_L3 + LP_COUNT,
	P_INPUT_MODE,// Text+ FX only
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
	P_FILL_SCALE,
	P_FX_FIRST,
	P_OL_CORNERS = P_FX_FIRST + kNumFx * FX_COUNT,
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
	// ---- Animators
	P_A1,
	P_A2 = P_A1 + AP_COUNT,
	P_A3 = P_A2 + AP_COUNT,
	// ---- Layout
	P_PLACEMENT = P_A3 + AP_COUNT,
	P_MARGIN,
	P_POS_X,
	P_POS_Y,
	P_ROTATION,
	P_ANCHOR_H,
	P_ANCHOR_V,
	P_ALIGN,
	P_WRAP,
	P_BOX_WIDTH,
	P_BOX_HEIGHT,
	P_FIT,
	P_PATH,
	P_PATH_RADIUS,
	P_PATH_AMOUNT,
	P_PATH_LENGTH,
	P_PATH_OFFSET,
	P_PATH_TILT,
	P_BACK_OPACITY,
	P_PERSPECTIVE,
	// ---- Options (set once)
	P_SHOW_OPTIONS,
	P_PIPE_NEWLINE,
	P_MARKUP,
	P_KERNING,
	P_LIGATURES,
	P_EDGE,
	P_FONT_FILE,
	P_OSC_PORT,
	P_COPY_LOOK,
	P_PASTE_LOOK,
	// ---- Hidden: the effect groups of 1.1, translated into slots when a 1.1 composition loads
	P_LEG_OL1_WIDTH,
	P_LEG_OL1_POSITION,
	P_LEG_OL1_H,
	P_LEG_OL1_S,
	P_LEG_OL1_B,
	P_LEG_OL1_A,
	P_LEG_OL2_WIDTH,
	P_LEG_OL2_H,
	P_LEG_OL2_S,
	P_LEG_OL2_B,
	P_LEG_OL2_A,
	P_LEG_SH_H,
	P_LEG_SH_S,
	P_LEG_SH_B,
	P_LEG_SH_A,
	P_LEG_SH_DISTANCE,
	P_LEG_SH_ANGLE,
	P_LEG_SH_SOFTNESS,
	P_LEG_SH_SPREAD,
	P_LEG_ISH_DISTANCE,
	P_LEG_ISH_SOFTNESS,
	P_LEG_ISH_H,
	P_LEG_ISH_S,
	P_LEG_ISH_B,
	P_LEG_ISH_A,
	P_LEG_GL_H,
	P_LEG_GL_S,
	P_LEG_GL_B,
	P_LEG_GL_A,
	P_LEG_GL_RADIUS,
	P_LEG_GL_STRENGTH,
	P_LEG_IGL_RADIUS,
	P_LEG_IGL_H,
	P_LEG_IGL_S,
	P_LEG_IGL_B,
	P_LEG_IGL_A,
	P_LEG_LAST = P_LEG_IGL_A,

	P_COUNT
};

enum SourceMode
{
	SOURCE_INPUT,
	SOURCE_FILE,
	SOURCE_CLOCK,
	SOURCE_WEB
};
enum DisplayMode
{
	DISPLAY_STATIC,
	DISPLAY_SLIDES_LINES,
	DISPLAY_SLIDES_BLOCKS,
	DISPLAY_TICKER,
	DISPLAY_ROLL,
	DISPLAY_WORDS,
	DISPLAY_PATTERN
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
enum Placement
{
	PLACE_CENTER,
	PLACE_CUSTOM,
	PLACE_TOP,
	PLACE_BOTTOM,
	PLACE_LEFT,
	PLACE_RIGHT,
	PLACE_TOP_LEFT,
	PLACE_TOP_RIGHT,
	PLACE_BOTTOM_LEFT,
	PLACE_BOTTOM_RIGHT,
	PLACE_LOWER_THIRD
};
enum PathMode
{
	PATH_FLAT,
	PATH_ARC,
	PATH_CIRCLE,
	PATH_WAVE,
	PATH_RING
};

constexpr int kNumLineStyles = 2;// "Line 2", "Line 3+"
constexpr unsigned lineParam( int style, LineParam p ) { return ( style == 0 ? P_L2 : P_L3 ) + p; }// style 0 = Line 2

constexpr int kNumAnimators = 3;
inline unsigned animParam( int animator, AnimParam p ) { return P_A1 + (unsigned)animator * AP_COUNT + p; }
inline unsigned fxParam( int slot, FxParam p ) { return P_FX_FIRST + (unsigned)slot * FX_COUNT + p; }

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

private:
	void defineParameters();
	void updateVisibility();
	int source() const { return (int)values[ P_SOURCE ]; }
	int display() const { return (int)values[ P_DISPLAY ]; }
	bool slidesOn() const { return display() == DISPLAY_SLIDES_LINES || display() == DISPLAY_SLIDES_BLOCKS || display() == DISPLAY_WORDS; }

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

	// Clock & timer
	double wallSeconds() const;
	double timerElapsed() const;
	void timerStart();
	void timerStop();
	void timerReset();
	std::string timerValueText( bool& hide );
	void onClipStart();

	// Animation
	void applyPreset( int animator, int preset );
	AnimatorParams animatorParams( int animator ) const;
	bool animatorOn( int animator ) const { return (int)values[ animParam( animator, AP_MODE ) ] != 0; }
	// Property rows: shown when added with "+ Property" or when the value is not neutral
	uint32_t propShown[ kNumAnimators ] = { 0, 0, 0 };
	bool propVisible( int animator, int prop ) const { return ( propShown[ animator ] >> prop ) & 1u; }
	void syncPropsFromValues( int animator );// show every property whose value is not neutral
	void onPropertyMenu( int animator, int value );
	void updatePropertyMenu( int animator );
	std::string propertyMenuKey[ kNumAnimators ];
	bool any3D() const;

	// Style
	void applyLook( int look );
	void onFxType( int slot, int type );
	void labelFx( int slot );
	int fxType( int slot ) const { return (int)values[ fxParam( slot, FX_TYPE ) ]; }
	int lastFxType[ kNumFx ] = { 0, 0, 0, 0, 0, 0 };
	int fxModeMenuType[ kNumFx ] = { -1, -1, -1, -1, -1, -1 };
	void setFxDefaults( int slot, int type );
	void copyLook();
	void pasteLook();
	// 1.1 compositions: Outline / Shadow / Glow groups -> slots
	bool legacyPending = false;
	void migrateLegacyEffects();

	// Layout
	void onPlacement( int placement );
	int lastPlacement = 0;
	bool multiLine     = false;// the source text has more than one line (shows Align, Leading, Line 2)
	int multiLineCount = 1;    // lines of the source text (shows "Line 2" / "Line 3+")
	float legacyDefault[ P_LEG_LAST - P_LEG_OL1_WIDTH + 1 ] = {};
	bool optionsShown() const { return values[ P_SHOW_OPTIONS ] > 0.5f; }

	LayoutParams currentLayoutParams( const std::string& text ) const;
	sdfkit::Style currentStyle() const;
	double currentBeats() const;

	std::string text, fontText, fontFile, textFile, url;
	// Live data
	std::string webStatus, oscStatus;
	int64_t webStamp = -1;
	int webFormat    = 0;
	std::string webBody;
	Json webJson;
	Json fileJson;
	int64_t jsonFileStamp = -2;
	int oscPort      = 0;
	double lastBpm   = 0.0;
	double remainingSeconds = 1e30;// countdowns: seconds left (for the warning colour)
	std::string typedText() const;
	PlaceholderContext placeholderContext() const;
	void updateOsc();
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
	int slideIndex         = 0;// shown slide
	int lastSlideShown     = -1;
	int reportedSlideCount = -1;// slide count the host's "2 / 5" display was last refreshed for
	double slideStartBeat  = 0.0;
	std::string lastSlidesSource;
	std::vector< std::string > slides;

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
