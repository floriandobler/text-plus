#include "FontFace.h"

#include <atomic>
#include <fstream>
#include <map>
#include <mutex>

#include <hb-ot.h>
#include <hb.h>

namespace kontur
{
namespace
{
std::mutex g_faceMutex;
std::map< std::string, std::weak_ptr< FontFace > > g_faces;
std::atomic< uint32_t > g_nextId{ 1 };

std::string keyOf( const FontEntry& e )
{
	return e.path + "#" + std::to_string( e.faceIndex ) + "#" + std::to_string( e.instance );
}

bool readFile( const std::string& path, std::vector< uint8_t >& out )
{
	std::ifstream in( path, std::ios::binary | std::ios::ate );
	if( !in )
		return false;
	std::streamsize size = in.tellg();
	if( size <= 0 || size > ( 512ll << 20 ) )
		return false;
	out.resize( (size_t)size );
	in.seekg( 0 );
	return (bool)in.read( (char*)out.data(), size );
}
}//namespace

std::shared_ptr< FontFace > FontFace::get( const FontEntry& entry )
{
	std::string key = keyOf( entry );
	std::lock_guard< std::mutex > lock( g_faceMutex );
	auto it = g_faces.find( key );
	if( it != g_faces.end() )
		if( auto existing = it->second.lock() )
			return existing;

	std::shared_ptr< FontFace > f( new FontFace() );
	if( !f->load( entry ) )
		return nullptr;
	g_faces[ key ] = f;
	return f;
}

bool FontFace::load( const FontEntry& e )
{
	fontEntry = e;
	if( !readFile( e.path, bytes ) )
		return false;

	blob = hb_blob_create( (const char*)bytes.data(), (unsigned)bytes.size(), HB_MEMORY_MODE_READONLY, nullptr, nullptr );
	face = hb_face_create( blob, (unsigned)e.faceIndex );
	if( !face || hb_face_get_glyph_count( face ) == 0 )
		return false;
	font = hb_font_create( face );
	upem = (int)hb_face_get_upem( face );
	if( upem <= 0 )
		upem = 1000;
	hb_font_set_scale( font, upem, upem );
	if( e.instance > 0 )
		hb_font_set_var_named_instance( font, (unsigned)( e.instance - 1 ) );
	hb_font_make_immutable( font );

	hb_font_extents_t ext{};
	if( hb_font_get_h_extents( font, &ext ) )
	{
		fontMetrics.ascender  = ext.ascender / (float)upem;
		fontMetrics.descender = ext.descender / (float)upem;
		fontMetrics.lineGap   = ext.line_gap / (float)upem;
	}
	hb_position_t v = 0;
	if( hb_ot_metrics_get_position( font, HB_OT_METRICS_TAG_CAP_HEIGHT, &v ) && v > 0 )
		fontMetrics.capHeight = v / (float)upem;
	else
		fontMetrics.capHeight = fontMetrics.ascender * 0.85f;
	if( hb_ot_metrics_get_position( font, HB_OT_METRICS_TAG_X_HEIGHT, &v ) && v > 0 )
		fontMetrics.xHeight = v / (float)upem;
	else
		fontMetrics.xHeight = fontMetrics.capHeight * 0.7f;

	uniqueId = g_nextId.fetch_add( 1 );
	return true;
}

FontFace::~FontFace()
{
	if( font )
		hb_font_destroy( font );
	if( face )
		hb_face_destroy( face );
	if( blob )
		hb_blob_destroy( blob );
}

}//namespace kontur
