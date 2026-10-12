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
#include "core/TextLayout.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace textplus
{

// Parameters of one animator, in order. Animator 2 repeats the block.
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
	AP_OFFSET_X,
	AP_OFFSET_Y,
	AP_SCALE,
	AP_ROTATION,
	AP_OPACITY,
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

// Order = order in Resolume: content, look, motion, set-once options.
enum ParamId : unsigned int
{
	// Text: where it comes from (Source) and how it is shown (Display)
	P_SOURCE,
	P_DISPLAY,
	P_TEXT,
	P_LINE_FIRST,                    // "Text 2" .. "Text 10": extra text fields, shown one by one with "+ Line"
	P_LINE_LAST = P_LINE_FIRST + 8,
	P_ADD_LINE,
	P_REMOVE_LINE,
	P_TEXT_FILE,
	P_CSV_HEADER,
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
	P_REPLAY_STAGGER,// Animator 2 starts later by this share of the replay
	// Ticker / Roll
	P_SCROLL_SPEED,
	P_SCROLL_POSITION,
	P_SEPARATOR,
	P_ROLL_LOOP,
	P_EDGE_FADE,
	P_SLIDES_LEGACY,// "Slides" of 1.0, hidden: migrates old compositions to Display
	// Font
	P_FAMILY,
	P_STYLE,
	P_SIZE,
	P_FONT,
	P_FONT_FILE,
	// Line styles
	P_L2,
	P_L3 = P_L2 + LP_COUNT,
	// Layout
	P_POS_X = P_L3 + LP_COUNT,
	P_POS_Y,
	P_ROTATION,
	P_ALIGN,
	P_CASE,
	P_ANCHOR_H,
	P_ANCHOR_V,
	P_WRAP,
	P_BOX_WIDTH,
	P_BOX_HEIGHT,
	P_FIT,
	P_TRACKING,
	P_LEADING,
	// Fill
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
	// Outline (two bands)
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
	// Plate
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
	// Shadow (drop shadow, then inner shadow; one light direction)
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
	// Glow (outer, then inner)
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
	// Animators
	P_A1,
	P_A2 = P_A1 + AP_COUNT,
	// Options (set once)
	P_PIPE_NEWLINE = P_A2 + AP_COUNT,
	P_MARKUP,
	P_KERNING,
	P_LIGATURES,
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
	DISPLAY_ROLL
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

constexpr int kNumLineStyles = 2;// "Line 2", "Line 3+"
constexpr unsigned lineParam( int style, LineParam p ) { return ( style == 0 ? P_L2 : P_L3 ) + p; }// style 0 = Line 2

constexpr int kNumAnimators = 2;
inline unsigned animParam( int animator, AnimParam p ) { return ( animator == 0 ? P_A1 : P_A2 ) + p; }

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
	bool slidesOn() const { return display() == DISPLAY_SLIDES_LINES || display() == DISPLAY_SLIDES_BLOCKS; }

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

	LayoutParams currentLayoutParams( const std::string& text ) const;
	sdfkit::Style currentStyle() const;
	double currentBeats() const;

	std::string text, fontText, fontFile, textFile;
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
