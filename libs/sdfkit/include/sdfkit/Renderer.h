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
//
// 3D (View3D). The 2D picture is composited into block-space textures instead
// of the screen: the text (front), everything behind it (plates, shadow,
// glow) and the silhouette's distance field. A view pass then looks at them
// through a perspective camera: "Tilt" shows them as a plane, "Extrude"
// raymarches the silhouette as a solid of the given depth with a rounded
// bevel, lit sides and the 2D picture on its front face. Plates, shadow and
// glow lie on the plane behind the letters (z = 0).
//
// Video (VideoInput, effect plugins). The composite can fill the text with an
// input texture, draw the text over it, or knock the text out of it.

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
	float skew = 0;    // horizontal shear (tan of the lean angle, positive leans right)
	float weightPx = 0;// grows (> 0) or thins (< 0) the shape, px
	float blurPx = 0;  // softens the shape's edge, px (0..64)
	float extra = 0;

	bool clipped() const { return clipX0 > -1e8f || clipY0 > -1e8f || clipX1 < 1e8f || clipY1 < 1e8f; }
};

enum class OutlinePosition
{
	Outside,
	Center,
	Inside
};

// Fill of the text. Patterns live in block space (they move and rotate with
// the text) and mix fill and fill2; patternOffset 0..1 is one seamless loop.
enum class FillMode
{
	Solid,
	GradientBox,   // linear gradient across the text block, rotates with the text
	GradientChars, // colour 1 -> colour 2 from the first to the last character (done by the caller as tint)
	Stripes,
	Halftone,      // dots of fill2 on fill, growing along the pattern angle across the block
	Checker,
	Noise,
	Led            // the fill becomes a grid of round dots, lit where the cell centre is inside the text
};

struct View3D
{
	enum Mode
	{
		Off,
		Tilt,   // the flat 2D picture, rotated in perspective
		Extrude // a solid with depth, bevel and light
	};
	Mode mode   = Off;
	float tiltX = 0, tiltY = 0;// radians: positive tilts the top / the right edge away
	float focal = 0;           // px, 0 = orthographic
	float depth = 0, bevel = 0;// px
	Color side{ 0.3f, 0.3f, 0.3f, 1.0f };
	float lightX = -0.5f, lightY = -0.5f, lightZ = 0.7f;// direction to the light (screen frame, y down, z towards the viewer)
	float shading = 0.7f;      // 0 = flat colours, 1 = full diffuse light
	float gloss   = 0.3f;      // specular highlight
	bool on() const { return mode != Off; }
};

struct VideoInput
{
	enum Mode
	{
		None,
		Over,    // the text drawn over the video
		Fill,    // the text filled with the video, nothing outside the text
		Knockout // the video with the text cut out
	};
	Mode mode       = None;
	GLuint texture  = 0;
	float maxU = 1, maxV = 1;// used part of the texture
	bool on() const { return mode != None && texture != 0; }
};

struct Style
{
	// Text group
	Color fill;
	FillMode fillMode = FillMode::Solid;
	Color fill2;
	float gradientAngle = 0;// radians, counter-clockwise, 0 = left -> right
	float patternScale  = 24;// px: stripe period, dot or cell size
	float patternAngle  = 0; // radians
	float patternOffset = 0; // 0..1, one loop
	float outline1Width = 0;
	OutlinePosition outline1Position = OutlinePosition::Outside;
	bool sharpCorners = false;// outline/glow/shadow corners: mitred instead of round
	Color outline1;
	float outline2Width = 0;
	Color outline2;
	Color glow;
	float glowRadius = 0, glowStrength = 0;
	Color innerGlow;
	float innerGlowRadius = 0;
	Color innerShadow;
	float innerShadowDx = 0, innerShadowDy = 0;// px, y down
	float innerShadowSoftness = 0;
	// Plate group
	Color plate;
	float plateBorderWidth = 0;// px, inside the plate
	Color plateBorderColor;
	float plateMerge = 0;// px: overlapping plates melt into one shape with fillets of this size
	// Both
	Color shadow;
	float shadowDx = 0, shadowDy = 0;// px, y down
	float shadowSoftness = 0, shadowSpread = 0;
	float edgeSoftness = 1.0f;// px of anti-aliasing
	// Extra reach the shapes need beyond the effects (animated weight and blur,
	// the 3D raymarch), px.
	float extraReach = 0;

	float outerEdge() const;
	// Distance beyond the glyph outline the text effects need, in px.
	float reach() const;
	// Distance beyond a plate's edge its effects need, in px.
	float plateReach() const;
	// Shrinks text effects so they fit into `maxReachPx` (the distance field's limit).
	void clampTo( float maxReachPx );
	bool innerShadowOn() const { return innerShadow.a > 0 && ( innerShadowDx != 0 || innerShadowDy != 0 || innerShadowSoftness > 0 ); }
	bool innerGlowOn() const { return innerGlow.a > 0 && innerGlowRadius > 0; }
	// All sizes multiplied by k (the 3D path renders the block at a scale).
	Style scaled( float k ) const;
};

struct BlockTransform
{
	float originX = 0, originY = 0;// anchor position in px, y down, origin top-left
	float rotation = 0;            // radians, counter-clockwise
	float sizePx   = 100;          // px per em for glyph quads
	// Metric box of the block relative to the anchor (px, y down), for box gradients
	float boxLeft = 0, boxTop = 0, boxRight = 0, boxBottom = 0;
};

// Shapes are visible inside, fading out over `fade` px towards the edges (block space).
struct Window
{
	bool on   = false;
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	float fade = 0;
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
				 const View3D& view = View3D(), const VideoInput& video = VideoInput() );

	struct SharedPrograms;// compiled once per GL context, shared by all instances

private:
	void drawField( GLuint vbo, size_t count, int kind, int layers, bool alphaVaries, float maxReach, const GLuint* fbos, const GLuint* prevA );
	void uploadInstances( GLuint vbo, size_t& capacity, const std::vector< ShapeInstance >& items );
	struct FrameSetup;
	void setCompositeUniforms( const FrameSetup& f );

	SharedPrograms* programs = nullptr;// programs and scratch targets, shared per GL context
	GLuint shapeVao = 0, cornerVbo = 0, glyphVbo = 0, boxVbo = 0;
	GLuint emptyVao = 0;
	size_t glyphCapacity = 0, boxCapacity = 0;
};

}//namespace sdfkit
