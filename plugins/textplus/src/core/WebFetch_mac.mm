// macOS: NSURLSession (system TLS, proxies, redirects).
#include "WebFetch.h"

#import <Foundation/Foundation.h>

namespace textplus
{
bool httpGet( const std::string& url, int timeoutSeconds, std::string& body, std::string& contentType, std::string& error )
{
	@autoreleasepool
	{
		NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
		if( !u )
		{
			error = "bad URL";
			return false;
		}
		NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		cfg.timeoutIntervalForRequest  = timeoutSeconds;
		cfg.timeoutIntervalForResource = timeoutSeconds;
		cfg.HTTPAdditionalHeaders      = @{ @"User-Agent" : @"TextPlus/2" };
		NSURLSession* session          = [NSURLSession sessionWithConfiguration:cfg];
		dispatch_semaphore_t done      = dispatch_semaphore_create( 0 );
		__block NSData* data           = nil;
		__block NSURLResponse* resp    = nil;
		__block NSError* err           = nil;
		NSURLSessionDataTask* task     = [session dataTaskWithURL:u
										  completionHandler:^( NSData* d, NSURLResponse* r, NSError* e ) {
											  data = d;
											  resp = r;
											  err  = e;
											  dispatch_semaphore_signal( done );
										  }];
		[task resume];
		dispatch_semaphore_wait( done, dispatch_time( DISPATCH_TIME_NOW, (int64_t)( timeoutSeconds + 2 ) * NSEC_PER_SEC ) );
		[session finishTasksAndInvalidate];
		if( err || !data )
		{
			error = err ? [[err localizedDescription] UTF8String] : "timeout";
			return false;
		}
		if( [resp isKindOfClass:[NSHTTPURLResponse class]] )
		{
			NSHTTPURLResponse* h = (NSHTTPURLResponse*)resp;
			if( h.statusCode >= 400 )
			{
				error = "HTTP " + std::to_string( (long)h.statusCode );
				return false;
			}
			NSString* type = [h valueForHTTPHeaderField:@"Content-Type"];
			contentType    = type ? [type UTF8String] : "";
		}
		body.assign( (const char*)data.bytes, (size_t)data.length );
		return true;
	}
}
}//namespace textplus
