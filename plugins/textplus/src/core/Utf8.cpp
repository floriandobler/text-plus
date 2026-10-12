#include "Utf8.h"

namespace textplus
{

uint32_t decodeUtf8At( const std::string& s, size_t i, size_t* length )
{
	auto fail = [ & ]() -> uint32_t {
		if( length )
			*length = 1;
		return 0xFFFD;
	};
	if( i >= s.size() )
		return fail();
	unsigned char c = (unsigned char)s[ i ];
	int n           = 0;
	uint32_t cp     = 0;
	if( c < 0x80 )
	{
		if( length )
			*length = 1;
		return c;
	}
	else if( ( c & 0xE0 ) == 0xC0 )
	{
		n  = 1;
		cp = c & 0x1F;
	}
	else if( ( c & 0xF0 ) == 0xE0 )
	{
		n  = 2;
		cp = c & 0x0F;
	}
	else if( ( c & 0xF8 ) == 0xF0 )
	{
		n  = 3;
		cp = c & 0x07;
	}
	else
		return fail();
	for( int k = 1; k <= n; ++k )
	{
		if( i + k >= s.size() )
			return fail();
		unsigned char cc = (unsigned char)s[ i + k ];
		if( ( cc & 0xC0 ) != 0x80 )
			return fail();
		cp = ( cp << 6 ) | ( cc & 0x3F );
	}
	if( length )
		*length = (size_t)n + 1;
	return cp;
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
	else if( cp < 0x10000 )
	{
		out.push_back( (char)( 0xE0 | ( cp >> 12 ) ) );
		out.push_back( (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) );
		out.push_back( (char)( 0x80 | ( cp & 0x3F ) ) );
	}
	else
	{
		out.push_back( (char)( 0xF0 | ( cp >> 18 ) ) );
		out.push_back( (char)( 0x80 | ( ( cp >> 12 ) & 0x3F ) ) );
		out.push_back( (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) ) );
		out.push_back( (char)( 0x80 | ( cp & 0x3F ) ) );
	}
}

uint32_t toUpper( uint32_t c )
{
	if( c >= 'a' && c <= 'z' )
		return c - 32;
	if( c < 0x80 )
		return c;
	if( ( c >= 0xE0 && c <= 0xFE && c != 0xF7 ) )
		return c - 32;
	if( c == 0xFF )
		return 0x178;
	if( c == 0xB5 )
		return 0x39C;// micro sign -> Greek capital mu
	// Latin Extended-A: pairs (even upper, odd lower) with two shifted runs
	if( c >= 0x100 && c <= 0x17F )
	{
		if( ( c >= 0x139 && c <= 0x148 ) || ( c >= 0x179 && c <= 0x17E ) )
			return ( c & 1 ) ? c : c - 1;// odd upper, even lower
		if( c == 0x131 )
			return 'I';
		if( c == 0x17F )
			return 'S';
		if( c == 0x138 || c == 0x149 )
			return c;
		return ( c & 1 ) ? c - 1 : c;
	}
	if( c == 0x180 )
		return 0x243;
	if( c >= 0x1CD && c <= 0x1DC )
		return ( c & 1 ) ? c : c - 1;
	if( c >= 0x1DE && c <= 0x1EF )
		return ( c & 1 ) ? c - 1 : c;
	if( c >= 0x1F8 && c <= 0x21F )
		return ( c & 1 ) ? c - 1 : c;
	if( c >= 0x222 && c <= 0x233 )
		return ( c & 1 ) ? c - 1 : c;
	// Greek
	if( c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2 )
		return c - 32;
	if( c == 0x3C2 )
		return 0x3A3;// final sigma
	if( c == 0x3AC )
		return 0x386;
	if( c >= 0x3AD && c <= 0x3AF )
		return c - 37;
	if( c == 0x3CC )
		return 0x38C;
	if( c == 0x3CD || c == 0x3CE )
		return c - 63;
	if( c == 0x3CA || c == 0x3CB )
		return c - 32;
	// Cyrillic
	if( c >= 0x430 && c <= 0x44F )
		return c - 32;
	if( c >= 0x450 && c <= 0x45F )
		return c - 80;
	if( c >= 0x460 && c <= 0x4FF && c != 0x482 && !( c >= 0x483 && c <= 0x489 ) )
	{
		if( c >= 0x4C1 && c <= 0x4CE )
			return ( c & 1 ) ? c : c - 1;
		if( c == 0x4CF )
			return 0x4C0;
		return ( c & 1 ) ? c - 1 : c;
	}
	// Latin Extended Additional (Vietnamese and others): pairs
	if( c >= 0x1E00 && c <= 0x1EFF && !( c >= 0x1E96 && c <= 0x1E9F ) )
		return ( c & 1 ) ? c - 1 : c;
	// Armenian
	if( c >= 0x561 && c <= 0x586 )
		return c - 48;
	return c;
}

uint32_t toLower( uint32_t c )
{
	if( c >= 'A' && c <= 'Z' )
		return c + 32;
	if( c < 0x80 )
		return c;
	if( c >= 0xC0 && c <= 0xDE && c != 0xD7 )
		return c + 32;
	if( c == 0x178 )
		return 0xFF;
	if( c == 0x1E9E )
		return 0xDF;// capital sharp s
	if( c >= 0x100 && c <= 0x17F )
	{
		if( ( c >= 0x139 && c <= 0x148 ) || ( c >= 0x179 && c <= 0x17E ) )
			return ( c & 1 ) ? c + 1 : c;
		if( c == 0x130 )
			return 'i';
		if( c == 0x138 || c == 0x149 || c == 0x17F )
			return c;
		return ( c & 1 ) ? c : c + 1;
	}
	if( c == 0x243 )
		return 0x180;
	if( c >= 0x1CD && c <= 0x1DC )
		return ( c & 1 ) ? c + 1 : c;
	if( c >= 0x1DE && c <= 0x1EF )
		return ( c & 1 ) ? c : c + 1;
	if( c >= 0x1F8 && c <= 0x21F )
		return ( c & 1 ) ? c : c + 1;
	if( c >= 0x222 && c <= 0x233 )
		return ( c & 1 ) ? c : c + 1;
	if( c >= 0x391 && c <= 0x3A9 && c != 0x3A2 )
		return c + 32;
	if( c == 0x386 )
		return 0x3AC;
	if( c >= 0x388 && c <= 0x38A )
		return c + 37;
	if( c == 0x38C )
		return 0x3CC;
	if( c == 0x38E || c == 0x38F )
		return c + 63;
	if( c == 0x3AA || c == 0x3AB )
		return c + 32;
	if( c >= 0x410 && c <= 0x42F )
		return c + 32;
	if( c >= 0x400 && c <= 0x40F )
		return c + 80;
	if( c >= 0x460 && c <= 0x4FF && c != 0x482 && !( c >= 0x483 && c <= 0x489 ) )
	{
		if( c >= 0x4C1 && c <= 0x4CE )
			return ( c & 1 ) ? c + 1 : c;
		if( c == 0x4C0 )
			return 0x4CF;
		return ( c & 1 ) ? c : c + 1;
	}
	if( c >= 0x1E00 && c <= 0x1EFF && !( c >= 0x1E96 && c <= 0x1E9F ) )
		return ( c & 1 ) ? c : c + 1;
	if( c >= 0x531 && c <= 0x556 )
		return c + 48;
	return c;
}

std::string applyCase( const std::string& s, TextCase mode )
{
	if( mode == TextCase::Unchanged )
		return s;
	std::string out;
	out.reserve( s.size() + 8 );
	for( size_t i = 0; i < s.size(); )
	{
		// Keep escapes ("\n", "\\") and markup tags ("{#ff8800}", "{/}", "{{") as they are.
		if( s[ i ] == '\\' && i + 1 < s.size() )
		{
			out.append( s, i, 2 );
			i += 2;
			continue;
		}
		if( s[ i ] == '{' )
		{
			size_t close = s.find( '}', i );
			if( close != std::string::npos && close - i <= 9 && ( i + 1 < s.size() && ( s[ i + 1 ] == '#' || s[ i + 1 ] == '/' ) ) )
			{
				out.append( s, i, close - i + 1 );
				i = close + 1;
				continue;
			}
		}
		size_t len  = 1;
		uint32_t cp = decodeUtf8At( s, i, &len );
		if( mode == TextCase::Upper && cp == 0xDF )
			out += "SS";// German sharp s has no single-letter capital in common use
		else
			appendUtf8( out, mode == TextCase::Upper ? toUpper( cp ) : toLower( cp ) );
		i += len;
	}
	return out;
}

bool isBreakingSpace( uint32_t cp )
{
	switch( cp )
	{
	case 0x20:
	case 0x09:
	case 0x1680:
	case 0x2000:
	case 0x2001:
	case 0x2002:
	case 0x2003:
	case 0x2004:
	case 0x2005:
	case 0x2006:
	case 0x2008:
	case 0x2009:
	case 0x200A:
	case 0x205F:
	case 0x3000:
		return true;
	default:
		return false;
	}
}

std::vector< std::string > splitParagraphs( const std::string& raw, bool pipeIsNewline )
{
	std::vector< std::string > result( 1 );
	for( size_t i = 0; i < raw.size(); ++i )
	{
		char c = raw[ i ];
		if( c == '\r' )
		{
			if( i + 1 < raw.size() && raw[ i + 1 ] == '\n' )
				++i;
			result.emplace_back();
		}
		else if( c == '\n' )
		{
			result.emplace_back();
		}
		else if( c == '\\' && i + 1 < raw.size() && raw[ i + 1 ] == 'n' )
		{
			++i;
			result.emplace_back();
		}
		else if( c == '\\' && i + 1 < raw.size() && raw[ i + 1 ] == '\\' )
		{
			++i;
			result.back().push_back( '\\' );
		}
		else if( c == '|' && pipeIsNewline )
		{
			result.emplace_back();
		}
		else
		{
			result.back().push_back( c );
		}
	}
	return result;
}

namespace
{
int hexValue( char c )
{
	if( c >= '0' && c <= '9' )
		return c - '0';
	if( c >= 'a' && c <= 'f' )
		return c - 'a' + 10;
	if( c >= 'A' && c <= 'F' )
		return c - 'A' + 10;
	return -1;
}

// Parses "#rgb" or "#rrggbb" (without braces). Returns false if malformed.
bool parseHexColor( const std::string& s, Rgb& out )
{
	if( s.size() != 4 && s.size() != 7 )
		return false;
	if( s[ 0 ] != '#' )
		return false;
	int v[ 6 ];
	for( size_t i = 1; i < s.size(); ++i )
		if( ( v[ i - 1 ] = hexValue( s[ i ] ) ) < 0 )
			return false;
	if( s.size() == 4 )
	{
		out.r = v[ 0 ] * 17 / 255.0f;
		out.g = v[ 1 ] * 17 / 255.0f;
		out.b = v[ 2 ] * 17 / 255.0f;
	}
	else
	{
		out.r = ( v[ 0 ] * 16 + v[ 1 ] ) / 255.0f;
		out.g = ( v[ 2 ] * 16 + v[ 3 ] ) / 255.0f;
		out.b = ( v[ 4 ] * 16 + v[ 5 ] ) / 255.0f;
	}
	return true;
}
}//namespace

std::vector< MarkedParagraph > parseMarkedText( const std::string& raw, bool pipeIsNewline, bool markup, std::vector< Rgb >& palette )
{
	std::vector< MarkedParagraph > result( 1 );
	int16_t current = -1;
	auto put        = [ & ]( char c ) {
		result.back().text.push_back( c );
		result.back().colorOfByte.push_back( current );
	};
	for( size_t i = 0; i < raw.size(); ++i )
	{
		char c = raw[ i ];
		if( c == '\r' )
		{
			if( i + 1 < raw.size() && raw[ i + 1 ] == '\n' )
				++i;
			result.emplace_back();
		}
		else if( c == '\n' )
			result.emplace_back();
		else if( c == '\\' && i + 1 < raw.size() && raw[ i + 1 ] == 'n' )
		{
			++i;
			result.emplace_back();
		}
		else if( c == '\\' && i + 1 < raw.size() && raw[ i + 1 ] == '\\' )
		{
			++i;
			put( '\\' );
		}
		else if( c == '|' && pipeIsNewline )
			result.emplace_back();
		else if( c == '{' && markup )
		{
			if( i + 1 < raw.size() && raw[ i + 1 ] == '{' )
			{
				++i;
				put( '{' );
				continue;
			}
			size_t close = raw.find( '}', i );
			std::string tag = close == std::string::npos ? std::string() : raw.substr( i + 1, close - i - 1 );
			Rgb rgb;
			if( tag == "/" )
			{
				current = -1;
				i       = close;
			}
			else if( parseHexColor( tag, rgb ) && palette.size() < 32000 )
			{
				palette.push_back( rgb );
				current = (int16_t)( palette.size() - 1 );
				i       = close;
			}
			else
				put( c );// not a tag: keep it
		}
		else
			put( c );
	}
	return result;
}

}//namespace textplus
