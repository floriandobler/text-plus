#pragma once
// Animator: per-glyph offset/scale/rotation/alpha as a pure function of the
// parameters and the host beat position. No internal state, so scrubbing,
// reversing, restarting a clip or BPM sync always produce the same frame.
//
// Phase 1 ships four presets. Phase 2 replaces them with a selector + property
// stack (After Effects / Smode style) that uses the same GlyphState output.

#include "TextLayout.h"

#include <vector>

namespace kontur
{

enum class AnimMode
{
	Off,
	Typewriter,
	Fade,
	Rise,
	Wave
};
enum class AnimUnit
{
	Character,
	Word,
	Line
};

struct AnimParams
{
	AnimMode mode  = AnimMode::Off;
	AnimUnit unit  = AnimUnit::Character;
	float progress = 1.0f;// 0..1
	float spread   = 3.0f;// units over which the transition is spread (Fade/Rise), wavelength (Wave)
	float amount   = 40.0f;// px (Rise distance, Wave amplitude)
	float speed    = 1.0f;// Wave: cycles per beat
	double beats   = 0.0; // host beat position
};

struct GlyphState
{
	float dx = 0, dy = 0;// px, y down
	float scale    = 1.0f;
	float rotation = 0.0f;// radians
	float alpha    = 1.0f;
};

void animateGlyphs( const LayoutResult& layout, const AnimParams& params, std::vector< GlyphState >& out );

// Largest scale any glyph can reach with these parameters (for spread selection).
float maxAnimatedScale( const AnimParams& params );

}//namespace kontur
