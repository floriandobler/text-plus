#include "Csv.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace textplus
{
namespace
{
bool isValidUtf8( const std::string& s )
{
	size_t i = 0;
	while( i < s.size() )
	{
		const unsigned char c = (unsigned char)s[ i ];
		int n                 = c < 0x80 ? 0 : ( c >> 5 ) == 0x6 ? 1 : ( c >> 4 ) == 0xE ? 2 : ( c >> 3 ) == 0x1E ? 3 : -1;
		if( n < 0 || ( n == 1 && c < 0xC2 ) )
			return false;
		for( int k = 1; k <= n; ++k )
			if( i + k >= s.size() || ( (unsigned char)s[ i + k ] & 0xC0 ) != 0x80 )
				return false;
		i += (size_t)n + 1;
	}
	return true;
}

void appendUtf8( std::string& out, uint32_t cp )
{
	if( cp < 0x80 )
		out.push_back( (char)cp );
	else if( cp < 0x800 )
	{
		out.push_back( (char)( 0xC0 | ( cp >> 6 ) ) );
		out.push_back( (char)( 0x80 | ( cp & 0x3F ) ) );
	}
	else
	{
		out.push_back( (char)( 0xE0 | ( cp >> 12 ) ) );
		out.push_back( (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) );
		out.push_back( (char)( 0x80 | ( cp & 0x3F ) ) );
	}
}

// Windows-1252 0x80..0x9F; the rest of the upper half equals Latin-1.
const uint16_t kCp1252High[ 32 ] = { 0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
									 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
									 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178 };

bool isBlank( const std::string& s )
{
	return std::all_of( s.begin(), s.end(), []( unsigned char c ) { return std::isspace( c ); } );
}

std::string trim( const std::string& s )
{
	size_t a = 0, b = s.size();
	while( a < b && std::isspace( (unsigned char)s[ a ] ) )
		++a;
	while( b > a && std::isspace( (unsigned char)s[ b - 1 ] ) )
		--b;
	return s.substr( a, b - a );
}
}//namespace

std::string toUtf8( const std::string& bytes )
{
	std::string s = bytes;
	if( s.size() >= 3 && (unsigned char)s[ 0 ] == 0xEF && (unsigned char)s[ 1 ] == 0xBB && (unsigned char)s[ 2 ] == 0xBF )
		s.erase( 0, 3 );
	if( isValidUtf8( s ) )
		return s;
	std::string out;
	out.reserve( s.size() + s.size() / 8 );
	for( unsigned char c : s )
	{
		if( c < 0x80 )
			out.push_back( (char)c );
		else if( c < 0xA0 )
			appendUtf8( out, kCp1252High[ c - 0x80 ] );
		else
			appendUtf8( out, c );
	}
	return out;
}

std::vector< std::vector< std::string > > parseCsv( const std::string& text )
{
	// Delimiter: the most frequent of ; , TAB in the first line (outside quotes).
	char delim = ',';
	{
		int counts[ 3 ] = { 0, 0, 0 };
		bool quoted     = false;
		for( char c : text )
		{
			if( c == '"' )
				quoted = !quoted;
			else if( !quoted && ( c == '\n' || c == '\r' ) )
				break;
			else if( !quoted )
				counts[ 0 ] += c == ',', counts[ 1 ] += c == ';', counts[ 2 ] += c == '\t';
		}
		if( counts[ 1 ] > counts[ 0 ] && counts[ 1 ] >= counts[ 2 ] )
			delim = ';';
		else if( counts[ 2 ] > counts[ 0 ] && counts[ 2 ] > counts[ 1 ] )
			delim = '\t';
	}

	std::vector< std::vector< std::string > > rows;
	std::vector< std::string > row;
	std::string cell;
	bool quoted = false, cellWasQuoted = false;
	auto endCell = [ & ]() {
		row.push_back( cellWasQuoted ? cell : trim( cell ) );
		cell.clear();
		cellWasQuoted = false;
	};
	auto endRow = [ & ]() {
		endCell();
		rows.push_back( row );
		row.clear();
	};
	for( size_t i = 0; i < text.size(); ++i )
	{
		const char c = text[ i ];
		if( quoted )
		{
			if( c == '"' )
			{
				if( i + 1 < text.size() && text[ i + 1 ] == '"' )
				{
					cell.push_back( '"' );
					++i;
				}
				else
					quoted = false;
			}
			else if( c == '\r' )
			{
				cell.push_back( '\n' );
				if( i + 1 < text.size() && text[ i + 1 ] == '\n' )
					++i;
			}
			else
				cell.push_back( c );
		}
		else if( c == '"' && isBlank( cell ) )
		{
			cell.clear();
			quoted        = true;
			cellWasQuoted = true;
		}
		else if( c == delim )
			endCell();
		else if( c == '\n' || c == '\r' )
		{
			if( c == '\r' && i + 1 < text.size() && text[ i + 1 ] == '\n' )
				++i;
			endRow();
		}
		else
			cell.push_back( c );
	}
	if( !cell.empty() || !row.empty() || cellWasQuoted )
		endRow();
	return rows;
}

std::vector< std::string > csvSlides( const std::vector< std::vector< std::string > >& rows, bool skipHeader )
{
	std::vector< std::string > slides;
	for( size_t r = skipHeader ? 1 : 0; r < rows.size(); ++r )
	{
		std::string slide;
		for( const std::string& cell : rows[ r ] )
		{
			if( isBlank( cell ) )
				continue;
			if( !slide.empty() )
				slide.push_back( '\n' );
			slide += cell;
		}
		if( !slide.empty() )
			slides.push_back( slide );
	}
	if( slides.empty() )
		slides.push_back( std::string() );
	return slides;
}

bool isCsvPath( const std::string& path )
{
	const size_t dot = path.find_last_of( '.' );
	if( dot == std::string::npos )
		return false;
	std::string ext = path.substr( dot + 1 );
	std::transform( ext.begin(), ext.end(), ext.begin(), []( unsigned char c ) { return (char)std::tolower( c ); } );
	return ext == "csv" || ext == "tsv";
}

}//namespace textplus
