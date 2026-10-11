#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace textplus
{
// Decodes the code point starting at byte offset i. Invalid bytes yield U+FFFD.
uint32_t decodeUtf8At( const std::string& s, size_t i, size_t* length = nullptr );
bool isBreakingSpace( uint32_t cp );

// Turns the raw parameter string into paragraphs:
//  - real newlines (\n, \r\n, \r) and the two-character escape "\n" split lines
//  - "\\" becomes a literal backslash
//  - optionally '|' splits lines too
std::vector< std::string > splitParagraphs( const std::string& raw, bool pipeIsNewline );

struct Rgb
{
	float r = 1, g = 1, b = 1;
};

// A paragraph with an inline colour index per byte (-1 = no inline colour).
struct MarkedParagraph
{
	std::string text;
	std::vector< int16_t > colorOfByte;
};

// splitParagraphs + inline colour markup:
//   {#f80} / {#ff8800}  start a colour      {/}  back to the normal fill
//   {{                  a literal "{"
// Colours carry across line breaks. Unknown tags are kept as text.
std::vector< MarkedParagraph > parseMarkedText( const std::string& raw, bool pipeIsNewline, bool markup, std::vector< Rgb >& palette );
}//namespace textplus
