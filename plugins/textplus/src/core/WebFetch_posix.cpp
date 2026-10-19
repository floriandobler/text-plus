// Linux (development and tests): libcurl when available, otherwise plain HTTP.
#include "WebFetch.h"

#include <cstring>
#include <string>

#if defined( TEXTPLUS_HAVE_CURL )
#include <curl/curl.h>

namespace textplus
{
static size_t onData( char* p, size_t size, size_t n, void* user )
{
	static_cast< std::string* >( user )->append( p, size * n );
	return size * n;
}

bool httpGet( const std::string& url, int timeoutSeconds, std::string& body, std::string& contentType, std::string& error )
{
	static bool init = [] { return curl_global_init( CURL_GLOBAL_DEFAULT ) == 0; }();
	if( !init )
	{
		error = "curl unavailable";
		return false;
	}
	CURL* c = curl_easy_init();
	if( !c )
	{
		error = "curl unavailable";
		return false;
	}
	body.clear();
	curl_easy_setopt( c, CURLOPT_URL, url.c_str() );
	curl_easy_setopt( c, CURLOPT_FOLLOWLOCATION, 1L );
	curl_easy_setopt( c, CURLOPT_MAXREDIRS, 8L );
	curl_easy_setopt( c, CURLOPT_TIMEOUT, (long)timeoutSeconds );
	curl_easy_setopt( c, CURLOPT_NOSIGNAL, 1L );
	curl_easy_setopt( c, CURLOPT_USERAGENT, "TextPlus/2" );
	curl_easy_setopt( c, CURLOPT_WRITEFUNCTION, onData );
	curl_easy_setopt( c, CURLOPT_WRITEDATA, &body );
	const CURLcode rc = curl_easy_perform( c );
	long status       = 0;
	char* type        = nullptr;
	curl_easy_getinfo( c, CURLINFO_RESPONSE_CODE, &status );
	curl_easy_getinfo( c, CURLINFO_CONTENT_TYPE, &type );
	contentType = type ? type : "";
	curl_easy_cleanup( c );
	if( rc != CURLE_OK )
	{
		error = curl_easy_strerror( rc );
		return false;
	}
	if( status >= 400 )
	{
		error = "HTTP " + std::to_string( status );
		return false;
	}
	return true;
}
}//namespace textplus

#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace textplus
{
bool httpGet( const std::string& urlIn, int timeoutSeconds, std::string& body, std::string& contentType, std::string& error )
{
	std::string url = urlIn;
	for( int redirect = 0; redirect < 6; ++redirect )
	{
		if( url.compare( 0, 7, "http://" ) != 0 )
		{
			error = "only http:// without libcurl";
			return false;
		}
		const size_t slash      = url.find( '/', 7 );
		std::string hostport    = url.substr( 7, slash == std::string::npos ? std::string::npos : slash - 7 );
		const std::string path  = slash == std::string::npos ? "/" : url.substr( slash );
		std::string port        = "80";
		const size_t colon      = hostport.find( ':' );
		std::string host        = hostport;
		if( colon != std::string::npos )
		{
			port = hostport.substr( colon + 1 );
			host = hostport.substr( 0, colon );
		}
		addrinfo hints{}, *res = nullptr;
		hints.ai_socktype = SOCK_STREAM;
		if( getaddrinfo( host.c_str(), port.c_str(), &hints, &res ) != 0 || !res )
		{
			error = "host not found";
			return false;
		}
		const int s = socket( res->ai_family, res->ai_socktype, res->ai_protocol );
		timeval tv{ timeoutSeconds, 0 };
		setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );
		setsockopt( s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );
		const bool connected = s >= 0 && connect( s, res->ai_addr, res->ai_addrlen ) == 0;
		freeaddrinfo( res );
		if( !connected )
		{
			if( s >= 0 )
				close( s );
			error = "cannot connect";
			return false;
		}
		const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + hostport + "\r\nUser-Agent: TextPlus/2\r\nConnection: close\r\n\r\n";
		send( s, req.data(), req.size(), 0 );
		std::string resp;
		char buf[ 8192 ];
		for( ;; )
		{
			const ssize_t n = recv( s, buf, sizeof( buf ), 0 );
			if( n <= 0 )
				break;
			resp.append( buf, (size_t)n );
		}
		close( s );
		const size_t headEnd = resp.find( "\r\n\r\n" );
		if( headEnd == std::string::npos )
		{
			error = "bad response";
			return false;
		}
		const std::string head = resp.substr( 0, headEnd );
		const int status       = head.size() > 12 ? std::atoi( head.c_str() + 9 ) : 0;
		auto header            = [ & ]( const char* name ) -> std::string {
			const std::string h = std::string( "\r\n" ) + name + ":";
			for( size_t i = 0; i + h.size() <= head.size(); ++i )
				if( strncasecmp( head.c_str() + i, h.c_str(), h.size() ) == 0 )
				{
					size_t a = i + h.size();
					while( a < head.size() && head[ a ] == ' ' )
						++a;
					const size_t e = head.find( "\r\n", a );
					return head.substr( a, e == std::string::npos ? std::string::npos : e - a );
				}
			return {};
		};
		if( status >= 300 && status < 400 )
		{
			url = header( "Location" );
			continue;
		}
		if( status >= 400 || status == 0 )
		{
			error = "HTTP " + std::to_string( status );
			return false;
		}
		contentType = header( "Content-Type" );
		body        = resp.substr( headEnd + 4 );
		return true;
	}
	error = "too many redirects";
	return false;
}
}//namespace textplus
#endif
