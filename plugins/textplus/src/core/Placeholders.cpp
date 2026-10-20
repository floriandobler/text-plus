#include "Placeholders.h"
#include "Osc.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace textplus
{
namespace
{
const char* kNames[] = { "time", "date", "weekday", "day", "month", "year", "slide", "slides", "bpm", "timer" };

// Name of the placeholder starting at `i` ("{name}" or "{osc:...}"), its end in `end`.
bool placeholderAt( const std::string& t, size_t i, std::string& name, size_t& end )
{
	if( t[ i ] != '{' || i + 1 >= t.size() || t[ i + 1 ] == '{' )
		return false;
	const size_t close = t.find( '}', i + 1 );
	if( close == std::string::npos || close - i > 200 )
		return false;
	name = t.substr( i + 1, close - i - 1 );
	end  = close + 1;
	if( name.compare( 0, 4, "osc:" ) == 0 && name.size() > 4 )
		return true;
	for( const char* n : kNames )
		if( name == n )
			return true;
	return false;
}
}//namespace

bool hasPlaceholders( const std::string& text )
{
	std::string name;
	size_t end = 0;
	for( size_t i = 0; i < text.size(); ++i )
	{
		if( text.compare( i, 2, "{{" ) == 0 )
		{
			++i;
			continue;
		}
		if( placeholderAt( text, i, name, end ) )
			return true;
	}
	return false;
}

std::string expandPlaceholders( const std::string& text, const PlaceholderContext& ctx )
{
	if( text.find( '{' ) == std::string::npos )
		return text;
	static const char* kWeekdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
	std::string out;
	out.reserve( text.size() + 16 );
	std::string name;
	size_t end = 0;
	char buf[ 48 ];
	for( size_t i = 0; i < text.size(); )
	{
		if( text.compare( i, 2, "{{" ) == 0 )
		{
			out += "{{";
			i += 2;
			continue;
		}
		if( !placeholderAt( text, i, name, end ) || name == "timer" )
		{
			out.push_back( text[ i ] );
			++i;
			continue;
		}
		if( name == "time" )
			out += formatTimeOfDay( ctx.now.secondsOfDay, TimeFormat::HHMM );
		else if( name == "date" )
		{
			std::snprintf( buf, sizeof( buf ), "%02d.%02d.%04d", ctx.now.day, ctx.now.month, ctx.now.year );
			out += buf;
		}
		else if( name == "weekday" )
			out += kWeekdays[ ( ( ctx.now.weekday % 7 ) + 7 ) % 7 ];
		else if( name == "day" || name == "month" )
		{
			std::snprintf( buf, sizeof( buf ), "%02d", name == "day" ? ctx.now.day : ctx.now.month );
			out += buf;
		}
		else if( name == "year" )
			out += std::to_string( ctx.now.year );
		else if( name == "slide" )
			out += std::to_string( ctx.slide );
		else if( name == "slides" )
			out += std::to_string( ctx.slides );
		else if( name == "bpm" )
		{
			std::snprintf( buf, sizeof( buf ), "%.0f", ctx.bpm );
			out += buf;
		}
		else// osc:
		{
			std::string v;
			if( ctx.osc && ctx.osc->value( name.substr( 4 ), v ) )
				out += v;
		}
		i = end;
	}
	return out;
}

}//namespace textplus
