#pragma once
// TextRenderer: the GPU passes.
//
//  Field passes: every glyph is an instanced quad. The fragment shader reads
//  the glyph's MTSDF and writes the signed distance to the outline in output
//  pixels (outside positive) plus the glyph's opacity and tint. The depth test
//  keeps the smallest distance per pixel: the distance field of the UNION of
//  all glyphs. When glyphs differ in opacity or colour, further passes peel
//  the 2nd..4th nearest glyph per pixel (depth peeling by glyph id).
//
//  Composite: one full-screen triangle (scissored to the text's bounding box)
//  turns the layers into fill, two outlines, shadow and glow. Opacity is
//  integrated as a threshold (see kCompositeFS), so a fading glyph leaves
//  merged outlines smoothly instead of punching a hole into them.
//
// Everything outside the text's bounding box is cleared and never shaded.

#include "GL.h"

#include <vector>

namespace textplus
{

struct GlyphInstance
{
	float planeL, planeB, planeR, planeT;// em, y up, relative to glyph origin
	float u0, v0, u1, v1;                // atlas texels
	float x, y, scale, rotation;         // px (y down, relative to anchor), scale, radians
	float alpha, index, pxPerAtlasPx, spreadAtlasPx;
	float tintR, tintG, tintB, tintAmount;// fill tint (inline colour, animator colour, per-character gradient)
};

struct Color
{
	float r = 1, g = 1, b = 1, a = 1;// straight (not premultiplied)
};

enum class OutlinePosition
{
	Outside,
	Center,
	Inside
};

enum class FillMode
{
	Solid,
	GradientBox,   // linear gradient across the text block, rotates with the text
	GradientChars  // colour 1 -> colour 2 from the first to the last character
};

struct Style
{
	Color fill;
	FillMode fillMode = FillMode::Solid;
	Color fill2;
	float gradientAngle = 0;// radians, counter-clockwise, 0 = left -> right
	float outline1Width = 0;
	OutlinePosition outline1Position = OutlinePosition::Outside;
	bool sharpCorners = false;// outline/glow/shadow corners: mitred instead of round
	Color outline1;
	float outline2Width = 0;
	Color outline2;
	Color shadow;
	float shadowDx = 0, shadowDy = 0;// px, y down
	float shadowSoftness = 0, shadowSpread = 0;
	Color glow;
	float glowRadius = 0, glowStrength = 0;
	float edgeSoftness = 1.0f;// px of anti-aliasing

	float outerEdge() const;
	// Distance beyond the glyph outline the effects need, in px.
	float reach() const;
	// Shrinks effects so they fit into `maxReachPx` (the distance field's limit).
	void clampTo( float maxReachPx );
};

struct BlockTransform
{
	float originX = 0, originY = 0;// anchor position in px, y down, origin top-left
	float rotation = 0;            // radians, counter-clockwise
	float sizePx   = 100;
	// Metric box of the block relative to the anchor (px, y down), for box gradients
	float boxLeft = 0, boxTop = 0, boxRight = 0, boxBottom = 0;
};

class TextRenderer
{
public:
	static constexpr int kLayers = 4;// nearest glyphs per pixel when opacities differ

	bool init();
	void release();
	bool isReady() const { return programs != nullptr; }

	// Renders into the host framebuffer `hostFbo` with viewport vp.
	void render( const FFGLViewportStruct& vp, GLuint hostFbo, const std::vector< GlyphInstance >& glyphs, const BlockTransform& transform,
				 const Style& style, GLuint atlasTexture, int atlasSize );

	struct SharedPrograms;// compiled once per GL context, shared by all instances

private:
	SharedPrograms* programs = nullptr;// programs and scratch targets, shared per GL context
	GLuint glyphVao = 0, cornerVbo = 0, instanceVbo = 0;
	GLuint emptyVao = 0;
	size_t instanceCapacity = 0;
};

}//namespace textplus
