#include "Clock.h"
#include "Placeholders.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

namespace textplus
{
namespace
{
std::string two( long v )
{
	char b[ 32 ];
	std::snprintf( b, sizeof( b ), "%02ld", v );
	return b;
}
}//namespace

std::string formatDuration( double seconds, TimeFormat format, bool roundUp )
{
	const bool negative = seconds < 0.0;
	double s            = std::fabs( seconds );
	std::string sign    = negative ? "-" : "";
	if( format == TimeFormat::MMSSTenths )
	{
		long tenths = (long)( roundUp && !negative ? std::ceil( s * 10.0 - 1e-6 ) : std::floor( s * 10.0 + 1e-6 ) );
		long total  = tenths / 10;
		char b[ 64 ];
		std::snprintf( b, sizeof( b ), "%s%02ld:%02ld.%ld", sign.c_str(), total / 60, total % 60, tenths % 10 );
		return b;
	}
	long total = (long)( roundUp && !negative ? std::ceil( s - 1e-6 ) : std::floor( s + 1e-6 ) );
	if( total == 0 )
		sign.clear();
	const long h = total / 3600, m = ( total / 60 ) % 60, sec = total % 60;
	switch( format )
	{
	case TimeFormat::Auto:
		if( h >= 24 )// days: "3d 04:12:09"
			return sign + std::to_string( h / 24 ) + "d " + two( h % 24 ) + ":" + two( m ) + ":" + two( sec );
		return h > 0 ? sign + std::to_string( h ) + ":" + two( m ) + ":" + two( sec ) : sign + std::to_string( m ) + ":" + two( sec );
	case TimeFormat::Days: {
		const long days = (long)( roundUp && !negative ? std::ceil( s / 86400.0 - 1e-9 ) : std::floor( s / 86400.0 + 1e-9 ) );
		return ( days == 0 ? std::string() : sign ) + std::to_string( days );
	}
	case TimeFormat::HHMMSS:
		return sign + two( h ) + ":" + two( m ) + ":" + two( sec );
	case TimeFormat::HHMM: {
		// Minutes round the same way as the seconds would
		long mins = (long)( roundUp && !negative ? std::ceil( s / 60.0 - 1e-9 ) : std::floor( s / 60.0 + 1e-9 ) );
		return ( mins == 0 ? std::string() : sign ) + two( mins / 60 ) + ":" + two( mins % 60 );
	}
	case TimeFormat::MMSS:
		return sign + two( total / 60 ) + ":" + two( sec );
	case TimeFormat::Seconds:
		return sign + std::to_string( total );
	default:
		break;
	}
	return sign + std::to_string( total );
}

std::string formatTimeOfDay( double secondsOfDay, TimeFormat format )
{
	long total = (long)std::floor( secondsOfDay + 1e-6 );
	total      = ( ( total % 86400 ) + 86400 ) % 86400;
	const long h = total / 3600, m = ( total / 60 ) % 60, sec = total % 60;
	switch( format )
	{
	case TimeFormat::Auto:
	case TimeFormat::HHMM:
		return two( h ) + ":" + two( m );
	case TimeFormat::HHMMSS:
		return two( h ) + ":" + two( m ) + ":" + two( sec );
	case TimeFormat::MMSS:
		return two( m ) + ":" + two( sec );
	case TimeFormat::MMSSTenths: {
		const long tenths = (long)std::floor( ( secondsOfDay - std::floor( secondsOfDay ) ) * 10.0 );
		return two( m ) + ":" + two( sec ) + "." + std::to_string( tenths );
	}
	case TimeFormat::Seconds:
		return std::to_string( total );
	case TimeFormat::Days:
		break;
	}
	return two( h ) + ":" + two( m );
}

std::string formatNumber( double value, int decimals, NumberStyle style )
{
	decimals = decimals < 0 ? 0 : decimals > 6 ? 6 : decimals;
	char buf[ 64 ];
	std::snprintf( buf, sizeof( buf ), "%.*f", decimals, std::fabs( value ) );
	std::string digits = buf, frac;
	const size_t dot   = digits.find( '.' );
	if( dot != std::string::npos )
	{
		frac   = digits.substr( dot + 1 );
		digits = digits.substr( 0, dot );
	}
	const char* group   = "";
	const char* decimal = ".";
	switch( style )
	{
	case NumberStyle::Plain: group = ""; decimal = "."; break;
	case NumberStyle::Comma: group = ","; decimal = "."; break;
	case NumberStyle::Dot: group = "."; decimal = ","; break;
	case NumberStyle::Space: group = "\xE2\x80\xAF"; decimal = ","; break;// narrow no-break space
	case NumberStyle::Apostrophe: group = "'"; decimal = "."; break;
	}
	std::string grouped;
	const int n = (int)digits.size();
	for( int i = 0; i < n; ++i )
	{
		grouped.push_back( digits[ i ] );
		const int rest = n - 1 - i;
		if( rest > 0 && rest % 3 == 0 )
			grouped += group;
	}
	bool zero = true;
	for( char c : digits + frac )
		zero = zero && ( c == '0' );
	std::string out = ( value < 0 && !zero ? "-" : "" ) + grouped;
	if( !frac.empty() )
		out += decimal + frac;
	return out;
}

namespace
{
// Splits "1:30:05" into numbers; false on anything else.
bool splitColon( const std::string& text, std::vector< double >& parts )
{
	parts.clear();
	std::string cur;
	bool any = false;
	for( char c : text + ":" )
	{
		if( c == ':' )
		{
			if( cur.empty() )
				return false;
			parts.push_back( std::atof( cur.c_str() ) );
			cur.clear();
			any = true;
		}
		else if( std::isdigit( (unsigned char)c ) || c == '.' || c == ',' )
			cur.push_back( c == ',' ? '.' : c );
		else if( !std::isspace( (unsigned char)c ) )
			return false;
	}
	return any && parts.size() <= 3;
}
}//namespace

bool parseTimeOfDay( const std::string& text, double& secondsOfDay )
{
	std::vector< double > p;
	if( !splitColon( text, p ) || p.size() < 2 )
	{
		// "20" or "20h"
		std::string t;
		for( char c : text )
			if( !std::isspace( (unsigned char)c ) )
				t.push_back( (char)std::tolower( (unsigned char)c ) );
		if( !t.empty() && t.back() == 'h' )
			t.pop_back();
		if( t.empty() || t.find_first_not_of( "0123456789" ) != std::string::npos )
			return false;
		const double h = std::atof( t.c_str() );
		if( h < 0 || h > 24 )
			return false;
		secondsOfDay = h * 3600.0;
		return true;
	}
	const double h = p[ 0 ], m = p[ 1 ], s = p.size() > 2 ? p[ 2 ] : 0.0;
	if( h < 0 || h > 24 || m < 0 || m >= 60 || s < 0 || s >= 60 )
		return false;
	secondsOfDay = h * 3600.0 + m * 60.0 + s;
	return true;
}

bool parseDuration( const std::string& text, double& seconds )
{
	std::vector< double > p;
	if( splitColon( text, p ) )
	{
		seconds = 0;
		for( double v : p )
			seconds = seconds * 60.0 + v;
		return true;
	}
	// Units: 1h30m, 90s, 5m, 2.5
	double total = 0, num = 0;
	bool haveNum = false, any = false;
	std::string cur;
	for( size_t i = 0; i <= text.size(); ++i )
	{
		const char c = i < text.size() ? (char)std::tolower( (unsigned char)text[ i ] ) : '\0';
		if( std::isdigit( (unsigned char)c ) || c == '.' || c == ',' )
		{
			cur.push_back( c == ',' ? '.' : c );
			continue;
		}
		if( !cur.empty() )
		{
			num     = std::atof( cur.c_str() );
			haveNum = true;
			cur.clear();
		}
		if( c == 'h' || c == 'm' || c == 's' )
		{
			if( !haveNum )
				return false;
			total += num * ( c == 'h' ? 3600.0 : c == 'm' ? 60.0 : 1.0 );
			haveNum = false;
			any     = true;
		}
		else if( c == '\0' )
		{
			if( haveNum )
			{
				total += num;// bare number = seconds
				any = true;
			}
		}
		else if( !std::isspace( (unsigned char)c ) && c != 'i' && c != 'n' && c != 'e' && c != 'c' )// "min", "sec"
			return false;
	}
	if( !any )
		return false;
	seconds = total;
	return true;
}

LocalTime localNow()
{
	using namespace std::chrono;
	const auto now     = system_clock::now();
	const std::time_t t = system_clock::to_time_t( now );
	std::tm tm{};
#if defined( _WIN32 )
	localtime_s( &tm, &t );
#else
	localtime_r( &t, &tm );
#endif
	const double frac = duration< double >( now.time_since_epoch() ).count() - std::floor( duration< double >( now.time_since_epoch() ).count() );
	LocalTime lt;
	lt.secondsOfDay = tm.tm_hour * 3600.0 + tm.tm_min * 60.0 + tm.tm_sec + frac;
	lt.year         = tm.tm_year + 1900;
	lt.month        = tm.tm_mon + 1;
	lt.day          = tm.tm_mday;
	lt.weekday      = tm.tm_wday;
	lt.epoch        = duration< double >( now.time_since_epoch() ).count();
	return lt;
}

bool parseDateTime( const std::string& text, const LocalTime& now, double& epoch )
{
	// Numbers and the separators between them
	std::vector< long > num;
	std::vector< char > sep;
	std::string cur;
	for( size_t i = 0; i <= text.size(); ++i )
	{
		const char c = i < text.size() ? text[ i ] : ' ';
		if( std::isdigit( (unsigned char)c ) )
		{
			cur.push_back( c );
			continue;
		}
		if( !cur.empty() )
		{
			num.push_back( std::atol( cur.c_str() ) );
			cur.clear();
			sep.push_back( c );// the character right after the number
		}
	}
	if( num.size() < 2 )
		return false;
	int year = now.year, month = 0, day = 0;
	size_t timeAt = 0;
	if( sep[ 0 ] == '-' && num.size() >= 3 )// 2026-12-24
	{
		year   = (int)num[ 0 ];
		month  = (int)num[ 1 ];
		day    = (int)num[ 2 ];
		timeAt = 3;
	}
	else if( sep[ 0 ] == '.' && num.size() >= 2 )// 24.12.2026 or 24.12.
	{
		day   = (int)num[ 0 ];
		month = (int)num[ 1 ];
		if( num.size() >= 3 && sep[ 1 ] == '.' && num[ 2 ] >= 1000 )
		{
			year   = (int)num[ 2 ];
			timeAt = 3;
		}
		else
		{
			year   = -1;// this year or the next
			timeAt = 2;
		}
	}
	else
		return false;
	if( month < 1 || month > 12 || day < 1 || day > 31 )
		return false;
	long hh = 0, mm = 0, ss = 0;
	if( num.size() > timeAt )
		hh = num[ timeAt ];
	if( num.size() > timeAt + 1 )
		mm = num[ timeAt + 1 ];
	if( num.size() > timeAt + 2 )
		ss = num[ timeAt + 2 ];
	if( hh > 24 || mm > 59 || ss > 59 )
		return false;
	auto toEpoch = [ & ]( int y ) {
		std::tm tm{};
		tm.tm_year  = y - 1900;
		tm.tm_mon   = month - 1;
		tm.tm_mday  = day;
		tm.tm_hour  = (int)hh;
		tm.tm_min   = (int)mm;
		tm.tm_sec   = (int)ss;
		tm.tm_isdst = -1;
		return (double)std::mktime( &tm );
	};
	if( year < 0 )
	{
		epoch = toEpoch( now.year );
		if( epoch < now.epoch - 12.0 * 3600.0 )
			epoch = toEpoch( now.year + 1 );
	}
	else
		epoch = toEpoch( year );
	return true;
}

double secondsUntil( double target, const LocalTime& now )
{
	double d = target - now.secondsOfDay;
	if( d < -12.0 * 3600.0 )
		d += 86400.0;// e.g. 01:00 while it is 23:00: tonight, not this morning
	else if( d > 12.0 * 3600.0 )
		d -= 86400.0;// e.g. 23:00 while it is 01:00: last night, so it is over
	return d;
}

std::string expandTimeTemplate( const std::string& tmpl, const std::string& timer, const LocalTime& now )
{
	char date[ 32 ];
	std::snprintf( date, sizeof( date ), "%02d.%02d.%04d", now.day, now.month, now.year );
	const std::string time = formatTimeOfDay( now.secondsOfDay, TimeFormat::HHMM );
	std::string out;
	bool found = false;
	for( size_t i = 0; i < tmpl.size(); )
	{
		if( tmpl.compare( i, 2, "{{" ) == 0 )
		{
			out += "{{";
			i += 2;
			continue;
		}
		if( tmpl.compare( i, 7, "{timer}" ) == 0 )
		{
			out += timer;
			i += 7;
			found = true;
			continue;
		}
		if( tmpl.compare( i, 6, "{time}" ) == 0 )
		{
			out += time;
			i += 6;
			found = true;
			continue;
		}
		if( tmpl.compare( i, 6, "{date}" ) == 0 )
		{
			out += date;
			i += 6;
			found = true;
			continue;
		}
		out.push_back( tmpl[ i ] );
		++i;
	}
	// Without any placeholder the text is the timer alone; other live values
	// ({osc:...}, {slide}) count as placeholders too.
	return found || hasPlaceholders( tmpl ) ? out : timer;
}

}//namespace textplus
