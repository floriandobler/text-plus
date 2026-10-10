#include "FontDatabase.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H

namespace kontur
{
namespace
{
const char* kCacheFileName = "fontdb-v1.tsv";

std::string trim( const std::string& s )
{
	size_t a = 0, b = s.size();
	while( a < b && std::isspace( (unsigned char)s[ a ] ) )
		++a;
	while( b > a && std::isspace( (unsigned char)s[ b - 1 ] ) )
		--b;
	return s.substr( a, b - a );
}

std::string sanitize( std::string s )
{
	for( char& c : s )
		if( c == '\t' || c == '\n' || c == '\r' )
			c = ' ';
	return s;
}

int weightFromStyleName( const std::string& styleNorm )
{
	struct W { const char* key; int weight; };
	static const W table[] = {
		{ "hairline", 100 }, { "thin", 100 }, { "extralight", 200 }, { "ultralight", 200 }, { "light", 300 },
		{ "book", 400 }, { "regular", 400 }, { "normal", 400 }, { "roman", 400 }, { "medium", 500 },
		{ "semibold", 600 }, { "demibold", 600 }, { "demi", 600 }, { "extrabold", 800 }, { "ultrabold", 800 },
		{ "bold", 700 }, { "heavy", 900 }, { "black", 900 }, { "ultra", 900 },
	};
	for( const W& w : table )
		if( styleNorm.find( w.key ) != std::string::npos )
			return w.weight;
	return 400;
}

struct FileStamp
{
	int64_t mtime = 0;
	int64_t size  = 0;
};

bool statFile( const std::string& path, FileStamp& out )
{
	struct stat st;
	if( stat( path.c_str(), &st ) != 0 )
		return false;
#if defined( __APPLE__ )
	out.mtime = (int64_t)st.st_mtimespec.tv_sec;
#else
	out.mtime = (int64_t)st.st_mtime;
#endif
	out.size = (int64_t)st.st_size;
	return true;
}

std::string lower( std::string s )
{
	for( char& c : s )
		c = (char)std::tolower( (unsigned char)c );
	return s;
}

bool isHiddenFamily( const std::string& family )
{
	return family.empty() || family[ 0 ] == '.' || family == "LastResort";
}

// Reads one face (optionally a named instance) into a FontEntry.
void describeFace( FT_Face face, const std::string& path, int faceIndex, int instance, std::vector< FontEntry >& out )
{
	if( !face->family_name )
		return;
	FontEntry e;
	e.path      = path;
	e.faceIndex = faceIndex;
	e.instance  = instance;
	e.family    = sanitize( face->family_name );
	e.style     = sanitize( face->style_name ? face->style_name : "Regular" );
	if( const char* ps = FT_Get_Postscript_Name( face ) )
		e.postscript = sanitize( ps );
	e.italic = ( face->style_flags & FT_STYLE_FLAG_ITALIC ) != 0;

	std::string styleNorm = normalizeFontName( e.style );
	if( styleNorm.find( "italic" ) != std::string::npos || styleNorm.find( "oblique" ) != std::string::npos )
		e.italic = true;

	e.weight = weightFromStyleName( styleNorm );
	if( TT_OS2* os2 = (TT_OS2*)FT_Get_Sfnt_Table( face, FT_SFNT_OS2 ) )
		if( instance == 0 && os2->usWeightClass >= 1 && os2->usWeightClass <= 1000 )
			e.weight = os2->usWeightClass;

	// Variable fonts: read the 'wght' coordinate of the named instance.
	if( instance > 0 && FT_HAS_MULTIPLE_MASTERS( face ) )
	{
		FT_MM_Var* mm = nullptr;
		if( FT_Get_MM_Var( face, &mm ) == 0 && mm )
		{
			if( (FT_UInt)instance <= mm->num_namedstyles )
			{
				const FT_Var_Named_Style& ns = mm->namedstyle[ instance - 1 ];
				for( FT_UInt a = 0; a < mm->num_axis; ++a )
					if( mm->axis[ a ].tag == FT_MAKE_TAG( 'w', 'g', 'h', 't' ) )
						e.weight = (int)( ns.coords[ a ] / 65536.0 + 0.5 );
			}
			FT_Done_MM_Var( face->glyph->library, mm );
		}
	}
	out.push_back( std::move( e ) );
}

void scanFileWithLibrary( FT_Library lib, const std::string& path, std::vector< FontEntry >& out )
{
	FT_Face face = nullptr;
	if( FT_New_Face( lib, path.c_str(), -1, &face ) != 0 || !face )
		return;
	FT_Long numFaces = face->num_faces;
	FT_Done_Face( face );

	for( FT_Long i = 0; i < numFaces && i < 256; ++i )
	{
		face = nullptr;
		if( FT_New_Face( lib, path.c_str(), i, &face ) != 0 || !face )
			continue;
		if( !FT_IS_SCALABLE( face ) )
		{
			FT_Done_Face( face );
			continue;
		}
		int numInstances = (int)( face->style_flags >> 16 );
		if( numInstances == 0 )
		{
			describeFace( face, path, (int)i, 0, out );
			FT_Done_Face( face );
			continue;
		}
		FT_Done_Face( face );
		for( int n = 1; n <= numInstances && n < 1024; ++n )
		{
			FT_Face inst = nullptr;
			if( FT_New_Face( lib, path.c_str(), ( (FT_Long)n << 16 ) | i, &inst ) == 0 && inst )
			{
				describeFace( inst, path, (int)i, n, out );
				FT_Done_Face( inst );
			}
		}
	}
}

struct CachedFile
{
	FileStamp stamp;
	std::vector< FontEntry > entries;
};

std::unordered_map< std::string, CachedFile > loadCache( const std::string& file )
{
	std::unordered_map< std::string, CachedFile > result;
	std::ifstream in( file );
	if( !in )
		return result;
	std::string line;
	while( std::getline( in, line ) )
	{
		std::vector< std::string > f;
		std::stringstream ss( line );
		std::string part;
		while( std::getline( ss, part, '\t' ) )
			f.push_back( part );
		// path mtime size [faceIndex instance family style ps weight italic]
		if( f.size() < 3 )
			continue;
		CachedFile& cf  = result[ f[ 0 ] ];
		cf.stamp.mtime  = std::atoll( f[ 1 ].c_str() );
		cf.stamp.size   = std::atoll( f[ 2 ].c_str() );
		if( f.size() < 10 )
			continue;// file without usable faces
		FontEntry e;
		e.path       = f[ 0 ];
		e.faceIndex  = std::atoi( f[ 3 ].c_str() );
		e.instance   = std::atoi( f[ 4 ].c_str() );
		e.family     = f[ 5 ];
		e.style      = f[ 6 ];
		e.postscript = f[ 7 ];
		e.weight     = std::atoi( f[ 8 ].c_str() );
		e.italic     = f[ 9 ] == "1";
		cf.entries.push_back( std::move( e ) );
	}
	return result;
}

void saveCache( const std::string& file, const std::map< std::string, CachedFile >& files )
{
	std::string tmp = file + ".tmp";
	{
		std::ofstream out( tmp, std::ios::trunc );
		if( !out )
			return;
		for( const auto& kv : files )
		{
			const CachedFile& cf = kv.second;
			if( cf.entries.empty() )
				out << kv.first << '\t' << cf.stamp.mtime << '\t' << cf.stamp.size << '\n';
			for( const FontEntry& e : cf.entries )
				out << kv.first << '\t' << cf.stamp.mtime << '\t' << cf.stamp.size << '\t' << e.faceIndex << '\t' << e.instance << '\t'
					<< e.family << '\t' << e.style << '\t' << e.postscript << '\t' << e.weight << '\t' << ( e.italic ? 1 : 0 ) << '\n';
		}
	}
	std::rename( tmp.c_str(), file.c_str() );
}

bool styleLess( const FontEntry& a, const FontEntry& b )
{
	if( a.weight != b.weight )
		return a.weight < b.weight;
	if( a.italic != b.italic )
		return !a.italic;
	return a.style < b.style;
}

}//namespace

std::string normalizeFontName( const std::string& s )
{
	std::string r;
	r.reserve( s.size() );
	for( char c : s )
	{
		if( c == ' ' || c == '-' || c == '_' || c == '\t' )
			continue;
		r.push_back( (char)std::tolower( (unsigned char)c ) );
	}
	return r;
}

FontDatabase& FontDatabase::instance()
{
	static FontDatabase db;
	return db;
}

FontDatabase::~FontDatabase()
{
	if( scanThread.joinable() )
		scanThread.join();
}

void FontDatabase::setFileListOverride( std::vector< std::string > files )
{
	std::lock_guard< std::mutex > lock( mutex );
	fileOverride = std::move( files );
}

void FontDatabase::ensureScanning()
{
	bool expected = false;
	if( !started.compare_exchange_strong( expected, true ) )
		return;
	scanThread = std::thread( [ this ] { scanThreadMain(); } );
}

bool FontDatabase::waitUntilReady( int timeoutMs )
{
	ensureScanning();
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( timeoutMs );
	while( !ready.load() && std::chrono::steady_clock::now() < deadline )
		std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
	return ready.load();
}

void FontDatabase::scanThreadMain()
{
	std::vector< std::string > files;
	{
		std::lock_guard< std::mutex > lock( mutex );
		if( fileOverride )
			files = *fileOverride;
	}
	if( files.empty() )
		files = platformFontFiles();
	std::sort( files.begin(), files.end() );
	files.erase( std::unique( files.begin(), files.end() ), files.end() );

	std::string cacheDir  = platformCacheDirectory();
	std::string cacheFile = cacheDir.empty() ? std::string() : cacheDir + "/" + kCacheFileName;
	auto cache            = cacheFile.empty() ? std::unordered_map< std::string, CachedFile >() : loadCache( cacheFile );

	FT_Library lib = nullptr;
	FT_Init_FreeType( &lib );

	std::map< std::string, CachedFile > fresh;
	bool changed = false;
	for( const std::string& path : files )
	{
		FileStamp stamp;
		if( !statFile( path, stamp ) )
			continue;
		auto it = cache.find( path );
		if( it != cache.end() && it->second.stamp.mtime == stamp.mtime && it->second.stamp.size == stamp.size )
		{
			fresh[ path ] = it->second;
			continue;
		}
		CachedFile cf;
		cf.stamp = stamp;
		if( lib )
			scanFileWithLibrary( lib, path, cf.entries );
		fresh[ path ] = std::move( cf );
		changed       = true;
	}
	if( fresh.size() != cache.size() )
		changed = true;
	if( lib )
		FT_Done_FreeType( lib );

	if( changed && !cacheFile.empty() )
		saveCache( cacheFile, fresh );

	std::vector< FontEntry > all;
	for( auto& kv : fresh )
		for( FontEntry& e : kv.second.entries )
			if( !isHiddenFamily( e.family ) )
				all.push_back( e );

	// De-duplicate identical family/style pairs (same font installed twice): keep the first.
	std::set< std::string > seen;
	std::vector< FontEntry > unique;
	for( FontEntry& e : all )
	{
		std::string key = normalizeFontName( e.family ) + "|" + normalizeFontName( e.style );
		if( seen.insert( key ).second )
			unique.push_back( std::move( e ) );
	}

	std::map< std::string, std::string > famByLower;
	for( const FontEntry& e : unique )
		famByLower.emplace( lower( e.family ), e.family );
	std::vector< std::string > fams;
	for( auto& kv : famByLower )
		fams.push_back( kv.second );

	{
		std::lock_guard< std::mutex > lock( mutex );
		entries     = std::move( unique );
		familyNames = std::move( fams );
	}
	ready.store( true );
}

std::vector< std::string > FontDatabase::families() const
{
	std::lock_guard< std::mutex > lock( mutex );
	return familyNames;
}

std::vector< FontEntry > FontDatabase::stylesOf( const std::string& family ) const
{
	std::vector< FontEntry > result;
	{
		std::lock_guard< std::mutex > lock( mutex );
		for( const FontEntry& e : entries )
			if( e.family == family )
				result.push_back( e );
	}
	std::sort( result.begin(), result.end(), styleLess );
	return result;
}

std::optional< FontEntry > FontDatabase::defaultFont() const
{
	static const char* preferred[] = { "Helvetica Neue", "Helvetica", "Arial", "DejaVu Sans", "Liberation Sans", "Noto Sans" };
	for( const char* fam : preferred )
	{
		auto styles = stylesOf( fam );
		if( styles.empty() )
			continue;
		for( const FontEntry& e : styles )
			if( !e.italic && e.weight == 400 )
				return e;
		return styles.front();
	}
	std::lock_guard< std::mutex > lock( mutex );
	if( !entries.empty() )
		return entries.front();
	return std::nullopt;
}

std::optional< FontEntry > FontDatabase::resolve( const std::string& rawQuery ) const
{
	std::string query = trim( rawQuery );
	if( query.empty() )
		return defaultFont();

	std::string famQuery, styleQuery;
	size_t slash = query.find( '/' );
	if( slash != std::string::npos )
	{
		famQuery   = trim( query.substr( 0, slash ) );
		styleQuery = trim( query.substr( slash + 1 ) );
	}
	else
	{
		famQuery = query;
	}

	std::vector< std::string > fams = families();
	std::string famNorm             = normalizeFontName( famQuery );
	std::string found;

	// 1) exact family match
	for( const std::string& f : fams )
		if( normalizeFontName( f ) == famNorm )
		{
			found = f;
			break;
		}
	// 2) "Family Style" typed without slash: longest family that prefixes the query
	if( found.empty() && slash == std::string::npos )
	{
		size_t bestLen = 0;
		for( const std::string& f : fams )
		{
			std::string fn = normalizeFontName( f );
			if( fn.size() > bestLen && famNorm.compare( 0, fn.size(), fn ) == 0 )
			{
				found   = f;
				bestLen = fn.size();
			}
		}
		if( !found.empty() )
			styleQuery = famNorm.substr( bestLen );
	}
	// 3) family starting with the query, then containing it (shortest wins)
	if( found.empty() && !famNorm.empty() )
	{
		for( int pass = 0; pass < 2 && found.empty(); ++pass )
		{
			size_t bestLen = SIZE_MAX;
			for( const std::string& f : fams )
			{
				std::string fn = normalizeFontName( f );
				bool hit       = pass == 0 ? fn.compare( 0, famNorm.size(), famNorm ) == 0 : fn.find( famNorm ) != std::string::npos;
				if( hit && fn.size() < bestLen )
				{
					found   = f;
					bestLen = fn.size();
				}
			}
		}
	}
	if( found.empty() )
		return defaultFont();

	std::vector< FontEntry > styles = stylesOf( found );
	if( styles.empty() )
		return defaultFont();

	std::string styleNorm = normalizeFontName( styleQuery );
	if( !styleNorm.empty() )
	{
		for( const FontEntry& e : styles )
			if( normalizeFontName( e.style ) == styleNorm )
				return e;
		for( const FontEntry& e : styles )
			if( normalizeFontName( e.style ).find( styleNorm ) != std::string::npos )
				return e;
		// Typed weight words like "bold italic" in a different order
		int wantWeight  = weightFromStyleName( styleNorm );
		bool wantItalic = styleNorm.find( "italic" ) != std::string::npos || styleNorm.find( "oblique" ) != std::string::npos;
		const FontEntry* best = nullptr;
		int bestScore         = INT32_MAX;
		for( const FontEntry& e : styles )
		{
			int score = std::abs( e.weight - wantWeight ) + ( e.italic != wantItalic ? 1000 : 0 );
			if( score < bestScore )
			{
				bestScore = score;
				best      = &e;
			}
		}
		if( best )
			return *best;
	}
	// Default style: upright, closest to 400
	const FontEntry* best = nullptr;
	int bestScore         = INT32_MAX;
	for( const FontEntry& e : styles )
	{
		int score = std::abs( e.weight - 400 ) + ( e.italic ? 1000 : 0 );
		if( score < bestScore )
		{
			bestScore = score;
			best      = &e;
		}
	}
	return *best;
}

std::vector< FontEntry > FontDatabase::scanFile( const std::string& path )
{
	std::vector< FontEntry > out;
	FT_Library lib = nullptr;
	if( FT_Init_FreeType( &lib ) != 0 )
		return out;
	scanFileWithLibrary( lib, path, out );
	FT_Done_FreeType( lib );
	std::sort( out.begin(), out.end(), styleLess );
	return out;
}

}//namespace kontur
