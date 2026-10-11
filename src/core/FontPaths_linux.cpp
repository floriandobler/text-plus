// Linux: only used for development and the headless test host.
// Scans the usual font directories recursively.
#include "FontDatabase.h"

#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>

namespace textplus
{
namespace
{
bool hasFontExtension( const std::string& name )
{
	auto dot = name.rfind( '.' );
	if( dot == std::string::npos )
		return false;
	std::string ext = name.substr( dot + 1 );
	for( char& c : ext )
		c = (char)tolower( (unsigned char)c );
	return ext == "ttf" || ext == "otf" || ext == "ttc" || ext == "otc";
}

void walk( const std::string& dir, std::vector< std::string >& out, int depth )
{
	if( depth > 8 )
		return;
	DIR* d = opendir( dir.c_str() );
	if( !d )
		return;
	while( dirent* e = readdir( d ) )
	{
		std::string name = e->d_name;
		if( name == "." || name == ".." )
			continue;
		std::string path = dir + "/" + name;
		struct stat st;
		if( stat( path.c_str(), &st ) != 0 )
			continue;
		if( S_ISDIR( st.st_mode ) )
			walk( path, out, depth + 1 );
		else if( hasFontExtension( name ) )
			out.push_back( path );
	}
	closedir( d );
}
}//namespace

std::vector< std::string > platformFontFiles()
{
	std::vector< std::string > files;
	walk( "/usr/share/fonts", files, 0 );
	walk( "/usr/local/share/fonts", files, 0 );
	if( const char* home = getenv( "HOME" ) )
	{
		walk( std::string( home ) + "/.fonts", files, 0 );
		walk( std::string( home ) + "/.local/share/fonts", files, 0 );
	}
	return files;
}

std::string platformCacheDirectory()
{
	std::string base;
	if( const char* xdg = getenv( "XDG_CACHE_HOME" ) )
		base = xdg;
	else if( const char* home = getenv( "HOME" ) )
		base = std::string( home ) + "/.cache";
	if( base.empty() )
		return {};
	std::string dir = base + "/text-plus";
	mkdir( base.c_str(), 0755 );
	mkdir( dir.c_str(), 0755 );
	return dir;
}

}//namespace textplus
