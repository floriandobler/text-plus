#include "FileUtil.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace textplus
{
// Generated from resources/ by cmake/EmbedFile.cmake
extern const unsigned char NotoEmoji[];
extern const unsigned long NotoEmojiSize;

std::filesystem::path pathFromUtf8( const std::string& utf8 )
{
#if defined( _WIN32 )
	return std::filesystem::u8path( utf8 );
#else
	return std::filesystem::path( utf8 );
#endif
}

std::string utf8FromPath( const std::filesystem::path& p )
{
#if defined( _WIN32 )
	return p.u8string();
#else
	return p.string();
#endif
}

bool fileInfo( const std::string& utf8Path, int64_t& mtime, int64_t& size )
{
	std::error_code ec;
	const std::filesystem::path p = pathFromUtf8( utf8Path );
	const auto sz                 = std::filesystem::file_size( p, ec );
	if( ec )
		return false;
	const auto t = std::filesystem::last_write_time( p, ec );
	if( ec )
		return false;
	size  = (int64_t)sz;
	mtime = (int64_t)std::chrono::duration_cast< std::chrono::seconds >( t.time_since_epoch() ).count();
	return true;
}

int64_t fileStamp( const std::string& utf8Path )
{
	std::error_code ec;
	const std::filesystem::path p = pathFromUtf8( utf8Path );
	const auto sz                 = std::filesystem::file_size( p, ec );
	if( ec )
		return -1;
	const auto t = std::filesystem::last_write_time( p, ec );
	if( ec )
		return -1;
	// The file clock's epoch is implementation defined (libstdc++: year 2174), so
	// times can be negative: keep the stamp >= 0, -1 means "missing".
	const int64_t ms = (int64_t)std::chrono::duration_cast< std::chrono::milliseconds >( t.time_since_epoch() ).count();
	const int64_t v  = ms + (int64_t)sz * 31;
	return v < 0 ? -v : v;
}

bool readFileBytes( const std::string& utf8Path, std::vector< uint8_t >& out, size_t maxBytes )
{
	if( isEmbeddedPath( utf8Path ) )
	{
		size_t n          = 0;
		const uint8_t* p  = embeddedFont( utf8Path, n );
		if( !p )
			return false;
		out.assign( p, p + n );
		return true;
	}
	std::ifstream in( pathFromUtf8( utf8Path ), std::ios::binary | std::ios::ate );
	if( !in )
		return false;
	std::streamsize size = in.tellg();
	if( size <= 0 || (size_t)size > maxBytes )
		return false;
	out.resize( (size_t)size );
	in.seekg( 0 );
	return (bool)in.read( (char*)out.data(), size );
}

std::string readTextFile( const std::string& utf8Path )
{
	std::ifstream in( pathFromUtf8( utf8Path ), std::ios::binary );
	if( !in )
		return {};
	std::stringstream ss;
	ss << in.rdbuf();
	std::string s = ss.str();
	if( s.size() >= 3 && (unsigned char)s[ 0 ] == 0xEF && (unsigned char)s[ 1 ] == 0xBB && (unsigned char)s[ 2 ] == 0xBF )
		s.erase( 0, 3 );// UTF-8 BOM
	while( !s.empty() && ( s.back() == '\n' || s.back() == '\r' ) )
		s.pop_back();
	return s;
}

bool isEmbeddedPath( const std::string& path )
{
	return path.compare( 0, 9, kEmbeddedPrefix ) == 0;
}

const uint8_t* embeddedFont( const std::string& path, size_t& size )
{
	if( path == std::string( kEmbeddedPrefix ) + "NotoEmoji" )
	{
		size = (size_t)NotoEmojiSize;
		return NotoEmoji;
	}
	size = 0;
	return nullptr;
}

}//namespace textplus
