#include "ffglkit/ParamPlugin.h"

#include <algorithm>

namespace ffglkit
{

Rgba hsbaToRgba( float h, float s, float v, float a )
{
	h       = h - std::floor( h );
	float r = v, g = v, b = v;
	if( s > 0 )
	{
		float hh = h * 6.0f;
		int i    = (int)hh % 6;
		float f  = hh - std::floor( hh );
		float p = v * ( 1 - s ), q = v * ( 1 - s * f ), t = v * ( 1 - s * ( 1 - f ) );
		switch( i )
		{
		case 0: r = v; g = t; b = p; break;
		case 1: r = q; g = v; b = p; break;
		case 2: r = p; g = v; b = t; break;
		case 3: r = p; g = q; b = v; break;
		case 4: r = t; g = p; b = v; break;
		default: r = v; g = p; b = q; break;
		}
	}
	return Rgba{ r, g, b, a };
}

ParamPlugin::ParamPlugin( unsigned n ) :
	values( n, 0.0f ),
	hostKnown( n, std::nanf( "" ) ),
	visibility( n, -1 ),
	textBuffers( n ),
	displayBuffers( n )
{
}

void ParamPlugin::defRange( unsigned id, const std::string& name, const char* group, float def, float lo, float hi )
{
	values[ id ] = def;
	SetParamInfo( id, name.c_str(), FF_TYPE_STANDARD, def );
	if( ParamInfo* info = FindParamInfo( id ) )// SetParamInfo clamps defaults of standard params to 0..1
		info->defaultFloatVal = def;
	SetParamRange( id, lo, hi );
	SetParamGroup( id, group );
}

void ParamPlugin::defBool( unsigned id, const std::string& name, const char* group, bool def )
{
	values[ id ] = def ? 1.0f : 0.0f;
	SetParamInfo( id, name.c_str(), FF_TYPE_BOOLEAN, def );
	SetParamGroup( id, group );
}

void ParamPlugin::defEvent( unsigned id, const std::string& name, const char* group )
{
	values[ id ] = 0.0f;
	SetParamInfo( id, name.c_str(), FF_TYPE_EVENT, false );
	SetParamGroup( id, group );
}

void ParamPlugin::defOption( unsigned id, const std::string& name, const char* group, const std::vector< const char* >& items, int def )
{
	std::vector< OptionItem > withValues;
	for( size_t i = 0; i < items.size(); ++i )
		withValues.push_back( OptionItem{ items[ i ], (float)i } );
	defOption( id, name, group, withValues, (float)def );
}

void ParamPlugin::defOption( unsigned id, const std::string& name, const char* group, const std::vector< OptionItem >& items, float def )
{
	values[ id ] = def;
	SetOptionParamInfo( id, name.c_str(), (unsigned)items.size(), def );
	for( unsigned i = 0; i < items.size(); ++i )
	{
		if( items[ i ].separatorBefore )
			AddElementSeparator( id, i );
		SetParamElementInfo( id, i, items[ i ].label, items[ i ].value );
	}
	SetParamGroup( id, group );
}

void ParamPlugin::defColor( unsigned h, const std::string& name, const char* group, float hv, float sv, float bv, float av )
{
	const float v[ 4 ]        = { hv, sv, bv, av };
	const unsigned types[ 4 ] = { FF_TYPE_HUE, FF_TYPE_SATURATION, FF_TYPE_BRIGHTNESS, FF_TYPE_ALPHA };
	const char* suffix[ 4 ]   = { "", "_Sat", "_Bri", "_Alpha" };
	for( unsigned i = 0; i < 4; ++i )
	{
		values[ h + i ] = v[ i ];
		SetParamInfo( h + i, ( name + suffix[ i ] ).c_str(), types[ i ], v[ i ] );
		SetParamGroup( h + i, group );
	}
}

void ParamPlugin::defText( unsigned id, const std::string& name, const char* group, const std::string& def )
{
	SetParamInfo( id, name.c_str(), FF_TYPE_TEXT, def.c_str() );
	SetParamGroup( id, group );
}

void ParamPlugin::defFile( unsigned id, const std::string& name, const char* group, const std::vector< std::string >& extensions )
{
	SetFileParamInfo( id, name.c_str(), extensions, "" );
	SetParamGroup( id, group );
}

void ParamPlugin::setValue( unsigned id, float v )
{
	if( id >= values.size() || values[ id ] == v )
		return;
	values[ id ] = v;
	RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
}

float ParamPlugin::defaultValue( unsigned id )
{
	ParamInfo* info = FindParamInfo( id );
	return info ? info->defaultFloatVal : 0.0f;
}

Rgba ParamPlugin::colorOf( unsigned h ) const
{
	return hsbaToRgba( values[ h ], values[ h + 1 ], values[ h + 2 ], values[ h + 3 ] );
}

bool ParamPlugin::hostChanged( unsigned id, float v )
{
	const bool changed = !( v == hostKnown[ id ] );
	hostKnown[ id ]    = v;
	return changed;
}

bool ParamPlugin::pressed( unsigned id, float v )
{
	const bool edge = v > 0.5f && values[ id ] <= 0.5f;
	values[ id ]    = v;
	return edge;
}

float ParamPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= values.size() )
		return 0.0f;
	hostKnown[ index ] = values[ index ];// the host now knows this value
	return values[ index ];
}

void ParamPlugin::show( unsigned id, bool v )
{
	const signed char want = v ? 1 : 0;
	if( id >= visibility.size() || visibility[ id ] == want )
		return;
	visibility[ id ] = want;
	SetParamVisibility( id, v, raiseVisibilityEvents );
}

char* ParamPlugin::returnText( unsigned id, const std::string& s )
{
	if( id >= textBuffers.size() )
		return nullptr;
	textBuffers[ id ] = s;
	return const_cast< char* >( textBuffers[ id ].c_str() );
}

char* ParamPlugin::returnDisplay( unsigned id, const std::string& s )
{
	if( id >= displayBuffers.size() )
		return nullptr;
	displayBuffers[ id ] = s;
	return const_cast< char* >( displayBuffers[ id ].c_str() );
}

}//namespace ffglkit
