#pragma once
// OSC receiver for the whole process. Every message that arrives on a port
// becomes a variable named by its address ("/score/home"), holding its
// arguments as text ("3", "Anna Musterfrau", "0.5 1"). Text+ instances show
// them with placeholders ("{/score/home}"). Bundles are unpacked.
//
// Ports are opened when an instance asks for them and stay open while any
// instance uses them (reference counted), so a second clip on the same port
// shares the socket. Resolume's own OSC input is a different port (7000 by
// default); pick another one, e.g. 7010.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace textplus
{

class OscReceiver
{
public:
	static OscReceiver& instance();

	// Starts listening on `port` (1..65535) for one user; balanced by release().
	void acquire( int port );
	void release( int port );
	// "listening", "port in use", ...; empty if the port is not open
	std::string status( int port ) const;
	// Messages received on the port so far
	uint64_t messages( int port ) const;

	// Decodes one UDP packet into (address, value text) pairs; exposed for tests.
	static std::vector< std::pair< std::string, std::string > > decode( const uint8_t* data, size_t size );

	~OscReceiver();

	struct Port;

private:
	OscReceiver() = default;
	mutable std::mutex mutex;
	std::vector< std::shared_ptr< Port > > ports;
};

}//namespace textplus
