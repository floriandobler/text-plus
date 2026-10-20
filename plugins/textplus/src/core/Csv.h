#pragma once
// CSV / TSV for slides: every row is a slide, its cells become the slide's lines.
// Typical use: lower thirds from a spreadsheet ("Name;Funktion"), one person per row.

#include <string>
#include <vector>

namespace textplus
{

// Bytes in an unknown encoding -> UTF-8. Valid UTF-8 (with or without BOM) is
// kept; anything else is read as Windows-1252, which is what Excel writes for
// "CSV" on most Western systems.
std::string toUtf8( const std::string& bytes );

// Parses CSV text. The delimiter (comma, semicolon or tab) is detected from the
// first line. Quoted cells may contain delimiters, line breaks and "" quotes.
std::vector< std::vector< std::string > > parseCsv( const std::string& text );

// Rows -> slide texts: non-empty cells joined by line breaks, empty rows dropped.
std::vector< std::string > csvSlides( const std::vector< std::vector< std::string > >& rows, bool skipHeader );
// Rows -> slide texts through a template: "{Name} · {Role}" takes the cells of
// the columns whose header (first row) is Name and Role; "{1}", "{2}" take
// columns by number. "|" in the template breaks the line. Unknown names stay.
// With skipHeader off there are no names, only numbers.
std::vector< std::string > csvSlides( const std::vector< std::vector< std::string > >& rows, bool skipHeader, const std::string& rowTemplate );

bool isCsvPath( const std::string& path );

}//namespace textplus
