#pragma once
// File helpers that take UTF-8 paths on every platform (Windows needs wide
// paths for anything outside the ANSI code page, e.g. "C:\Users\Jürgen").

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace textplus
{

std::filesystem::path pathFromUtf8( const std::string& utf8 );
std::string utf8FromPath( const std::filesystem::path& p );

// Modification time (s) and size; false if the file does not exist.
bool fileInfo( const std::string& utf8Path, int64_t& mtime, int64_t& size );
// Changes whenever the file is saved (time in ms when available, plus size). -1 if missing.
int64_t fileStamp( const std::string& utf8Path );
bool readFileBytes( const std::string& utf8Path, std::vector< uint8_t >& out, size_t maxBytes = size_t( 512 ) << 20 );
// Whole file as text, UTF-8 BOM and trailing line breaks removed.
std::string readTextFile( const std::string& utf8Path );

// Paths of fonts compiled into the plugin start with this prefix.
constexpr const char* kEmbeddedPrefix = "embedded:";
bool isEmbeddedPath( const std::string& path );
// Bytes of an embedded font ("embedded:NotoEmoji"), nullptr if unknown.
const uint8_t* embeddedFont( const std::string& path, size_t& size );

}//namespace textplus
