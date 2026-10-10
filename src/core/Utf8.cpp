#include "Utf8.h"

namespace kontur
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

}//namespace kontur
