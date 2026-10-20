#include "Osc.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#if defined( _WIN32 )
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET SocketHandle;
static const SocketHandle kNoSocket = INVALID_SOCKET;
static void closeSocket( SocketHandle s ) { closesocket( s ); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SocketHandle;
static const SocketHandle kNoSocket = -1;
static void closeSocket( SocketHandle s ) { close( s ); }
#endif

namespace textplus
{
namespace
{
std::mutex g_registryMutex;
std::map< int, std::weak_ptr< OscReceiver > > g_registry;

uint32_t be32( const uint8_t* p ) { return ( (uint32_t)p[ 0 ] << 24 ) | ( (uint32_t)p[ 1 ] << 16 ) | ( (uint32_t)p[ 2 ] << 8 ) | p[ 3 ]; }
uint64_t be64( const uint8_t* p ) { return ( (uint64_t)be32( p ) << 32 ) | be32( p + 4 ); }

// OSC string at `pos`: null-terminated, padded to a multiple of 4. False if malformed.
bool readString( const uint8_t* d, size_t size, size_t& pos, std::string& out )
{
	size_t end = pos;
	while( end < size && d[ end ] != 0 )
		++end;
	if( end >= size )
		return false;
	out.assign( (const char*)d + pos, end - pos );
	pos = ( end + 4 ) & ~(size_t)3;
	return pos <= size;
}

std::string number( double v, bool integer )
{
	char buf[ 64 ];
	if( integer )
		std::snprintf( buf, sizeof( buf ), "%lld", (long long)v );
	else
		std::snprintf( buf, sizeof( buf ), "%.6g", v );
	return buf;
}

void parseMessage( const uint8_t* d, size_t size, std::vector< std::pair< std::string, std::string > >& out )
{
	size_t pos = 0;
	std::string address, tags;
	if( !readString( d, size, pos, address ) || address.empty() || address[ 0 ] != '/' )
		return;
	std::string value;
	if( pos < size && d[ pos ] == ',' )
	{
		if( !readString( d, size, pos, tags ) )
			return;// broken type tags
		for( size_t t = 1; t < tags.size(); ++t )
		{
			std::string item;
			bool have = true;
			switch( tags[ t ] )
			{
			case 'i':
			case 'c':
				if( pos + 4 > size )
					return;
				item = tags[ t ] == 'c' ? std::string( 1, (char)be32( d + pos ) ) : number( (int32_t)be32( d + pos ), true );
				pos += 4;
				break;
			case 'f': {
				if( pos + 4 > size )
					return;
				uint32_t u = be32( d + pos );
				float f;
				std::memcpy( &f, &u, 4 );
				item = number( f, false );
				pos += 4;
				break;
			}
			case 'h':
				if( pos + 8 > size )
					return;
				item = number( (double)(int64_t)be64( d + pos ), true );
				pos += 8;
				break;
			case 'd': {
				if( pos + 8 > size )
					return;
				uint64_t u = be64( d + pos );
				double v;
				std::memcpy( &v, &u, 8 );
				item = number( v, false );
				pos += 8;
				break;
			}
			case 's':
			case 'S':
				if( !readString( d, size, pos, item ) )
					return;
				break;
			case 'b': {
				if( pos + 4 > size )
					return;
				const uint32_t n = be32( d + pos );
				pos              = ( pos + 4 + n + 3 ) & ~(size_t)3;
				have             = false;
				if( pos > size )
					return;
				break;
			}
			case 't':
				pos += 8;
				have = false;
				break;
			case 'r':
			case 'm':
				pos += 4;
				have = false;
				break;
			case 'T': item = "1"; break;
			case 'F': item = "0"; break;
			case 'N':
			case 'I':
				have = false;
				break;
			default:
				return;// unknown type: the rest cannot be read
			}
			if( pos > size )
				return;
			if( have )
				value += ( value.empty() ? "" : " " ) + item;
		}
	}
	out.emplace_back( address, value );
}
}//namespace

void OscReceiver::parsePacket( const uint8_t* d, size_t size, std::vector< std::pair< std::string, std::string > >& out )
{
	if( size >= 16 && std::memcmp( d, "#bundle", 8 ) == 0 )
	{
		size_t pos = 16;// "#bundle\0" + time tag
		while( pos + 4 <= size )
		{
			const uint32_t n = be32( d + pos );
			pos += 4;
			if( n > size - pos )
				return;
			parsePacket( d + pos, n, out );
			pos += n;
		}
		return;
	}
	parseMessage( d, size, out );
}

int parseOscPort( const std::string& text )
{
	int port   = 0;
	bool digit = false;
	for( char c : text )
	{
		if( c >= '0' && c <= '9' )
		{
			port  = port * 10 + ( c - '0' );
			digit = true;
			if( port > 65535 )
				return 0;
		}
		else if( c != ' ' && c != '\t' )
			return 0;
	}
	return digit ? port : 0;
}

std::shared_ptr< OscReceiver > OscReceiver::forPort( int port )
{
	if( port <= 0 || port > 65535 )
		return nullptr;
	std::lock_guard< std::mutex > lock( g_registryMutex );
	auto it = g_registry.find( port );
	if( it != g_registry.end() )
		if( auto existing = it->second.lock() )
			return existing;
	std::shared_ptr< OscReceiver > r( new OscReceiver( port ) );
	g_registry[ port ] = r;
	return r;
}

OscReceiver::OscReceiver( int port ) :
	portNumber( port )
{
	worker = std::thread( [ this ] { run(); } );
}

OscReceiver::~OscReceiver()
{
	quit = true;
	if( worker.joinable() )
		worker.join();
}

bool OscReceiver::value( const std::string& address, std::string& out ) const
{
	std::lock_guard< std::mutex > lock( mutex );
	auto it = values.find( address );
	if( it == values.end() )
		return false;
	out = it->second;
	return true;
}

void OscReceiver::run()
{
#if defined( _WIN32 )
	WSADATA wsa;
	if( WSAStartup( MAKEWORD( 2, 2 ), &wsa ) != 0 )
		return;
#endif
	std::vector< uint8_t > buf( 65536 );
	std::vector< std::pair< std::string, std::string > > parsed;
	while( !quit )
	{
		// (Re)open the socket; a busy port is retried every two seconds.
		SocketHandle sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
		bool ok           = sock != kNoSocket;
		if( ok )
		{
			sockaddr_in addr{};
			addr.sin_family      = AF_INET;
			addr.sin_port        = htons( (uint16_t)portNumber );
			addr.sin_addr.s_addr = htonl( INADDR_ANY );
			ok                   = bind( sock, (const sockaddr*)&addr, sizeof( addr ) ) == 0;
		}
		socketOk = ok;
		if( !ok )
		{
			if( sock != kNoSocket )
				closeSocket( sock );
			for( int i = 0; i < 20 && !quit; ++i )
				std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
			continue;
		}
		while( !quit )
		{
			fd_set set;
			FD_ZERO( &set );
			FD_SET( sock, &set );
			timeval tv{ 0, 200000 };
			const int ready = select( (int)sock + 1, &set, nullptr, nullptr, &tv );
			if( ready < 0 )
				break;
			if( ready == 0 )
				continue;
			const int n = (int)recvfrom( sock, (char*)buf.data(), (int)buf.size(), 0, nullptr, nullptr );
			if( n <= 0 )
				continue;
			parsed.clear();
			parsePacket( buf.data(), (size_t)n, parsed );
			if( parsed.empty() )
				continue;
			{
				std::lock_guard< std::mutex > lock( mutex );
				for( auto& kv : parsed )
					values[ kv.first ] = kv.second;
			}
			++messages;
		}
		closeSocket( sock );
		socketOk = false;
	}
#if defined( _WIN32 )
	WSACleanup();
#endif
}

}//namespace textplus
