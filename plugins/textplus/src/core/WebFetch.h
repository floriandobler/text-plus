#pragma once
// Web data for the text: URLs are fetched in one background thread, never on
// the render thread. An instance polls every frame and gets the latest result
// at once; the fetch happens when the refresh interval is due. URLs nobody
// polled for a while are forgotten.
//
// HTTP(S) uses the operating system: NSURLSession on macOS, WinHTTP on
// Windows, libcurl on Linux (development) or plain HTTP without it.

#include <cstdint>
#include <string>

namespace textplus
{

// Blocking GET with redirects; implemented per platform.
bool httpGet( const std::string& url, int timeoutSeconds, std::string& body, std::string& contentType, std::string& error );

class WebFetcher
{
public:
	static WebFetcher& instance();

	struct Result
	{
		std::string body, contentType;
		int64_t stamp = -1;// changes whenever new content arrived (-1 = nothing yet)
		std::string status;// "ok 14:02:11", "loading...", "error: ..."
	};
	// Non-blocking: returns what is there now and schedules a fetch when due.
	Result poll( const std::string& url, double refreshSeconds );

	~WebFetcher();

	struct Impl;

private:
	WebFetcher();
	Impl* impl;
};

enum class WebFormat
{
	Text,
	Json,
	Csv
};
WebFormat detectWebFormat( const std::string& url, const std::string& contentType, const std::string& body );

}//namespace textplus
