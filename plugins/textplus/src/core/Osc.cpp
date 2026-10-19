#include "Osc.h"

#include "LiveData.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined( _WIN32 )
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET SocketHandle;
static const SocketHandle kNoSocket = INVALID_SOCKET;
static void closeSocket( SocketHandle s ) { closesocket( s ); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int SocketHandle;
static const SocketHandle kNoSocket = -1;
static void closeSocket( SocketHandle s ) { close( s ); }
#endif

namespace textplus
{

struct OscReceiver::Port
{
	int number = 0;
	int refs   = 0;
	SocketHandle sock = kNoSocket;
	std::thread thread;
	std::atomic< bool > stop{ false };
	std::atomic< uint64_t > count{ 0 };
	std::string status;
};

namespace
{
uint32_t be32( const uint8_t* p ) { return ( (uint32_t)p[ 0 ] << 24 ) | ( (uint32_t)p[ 1 ] << 16 ) | ( (uint32_t)p[ 2 ] << 8 ) | p[ 3 ]; }
uint64_t be64( const uint8_t* p ) { return ( (uint64_t)be32( p ) << 32 ) | be32( p + 4 ); }

// OSC string: zero terminated, padded to 4 bytes. Returns false if it runs past the end.
bool readString( const uint8_t* data, size_t size, size_t& pos, std::string& out )
{
	const size_t start = pos;
	while( pos < size && data[ pos ] != 0 )
		++pos;
	if( pos >= size )
		return false;
	out.assign( (const char*)data + start, pos - start );
	pos = ( pos + 4 ) & ~(size_t)3;
	return pos <= size;
}

std::string numberText( double v )
{
	char buf[ 64 ];
	if( std::fabs( v - std::round( v ) ) < 1e-9 && std::fabs( v ) < 1e15 )
		std::snprintf( buf, sizeof( buf ), "%.0f", v );
	else
		std::snprintf( buf, sizeof( buf ), "%g", v );
	return buf;
}

void decodeInto( const uint8_t* data, size_t size, std::vector< std::pair< std::string, std::string > >& out, int depth )
{
	if( size < 4 || depth > 8 )
		return;
	if( size >= 16 && std::memcmp( data, "#bundle", 8 ) == 0 )
	{
		size_t pos = 16;// "#bundle\0" + timetag
		while( pos + 4 <= size )
		{
			const uint32_t len = be32( data + pos );
			pos += 4;
			if( len > size - pos )
				return;
			decodeInto( data + pos, len, out, depth + 1 );
			pos += len;
		}
		return;
	}
	size_t pos = 0;
	std::string address, tags;
	if( !readString( data, size, pos, address ) || address.empty() || address[ 0 ] != '/' )
		return;
	if( pos < size && data[ pos ] == ',' )
	{
		if( !readString( data, size, pos, tags ) )
			return;
	}
	std::string value;
	auto add = [ & ]( const std::string& part ) { value += ( value.empty() ? "" : " " ) + part; };
	for( size_t t = 1; t < tags.size(); ++t )
	{
		switch( tags[ t ] )
		{
		case 'i':
			if( pos + 4 > size )
				return;
			add( std::to_string( (int32_t)be32( data + pos ) ) );
			pos += 4;
			break;
		case 'f': {
			if( pos + 4 > size )
				return;
			const uint32_t bits = be32( data + pos );
			float f;
			std::memcpy( &f, &bits, 4 );
			add( numberText( f ) );
			pos += 4;
			break;
		}
		case 'h':
			if( pos + 8 > size )
				return;
			add( std::to_string( (int64_t)be64( data + pos ) ) );
			pos += 8;
			break;
		case 'd': {
			if( pos + 8 > size )
				return;
			const uint64_t bits = be64( data + pos );
			double d;
			std::memcpy( &d, &bits, 8 );
			add( numberText( d ) );
			pos += 8;
			break;
		}
		case 's':
		case 'S': {
			std::string s;
			if( !readString( data, size, pos, s ) )
				return;
			add( s );
			break;
		}
		case 'c':
			if( pos + 4 > size )
				return;
			add( std::string( 1, (char)be32( data + pos ) ) );
			pos += 4;
			break;
		case 'T': add( "1" ); break;
		case 'F': add( "0" ); break;
		case 'N':
		case 'I': break;
		case 'b': {// blob: skipped
			if( pos + 4 > size )
				return;
			const uint32_t len = be32( data + pos );
			pos += 4 + ( ( len + 3 ) & ~3u );
			if( pos > size )
				return;
			break;
		}
		default:
			return;// unknown type: the rest cannot be read
		}
	}
	out.emplace_back( address, value );
}

void portLoop( OscReceiver::Port* p )
{
	std::vector< uint8_t > buf( 65536 );
	while( !p->stop.load() )
	{
		const int n = (int)recv( p->sock, (char*)buf.data(), (int)buf.size(), 0 );
		if( n <= 0 )
			continue;// timeout (checks stop) or error
		for( const auto& kv : OscReceiver::decode( buf.data(), (size_t)n ) )
		{
			Variables::instance().set( kv.first, kv.second );
			p->count.fetch_add( 1 );
		}
	}
}

bool openSocket( OscReceiver::Port& p )
{
#if defined( _WIN32 )
	static bool wsa = [] {
		WSADATA d;
		return WSAStartup( MAKEWORD( 2, 2 ), &d ) == 0;
	}();
	if( !wsa )
	{
		p.status = "network unavailable";
		return false;
	}
#endif
	p.sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if( p.sock == kNoSocket )
	{
		p.status = "no socket";
		return false;
	}
	// A short receive timeout lets the thread notice when it should stop.
#if defined( _WIN32 )
	DWORD timeoutMs = 200;
	setsockopt( p.sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof( timeoutMs ) );
#else
	timeval tv{ 0, 200000 };
	setsockopt( p.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );
#endif
	sockaddr_in addr{};
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons( (uint16_t)p.number );
	addr.sin_addr.s_addr = htonl( INADDR_ANY );
	if( bind( p.sock, (sockaddr*)&addr, sizeof( addr ) ) != 0 )
	{
		closeSocket( p.sock );
		p.sock   = kNoSocket;
		p.status = "port in use";
		return false;
	}
	p.status = "listening";
	return true;
}
}//namespace

std::vector< std::pair< std::string, std::string > > OscReceiver::decode( const uint8_t* data, size_t size )
{
	std::vector< std::pair< std::string, std::string > > out;
	decodeInto( data, size, out, 0 );
	return out;
}

OscReceiver& OscReceiver::instance()
{
	static OscReceiver r;
	return r;
}

void OscReceiver::acquire( int port )
{
	if( port < 1 || port > 65535 )
		return;
	std::lock_guard< std::mutex > lock( mutex );
	for( auto& p : ports )
		if( p->number == port )
		{
			++p->refs;
			return;
		}
	auto p    = std::make_shared< Port >();
	p->number = port;
	p->refs   = 1;
	if( openSocket( *p ) )
		p->thread = std::thread( portLoop, p.get() );
	ports.push_back( p );
}

void OscReceiver::release( int port )
{
	std::shared_ptr< Port > done;
	{
		std::lock_guard< std::mutex > lock( mutex );
		for( size_t i = 0; i < ports.size(); ++i )
			if( ports[ i ]->number == port && --ports[ i ]->refs <= 0 )
			{
				done = ports[ i ];
				ports.erase( ports.begin() + (long)i );
				break;
			}
	}
	if( !done )
		return;
	done->stop = true;
	if( done->thread.joinable() )
		done->thread.join();
	if( done->sock != kNoSocket )
		closeSocket( done->sock );
}

std::string OscReceiver::status( int port ) const
{
	std::lock_guard< std::mutex > lock( mutex );
	for( const auto& p : ports )
		if( p->number == port )
			return p->status;
	return {};
}

uint64_t OscReceiver::messages( int port ) const
{
	std::lock_guard< std::mutex > lock( mutex );
	for( const auto& p : ports )
		if( p->number == port )
			return p->count.load();
	return 0;
}

OscReceiver::~OscReceiver()
{
	// Normally every instance released its port before the plugin is unloaded.
	// If not, do not join here (a DLL unload holds the loader lock on Windows).
	for( auto& p : ports )
	{
		p->stop = true;
		if( p->thread.joinable() )
			p->thread.detach();
	}
}

}//namespace textplus
