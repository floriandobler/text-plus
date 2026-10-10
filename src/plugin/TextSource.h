#pragma once
// TextSource: the FFGL adapter. Owns the parameters, talks to Resolume
// (dynamic dropdowns, visibility, value events) and drives the core + renderer.

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

namespace kontur
{

enum ParamId : unsigned int
{
	// Text
	P_TEXT,
	P_PIPE_NEWLINE,
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
	P_FILL_H,
	P_FILL_S,
	P_FILL_B,
	P_FILL_A,
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
	// Animation
	P_AN_MODE,
	P_AN_UNIT,
	P_AN_PROGRESS,
	P_AN_SPREAD,
	P_AN_AMOUNT,
	P_AN_SPEED,
	// Quality
	P_EDGE,

	P_COUNT
};

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
	void SetBeatInfo( float bpm, float barPhase ) override;

	// Test hook: lets the headless host wait for fonts/glyphs.
	bool hasPendingGlyphs() const { return pendingGlyphs; }

private:
	void defineParameters();
	void populateFontMenus();// once the font database is ready
	void syncMenusToFont();  // family/style dropdowns follow the "Font" text field
	void resolveFont();
	void onFamilyPicked( int index );
	void onStylePicked( int index );
	void setFontText( const std::string& text, bool raiseEvent );
	LayoutParams currentLayoutParams() const;
	Style currentStyle() const;
	AnimParams currentAnimParams() const;
	void updateVisibility( bool raiseEvent );

	float values[ P_COUNT ]{};
	std::string text, fontText, fontFile;
	std::string textReturnBuffer, displayBuffer;

	// Font state
	std::shared_ptr< FontFace > face;
	bool fontDirty      = true;
	bool menusPopulated = false;
	std::vector< std::string > familyMenu;
	std::vector< FontEntry > styleMenu;
	std::string styleMenuFamily;
	bool fileStylesActive = false;// style menu lists the faces of the Font File

	// Layout cache
	LayoutParams lastLayoutParams;
	uint32_t lastLayoutFaceId = 0;
	LayoutResult layout;
	bool layoutValid = false;

	// Animation / beat tracking
	double beats       = 0.0;
	float lastBarPhase = -1.0f;
	double barCount    = 0.0;
	bool haveBeatInfo  = false;

	// GL
	GlyphAtlas atlas;
	TextRenderer renderer;
	std::vector< GlyphState > glyphStates;
	std::vector< GlyphInstance > instances;
	bool pendingGlyphs = false;
	bool lastVisibilityBox = true, lastVisibilityOutline2 = true;
	bool visibilityInitialised = false;
};

}//namespace kontur
