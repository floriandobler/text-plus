#pragma once
// Live data in the text:
//
//  - JSON: a small DOM parser with path lookup ("main.temp", "items[0].title").
//  - Placeholders: {bpm} {beat} {bar} {slide} {slides} {time} {date} {weekday},
//    OSC variables {/score/home} and JSON fields {main.temp}. Unknown
//    placeholders stay as they are, so typos are visible. "{{" stays a literal
//    brace for the markup; colour tags ({#ff8800} ... {/}) are left alone.
//  - Shared variables: one store for the whole process, written by the OSC
//    receiver, read by every Text+ instance.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace textplus
{

class Json
{
public:
	enum class Type
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object
	};

	// Parses a JSON document; on error the result is Null and `error` says why.
	static Json parse( const std::string& text, std::string* error = nullptr );

	Type type() const { return t; }
	bool isNull() const { return t == Type::Null; }
	// "a.b[2].c" or "a.b.2.c"; nullptr if the path does not exist
	const Json* find( const std::string& path ) const;
	// Text for a value: strings as they are, numbers without trailing zeros, arrays joined by ", "
	std::string text() const;
	// Top-level fields as "key: value" lines (shows a user what a document contains)
	std::string outline( int maxLines = 12 ) const;

	const std::vector< Json >& items() const { return arr; }
	const std::vector< std::pair< std::string, Json > >& members() const { return obj; }

private:
	Type t = Type::Null;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector< Json > arr;
	std::vector< std::pair< std::string, Json > > obj;
	friend struct JsonParser;
};

// What the placeholders can refer to at this moment.
struct PlaceholderContext
{
	double bpm      = 0;// 0 = unknown
	double beats    = 0;// beats since the clip started
	int slide       = 1;
	int slides      = 1;
	const Json* json = nullptr;// web source document
};

// Replaces the placeholders in `text`. `found` (optional) is set when at least one was replaced.
std::string expandPlaceholders( const std::string& text, const PlaceholderContext& ctx, bool* found = nullptr );

// ---------------------------------------------------------------------------
// Process-wide variables (OSC addresses and their latest values)
// ---------------------------------------------------------------------------
class Variables
{
public:
	static Variables& instance();
	void set( const std::string& name, const std::string& value );
	bool get( const std::string& name, std::string& value ) const;
	// Changes whenever a value changes (cheap check for "anything new?")
	uint64_t generation() const;
	std::vector< std::pair< std::string, std::string > > snapshot() const;

private:
	mutable std::mutex mutex;
	std::map< std::string, std::string > values;
	uint64_t gen = 0;
};

}//namespace textplus
