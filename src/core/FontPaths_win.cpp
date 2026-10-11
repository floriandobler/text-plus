// Windows: placeholder for the cross-platform phase.
// TODO(windows): enumerate fonts via DirectWrite (IDWriteFontCollection1 +
// IDWriteLocalFontFileLoader) so per-user fonts and font-manager activations
// are found too. For now: system and per-user font folders.
#include "FontDatabase.h"

#include <cstdlib>
#include <filesystem>

namespace textplus
{

std::vector< std::string > platformFontFiles()
{
	std::vector< std::string > files;
	std::vector< std::filesystem::path > dirs;
	if( const char* windir = std::getenv( "WINDIR" ) )
		dirs.emplace_back( std::filesystem::path( windir ) / "Fonts" );
	if( const char* local = std::getenv( "LOCALAPPDATA" ) )
		dirs.emplace_back( std::filesystem::path( local ) / "Microsoft" / "Windows" / "Fonts" );
	for( const auto& dir : dirs )
	{
		std::error_code ec;
		for( const auto& entry : std::filesystem::directory_iterator( dir, ec ) )
		{
			auto ext = entry.path().extension().string();
			for( char& c : ext )
				c = (char)tolower( (unsigned char)c );
			if( ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc" )
				files.push_back( entry.path().string() );
		}
	}
	return files;
}

std::string platformCacheDirectory()
{
	if( const char* local = std::getenv( "LOCALAPPDATA" ) )
	{
		std::filesystem::path dir = std::filesystem::path( local ) / "TextPlus";
		std::error_code ec;
		std::filesystem::create_directories( dir, ec );
		return dir.string();
	}
	return {};
}

}//namespace textplus
