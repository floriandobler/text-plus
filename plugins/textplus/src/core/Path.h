#pragma once
// Path: bends laid-out text along an arc, around a circle or along a wave.
//
// The path runs through the anchor (layout y = 0). A glyph keeps its distance
// from that line: lines above it ride on a larger radius (or higher on the
// wave), so multi-line text stays concentric. Every glyph is placed by its
// centre and turned along the tangent; its own rotation, offsets and skew from
// the animators then work in that turned frame.
//
//  Arc     the line bent into a circle of radius R (R > 0: the ends go down, a
//          rainbow; R < 0: the ends go up, a smile). Spacing stays natural.
//  Circle  a ring around the anchor: the first line's baseline on the circle of
//          radius R, every line spread over the full circle and centred at the
//          top; further lines form inner rings. The spacing follows the
//          circumference.
//  Wave    a sine wave of the given height and length.
// Offset 0..1 moves the text once around the circle (Arc, Circle) or by one
// wavelength (Wave), so a looping ramp is seamless.

#include <vector>

namespace textplus
{

enum class PathMode
{
	Off,
	Arc,
	Circle,
	Wave
};

struct PathParams
{
	PathMode mode    = PathMode::Off;
	float radius     = 400.0f;// px
	float offset     = 0.0f;  // 0..1
	float waveHeight = 40.0f; // px
	float waveLength = 600.0f;// px
	bool on() const { return mode != PathMode::Off; }
};

// Where a glyph goes: its origin (px, y down) and the turn (radians, the
// renderer's convention: positive turns the glyph's x axis towards +y).
struct PathPlacement
{
	float x = 0, y = 0, angle = 0;
};

// `centreX`: the glyph's centre along the line (px); `y`: its baseline (px, y
// down, 0 = the path); `advance`: its width; `lineStart` / `lineLength`: the
// line's extent and `firstBaseline` the first line's baseline (Circle).
PathPlacement placeOnPath( const PathParams& p, float centreX, float y, float advance, float lineStart, float lineLength, float firstBaseline = 0.0f );

}//namespace textplus
