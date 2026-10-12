#pragma once
// ParamPlugin: the parameter plumbing every plugin of this family shares.
//
//  - Definition helpers for sliders with real ranges, options (with stable
//    element values, so menus can be reordered without breaking saved
//    compositions), colours (HSBA), switches, buttons, text and file fields.
//  - `hostKnown`: the value the host last sent or read back. Menus that act on
//    a choice (presets, font menus) react only when this changes, so a host
//    that re-sends unchanged values never re-applies them.
//  - Visibility is only sent to the host when it really changes.
//  - One return buffer per parameter for text and display strings: a host may
//    keep several returned strings at once.

#include <FFGLSDK.h>

#include <cmath>
#include <string>
#include <vector>

namespace ffglkit
{

struct Rgba
{
	float r = 1, g = 1, b = 1, a = 1;// straight (not premultiplied)
};

Rgba hsbaToRgba( float h, float s, float v, float a );

struct OptionItem
{
	const char* label;
	float value;                 // what the host sends and stores
	bool separatorBefore = false;// a line in the menu above this item
};

class ParamPlugin : public CFFGLPlugin
{
public:
	explicit ParamPlugin( unsigned paramCount );

	float GetFloatParameter( unsigned int index ) override;

protected:
	// ---- Definition ----
	void defRange( unsigned id, const std::string& name, const char* group, float def, float lo, float hi );
	void defBool( unsigned id, const std::string& name, const char* group, bool def );
	void defEvent( unsigned id, const std::string& name, const char* group );
	// Items with value = index
	void defOption( unsigned id, const std::string& name, const char* group, const std::vector< const char* >& items, int def );
	// Items with explicit values; `def` is a value
	void defOption( unsigned id, const std::string& name, const char* group, const std::vector< OptionItem >& items, float def );
	// Colour = four parameters hue, saturation, brightness, alpha starting at `hueId`
	void defColor( unsigned hueId, const std::string& name, const char* group, float h, float s, float b, float a );
	void defText( unsigned id, const std::string& name, const char* group, const std::string& def );
	void defFile( unsigned id, const std::string& name, const char* group, const std::vector< std::string >& extensions );

	// ---- Runtime ----
	unsigned paramCount() const { return (unsigned)values.size(); }
	// Changes a value from inside the plugin and tells the host.
	void setValue( unsigned id, float v );
	Rgba colorOf( unsigned hueId ) const;
	// Records the host's value; true if it differs from what the host knew before.
	bool hostChanged( unsigned id, float v );
	float hostKnownValue( unsigned id ) const { return id < hostKnown.size() ? hostKnown[ id ] : std::nanf( "" ); }
	// Button helper: true on the rising edge (value goes above 0.5). Stores the value.
	bool pressed( unsigned id, float v );

	// Visibility: collect with show(), sent to the host only when it changed.
	void show( unsigned id, bool visible );
	bool raiseVisibilityEvents = false;// false while defining (part of the initial info)

	// String return buffers
	char* returnText( unsigned id, const std::string& s );
	char* returnDisplay( unsigned id, const std::string& s );

	std::vector< float > values;
	std::vector< float > hostKnown;

private:
	std::vector< signed char > visibility;// last visibility sent to the host
	std::vector< std::string > textBuffers, displayBuffers;
};

}//namespace ffglkit
