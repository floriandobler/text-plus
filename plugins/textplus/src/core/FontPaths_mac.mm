// macOS: CoreText knows every font the system can use, including fonts that
// font managers (Suitcase, FontBase, RightFont ...) activate at runtime.
#include "FontDatabase.h"

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#include <sys/stat.h>

namespace textplus
{

std::vector< std::string > platformFontFiles()
{
	std::vector< std::string > files;
	@autoreleasepool
	{
		CFArrayRef urls = CTFontManagerCopyAvailableFontURLs();
		if( urls )
		{
			CFIndex n = CFArrayGetCount( urls );
			files.reserve( (size_t)n );
			for( CFIndex i = 0; i < n; ++i )
			{
				CFURLRef url = (CFURLRef)CFArrayGetValueAtIndex( urls, i );
				char buf[ 4096 ];
				if( url && CFURLGetFileSystemRepresentation( url, true, (UInt8*)buf, sizeof( buf ) ) )
					files.emplace_back( buf );
			}
			CFRelease( urls );
		}
	}
	return files;
}

std::string platformCacheDirectory()
{
	std::string dir;
	@autoreleasepool
	{
		NSArray* paths = NSSearchPathForDirectoriesInDomains( NSCachesDirectory, NSUserDomainMask, YES );
		if( paths.count > 0 )
		{
			NSString* path = [paths[ 0 ] stringByAppendingPathComponent:@"TextPlus"];
			[[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
			dir = path.fileSystemRepresentation;
		}
	}
	return dir;
}

}//namespace textplus

#include "FontFallback.h"
#include <map>
#include <mutex>

namespace textplus
{
namespace
{
std::string toStd( CFStringRef s )
{
	if( !s )
		return {};
	char buf[ 1024 ];
	if( CFStringGetCString( s, buf, sizeof( buf ), kCFStringEncodingUTF8 ) )
		return buf;
	return {};
}
}//namespace

// Asks CoreText which font it would use for the text (the same cascade Safari and
// TextEdit use), then finds that face in the font file.
bool platformFallbackFont( const FontEntry& base, const std::string& utf8, FontEntry& out )
{
	static std::mutex mutex;
	static std::map< std::string, std::vector< FontEntry > > scanned;
	std::string path, ps;
	@autoreleasepool
	{
		CFStringRef baseName = CFStringCreateWithCString( nullptr, base.postscript.empty() ? "Helvetica" : base.postscript.c_str(), kCFStringEncodingUTF8 );
		if( !baseName )
			return false;
		CTFontRef font = CTFontCreateWithName( baseName, 12.0, nullptr );
		CFRelease( baseName );
		if( !font )
			return false;
		CFStringRef str = CFStringCreateWithCString( nullptr, utf8.c_str(), kCFStringEncodingUTF8 );
		if( !str )
		{
			CFRelease( font );
			return false;
		}
		CTFontRef fb = CTFontCreateForString( font, str, CFRangeMake( 0, CFStringGetLength( str ) ) );
		CFRelease( str );
		CFRelease( font );
		if( !fb )
			return false;
		if( CFURLRef url = (CFURLRef)CTFontCopyAttribute( fb, kCTFontURLAttribute ) )
		{
			char buf[ 4096 ];
			if( CFURLGetFileSystemRepresentation( url, true, (UInt8*)buf, sizeof( buf ) ) )
				path = buf;
			CFRelease( url );
		}
		if( CFStringRef psName = CTFontCopyPostScriptName( fb ) )
		{
			ps = toStd( psName );
			CFRelease( psName );
		}
		CFRelease( fb );
	}
	// Colour bitmap fonts have no outlines; LastResort only draws boxes.
	if( path.empty() || ps == "AppleColorEmoji" || ps == "LastResort" || ps == base.postscript )
		return false;
	std::lock_guard< std::mutex > lock( mutex );
	auto it = scanned.find( path );
	if( it == scanned.end() )
		it = scanned.emplace( path, FontDatabase::scanFile( path ) ).first;
	for( const FontEntry& e : it->second )
		if( e.postscript == ps && e.instance == 0 )
		{
			out = e;
			return true;
		}
	for( const FontEntry& e : it->second )
		if( e.postscript == ps )
		{
			out = e;
			return true;
		}
	if( it->second.empty() )
		return false;
	out = it->second.front();
	return true;
}

}//namespace textplus
