#pragma once
// OSC receiver for live data in the text: "{osc:/score/home}" shows the last
// value that arrived at that address. One UDP socket per port, shared by all
// instances that use the port; it closes when the last one lets go.
//
// Values: the arguments of the last message, joined by spaces. Floats are
// printed with up to six significant digits, integers as integers, strings
// and symbols as they are, True/False as 1/0. Bundles are unpacked.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

namespace textplus
{

class OscReceiver
{
public:
	// The receiver for `port`, opened on first use. nullptr if the port is invalid.
	static std::shared_ptr< OscReceiver > forPort( int port );
	~OscReceiver();

	// Last value at `address`; false if none has arrived yet.
	bool value( const std::string& address, std::string& out ) const;
	// Increments with every message: callers can skip work when it is unchanged.
	uint64_t version() const { return messages.load(); }
	bool listening() const { return socketOk.load(); }
	int port() const { return portNumber; }

	// Parses one OSC packet (message or bundle) into address/value pairs. Exposed for tests.
	static void parsePacket( const uint8_t* data, size_t size, std::vector< std::pair< std::string, std::string > >& out );

private:
	explicit OscReceiver( int port );
	void run();

	int portNumber;
	std::thread worker;
	std::atomic< bool > quit{ false };
	std::atomic< bool > socketOk{ false };
	std::atomic< uint64_t > messages{ 0 };
	mutable std::mutex mutex;
	std::map< std::string, std::string > values;
};

// Text port parameter ("7001", " 7001 ") -> port; 0 if empty or invalid.
int parseOscPort( const std::string& text );

}//namespace textplus
