#pragma once
// Font fallback: finds a face for characters the chosen font does not have.
//
//  1. Emoji go to the monochrome Noto Emoji compiled into the plugin (colour
//     emoji fonts are bitmaps or colour layers and cannot be drawn as distance
//     fields with outlines and glow).
//  2. Everything else asks the operating system, like browsers do: CoreText
//     on macOS (CTFontCreateForString), DirectWrite on Windows
//     (IDWriteFontFallback::MapCharacters). The OS picks per script (PingFang
//     for Chinese, Hiragino for Japanese, Apple Symbols ...).
//  3. Last resort: broad fonts that are usually installed (Arial Unicode MS,
//     Segoe UI Symbol, Noto Sans, DejaVu Sans ...), checked for the character.
//
// Results are cached per character for the whole process.

#include "FontFace.h"

#include <cstdint>
#include <memory>
#include <string>

namespace textplus
{

// Default emoji presentation (pictographs, regional indicators ...).
bool isEmojiPresentation( uint32_t cp );
// Characters that belong to the previous character's font: joiners, variation
// selectors, skin tones, tags, combining marks, keycaps.
bool continuesCluster( uint32_t cp );

// The embedded Noto Emoji face.
std::shared_ptr< FontFace > emojiFace();

// A face that has a glyph for `cp`, or nullptr. `base` is the font the text is set in.
std::shared_ptr< FontFace > fallbackFaceFor( uint32_t cp, const FontFace& base );

// Platform hook (FontPaths_<platform>): the font the OS would use for `utf8`
// when `base` lacks it. false if the platform has no answer.
bool platformFallbackFont( const FontEntry& base, const std::string& utf8, FontEntry& out );

}//namespace textplus
