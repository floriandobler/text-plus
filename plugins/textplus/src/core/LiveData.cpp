#include "LiveData.h"

#include "Clock.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace textplus
{

// ===========================================================================
// JSON
// ===========================================================================
struct JsonParser
{
	const std::string& src;
	size_t i = 0;
	std::string error;
	int depth = 0;

	void ws()
	{
		while( i < src.size() && ( src[ i ] == ' ' || src[ i ] == '\t' || src[ i ] == '\n' || src[ i ] == '\r' ) )
			++i;
	}
	bool fail( const char* what )
	{
		if( error.empty() )
			error = std::string( what ) + " at byte " + std::to_string( i );
		return false;
	}
	static void putUtf8( std::string& out, uint32_t cp )
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
	bool hex4( uint32_t& v )
	{
		if( i + 4 > src.size() )
			return fail( "short \\u escape" );
		v = 0;
		for( int k = 0; k < 4; ++k )
		{
			const char c = src[ i++ ];
			v <<= 4;
			if( c >= '0' && c <= '9' )
				v |= (uint32_t)( c - '0' );
			else if( c >= 'a' && c <= 'f' )
				v |= (uint32_t)( c - 'a' + 10 );
			else if( c >= 'A' && c <= 'F' )
				v |= (uint32_t)( c - 'A' + 10 );
			else
				return fail( "bad \\u escape" );
		}
		return true;
	}
	bool str( std::string& out )
	{
		if( i >= src.size() || src[ i ] != '"' )
			return fail( "expected string" );
		++i;
		while( i < src.size() )
		{
			const char c = src[ i++ ];
			if( c == '"' )
				return true;
			if( c != '\\' )
			{
				out.push_back( c );
				continue;
			}
			if( i >= src.size() )
				break;
			const char e = src[ i++ ];
			switch( e )
			{
			case 'n': out.push_back( '\n' ); break;
			case 't': out.push_back( '\t' ); break;
			case 'r': out.push_back( '\r' ); break;
			case 'b': out.push_back( '\b' ); break;
			case 'f': out.push_back( '\f' ); break;
			case 'u': {
				uint32_t cp = 0;
				if( !hex4( cp ) )
					return false;
				if( cp >= 0xD800 && cp < 0xDC00 && i + 1 < src.size() && src[ i ] == '\\' && src[ i + 1 ] == 'u' )
				{
					i += 2;
					uint32_t lo = 0;
					if( !hex4( lo ) )
						return false;
					cp = 0x10000 + ( ( cp - 0xD800 ) << 10 ) + ( lo - 0xDC00 );
				}
				putUtf8( out, cp );
				break;
			}
			default: out.push_back( e ); break;
			}
		}
		return fail( "unterminated string" );
	}
	bool value( Json& v )
	{
		if( ++depth > 200 )
			return fail( "nested too deeply" );
		ws();
		if( i >= src.size() )
			return fail( "unexpected end" );
		const char c = src[ i ];
		bool ok      = true;
		if( c == '{' )
		{
			v.t = Json::Type::Object;
			++i;
			ws();
			if( i < src.size() && src[ i ] == '}' )
				++i;
			else
				for( ;; )
				{
					ws();
					std::string key;
					if( !str( key ) )
						return false;
					ws();
					if( i >= src.size() || src[ i ] != ':' )
						return fail( "expected ':'" );
					++i;
					Json member;
					if( !value( member ) )
						return false;
					v.obj.emplace_back( std::move( key ), std::move( member ) );
					ws();
					if( i < src.size() && src[ i ] == ',' )
					{
						++i;
						continue;
					}
					if( i < src.size() && src[ i ] == '}' )
					{
						++i;
						break;
					}
					return fail( "expected ',' or '}'" );
				}
		}
		else if( c == '[' )
		{
			v.t = Json::Type::Array;
			++i;
			ws();
			if( i < src.size() && src[ i ] == ']' )
				++i;
			else
				for( ;; )
				{
					Json item;
					if( !value( item ) )
						return false;
					v.arr.push_back( std::move( item ) );
					ws();
					if( i < src.size() && src[ i ] == ',' )
					{
						++i;
						continue;
					}
					if( i < src.size() && src[ i ] == ']' )
					{
						++i;
						break;
					}
					return fail( "expected ',' or ']'" );
				}
		}
		else if( c == '"' )
		{
			v.t = Json::Type::String;
			ok  = str( v.s );
		}
		else if( src.compare( i, 4, "true" ) == 0 )
		{
			v.t = Json::Type::Bool;
			v.b = true;
			i += 4;
		}
		else if( src.compare( i, 5, "false" ) == 0 )
		{
			v.t = Json::Type::Bool;
			i += 5;
		}
		else if( src.compare( i, 4, "null" ) == 0 )
			i += 4;
		else if( c == '-' || ( c >= '0' && c <= '9' ) )
		{
			const char* begin = src.c_str() + i;
			char* end         = nullptr;
			v.t               = Json::Type::Number;
			v.n               = std::strtod( begin, &end );
			if( end == begin )
				return fail( "bad number" );
			i += (size_t)( end - begin );
		}
		else
			return fail( "unexpected character" );
		--depth;
		return ok;
	}
};

Json Json::parse( const std::string& text, std::string* error )
{
	JsonParser p{ text };
	Json v;
	// Skip a UTF-8 byte order mark
	if( text.size() >= 3 && (unsigned char)text[ 0 ] == 0xEF && (unsigned char)text[ 1 ] == 0xBB && (unsigned char)text[ 2 ] == 0xBF )
		p.i = 3;
	if( !p.value( v ) )
	{
		if( error )
			*error = p.error;
		return Json();
	}
	if( error )
		error->clear();
	return v;
}

const Json* Json::find( const std::string& path ) const
{
	const Json* cur = this;
	size_t i        = 0;
	while( cur && i < path.size() )
	{
		if( path[ i ] == '.' )
		{
			++i;
			continue;
		}
		if( path[ i ] == '[' )
		{
			const size_t close = path.find( ']', i );
			if( close == std::string::npos )
				return nullptr;
			const std::string idx = path.substr( i + 1, close - i - 1 );
			i                     = close + 1;
			if( cur->t != Type::Array )
				return nullptr;
			char* end     = nullptr;
			const long k  = std::strtol( idx.c_str(), &end, 10 );
			const long nn = (long)cur->arr.size();
			const long kk = k < 0 ? nn + k : k;// [-1] = last
			if( end == idx.c_str() || kk < 0 || kk >= nn )
				return nullptr;
			cur = &cur->arr[ (size_t)kk ];
			continue;
		}
		size_t end = i;
		while( end < path.size() && path[ end ] != '.' && path[ end ] != '[' )
			++end;
		const std::string key = path.substr( i, end - i );
		i                     = end;
		if( cur->t == Type::Object )
		{
			const Json* next = nullptr;
			for( const auto& m : cur->obj )
				if( m.first == key )
				{
					next = &m.second;
					break;
				}
			cur = next;
		}
		else if( cur->t == Type::Array && !key.empty() && key.find_first_not_of( "-0123456789" ) == std::string::npos )
		{
			const long k  = std::strtol( key.c_str(), nullptr, 10 );
			const long nn = (long)cur->arr.size();
			const long kk = k < 0 ? nn + k : k;
			cur           = kk >= 0 && kk < nn ? &cur->arr[ (size_t)kk ] : nullptr;
		}
		else
			return nullptr;
	}
	return cur;
}

static std::string numberText( double v )
{
	char buf[ 64 ];
	if( std::fabs( v - std::round( v ) ) < 1e-9 && std::fabs( v ) < 1e15 )
		std::snprintf( buf, sizeof( buf ), "%.0f", v );
	else
	{
		std::snprintf( buf, sizeof( buf ), "%.6f", v );
		std::string s = buf;
		while( !s.empty() && s.back() == '0' )
			s.pop_back();
		if( !s.empty() && s.back() == '.' )
			s.pop_back();
		return s;
	}
	return buf;
}

std::string Json::text() const
{
	switch( t )
	{
	case Type::Null: return "";
	case Type::Bool: return b ? "true" : "false";
	case Type::Number: return numberText( n );
	case Type::String: return s;
	case Type::Array: {
		std::string out;
		for( size_t k = 0; k < arr.size(); ++k )
			out += ( k ? ", " : "" ) + arr[ k ].text();
		return out;
	}
	case Type::Object: return "{...}";
	}
	return "";
}

std::string Json::outline( int maxLines ) const
{
	// Leaf values with the path that reaches them ("main.temp: 17.4",
	// "weather[0].description: light rain"): exactly what a placeholder needs.
	std::string out;
	int lines = 0;
	std::function< void( const Json&, const std::string&, int ) > walk = [ & ]( const Json& v, const std::string& path, int depth ) {
		if( lines >= maxLines )
			return;
		if( ( v.t == Type::Object || v.t == Type::Array ) && depth < 4 )
		{
			if( v.t == Type::Object )
				for( const auto& m : v.obj )
					walk( m.second, path.empty() ? m.first : path + "." + m.first, depth + 1 );
			else
				for( size_t k = 0; k < v.arr.size() && k < 3; ++k )
					walk( v.arr[ k ], path + "[" + std::to_string( k ) + "]", depth + 1 );
			return;
		}
		out += ( lines ? "\n" : "" ) + ( path.empty() ? std::string() : path + ": " ) + ( v.t == Type::Object ? "{...}" : v.text() );
		++lines;
	};
	walk( *this, "", 0 );
	return out;
}

// ===========================================================================
// Placeholders
// ===========================================================================
std::string expandPlaceholders( const std::string& text, const PlaceholderContext& ctx, bool* found )
{
	if( found )
		*found = false;
	if( text.find( '{' ) == std::string::npos )
		return text;
	std::string out;
	out.reserve( text.size() );
	LocalTime now;
	bool haveNow = false;
	for( size_t i = 0; i < text.size(); )
	{
		if( text.compare( i, 2, "{{" ) == 0 )
		{
			out += "{{";
			i += 2;
			continue;
		}
		if( text[ i ] != '{' )
		{
			out.push_back( text[ i++ ] );
			continue;
		}
		const size_t close = text.find( '}', i + 1 );
		if( close == std::string::npos || close - i > 200 )
		{
			out.push_back( text[ i++ ] );
			continue;
		}
		const std::string key = text.substr( i + 1, close - i - 1 );
		std::string value;
		bool ok = false;
		auto ensureNow = [ & ]() {
			if( !haveNow )
			{
				now     = localNow();
				haveNow = true;
			}
		};
		if( key.empty() || key[ 0 ] == '#' || key == "/" )
			ok = false;// colour tags belong to the markup
		else if( key == "bpm" )
		{
			if( ctx.bpm > 0 )
			{
				value = numberText( std::round( ctx.bpm * 10.0 ) / 10.0 );
				ok    = true;
			}
		}
		else if( key == "beat" )
		{
			value = std::to_string( (int)std::floor( std::fmod( std::max( 0.0, ctx.beats ), 4.0 ) ) + 1 );
			ok    = true;
		}
		else if( key == "bar" )
		{
			value = std::to_string( (int)std::floor( std::max( 0.0, ctx.beats ) / 4.0 ) + 1 );
			ok    = true;
		}
		else if( key == "slide" )
		{
			value = std::to_string( ctx.slide );
			ok    = true;
		}
		else if( key == "slides" )
		{
			value = std::to_string( ctx.slides );
			ok    = true;
		}
		else if( key == "time" || key == "time:s" )
		{
			ensureNow();
			value = formatTimeOfDay( now.secondsOfDay, key == "time" ? TimeFormat::HHMM : TimeFormat::HHMMSS );
			ok    = true;
		}
		else if( key == "date" || key == "date:iso" )
		{
			ensureNow();
			char buf[ 32 ];
			if( key == "date" )
				std::snprintf( buf, sizeof( buf ), "%02d.%02d.%04d", now.day, now.month, now.year );
			else
				std::snprintf( buf, sizeof( buf ), "%04d-%02d-%02d", now.year, now.month, now.day );
			value = buf;
			ok    = true;
		}
		else if( key == "weekday" )
		{
			ensureNow();
			// Zeller-style day of week from the date (0 = Sunday)
			int y = now.year, m = now.month, d = now.day;
			if( m < 3 )
			{
				m += 12;
				--y;
			}
			const int k = y % 100, j = y / 100;
			const int h = ( d + 13 * ( m + 1 ) / 5 + k + k / 4 + j / 4 + 5 * j ) % 7;// 0 = Saturday
			static const char* names[] = { "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday" };
			value = names[ h ];
			ok    = true;
		}
		else if( key[ 0 ] == '/' )
			ok = Variables::instance().get( key, value );// OSC address
		else if( ctx.json )
		{
			const Json* v = ctx.json->find( key );
			if( v )
			{
				value = v->text();
				ok    = true;
			}
		}
		if( ok )
		{
			out += value;
			if( found )
				*found = true;
		}
		else
			out += text.substr( i, close - i + 1 );
		i = close + 1;
	}
	return out;
}

// ===========================================================================
// Variables
// ===========================================================================
Variables& Variables::instance()
{
	static Variables v;
	return v;
}

void Variables::set( const std::string& name, const std::string& value )
{
	std::lock_guard< std::mutex > lock( mutex );
	auto it = values.find( name );
	if( it != values.end() && it->second == value )
		return;
	values[ name ] = value;
	++gen;
}

bool Variables::get( const std::string& name, std::string& value ) const
{
	std::lock_guard< std::mutex > lock( mutex );
	auto it = values.find( name );
	if( it == values.end() )
		return false;
	value = it->second;
	return true;
}

uint64_t Variables::generation() const
{
	std::lock_guard< std::mutex > lock( mutex );
	return gen;
}

std::vector< std::pair< std::string, std::string > > Variables::snapshot() const
{
	std::lock_guard< std::mutex > lock( mutex );
	return std::vector< std::pair< std::string, std::string > >( values.begin(), values.end() );
}

}//namespace textplus
