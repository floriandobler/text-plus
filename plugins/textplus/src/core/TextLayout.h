#pragma once
// TextLayout: shaping (HarfBuzz), line breaking, alignment and anchoring.
//
// Coordinate system of the result: pixels, x right, y DOWN, origin at the
// anchor point. The renderer only has to translate/rotate the whole block.
//
// Design rules (see the architecture doc):
//  - Vertical metrics come from the font (ascender/descender), never from the
//    ink bounding box, so the block does not jump when the text changes.
//  - The anchor is independent from the alignment. With AnchorH::Auto the
//    anchor sits on the alignment side, so left-aligned text keeps its left
//    edge when lines grow (Illustrator "point text" behaviour). With a fixed
//    box width (area text) Auto centres the box on the anchor.

#include "FontFace.h"
#include "Utf8.h"

#include <string>
#include <vector>

namespace textplus
{

enum class Align
{
	Left,
	Center,
	Right,
	Justify
};
enum class AnchorH
{
	Auto,
	Left,
	Center,
	Right
};
enum class AnchorV
{
	Top,
	Middle,
	Bottom,
	FirstBaseline,
	LastBaseline
};
enum class FitMode
{
	Off,
	ShrinkToBox,
	FitWidth
};

// Style of a line position. Index 0 is the main style (line 1); index 1 is
// "Line 2", index 2 "Line 3+" (every line from the third on). A style that is
// not `own` repeats the previous one, so by default all lines look alike.
// A "line" is a line of the source text (a newline, "|" or CSV cell), not a
// line created by wrapping: wrapped parts keep the style of their line.
struct LineStyle
{
	bool own                = false;
	const FontFace* face    = nullptr;// nullptr = the main face
	float sizePercent       = 100.0f; // of the main size
	float trackingPermille  = 0.0f;   // added to the main tracking
	float spacingPercent    = 0.0f;   // extra space above the line, % of the main size

	bool operator==( const LineStyle& o ) const
	{
		return own == o.own && face == o.face && sizePercent == o.sizePercent && trackingPermille == o.trackingPermille &&
			spacingPercent == o.spacingPercent;
	}
};
constexpr int kLineStyles = 3;

struct LayoutParams
{
	std::string text;
	float sizePx           = 160.0f;
	float trackingPermille = 0.0f;  // 1/1000 em added after every glyph
	float leadingPercent   = 120.0f;// baseline-to-baseline distance in % of size
	float boxWidth         = 0.0f;  // px, 0 = no wrapping
	float boxHeight        = 0.0f;  // px, 0 = unlimited (used by ShrinkToBox)
	Align align            = Align::Center;
	AnchorH anchorH        = AnchorH::Auto;
	AnchorV anchorV        = AnchorV::Middle;
	FitMode fit            = FitMode::Off;
	bool kerning           = true;
	bool ligatures         = true;
	bool pipeIsNewline     = true;
	bool markup            = true;// inline colours {#ff8800}...{/}
	LineStyle lines[ kLineStyles ];// [0] is ignored: line 1 always uses the main style

	bool operator==( const LayoutParams& o ) const
	{
		for( int i = 1; i < kLineStyles; ++i )
			if( !( lines[ i ] == o.lines[ i ] ) )
				return false;
		return text == o.text && sizePx == o.sizePx && trackingPermille == o.trackingPermille && leadingPercent == o.leadingPercent &&
			boxWidth == o.boxWidth && boxHeight == o.boxHeight && align == o.align && anchorH == o.anchorH && anchorV == o.anchorV &&
			fit == o.fit && kerning == o.kerning && ligatures == o.ligatures && pipeIsNewline == o.pipeIsNewline && markup == o.markup;
	}
	bool operator!=( const LayoutParams& o ) const { return !( *this == o ); }
};

struct LaidGlyph
{
	uint32_t glyphId = 0;
	float x = 0, y = 0;// glyph origin on the baseline, px, relative to the anchor
	int charIndex = -1;// running index over visible glyphs (spaces excluded)
	int wordIndex = 0;
	int lineIndex = 0;
	bool visible  = true;// false for spaces and other blank glyphs
	int colorIndex = -1; // inline colour (index into LayoutResult::palette), -1 = none
	int style      = 0;  // effective line style (0 main, 1 "Line 2", 2 "Line 3+")
	float scale    = 1.0f;// glyph size relative to LayoutResult::sizePx
	float advance  = 0;   // px, without tracking
	const FontFace* face = nullptr;// fallback face; nullptr = the face of the line style
};

// One laid-out line (after wrapping), relative to the anchor, px, y down.
struct LineBox
{
	float left = 0, right = 0;// ink extent (trailing spaces excluded)
	float penEnd = 0;         // pen position after the last glyph, spaces and tracking included
	float top = 0, bottom = 0;// ascender / descender of the line's style
	float baseline = 0;
	int style = 0;
	int firstGlyph = 0, glyphCount = 0;// range in LayoutResult::glyphs
	bool empty() const { return glyphCount == 0; }
};

struct LayoutResult
{
	std::vector< LaidGlyph > glyphs;
	std::vector< LineBox > lines;
	std::vector< Rgb > palette;// inline colours
	float sizePx   = 0;// effective size after auto-fit
	int numChars   = 0;
	int numWords   = 0;
	int numLines   = 0;
	bool overflow  = false;// text does not fit the box
	// Metric box of the whole block relative to the anchor (px, y down)
	float left = 0, top = 0, right = 0, bottom = 0;
};

// Effective style of source line `index` (0-based) for the given styles.
int lineStyleOf( int index, const LineStyle* styles );

LayoutResult layoutText( const FontFace& face, const LayoutParams& params );

}//namespace textplus
