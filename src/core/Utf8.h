#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kontur
{
// Decodes the code point starting at byte offset i. Invalid bytes yield U+FFFD.
uint32_t decodeUtf8At( const std::string& s, size_t i, size_t* length = nullptr );
bool isBreakingSpace( uint32_t cp );

// Turns the raw parameter string into paragraphs:
//  - real newlines (\n, \r\n, \r) and the two-character escape "\n" split lines
//  - "\\" becomes a literal backslash
//  - optionally '|' splits lines too
std::vector< std::string > splitParagraphs( const std::string& raw, bool pipeIsNewline );
}//namespace kontur
