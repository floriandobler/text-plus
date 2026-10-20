#pragma once
// Animator: per-glyph offset / scale / rotation / opacity / colour as a pure
// function of its parameters and the progress or beat position. No hidden
// state, so scrubbing, reversing, restarting and BPM sync always give the
// same frame.
//
// Model (close to Apple Motion's "Sequence Text" and After Effects' range
// selector, reduced to what is controllable with sliders):
//  - The animator describes an "away" state: Offset X/Y, Scale, Rotation,
//    Opacity, Colour. At amount 0 a unit is at rest, at amount 1 it is fully
//    in the away state.
//  - The mode decides the amount per unit (character, word or line):
//      Sequence In   units travel from away -> rest one after another as
//                    Progress goes 0 -> 1 ("Spread" = how many units move at once)
//      Sequence Out  rest -> away
//      Sequence In/Out  in over the first part of Progress, rest for "Hold",
//                    out over the last part (first in, first out)
//      Wave          travelling sine wave; Progress is the phase (0..1 = one
//                    cycle, loops seamlessly), Spread the wavelength in units
//      Random        per-unit random values; Progress runs through a loop of
//                    Spread random states (0 and 1 are the same state)
//      Constant      every unit gets amount = Progress
// There is no clock inside the animator: everything moves because the host
// animates Progress (BPM sync, timeline, OSC), so it is always scrubbable.
//  - Order changes which unit goes first (forward, reverse, centre, edges, random).
//  - Wipe hides a unit behind a moving edge (from left, right, centre, top or
//    bottom of the unit) while it is away; Scramble shows random characters.
//  - Range limits the animator to a part of the text (from..to in reading
//    order); a sequence then travels across that part only.
// Several animators stack: offsets and rotations add, scale and opacity
// multiply, colours layer.

#include "TextLayout.h"

#include <vector>

namespace textplus
{

// Values are stored in compositions: append, never renumber.
enum class AnimMode
{
	Off,
	SequenceIn,
	SequenceOut,
	Wave,
	Random,
	Constant,
	SequenceInOut
};
enum class AnimTarget
{
	Text,
	Plate,
	Both
};
enum class WipeFrom
{
	Left,
	Right,
	Center,
	Top,
	Bottom
};
enum class Charset
{
	Letters,
	Digits,
	Symbols,
	Binary,
	SameAsText
};
enum class AnimUnit
{
	Character,
	Word,
	Line
};
enum class AnimOrder
{
	Forward,
	Reverse,
	FromCenter,
	FromEdges,
	Random,
	LeftToRight,// by position: units at the same x in different lines move together
	TopToBottom // line by line, whatever the unit
};
enum class AnimEase
{
	Linear,
	Smooth,
	EaseOut,
	Overshoot,
	EaseIn,
	EaseInOut,
	Bounce,
	Elastic
};
constexpr int kNumOrders = 7;
constexpr int kNumEases  = 8;

struct AnimatorParams
{
	AnimMode mode   = AnimMode::Off;
	AnimUnit unit   = AnimUnit::Character;
	AnimOrder order = AnimOrder::Forward;
	AnimEase ease   = AnimEase::Smooth;
	float progress  = 1.0f;// 0..1: sequence position, wave phase, random position, constant amount
	float spread    = 3.0f;// units moving at once (sequence), wavelength (wave), random states (random)
	// away state
	float offsetX = 0, offsetY = 0;// px, y down
	float scale       = 1.0f;      // 1 = 100 %
	float rotationDeg = 0.0f;      // counter-clockwise
	float opacity     = 1.0f;      // 0..1
	float colorAmount = 0.0f;      // 0..1
	Rgb color;
	float skew     = 0.0f;         // tan of the lean angle when away
	float spacing  = 0.0f;         // px added between characters when away (tracking)
	float thickness = 0.0f;        // px the shape grows (> 0) or thins when away
	float blur     = 0.0f;         // px of edge softness when away
	float rangeStart = 0.0f, rangeEnd = 1.0f;// share of the units (reading order) the animator works on
	float wipe     = 0.0f;         // 0..1: share of the unit hidden when away
	WipeFrom wipeFrom = WipeFrom::Left;
	float scramble = 0.0f;         // 0..1: chance of a random character when away
	Charset charset = Charset::Letters;
	float hold     = 0.5f;         // Sequence In/Out: share of Progress at rest
	double beats   = 0.0;          // host beat position (scramble changes on the beat)
	unsigned seed = 1;
};

// Unit boxes in layout space (px, y down) for wipes.
struct UnitBox
{
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// What an animator moves: glyphs, or plates (then all three indices are the plate index).
struct AnimItem
{
	int charIndex = 0, wordIndex = 0, lineIndex = 0;
	UnitBox charBox, wordBox, lineBox;
	float lineOffset = 0;// characters from the middle of its line (spacing)
};
struct UnitCounts
{
	int chars = 1, words = 1, lines = 1;
};

struct GlyphState
{
	float dx = 0, dy = 0;// px, y down
	float scale    = 1.0f;
	float rotation = 0.0f;// radians, counter-clockwise on screen
	float alpha    = 1.0f;
	Rgb tint;             // fill tint colour
	float tintAmount = 0;// 0 = plain fill
	// Visible rectangle in layout space (wipes); infinite = no clip
	float clipX0 = -1e9f, clipY0 = -1e9f, clipX1 = 1e9f, clipY1 = 1e9f;
	uint32_t scramble = 0;// != 0: show a random character, this is its seed
	Charset charset   = Charset::Letters;
	float skew   = 0;// tan
	float weight = 0;// px
	float blur   = 0;// px
};

// Items for laid glyphs. Word and line boxes are the union of their characters'
// boxes; characters span their advance horizontally and their line vertically.
// `margin` widens every box (effects that reach beyond the glyph).
std::vector< AnimItem > glyphItems( const std::vector< LaidGlyph >& glyphs, const std::vector< LineBox >& lines, float margin );
UnitCounts unitCounts( const LayoutResult& layout );

// Resets `states` to rest (one per laid glyph) with inline colours applied.
void initGlyphStates( const LayoutResult& layout, std::vector< GlyphState >& states );
// Applies one animator on top of the current states (one state per item).
void applyAnimator( const std::vector< AnimItem >& items, const UnitCounts& counts, const AnimatorParams& params, std::vector< GlyphState >& states );
// Same for the glyphs of a layout.
void applyAnimator( const LayoutResult& layout, const AnimatorParams& params, std::vector< GlyphState >& states );
// The characters a charset draws from (UTF-8 code points); empty for SameAsText.
const std::vector< uint32_t >& charsetCodepoints( Charset c );
// Layers colour c with amount k over the glyph's current tint.
void layerTint( GlyphState& s, const Rgb& c, float k );

// Largest scale a glyph can reach with these parameters (for distance-field reach).
float maxAnimatedScale( const AnimatorParams& params );

}//namespace textplus
