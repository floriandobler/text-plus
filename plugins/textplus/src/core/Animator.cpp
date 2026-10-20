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
	case AnimEase::EaseIn:
		return t * t * t;
	case AnimEase::EaseInOut:
		return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow( -2.0f * t + 2.0f, 3.0f ) * 0.5f;
	case AnimEase::Bounce: {
		// "bounce out": lands, bounces three times
		const float n1 = 7.5625f, d1 = 2.75f;
		if( t < 1.0f / d1 )
			return n1 * t * t;
		if( t < 2.0f / d1 )
		{
			t -= 1.5f / d1;
			return n1 * t * t + 0.75f;
		}
		if( t < 2.5f / d1 )
		{
			t -= 2.25f / d1;
			return n1 * t * t + 0.9375f;
		}
		t -= 2.625f / d1;
		return n1 * t * t + 0.984375f;
	}
	case AnimEase::Elastic: {
		if( t <= 0.0f || t >= 1.0f )
			return t;
		const float c4 = 6.2831853f / 3.0f;
		return std::pow( 2.0f, -10.0f * t ) * std::sin( ( t * 10.0f - 0.75f ) * c4 ) + 1.0f;
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

int unitOf( const AnimItem& g, AnimUnit u )
{
	return u == AnimUnit::Character ? g.charIndex : u == AnimUnit::Word ? g.wordIndex : g.lineIndex;
}

const UnitBox& boxOf( const AnimItem& g, AnimUnit u )
{
	return u == AnimUnit::Character ? g.charBox : u == AnimUnit::Word ? g.wordBox : g.lineBox;
}

int unitCount( const UnitCounts& c, AnimUnit u )
{
	return std::max( 1, u == AnimUnit::Character ? c.chars : u == AnimUnit::Word ? c.words : c.lines );
}

// 0 = at rest, 1 = away, for a sequence position `progress` (0..1).
float sequenceAway( const AnimatorParams& p, float progress, float r, float maxRank, bool in )
{
	const float spread = std::max( 1e-4f, p.spread );
	float e;
	if( p.spread < 0.01f )// hard steps (typewriter): exactly floor(progress * units) units have arrived
		e = r < std::floor( progress * ( maxRank + 1.0f ) + 1e-4f ) ? 1.0f : 0.0f;
	else
		e = applyEase( p.ease, ( progress * ( maxRank + 1.0f + spread ) - r ) / spread );
	return in ? 1.0f - e : e;
}

void unionBox( UnitBox& b, const UnitBox& c, bool first )
{
	if( first )
	{
		b = c;
		return;
	}
	b.x0 = std::min( b.x0, c.x0 );
	b.y0 = std::min( b.y0, c.y0 );
	b.x1 = std::max( b.x1, c.x1 );
	b.y1 = std::max( b.y1, c.y1 );
}

// Rank in which a unit starts moving (0 = first). FromCenter/FromEdges give
// symmetric units the same rank, so they move together. `centreX` / `line`
// (per unit) serve the position based orders.
std::vector< float > ranks( int n, AnimOrder order, unsigned seed, const std::vector< float >& centreX, const std::vector< float >& widthX,
							const std::vector< int >& line )
{
	std::vector< float > r( (size_t)n );
	const float c = ( n - 1 ) * 0.5f;
	switch( order )
	{
	case AnimOrder::LeftToRight: {
		// In unit widths from the leftmost unit, so Spread keeps its meaning
		float lo = 1e30f, sumW = 0;
		for( int i = 0; i < n; ++i )
		{
			lo = std::min( lo, centreX[ i ] );
			sumW += widthX[ i ];
		}
		const float unit = std::max( 1e-3f, sumW / std::max( 1, n ) );
		for( int i = 0; i < n; ++i )
			r[ i ] = ( centreX[ i ] - lo ) / unit;
		break;
	}
	case AnimOrder::TopToBottom: {
		int first = line.empty() ? 0 : *std::min_element( line.begin(), line.end() );
		for( int i = 0; i < n; ++i )
			r[ i ] = (float)( line[ i ] - first );
		break;
	}
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

std::vector< AnimItem > glyphItems( const std::vector< LaidGlyph >& glyphs, const std::vector< LineBox >& lines, float margin )
{
	std::vector< AnimItem > items( glyphs.size() );
	for( size_t i = 0; i < glyphs.size(); ++i )
	{
		const LaidGlyph& g = glyphs[ i ];
		AnimItem& it       = items[ i ];
		it.charIndex       = g.charIndex;
		it.wordIndex       = g.wordIndex;
		it.lineIndex       = g.lineIndex;
		float top = g.y, bottom = g.y;
		if( g.lineIndex >= 0 && g.lineIndex < (int)lines.size() )
		{
			// The line box is relative to the line's own baseline: keep that offset
			// for copies of the line placed elsewhere (tickers).
			const LineBox& lb = lines[ g.lineIndex ];
			top               = g.y + ( lb.top - lb.baseline );
			bottom            = g.y + ( lb.bottom - lb.baseline );
		}
		it.charBox = UnitBox{ g.x - margin, top - margin, g.x + std::max( g.advance, 0.0f ) + margin, bottom + margin };
	}
	// Words and lines: union over contiguous runs of the same index
	auto runs = [ & ]( auto indexOf, auto boxRef ) {
		size_t start = 0;
		for( size_t i = 1; i <= items.size(); ++i )
			if( i == items.size() || indexOf( items[ i ] ) != indexOf( items[ start ] ) )
			{
				UnitBox b;
				for( size_t k = start; k < i; ++k )
					unionBox( b, items[ k ].charBox, k == start );
				for( size_t k = start; k < i; ++k )
					boxRef( items[ k ] ) = b;
				start = i;
			}
	};
	if( !items.empty() )
	{
		runs( []( const AnimItem& a ) { return a.wordIndex; }, []( AnimItem& a ) -> UnitBox& { return a.wordBox; } );
		runs( []( const AnimItem& a ) { return a.lineIndex; }, []( AnimItem& a ) -> UnitBox& { return a.lineBox; } );
		// Position of each character relative to the middle of its line (spacing)
		size_t start = 0;
		for( size_t i = 1; i <= items.size(); ++i )
			if( i == items.size() || items[ i ].lineIndex != items[ start ].lineIndex || items[ i ].charBox.x0 < items[ i - 1 ].charBox.x0 - 1e-3f )
			{
				const float mid = 0.5f * (float)( i - start - 1 );
				for( size_t k = start; k < i; ++k )
					items[ k ].lineOffset = (float)( k - start ) - mid;
				start = i;
			}
	}
	return items;
}

UnitCounts unitCounts( const LayoutResult& l )
{
	return UnitCounts{ std::max( 1, l.numChars ), std::max( 1, l.numWords ), std::max( 1, l.numLines ) };
}

const std::vector< uint32_t >& charsetCodepoints( Charset c )
{
	static const std::vector< uint32_t > letters = [] {
		std::vector< uint32_t > v;
		for( uint32_t ch = 'A'; ch <= 'Z'; ++ch )
			v.push_back( ch );
		for( uint32_t ch = 'a'; ch <= 'z'; ++ch )
			v.push_back( ch );
		return v;
	}();
	static const std::vector< uint32_t > digits = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9' };
	static const std::vector< uint32_t > symbols = { '!', '#', '$', '%', '&', '*', '+', '-', '=', '?', '@', '^', '~', '<', '>', '/', '\\', '|', '[', ']', '{', '}' };
	static const std::vector< uint32_t > binary  = { '0', '1' };
	static const std::vector< uint32_t > none;
	switch( c )
	{
	case Charset::Letters: return letters;
	case Charset::Digits: return digits;
	case Charset::Symbols: return symbols;
	case Charset::Binary: return binary;
	case Charset::SameAsText: return none;
	}
	return letters;
}

void applyAnimator( const LayoutResult& layout, const AnimatorParams& p, std::vector< GlyphState >& states )
{
	applyAnimator( glyphItems( layout.glyphs, layout.lines, 0.0f ), unitCounts( layout ), p, states );
}

void applyAnimator( const std::vector< AnimItem >& items, const UnitCounts& counts, const AnimatorParams& p, std::vector< GlyphState >& states )
{
	if( p.mode == AnimMode::Off || items.empty() )
		return;
	states.resize( items.size() );

	const int n = unitCount( counts, p.unit );
	// Range: the units the animator works on (reading order), renumbered 0..m-1
	std::vector< int > sel( (size_t)n, -1 );
	int m = 0;
	{
		const float a = std::clamp( std::min( p.rangeStart, p.rangeEnd ), 0.0f, 1.0f ), b = std::clamp( std::max( p.rangeStart, p.rangeEnd ), 0.0f, 1.0f );
		for( int u = 0; u < n; ++u )
		{
			const float c = ( u + 0.5f ) / (float)n;
			if( c >= a && c <= b )
				sel[ u ] = m++;
		}
	}
	if( m == 0 )
		return;
	// Per selected unit: centre, width, line (position based orders)
	std::vector< float > cx( (size_t)m, 0.0f ), wx( (size_t)m, 0.0f );
	std::vector< int > ln( (size_t)m, 0 );
	if( p.order == AnimOrder::LeftToRight || p.order == AnimOrder::TopToBottom )
	{
		std::vector< bool > seen( (size_t)m, false );
		for( const AnimItem& it : items )
		{
			const int u = std::clamp( unitOf( it, p.unit ), 0, n - 1 );
			const int k = sel[ u ];
			if( k < 0 || seen[ k ] )
				continue;
			seen[ k ]      = true;
			const UnitBox& b = boxOf( it, p.unit );
			cx[ k ]        = 0.5f * ( b.x0 + b.x1 );
			wx[ k ]        = b.x1 - b.x0;
			ln[ k ]        = it.lineIndex;
		}
	}
	const std::vector< float > rk = ranks( m, p.order, p.seed, cx, wx, ln );
	const float maxRank           = *std::max_element( rk.begin(), rk.end() );
	const float progress          = std::clamp( p.progress, 0.0f, 1.0f );
	const float rotRad            = p.rotationDeg * 3.14159265f / 180.0f;
	// Scramble: the random characters change with Progress and on every 16th note
	const long scrambleStep = (long)std::floor( progress * 50.0f ) + (long)std::floor( p.beats * 4.0 );

	for( size_t i = 0; i < items.size(); ++i )
	{
		const int uAll = std::clamp( unitOf( items[ i ], p.unit ), 0, n - 1 );
		const int u    = sel[ uAll ];
		if( u < 0 )
			continue;// outside the range
		const float r  = rk[ u ];
		float signedA  = 0;// drives offsets and rotation
		float unsignedA = 0;// drives scale, opacity, colour, wipe, scramble

		switch( p.mode )
		{
		case AnimMode::Off:
			break;
		case AnimMode::SequenceIn:
		case AnimMode::SequenceOut:
			signedA = unsignedA = sequenceAway( p, progress, r, maxRank, p.mode == AnimMode::SequenceIn );
			break;
		case AnimMode::SequenceInOut: {
			// In over [0, a], at rest over [a, 1 - a], out over [1 - a, 1]
			const float hold = std::clamp( p.hold, 0.0f, 1.0f );
			const float a    = std::max( 1e-4f, ( 1.0f - hold ) * 0.5f );
			if( progress <= 0.5f )
				signedA = unsignedA = sequenceAway( p, std::min( 1.0f, progress / a ), r, maxRank, true );
			else
				signedA = unsignedA = sequenceAway( p, std::clamp( ( progress - ( 1.0f - a ) ) / a, 0.0f, 1.0f ), r, maxRank, false );
			break;
		}
		case AnimMode::Wave: {
			// Progress is the phase: 0..1 is one full cycle, so a looping ramp is seamless.
			const double phase = (double)p.progress - r / std::max( 0.5f, p.spread );
			const float s      = (float)std::sin( kTwoPi * phase );
			signedA            = s;
			unsignedA          = 0.5f + 0.5f * s;
			break;
		}
		case AnimMode::Random: {
			// Progress 0..1 runs through a loop of `spread` random states, smoothly
			// interpolated; state 0 follows the last one, so a looping ramp is seamless.
			const int nStates = std::max( 2, (int)std::lround( p.spread ) );
			const float pos   = ( p.progress - std::floor( p.progress ) ) * (float)nStates;
			const int step    = std::min( nStates - 1, (int)std::floor( pos ) );
			float f           = pos - (float)step;
			f                 = f * f * ( 3.0f - 2.0f * f );
			const float v0    = hashSigned( (uint32_t)u, (uint32_t)step, p.seed );
			const float v1    = hashSigned( (uint32_t)u, (uint32_t)( ( step + 1 ) % nStates ), p.seed );
			const float v     = v0 + ( v1 - v0 ) * f;
			signedA           = v;
			unsignedA         = 0.5f + 0.5f * v;
			break;
		}
		case AnimMode::Constant:
			signedA = unsignedA = progress;
			break;
		}

		GlyphState& s = states[ i ];
		s.dx += p.offsetX * signedA + p.spacing * items[ i ].lineOffset * unsignedA;
		s.skew += p.skew * signedA;
		s.weight += p.thickness * unsignedA;
		s.blur += p.blur * unsignedA;
		s.dy += p.offsetY * signedA;
		s.rotation += rotRad * signedA;
		s.scale *= std::max( 0.0f, 1.0f + ( p.scale - 1.0f ) * unsignedA );
		s.alpha *= std::clamp( 1.0f + ( p.opacity - 1.0f ) * unsignedA, 0.0f, 1.0f );
		if( p.colorAmount > 0.0f )
			layerTint( s, p.color, p.colorAmount * unsignedA );

		const float hidden = std::clamp( p.wipe * unsignedA, 0.0f, 1.0f );
		if( hidden > 0.0f )
		{
			const UnitBox& b = boxOf( items[ i ], p.unit );
			const float keep = 1.0f - hidden;
			float x0 = -1e9f, y0 = -1e9f, x1 = 1e9f, y1 = 1e9f;
			switch( p.wipeFrom )
			{
			case WipeFrom::Left:// revealed from the left edge to the right
				x1 = b.x0 + ( b.x1 - b.x0 ) * keep;
				break;
			case WipeFrom::Right:
				x0 = b.x1 - ( b.x1 - b.x0 ) * keep;
				break;
			case WipeFrom::Center: {
				const float c = 0.5f * ( b.x0 + b.x1 ), h = 0.5f * ( b.x1 - b.x0 ) * keep;
				x0            = c - h;
				x1            = c + h;
				break;
			}
			case WipeFrom::Top:
				y1 = b.y0 + ( b.y1 - b.y0 ) * keep;
				break;
			case WipeFrom::Bottom:
				y0 = b.y1 - ( b.y1 - b.y0 ) * keep;
				break;
			}
			s.clipX0 = std::max( s.clipX0, x0 );
			s.clipY0 = std::max( s.clipY0, y0 );
			s.clipX1 = std::min( s.clipX1, x1 );
			s.clipY1 = std::min( s.clipY1, y1 );
		}

		const float scrambleChance = std::clamp( p.scramble * unsignedA, 0.0f, 1.0f );
		if( scrambleChance > 0.0f )
		{
			// Per character, whatever the unit: decode effects flip letter by letter.
			const uint32_t c = (uint32_t)items[ i ].charIndex;
			const float roll = 0.5f + 0.5f * hashSigned( c, (uint32_t)scrambleStep, p.seed ^ 0xA5A5u );
			if( roll < scrambleChance || scrambleChance >= 1.0f )
			{
				s.scramble = 1u + ( hash32( c * 7919u + (uint32_t)scrambleStep * 104729u + p.seed ) & 0x7FFFFFFFu );
				s.charset  = p.charset;
			}
			else
				s.scramble = 0;
		}
	}
}

float maxAnimatedScale( const AnimatorParams& p )
{
	if( p.mode == AnimMode::Off )
		return 1.0f;
	float s = std::max( 1.0f, p.scale );
	const bool seq = p.mode == AnimMode::SequenceIn || p.mode == AnimMode::SequenceOut || p.mode == AnimMode::SequenceInOut;
	if( p.ease == AnimEase::Overshoot && seq )
		s = std::max( s, 1.0f + std::fabs( p.scale - 1.0f ) * 0.1f );// back-out passes the target by ~10 %
	if( p.ease == AnimEase::Elastic && seq )
		s = std::max( s, 1.0f + std::fabs( p.scale - 1.0f ) * 0.4f );// elastic swings past the target
	return s;
}

}//namespace textplus
