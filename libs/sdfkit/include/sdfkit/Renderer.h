#pragma once
// sdfkit::Renderer — distance-field shapes with a style stack, for FFGL plugins.
//
// Primitives. Every shape is an instanced quad whose fragment shader writes
// the signed distance to the shape's outline in output pixels (outside
// positive):
//   glyph  MTSDF from an Atlas (text, later logos and other vector shapes)
//   box    analytic rounded, slanted rectangle (plates, later frames and bars)
//
// Groups. Shapes belong to a group; the style stack decides what each group
// gets. Today: Text (glyphs: fill, outlines, inner glow, inner shadow, glow)
// and Plate (boxes behind the text: colour and border). The drop shadow is
// cast by both together.
//
// Field passes. Per group, the depth test keeps the smallest distance per
// pixel: the distance field of the UNION of the group's shapes. When shapes
// differ in opacity or colour, further passes peel more shapes per pixel
// (depth peeling): up to kLayers glyphs and kPlateLayers plates.
//
// Composite. One full-screen triangle (scissored to the shapes' bounding box)
// turns the layers into colour. Opacity is integrated as a threshold (see
// kCompositeFS), so a fading shape leaves merged outlines smoothly instead of
// punching a hole into them.
//
// Clipping. A shape can be revealed by a rectangle in layout space (wipes):
// glyphs are masked by it, boxes are cut by it (the plate "grows" with the
// wipe edge, keeping its border and shadow). A window in block space fades
// both groups at its edges (tickers, rolls).

#include "ffglkit/GL.h"

#include <vector>

namespace sdfkit
{

struct Color
{
	float r = 1, g = 1, b = 1, a = 1;// straight (not premultiplied)
};

// One instanced quad of a field pass.
struct ShapeInstance
{
	float quadL = 0, quadB = 0, quadR = 0, quadT = 0;// quad around the origin, y up; glyphs: em, boxes: px
	float u0 = 0, v0 = 0, u1 = 0, v1 = 0;            // glyphs: atlas texels
	float x = 0, y = 0, scale = 1, rotation = 0;     // px (y down, block space), scale, radians counter-clockwise
	float alpha = 1, index = 0, pxPerAtlasPx = 1, spreadAtlasPx = 1;
	float tintR = 0, tintG = 0, tintB = 0, tintAmount = 0;// colour tint (inline colour, animator colour, gradient)
	float restX = 0, restY = 0, restScale = 1, reserved = 0;// position and scale without animation (layout space)
	float clipX0 = -1e9f, clipY0 = -1e9f, clipX1 = 1e9f, clipY1 = 1e9f;// visible rectangle in layout space (px, y down)
	float boxHalfW = 0, boxHalfH = 0, boxRadius = 0, boxSlant = 0;  // boxes: half size, corner radius (px), slant (tan)
	float rotX = 0, rotY = 0, depth = 0, skew = 0;  // glyphs: 3D turn (radians; top away, right away), px towards the back, slant (tan)
	float blur = 0, weight = 0, spare0 = 0, spare1 = 0;// glyphs: edge softness (px), thicker / thinner (px)

	bool clipped() const { return clipX0 > -1e8f || clipY0 > -1e8f || clipX1 < 1e8f || clipY1 < 1e8f; }
	bool projected() const { return rotX != 0 || rotY != 0 || depth != 0 || skew != 0; }
};

enum class FillMode
{
	Solid,
	GradientBox,   // linear gradient across the text block, rotates with the text
	GradientChars, // colour 1 -> colour 2 from the first to the last character (done by the caller as tint)
	Radial,        // colour 1 in the centre of the block, colour 2 outside
	Stripes,       // bands of colour 1 and 2
	Halftone,      // dots of colour 2 on colour 1, growing across the block
	Input          // the input video (Text+ FX)
};

// Effects of the style stack. Values are stored in compositions: append, never renumber.
enum class EffectType
{
	Off,
	Outline,    // band around the text; several stack outwards
	Glow,       // outer glow
	Shadow,     // drop shadow (text and plates)
	InnerGlow,
	InnerShadow,
	Bevel,      // lit relief on the fill
	Extrude,    // 2.5D depth along a direction
	Contours,   // rings at equal distances around (or inside) the text
	Roughen     // noisy edge for everything
};

// One entry of the effect stack. What the generic values mean depends on the type:
//   Outline      size = width, mode = position of the FIRST outline (0 outside, 1 centre, 2 inside)
//   Glow         size = radius, amount = strength
//   Shadow       size = spread, softness, distance + angle = offset
//   InnerGlow    size = radius, amount = strength
//   InnerShadow  softness, distance + angle = offset
//   Bevel        size, softness, angle = light direction, amount = depth, mode 0 smooth / 1 chisel / 2 chrome,
//                color = highlight, color2 = shadow
//   Extrude      size = depth, angle = direction, amount = shading, color = near, color2 = far end
//   Contours     size = spacing, distance = reach, amount = line width (share of the spacing), mode 0 outside / 1 inside / 2 both
//   Roughen      size = scale, distance = strength, angle = evolution (a full turn loops)
// Angles in radians, counter-clockwise, 0 = to the right. Sizes in output px.
struct Effect
{
	EffectType type = EffectType::Off;
	Color color, color2;
	float size = 0, softness = 0, distance = 0, angle = 0, amount = 1;
	int mode = 0;
};

constexpr int kMaxEffects = 6;

struct Style
{
	// Text group
	Color fill;
	FillMode fillMode = FillMode::Solid;
	Color fill2;
	float gradientAngle = 0;// radians, counter-clockwise, 0 = left -> right
	float fillScale     = 1;// stripes / halftone period and radial size, share of the text size
	bool sharpCorners   = false;// outline/glow/shadow corners: mitred instead of round
	float weight        = 0;// px: thicker (> 0) or thinner (< 0) text
	Effect effects[ kMaxEffects ];
	// Plate group
	Color plate;
	float plateBorderWidth = 0;// px, inside the plate
	Color plateBorderColor;
	float plateMerge = 0;// px: overlapping plates melt into one shape with fillets of this size
	float edgeSoftness = 1.0f;// px of anti-aliasing
	// Largest per-glyph blur and weight (animators), for the reach
	float glyphBlurMax = 0, glyphWeightMax = 0;

	// Edge of the fill (<= 0: the first outline lies inside) and outer edge of all outlines, px
	float fillEdge() const;
	float outerEdge() const;
	// Distance beyond the glyph outline the text effects need, in px.
	float reach() const;
	// Distance beyond a plate's edge its effects need, in px.
	float plateReach() const;
	// Shrinks text effects so they fit into `maxReachPx` (the distance field's limit).
	void clampTo( float maxReachPx );
	// First active effect of a type, or nullptr
	const Effect* first( EffectType t ) const;
	bool has( EffectType t ) const { return first( t ) != nullptr; }
	float roughStrength() const;
};

struct BlockTransform
{
	float originX = 0, originY = 0;// anchor position in px, y down, origin top-left
	float rotation = 0;            // radians, counter-clockwise
	float sizePx   = 100;          // px per em for glyph quads
	// Metric box of the block relative to the anchor (px, y down), for box gradients
	float boxLeft = 0, boxTop = 0, boxRight = 0, boxBottom = 0;
	// 3D: distance of the eye from the screen in px (0 = parallel projection);
	// glyphs with depth or a 3D turn are projected towards the viewport centre.
	float perspective = 0;
};

// Shapes are visible inside, fading out over `fade` px towards the edges (block space).
struct Window
{
	bool on   = false;
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	float fade = 0;
};

// Text+ FX: the input video and how the text uses it.
enum class InputMode
{
	None,
	Over,  // text over the video
	Fill,  // the video inside the text (Fill mode Input), nothing outside
	CutOut // the video with the text cut out
};
struct Input
{
	GLuint texture = 0;
	float maxU = 1, maxV = 1;// texture coordinates of the image's top-right corner
	InputMode mode = InputMode::None;
};

class Renderer
{
public:
	static constexpr int kLayers      = 4;// glyphs per pixel when opacities differ
	static constexpr int kPlateLayers = 2;// plates per pixel when opacities differ

	bool init();
	void release();
	bool isReady() const { return programs != nullptr; }

	// Renders into the host framebuffer `hostFbo` with viewport vp.
	void render( const FFGLViewportStruct& vp, GLuint hostFbo, const std::vector< ShapeInstance >& glyphs, const std::vector< ShapeInstance >& boxes,
				 const BlockTransform& transform, const Style& style, const Window& window, GLuint atlasTexture, int atlasSize,
				 const Input& input = Input() );

	struct SharedPrograms;// compiled once per GL context, shared by all instances

private:
	void drawField( GLuint vbo, size_t count, int kind, int layers, bool alphaVaries, float maxReach, const GLuint* fbos, const GLuint* prevA );
	void copyInput( const FFGLViewportStruct& vp, GLuint hostFbo, const Input& input );
	void uploadInstances( GLuint vbo, size_t& capacity, const std::vector< ShapeInstance >& items );

	SharedPrograms* programs = nullptr;// programs and scratch targets, shared per GL context
	GLuint shapeVao = 0, cornerVbo = 0, glyphVbo = 0, boxVbo = 0;
	GLuint emptyVao = 0;
	size_t glyphCapacity = 0, boxCapacity = 0;
};

}//namespace sdfkit
