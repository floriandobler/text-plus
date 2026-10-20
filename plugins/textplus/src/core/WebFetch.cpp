#include "WebFetch.h"

#include "Clock.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace textplus
{

struct WebFetcher::Impl
{
	struct Entry
	{
		std::string url;
		double refresh     = 30;
		double lastPolled  = 0;
		double lastFetched = -1e9;
		bool busy          = false;
		Result result;
	};
	std::mutex mutex;
	std::condition_variable wake;
	std::map< std::string, Entry > entries;
	std::thread worker;
	bool running = false;
	bool quit    = false;
	int64_t nextStamp = 1;

	static double now()
	{
		return std::chrono::duration< double >( std::chrono::steady_clock::now().time_since_epoch() ).count();
	}

	void run()
	{
		std::unique_lock< std::mutex > lock( mutex );
		while( !quit )
		{
			const double t = now();
			// forget URLs nobody looked at for a minute
			for( auto it = entries.begin(); it != entries.end(); )
				it = t - it->second.lastPolled > 60.0 && !it->second.busy ? entries.erase( it ) : std::next( it );
			Entry* due = nullptr;
			for( auto& kv : entries )
				if( !kv.second.busy && t - kv.second.lastFetched >= kv.second.refresh && ( !due || kv.second.lastFetched < due->lastFetched ) )
					due = &kv.second;
			if( entries.empty() )
			{
				running = false;// nothing to do: end; the next poll starts a new thread
				return;
			}
			if( !due )
			{
				wake.wait_for( lock, std::chrono::milliseconds( 250 ) );
				continue;
			}
			due->busy              = true;
			due->lastFetched       = t;
			const std::string url  = due->url;
			if( due->result.stamp < 0 )
				due->result.status = "loading...";
			lock.unlock();
			std::string body, type, error;
			const bool ok = httpGet( url, 15, body, type, error );
			lock.lock();
			auto it = entries.find( url );
			if( it == entries.end() )
				continue;
			Entry& e = it->second;
			e.busy   = false;
			const LocalTime lt = localNow();
			const std::string clock = formatTimeOfDay( lt.secondsOfDay, TimeFormat::HHMMSS );
			if( ok )
			{
				if( body != e.result.body || e.result.stamp < 0 )
				{
					e.result.body        = body;
					e.result.contentType = type;
					e.result.stamp       = nextStamp++;
				}
				e.result.status = "ok " + clock;
			}
			else
				e.result.status = "error: " + error + " (" + clock + ")";// the last good content stays
		}
	}
};

WebFetcher::WebFetcher() :
	impl( new Impl() )
{
}

WebFetcher::~WebFetcher()
{
	{
		std::lock_guard< std::mutex > lock( impl->mutex );
		impl->quit = true;
	}
	impl->wake.notify_all();
	// Never join while the plugin is being unloaded (loader lock on Windows):
	// the thread wakes up within 250 ms and ends on its own.
	if( impl->worker.joinable() )
		impl->worker.detach();
	// impl is leaked on purpose: the detached thread may still touch it.
}

WebFetcher& WebFetcher::instance()
{
	static WebFetcher f;
	return f;
}

WebFetcher::Result WebFetcher::poll( const std::string& url, double refreshSeconds )
{
	if( url.empty() )
		return Result();
	std::lock_guard< std::mutex > lock( impl->mutex );
	Impl::Entry& e = impl->entries[ url ];
	const bool fresh = e.url.empty();
	e.url        = url;
	e.refresh    = std::max( 1.0, refreshSeconds );
	e.lastPolled = Impl::now();
	if( !impl->running )
	{
		if( impl->worker.joinable() )
			impl->worker.join();// the previous worker has ended (it cleared `running` on its way out)
		impl->running = true;
		impl->worker  = std::thread( [ this ] { impl->run(); } );
	}
	if( fresh )
		impl->wake.notify_all();
	return e.result;
}

static std::string lower( std::string s )
{
	std::transform( s.begin(), s.end(), s.begin(), []( unsigned char c ) { return (char)std::tolower( c ); } );
	return s;
}

WebFormat detectWebFormat( const std::string& url, const std::string& contentType, const std::string& body )
{
	const std::string ct = lower( contentType ), u = lower( url );
	if( ct.find( "json" ) != std::string::npos )
		return WebFormat::Json;
	if( ct.find( "csv" ) != std::string::npos || u.find( "output=csv" ) != std::string::npos || u.find( "format=csv" ) != std::string::npos ||
		( u.size() > 4 && u.compare( u.size() - 4, 4, ".csv" ) == 0 ) )
		return WebFormat::Csv;
	size_t i = 0;
	while( i < body.size() && ( body[ i ] == ' ' || body[ i ] == '\n' || body[ i ] == '\r' || body[ i ] == '\t' ) )
		++i;
	if( i < body.size() && ( body[ i ] == '{' || body[ i ] == '[' ) )
		return WebFormat::Json;
	return WebFormat::Text;
}

}//namespace textplus
