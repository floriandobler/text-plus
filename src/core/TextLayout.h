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

	bool operator==( const LayoutParams& o ) const
	{
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
};

struct LayoutResult
{
	std::vector< LaidGlyph > glyphs;
	std::vector< Rgb > palette;// inline colours
	float sizePx   = 0;// effective size after auto-fit
	int numChars   = 0;
	int numWords   = 0;
	int numLines   = 0;
	bool overflow  = false;// text does not fit the box
	// Metric box of the whole block relative to the anchor (px, y down)
	float left = 0, top = 0, right = 0, bottom = 0;
};

LayoutResult layoutText( const FontFace& face, const LayoutParams& params );

}//namespace textplus
