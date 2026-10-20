#include "Path.h"

#include <algorithm>
#include <cmath>

namespace textplus
{
namespace
{
constexpr float kTwoPi = 6.2831853f;
}

PathPlacement placeOnPath( const PathParams& p, float centreX, float y, float advance, float lineStart, float lineLength, float firstBaseline )
{
	PathPlacement out;
	float px = centreX, py = y, angle = 0.0f;
	switch( p.mode )
	{
	case PathMode::Off:
		break;
	case PathMode::Arc:
	case PathMode::Circle: {
		float r = p.radius;
		if( std::fabs( r ) < 1.0f )
			r = r < 0.0f ? -1.0f : 1.0f;
		float theta;
		if( p.mode == PathMode::Arc )
			theta = centreX / r;
		else
		{
			// The line's middle at the top; the line spans the whole circle plus
			// one average glyph of gap, so first and last glyph do not touch.
			const float len = std::max( 1.0f, lineLength );
			theta           = ( centreX - ( lineStart + 0.5f * len ) ) / len * kTwoPi * ( r < 0.0f ? -1.0f : 1.0f );
		}
		theta += ( r < 0.0f ? -1.0f : 1.0f ) * kTwoPi * p.offset;
		if( p.mode == PathMode::Arc )
		{
			// Centre of the circle at (0, r); the glyph rides on radius r - y
			const float rr = r - y;
			px             = rr * std::sin( theta );
			py             = r - rr * std::cos( theta );
		}
		else
		{
			// A ring around the anchor; lines below the first ride further in
			const float rr = r - ( y - firstBaseline );
			px             = rr * std::sin( theta );
			py             = -rr * std::cos( theta );
		}
		angle = theta;
		break;
	}
	case PathMode::Wave: {
		const float len   = std::max( 1.0f, std::fabs( p.waveLength ) );
		const float phase = kTwoPi * ( centreX / len + p.offset );
		py                = y + p.waveHeight * std::sin( phase );
		angle             = std::atan( p.waveHeight * kTwoPi / len * std::cos( phase ) );
		break;
	}
	}
	// From the centre back to the origin, along the tangent
	out.x     = px - 0.5f * advance * std::cos( angle );
	out.y     = py - 0.5f * advance * std::sin( angle );
	out.angle = angle;
	return out;
}

}//namespace textplus
