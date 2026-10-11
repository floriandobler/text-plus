#pragma once
// FontFace: one loaded face (file + face index + named instance).
// Holds the file bytes (shared by FreeType and HarfBuzz), the HarfBuzz font
// used for shaping and the vertical metrics used for layout.
//
// Immutable after creation, so it is safe to share between plugin instances
// and threads. FreeType faces are NOT stored here: FT_Face is not thread-safe,
// so every thread that needs outlines creates its own (see GlyphCache).

#include "FontDatabase.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct hb_face_t;
struct hb_font_t;
struct hb_blob_t;

namespace textplus
{

struct FontMetrics
{
	// All values in em (1.0 = font size), y up.
	float ascender  = 0.8f;
	float descender = -0.2f;// negative
	float lineGap   = 0.0f;
	float capHeight = 0.7f;
	float xHeight   = 0.5f;
};

class FontFace
{
public:
	// Loads (or returns the already loaded) face for an entry. nullptr on failure.
	static std::shared_ptr< FontFace > get( const FontEntry& entry );

	~FontFace();

	const FontEntry& entry() const { return fontEntry; }
	uint32_t id() const { return uniqueId; }// stable within a process, used as cache key
	hb_font_t* hbFont() const { return font; }
	int unitsPerEm() const { return upem; }
	const FontMetrics& metrics() const { return fontMetrics; }
	const std::vector< uint8_t >& data() const { return bytes; }
	// FreeType face index including the named-instance bits.
	long ftFaceIndex() const { return ( (long)fontEntry.instance << 16 ) | fontEntry.faceIndex; }

private:
	FontFace() = default;
	bool load( const FontEntry& e );

	FontEntry fontEntry;
	uint32_t uniqueId = 0;
	std::vector< uint8_t > bytes;
	hb_blob_t* blob = nullptr;
	hb_face_t* face = nullptr;
	hb_font_t* font = nullptr;
	int upem        = 1000;
	FontMetrics fontMetrics;
};

}//namespace textplus
