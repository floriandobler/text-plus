// Windows: WinHTTP (system TLS, proxies, redirects).
#include "WebFetch.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

namespace textplus
{
static std::wstring widen( const std::string& s )
{
	if( s.empty() )
		return {};
	const int n = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0 );
	std::wstring w( (size_t)n, L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), &w[ 0 ], n );
	return w;
}
static std::string narrow( const std::wstring& w )
{
	if( w.empty() )
		return {};
	const int n = WideCharToMultiByte( CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr );
	std::string s( (size_t)n, '\0' );
	WideCharToMultiByte( CP_UTF8, 0, w.c_str(), (int)w.size(), &s[ 0 ], n, nullptr, nullptr );
	return s;
}

bool httpGet( const std::string& url, int timeoutSeconds, std::string& body, std::string& contentType, std::string& error )
{
	const std::wstring wurl = widen( url );
	URL_COMPONENTS uc{};
	uc.dwStructSize = sizeof( uc );
	wchar_t host[ 256 ], path[ 4096 ];
	uc.lpszHostName     = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath      = path;
	uc.dwUrlPathLength  = 4096;
	wchar_t extra[ 4096 ];
	uc.lpszExtraInfo     = extra;
	uc.dwExtraInfoLength = 4096;
	if( !WinHttpCrackUrl( wurl.c_str(), 0, 0, &uc ) )
	{
		error = "bad URL";
		return false;
	}
	HINTERNET session = WinHttpOpen( L"TextPlus/2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
	if( !session )
		session = WinHttpOpen( L"TextPlus/2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
	if( !session )
	{
		error = "no network session";
		return false;
	}
	const int ms = timeoutSeconds * 1000;
	WinHttpSetTimeouts( session, ms, ms, ms, ms );
	bool ok           = false;
	HINTERNET connect = WinHttpConnect( session, std::wstring( host, uc.dwHostNameLength ).c_str(), uc.nPort, 0 );
	HINTERNET request = nullptr;
	if( connect )
	{
		const std::wstring object = std::wstring( path, uc.dwUrlPathLength ) + std::wstring( extra, uc.dwExtraInfoLength );
		request = WinHttpOpenRequest( connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
									  uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0 );
	}
	if( request && WinHttpSendRequest( request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) && WinHttpReceiveResponse( request, nullptr ) )
	{
		DWORD status = 0, size = sizeof( status );
		WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX );
		wchar_t type[ 256 ];
		DWORD typeSize = sizeof( type );
		if( WinHttpQueryHeaders( request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, type, &typeSize, WINHTTP_NO_HEADER_INDEX ) )
			contentType = narrow( std::wstring( type, typeSize / sizeof( wchar_t ) ) );
		body.clear();
		for( ;; )
		{
			DWORD avail = 0;
			if( !WinHttpQueryDataAvailable( request, &avail ) || avail == 0 )
				break;
			std::vector< char > buf( avail );
			DWORD read = 0;
			if( !WinHttpReadData( request, buf.data(), avail, &read ) || read == 0 )
				break;
			body.append( buf.data(), read );
		}
		if( status >= 400 )
			error = "HTTP " + std::to_string( status );
		else
			ok = true;
	}
	else
		error = "request failed (" + std::to_string( GetLastError() ) + ")";
	if( request )
		WinHttpCloseHandle( request );
	if( connect )
		WinHttpCloseHandle( connect );
	WinHttpCloseHandle( session );
	return ok;
}
}//namespace textplus
