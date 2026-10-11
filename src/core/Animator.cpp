#include "Animator.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace textplus
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;

float applyEase( AnimEase e, float t )
{
	t = std::clamp( t, 0.0f, 1.0f );
	switch( e )
	{
	case AnimEase::Linear:
		return t;
	case AnimEase::Smooth:
		return t * t * ( 3.0f - 2.0f * t );
	case AnimEase::EaseOut: {
		float u = 1.0f - t;
		return 1.0f - u * u * u;
	}
	case AnimEase::Overshoot: {
		const float c1 = 1.70158f, c3 = c1 + 1.0f;
		float u        = t - 1.0f;
		return 1.0f + c3 * u * u * u + c1 * u * u;// "back out": passes 1, then settles
	}
	}
	return t;
}

uint32_t hash32( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

// Deterministic value in [-1, 1]
float hashSigned( uint32_t a, uint32_t b, uint32_t c )
{
	uint32_t h = hash32( a * 0x9E3779B9u ^ hash32( b + 0x632BE5ABu ) ^ hash32( c * 0x85EBCA77u + 0x165667B1u ) );
	return ( h & 0xFFFFFF ) / float( 0xFFFFFF ) * 2.0f - 1.0f;
}

int unitOf( const LaidGlyph& g, AnimUnit u )
{
	return u == AnimUnit::Character ? g.charIndex : u == AnimUnit::Word ? g.wordIndex : g.lineIndex;
}

int unitCount( const LayoutResult& l, AnimUnit u )
{
	return std::max( 1, u == AnimUnit::Character ? l.numChars : u == AnimUnit::Word ? l.numWords : l.numLines );
}

// Rank in which a unit starts moving (0 = first). FromCenter/FromEdges give
// symmetric units the same rank, so they move together.
std::vector< float > ranks( int n, AnimOrder order, unsigned seed )
{
	std::vector< float > r( (size_t)n );
	const float c = ( n - 1 ) * 0.5f;
	switch( order )
	{
	case AnimOrder::Forward:
		for( int i = 0; i < n; ++i )
			r[ i ] = (float)i;
		break;
	case AnimOrder::Reverse:
		for( int i = 0; i < n; ++i )
			r[ i ] = (float)( n - 1 - i );
		break;
	case AnimOrder::FromCenter:
		for( int i = 0; i < n; ++i )
			r[ i ] = std::fabs( i - c );
		break;
	case AnimOrder::FromEdges:
		for( int i = 0; i < n; ++i )
			r[ i ] = c - std::fabs( i - c );
		break;
	case AnimOrder::Random: {
		std::vector< int > idx( (size_t)n );
		std::iota( idx.begin(), idx.end(), 0 );
		std::sort( idx.begin(), idx.end(), [ & ]( int a, int b ) { return hash32( (uint32_t)a * 2654435761u + seed ) < hash32( (uint32_t)b * 2654435761u + seed ); } );
		for( int k = 0; k < n; ++k )
			r[ idx[ k ] ] = (float)k;
		break;
	}
	}
	return r;
}
}//namespace

void layerTint( GlyphState& s, const Rgb& c, float k )
{
	k = std::clamp( k, 0.0f, 1.0f );
	if( k <= 0.0f )
		return;
	// mix( mix( fill, T, A ), C, k ) == mix( fill, T', A' )
	const float a2 = 1.0f - ( 1.0f - s.tintAmount ) * ( 1.0f - k );
	const float wT = s.tintAmount * ( 1.0f - k ) / a2, wC = k / a2;
	s.tint         = Rgb{ s.tint.r * wT + c.r * wC, s.tint.g * wT + c.g * wC, s.tint.b * wT + c.b * wC };
	s.tintAmount   = a2;
}

void initGlyphStates( const LayoutResult& layout, std::vector< GlyphState >& states )
{
	states.assign( layout.glyphs.size(), GlyphState{} );
	for( size_t i = 0; i < layout.glyphs.size(); ++i )
	{
		int ci = layout.glyphs[ i ].colorIndex;
		if( ci >= 0 && ci < (int)layout.palette.size() )
			layerTint( states[ i ], layout.palette[ ci ], 1.0f );
	}
}

void applyAnimator( const LayoutResult& layout, const AnimatorParams& p, double beats, std::vector< GlyphState >& states )
{
	if( p.mode == AnimMode::Off || layout.glyphs.empty() )
		return;
	states.resize( layout.glyphs.size() );

	const int n                   = unitCount( layout, p.unit );
	const std::vector< float > rk = ranks( n, p.order, p.seed );
	const float maxRank           = *std::max_element( rk.begin(), rk.end() );
	const float progress          = std::clamp( p.progress, 0.0f, 1.0f );
	const float spread            = std::max( 1e-4f, p.spread );
	const float rotRad            = p.rotationDeg * 3.14159265f / 180.0f;

	for( size_t i = 0; i < layout.glyphs.size(); ++i )
	{
		const int u    = std::clamp( unitOf( layout.glyphs[ i ], p.unit ), 0, n - 1 );
		const float r  = rk[ u ];
		float signedA  = 0;// drives offsets and rotation
		float unsignedA = 0;// drives scale, opacity, colour

		switch( p.mode )
		{
		case AnimMode::Off:
			break;
		case AnimMode::SequenceIn:
		case AnimMode::SequenceOut: {
			// progress 0 -> 1 sweeps through ranks 0..maxRank; `spread` ranks move at once
			float e;
			if( p.spread < 0.01f )// hard steps (typewriter): exactly floor(progress * units) units have arrived
				e = r < std::floor( progress * ( maxRank + 1.0f ) + 1e-4f ) ? 1.0f : 0.0f;
			else
				e = applyEase( p.ease, ( progress * ( maxRank + 1.0f + spread ) - r ) / spread );
			signedA = unsignedA = p.mode == AnimMode::SequenceIn ? 1.0f - e : e;
			break;
		}
		case AnimMode::Wave: {
			const double phase = beats * p.speed - r / std::max( 0.5f, p.spread );
			signedA            = (float)std::sin( kTwoPi * phase ) * progress;
			unsignedA          = ( 0.5f + 0.5f * (float)std::sin( kTwoPi * phase ) ) * progress;
			break;
		}
		case AnimMode::Random: {
			// Smoothly interpolated random values, re-rolled `speed` times per beat
			const double tpos = p.speed != 0.0f ? beats * std::fabs( p.speed ) : 0.0;
			const double step = std::floor( tpos );
			float f           = (float)( tpos - step );
			f                 = f * f * ( 3.0f - 2.0f * f );
			const float v0 = hashSigned( (uint32_t)u, (uint32_t)(int64_t)step, p.seed );
			const float v1 = hashSigned( (uint32_t)u, (uint32_t)(int64_t)step + 1u, p.seed );
			const float v  = v0 + ( v1 - v0 ) * f;
			signedA        = v * progress;
			unsignedA      = ( 0.5f + 0.5f * v ) * progress;
			break;
		}
		case AnimMode::Constant:
			signedA = unsignedA = progress;
			break;
		}

		GlyphState& s = states[ i ];
		s.dx += p.offsetX * signedA;
		s.dy += p.offsetY * signedA;
		s.rotation += rotRad * signedA;
		s.scale *= std::max( 0.0f, 1.0f + ( p.scale - 1.0f ) * unsignedA );
		s.alpha *= std::clamp( 1.0f + ( p.opacity - 1.0f ) * unsignedA, 0.0f, 1.0f );
		if( p.colorAmount > 0.0f )
			layerTint( s, p.color, p.colorAmount * unsignedA );
	}
}

float maxAnimatedScale( const AnimatorParams& p )
{
	if( p.mode == AnimMode::Off )
		return 1.0f;
	float s = std::max( 1.0f, p.scale );
	if( p.ease == AnimEase::Overshoot && ( p.mode == AnimMode::SequenceIn || p.mode == AnimMode::SequenceOut ) )
		s = std::max( s, 1.0f + std::fabs( p.scale - 1.0f ) * 0.1f );// back-out passes the target by ~10 %
	return s;
}

}//namespace textplus
