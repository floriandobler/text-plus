#pragma once
// TextRenderer: the two GPU passes.
//
//  Pass 1 (field): every glyph is an instanced quad. The fragment shader reads
//  the glyph's MTSDF and writes the signed distance to the outline in output
//  pixels (outside positive). The depth test keeps the smallest distance per
//  pixel, which is the distance field of the UNION of all glyphs. Extra
//  channels carry the glyph's animation alpha and its index.
//
//  Pass 2 (composite): one full-screen triangle turns the field into fill,
//  two outlines, drop shadow and glow, anti-aliased by pixel distance, and
//  writes premultiplied RGBA into the host framebuffer.

#include "GL.h"

#include <vector>

namespace kontur
{

struct GlyphInstance
{
	float planeL, planeB, planeR, planeT;// em, y up, relative to glyph origin
	float u0, v0, u1, v1;                // atlas texels
	float x, y, scale, rotation;         // px (y down, relative to anchor), scale, radians
	float alpha, index, pxPerAtlasPx, spreadAtlasPx;
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

struct Style
{
	Color fill;
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
};

class TextRenderer
{
public:
	bool init();
	void release();
	bool isReady() const { return fieldProgram && compositeProgram; }

	// Renders into the currently bound host framebuffer `hostFbo` with viewport vp.
	void render( const FFGLViewportStruct& vp, GLuint hostFbo, const std::vector< GlyphInstance >& glyphs, const BlockTransform& transform,
				 const Style& style, GLuint atlasTexture, int atlasSize );

private:
	bool ensureFieldTarget( int w, int h );

	GLuint fieldProgram = 0, compositeProgram = 0;
	GLuint glyphVao = 0, cornerVbo = 0, instanceVbo = 0;
	GLuint emptyVao = 0;
	size_t instanceCapacity = 0;
	GLuint fieldFbo = 0, fieldTex = 0, fieldDepth = 0;
	int fieldW = 0, fieldH = 0;
};

}//namespace kontur
