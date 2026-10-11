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
//      Wave          travelling sine wave; Progress is the phase (0..1 = one
//                    cycle, loops seamlessly), Spread the wavelength in units
//      Random        per-unit random values; Progress runs through a loop of
//                    Spread random states (0 and 1 are the same state)
//      Constant      every unit gets amount = Progress
// There is no clock inside the animator: everything moves because the host
// animates Progress (BPM sync, timeline, OSC), so it is always scrubbable.
//  - Order changes which unit goes first (forward, reverse, centre, edges, random).
// Several animators stack: offsets and rotations add, scale and opacity
// multiply, colours layer.

#include "TextLayout.h"

#include <vector>

namespace textplus
{

enum class AnimMode
{
	Off,
	SequenceIn,
	SequenceOut,
	Wave,
	Random,
	Constant
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
	Random
};
enum class AnimEase
{
	Linear,
	Smooth,
	EaseOut,
	Overshoot
};

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
	unsigned seed = 1;
};

struct GlyphState
{
	float dx = 0, dy = 0;// px, y down
	float scale    = 1.0f;
	float rotation = 0.0f;// radians, counter-clockwise on screen
	float alpha    = 1.0f;
	Rgb tint;             // fill tint colour
	float tintAmount = 0;// 0 = plain fill
};

// Resets `states` to rest (one per laid glyph) with inline colours applied.
void initGlyphStates( const LayoutResult& layout, std::vector< GlyphState >& states );
// Applies one animator on top of the current states.
void applyAnimator( const LayoutResult& layout, const AnimatorParams& params, std::vector< GlyphState >& states );
// Layers colour c with amount k over the glyph's current tint.
void layerTint( GlyphState& s, const Rgb& c, float k );

// Largest scale a glyph can reach with these parameters (for distance-field reach).
float maxAnimatedScale( const AnimatorParams& params );

}//namespace textplus
