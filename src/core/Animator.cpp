#include "Animator.h"

#include <algorithm>
#include <cmath>

namespace kontur
{
namespace
{
float smooth( float t )
{
	t = std::clamp( t, 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );
}
}//namespace

void animateGlyphs( const LayoutResult& layout, const AnimParams& p, std::vector< GlyphState >& out )
{
	out.assign( layout.glyphs.size(), GlyphState{} );
	if( p.mode == AnimMode::Off || layout.glyphs.empty() )
		return;

	const int count = p.unit == AnimUnit::Character ? layout.numChars : p.unit == AnimUnit::Word ? layout.numWords : layout.numLines;
	const float n   = (float)std::max( 1, count );
	const float progress = std::clamp( p.progress, 0.0f, 1.0f );
	const float window   = std::max( 0.001f, p.spread );

	for( size_t i = 0; i < layout.glyphs.size(); ++i )
	{
		const LaidGlyph& g = layout.glyphs[ i ];
		GlyphState& s      = out[ i ];
		const float u      = (float)( p.unit == AnimUnit::Character ? g.charIndex : p.unit == AnimUnit::Word ? g.wordIndex : g.lineIndex );

		switch( p.mode )
		{
		case AnimMode::Off:
			break;
		case AnimMode::Typewriter: {
			// progress 0 = nothing, 1 = everything; the step lands exactly on unit boundaries
			const float shown = std::floor( progress * n + 1e-4f );
			s.alpha           = u < shown ? 1.0f : 0.0f;
			break;
		}
		case AnimMode::Fade: {
			s.alpha = smooth( ( progress * ( n + window ) - u ) / window );
			break;
		}
		case AnimMode::Rise: {
			const float t = smooth( ( progress * ( n + window ) - u ) / window );
			s.alpha       = t;
			s.dy          = ( 1.0f - t ) * p.amount;
			break;
		}
		case AnimMode::Wave: {
			const double phase = p.beats * p.speed - (double)u / std::max( 0.5f, p.spread );
			s.dy               = -p.amount * progress * (float)std::sin( 6.283185307179586 * phase );
			break;
		}
		}
	}
}

float maxAnimatedScale( const AnimParams& )
{
	return 1.0f;// no scaling presets yet
}

}//namespace kontur
