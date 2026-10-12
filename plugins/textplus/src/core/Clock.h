#pragma once
// Clock & timer text: time formats, number formats and the template that
// places them into the text ("Doors open in {timer}").

#include <cstdint>
#include <string>

namespace textplus
{

enum class TimeFormat
{
	Auto,        // clock: HH:MM; durations: H:MM:SS, without hours below one hour (M:SS)
	HHMMSS,      // 09:05:03
	HHMM,        // 09:05
	MMSS,        // 125:03 (minutes are not limited to 59)
	MMSSTenths,  // 05:03.4
	Seconds      // 303
};

enum class NumberStyle
{
	Plain,       // 12840.5
	Comma,       // 12,840.5
	Dot,         // 12.840,5
	Space,       // 12 840,5
	Apostrophe   // 12'840.5
};

// A duration in seconds. Countdowns round up (the display reaches 0 exactly when
// the time is over), stopwatches round down. Negative durations get a minus sign.
std::string formatDuration( double seconds, TimeFormat format, bool roundUp );
// A time of day (seconds since midnight, local time).
std::string formatTimeOfDay( double secondsOfDay, TimeFormat format );
std::string formatNumber( double value, int decimals, NumberStyle style );

// "20:00", "20:00:30", "8:15" -> seconds since midnight; false if unreadable.
bool parseTimeOfDay( const std::string& text, double& secondsOfDay );
// "5:00", "1:30:00", "90", "90s", "5m", "1h30m", "2.5" -> seconds; false if unreadable.
bool parseDuration( const std::string& text, double& seconds );

// Local wall clock: seconds since midnight and the date.
struct LocalTime
{
	double secondsOfDay = 0;
	int year = 2000, month = 1, day = 1;
};
LocalTime localNow();
// Seconds from now until the next `targetSecondsOfDay` (negative if it was up to
// 12 hours ago, so a passed start time counts as "over", not as "tomorrow").
double secondsUntil( double targetSecondsOfDay, const LocalTime& now );

// Replaces {timer}, {time} and {date} in `tmpl`. Without any placeholder the
// result is the timer alone. "{{" is a literal brace (handled by the markup).
std::string expandTimeTemplate( const std::string& tmpl, const std::string& timer, const LocalTime& now );

}//namespace textplus
