#include "FontFallback.h"
#include "FileUtil.h"
#include "Utf8.h"

#include <mutex>
#include <unordered_map>

#include <hb.h>

namespace textplus
{

bool isEmojiPresentation( uint32_t cp )
{
	return ( cp >= 0x1F300 && cp <= 0x1F5FF ) ||// symbols & pictographs
		   ( cp >= 0x1F600 && cp <= 0x1F64F ) ||// emoticons
		   ( cp >= 0x1F680 && cp <= 0x1F6FF ) ||// transport & map
		   ( cp >= 0x1F900 && cp <= 0x1F9FF ) ||// supplemental symbols & pictographs
		   ( cp >= 0x1FA70 && cp <= 0x1FAFF ) ||// symbols & pictographs extended-A
		   ( cp >= 0x1F1E6 && cp <= 0x1F1FF ) ||// regional indicators (flags)
		   ( cp >= 0x1F004 && cp <= 0x1F0CF ) ||// mahjong / playing cards with emoji forms
		   ( cp >= 0x1F170 && cp <= 0x1F251 );  // enclosed alphanumeric / ideographic supplements
}

bool continuesCluster( uint32_t cp )
{
	return cp == 0x200D ||                     // zero width joiner
		   ( cp >= 0xFE00 && cp <= 0xFE0F ) || // variation selectors
		   ( cp >= 0x1F3FB && cp <= 0x1F3FF ) ||// skin tones
		   ( cp >= 0xE0020 && cp <= 0xE007F ) ||// tags (subdivision flags)
		   cp == 0x20E3 ||                     // combining keycap
		   ( cp >= 0x0300 && cp <= 0x036F ) || ( cp >= 0x1AB0 && cp <= 0x1AFF ) || ( cp >= 0x1DC0 && cp <= 0x1DFF ) ||
		   ( cp >= 0x20D0 && cp <= 0x20FF ) || ( cp >= 0xFE20 && cp <= 0xFE2F );
}

namespace
{
std::mutex g_mutex;
std::unordered_map< uint32_t, std::shared_ptr< FontFace > > g_byChar;// nullptr = nothing found
std::unordered_map< std::string, std::shared_ptr< FontFace > > g_faces;// keeps fallback faces alive

bool hasGlyph( const FontFace& f, uint32_t cp )
{
	hb_codepoint_t gid = 0;
	return hb_font_get_nominal_glyph( f.hbFont(), cp, &gid ) && gid != 0;
}

std::shared_ptr< FontFace > keep( const FontEntry& e )
{
	const std::string key = e.path + "#" + std::to_string( e.faceIndex ) + "#" + std::to_string( e.instance );
	auto it               = g_faces.find( key );
	if( it != g_faces.end() )
		return it->second;
	auto f = FontFace::get( e );
	if( f )
		g_faces[ key ] = f;
	return f;
}

const char* kLastResort[] = { "Arial Unicode MS", "Segoe UI Symbol", "Segoe UI", "Microsoft YaHei", "Yu Gothic", "Malgun Gothic", "Nirmala UI",
							  "Apple Symbols", "Noto Sans", "Noto Sans CJK SC", "Noto Sans Symbols", "Noto Sans Symbols 2", "DejaVu Sans", "FreeSerif",
							  "Symbola" };
}//namespace

std::shared_ptr< FontFace > emojiFace()
{
	static std::shared_ptr< FontFace > face = [] {
		FontEntry e;
		e.path       = std::string( kEmbeddedPrefix ) + "NotoEmoji";
		e.family     = "Noto Emoji";
		e.style      = "Regular";
		e.postscript = "NotoEmoji-Regular";
		return FontFace::get( e );
	}();
	return face;
}

std::shared_ptr< FontFace > fallbackFaceFor( uint32_t cp, const FontFace& base )
{
	if( isEmojiPresentation( cp ) )
	{
		auto e = emojiFace();
		if( e && hasGlyph( *e, cp ) )
			return e;
	}
	std::lock_guard< std::mutex > lock( g_mutex );
	auto it = g_byChar.find( cp );
	if( it != g_byChar.end() && ( !it->second || hasGlyph( *it->second, cp ) ) )
		return it->second;

	std::shared_ptr< FontFace > found;
	// 1. The operating system
	std::string s;
	appendUtf8( s, cp );
	FontEntry e;
	if( platformFallbackFont( base.entry(), s, e ) )
	{
		auto f = keep( e );
		if( f && hasGlyph( *f, cp ) )
			found = f;
	}
	// 2. The embedded emoji font covers many symbols too
	if( !found )
	{
		auto em = emojiFace();
		if( em && hasGlyph( *em, cp ) )
			found = em;
	}
	// 3. Broad fonts that are usually installed
	if( !found )
	{
		FontDatabase& db = FontDatabase::instance();
		if( db.isReady() )
			for( const char* fam : kLastResort )
			{
				auto styles = db.stylesOf( fam );
				if( styles.empty() )
					continue;
				const FontEntry* pick = &styles.front();
				for( const FontEntry& st : styles )
					if( !st.italic && st.weight == 400 )
					{
						pick = &st;
						break;
					}
				auto f = keep( *pick );
				if( f && hasGlyph( *f, cp ) )
				{
					found = f;
					break;
				}
			}
	}
	g_byChar[ cp ] = found;
	return found;
}

}//namespace textplus
