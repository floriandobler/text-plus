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
