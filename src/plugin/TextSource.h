#pragma once
// TextSource: the FFGL adapter. Owns the parameters, talks to Resolume
// (dynamic dropdowns, visibility, value events, presets) and drives the core
// and the renderer.

#include <FFGLSDK.h>

#include "core/Animator.h"
#include "core/FontDatabase.h"
#include "core/FontFace.h"
#include "core/TextLayout.h"
#include "render/GlyphAtlas.h"
#include "render/TextRenderer.h"

#include <memory>
#include <string>
#include <vector>

namespace textplus
{

// Parameters of one animator, in order. Animator 2 repeats the block.
enum AnimParam : unsigned int
{
	AP_PRESET,
	AP_MODE,
	AP_UNIT,
	AP_ORDER,
	AP_DRIVE,
	AP_PROGRESS,
	AP_DURATION,
	AP_SPREAD,
	AP_EASE,
	AP_SPEED,
	AP_OFFSET_X,
	AP_OFFSET_Y,
	AP_SCALE,
	AP_ROTATION,
	AP_OPACITY,
	AP_COLOR_H,
	AP_COLOR_S,
	AP_COLOR_B,
	AP_COLOR_A,
	AP_COUNT
};

enum ParamId : unsigned int
{
	// Text
	P_TEXT,
	P_PIPE_NEWLINE,
	P_MARKUP,
	P_TEXT_FILE,
	P_SLIDES,
	P_SLIDE,// position 0..1, split into equal shares per slide (BPM sync friendly)
	P_NEXT_SLIDE,
	P_PREV_SLIDE,
	// Font
	P_FONT,
	P_FAMILY,
	P_STYLE,
	P_FONT_FILE,
	P_SIZE,
	// Layout
	P_POS_X,
	P_POS_Y,
	P_ROTATION,
	P_ALIGN,
	P_ANCHOR_H,
	P_ANCHOR_V,
	P_BOX_WIDTH,
	P_BOX_HEIGHT,
	P_FIT,
	P_TRACKING,
	P_LEADING,
	P_KERNING,
	P_LIGATURES,
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
	// Outline 1
	P_OL1_WIDTH,
	P_OL1_POSITION,
	P_OL_CORNERS,
	P_OL1_H,
	P_OL1_S,
	P_OL1_B,
	P_OL1_A,
	// Outline 2
	P_OL2_WIDTH,
	P_OL2_H,
	P_OL2_S,
	P_OL2_B,
	P_OL2_A,
	// Shadow
	P_SH_H,
	P_SH_S,
	P_SH_B,
	P_SH_A,
	P_SH_DISTANCE,
	P_SH_ANGLE,
	P_SH_SOFTNESS,
	P_SH_SPREAD,
	// Glow
	P_GL_H,
	P_GL_S,
	P_GL_B,
	P_GL_A,
	P_GL_RADIUS,
	P_GL_STRENGTH,
	// Animators
	P_A1,
	P_A2 = P_A1 + AP_COUNT,
	// Quality
	P_EDGE = P_A2 + AP_COUNT,

	P_COUNT
};

constexpr int kNumAnimators = 2;
inline unsigned animParam( int animator, AnimParam p ) { return ( animator == 0 ? P_A1 : P_A2 ) + p; }

class TextSource : public CFFGLPlugin
{
public:
	TextSource();
	~TextSource() override;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult DeInitGL() override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetTextParameter( unsigned int index ) override;
	char* GetParameterDisplay( unsigned int index ) override;
	FFResult SetTime( double time ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;

private:
	void defineParameters();
	void updateVisibility( bool raiseEvent );
	void setValue( unsigned id, float v );// change own value + tell the host

	// Fonts
	void populateFontMenus( bool raiseEvent );
	void syncMenusToFont();
	void resolveFont();
	void onFamilyPicked( int index );
	void onStylePicked( int index );
	void setFontText( const std::string& text, bool raiseEvent );

	// Text & slides
	void pollTextFile();
	std::string currentText();
	void stepSlide( int delta );

	// Animation
	void applyPreset( int animator, int preset );
	AnimatorParams animatorParams( int animator ) const;
	float animatorProgress( int animator ) const;

	LayoutParams currentLayoutParams( const std::string& text ) const;
	Style currentStyle() const;
	double currentBeats() const;

	float values[ P_COUNT ]{};
	// The value the host last sent or read back, per parameter. Menus that act on
	// a choice (presets, font menus) and the Slide position react only when this
	// changes, so a host that re-sends unchanged values never re-applies them.
	float hostKnown[ P_COUNT ];
	bool hostChanged( unsigned index, float value );
	std::vector< signed char > visibility;// last visibility sent to the host
	std::string text, fontText, fontFile, textFile;
	std::string textReturnBuffer, displayBuffer;
	bool live = false;// first frame rendered: value changes now come from the user

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

	// Slides
	int slideCount       = 1;
	int slideIndex       = 0;// shown slide
	int lastSlideShown   = -1;
	double slideStartBeat = 0.0;
	std::string lastSlidesSource;
	std::vector< std::string > slides;

	// Layout cache
	LayoutParams lastLayoutParams;
	uint32_t lastLayoutFaceId = 0;
	LayoutResult layout;
	bool layoutValid = false;

	// Beat tracking
	float lastBarPhase = -1.0f;
	double barCount    = 0.0;
	bool haveBeatInfo  = false;
	double beatsNow    = 0.0;
	double lastHostTime = -1.0;

	// GL
	GlyphAtlas atlas;
	TextRenderer renderer;
	std::vector< GlyphState > glyphStates;
	std::vector< GlyphInstance > instances;
	uint64_t prewarmedKey = 0;
};

}//namespace textplus
