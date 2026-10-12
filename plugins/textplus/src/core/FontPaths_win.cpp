// Windows: the system font folder and the per-user font folder (fonts installed
// "for this user only"). Font managers that activate fonts without installing
// them are not visible here (a DirectWrite enumeration could add them later).
#include "FontDatabase.h"
#include "FileUtil.h"

#include <cstdlib>
#include <filesystem>

namespace textplus
{

std::vector< std::string > platformFontFiles()
{
	std::vector< std::string > files;
	std::vector< std::filesystem::path > dirs;
	if( const wchar_t* windir = _wgetenv( L"WINDIR" ) )
		dirs.emplace_back( std::filesystem::path( windir ) / "Fonts" );
	if( const wchar_t* local = _wgetenv( L"LOCALAPPDATA" ) )
		dirs.emplace_back( std::filesystem::path( local ) / "Microsoft" / "Windows" / "Fonts" );
	for( const auto& dir : dirs )
	{
		std::error_code ec;
		for( const auto& entry : std::filesystem::directory_iterator( dir, ec ) )
		{
			auto ext = entry.path().extension().string();
			for( char& c : ext )
				c = (char)tolower( (unsigned char)c );
			if( ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc" )
				files.push_back( utf8FromPath( entry.path() ) );
		}
	}
	return files;
}

std::string platformCacheDirectory()
{
	if( const wchar_t* local = _wgetenv( L"LOCALAPPDATA" ) )
	{
		std::filesystem::path dir = std::filesystem::path( local ) / "TextPlus";
		std::error_code ec;
		std::filesystem::create_directories( dir, ec );
		return utf8FromPath( dir );
	}
	return {};
}

}//namespace textplus

// ---------------------------------------------------------------------------
// Font fallback through DirectWrite (Windows 8.1+): the same cascade Edge and
// Office use.
// ---------------------------------------------------------------------------
#include "FontFallback.h"

#include <windows.h>
#include <dwrite_2.h>

#include <map>
#include <mutex>

namespace textplus
{
namespace
{
std::wstring widen( const std::string& s )
{
	if( s.empty() )
		return {};
	int n = MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0 );
	std::wstring w( (size_t)n, L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), &w[ 0 ], n );
	return w;
}

std::string narrow( const std::wstring& w )
{
	if( w.empty() )
		return {};
	int n = WideCharToMultiByte( CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr );
	std::string s( (size_t)n, '\0' );
	WideCharToMultiByte( CP_UTF8, 0, w.data(), (int)w.size(), &s[ 0 ], n, nullptr, nullptr );
	return s;
}

// Minimal text source for IDWriteFontFallback::MapCharacters.
class AnalysisSource : public IDWriteTextAnalysisSource
{
public:
	AnalysisSource( const std::wstring& t ) :
		text( t )
	{
	}
	HRESULT STDMETHODCALLTYPE QueryInterface( REFIID riid, void** obj ) override
	{
		if( riid == __uuidof( IUnknown ) || riid == __uuidof( IDWriteTextAnalysisSource ) )
		{
			*obj = this;
			AddRef();
			return S_OK;
		}
		*obj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
	ULONG STDMETHODCALLTYPE Release() override { return --refs; }// lives on the stack
	HRESULT STDMETHODCALLTYPE GetTextAtPosition( UINT32 pos, WCHAR const** str, UINT32* len ) override
	{
		if( pos >= text.size() )
		{
			*str = nullptr;
			*len = 0;
		}
		else
		{
			*str = text.c_str() + pos;
			*len = (UINT32)text.size() - pos;
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetTextBeforePosition( UINT32 pos, WCHAR const** str, UINT32* len ) override
	{
		if( pos == 0 || pos > text.size() )
		{
			*str = nullptr;
			*len = 0;
		}
		else
		{
			*str = text.c_str();
			*len = pos;
		}
		return S_OK;
	}
	DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override { return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT; }
	HRESULT STDMETHODCALLTYPE GetLocaleName( UINT32 pos, UINT32* len, WCHAR const** locale ) override
	{
		*locale = L"";
		*len    = pos < text.size() ? (UINT32)text.size() - pos : 0;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetNumberSubstitution( UINT32 pos, UINT32* len, IDWriteNumberSubstitution** subst ) override
	{
		*subst = nullptr;
		*len   = pos < text.size() ? (UINT32)text.size() - pos : 0;
		return S_OK;
	}

private:
	std::wstring text;
	ULONG refs = 1;
};

template < typename T >
void releaseCom( T*& p )
{
	if( p )
		p->Release();
	p = nullptr;
}
}//namespace

bool platformFallbackFont( const FontEntry& base, const std::string& utf8, FontEntry& out )
{
	static std::mutex mutex;
	static IDWriteFactory2* factory = nullptr;
	static bool tried               = false;
	static std::map< std::string, std::vector< FontEntry > > scanned;
	std::lock_guard< std::mutex > lock( mutex );
	if( !tried )
	{
		tried = true;
		DWriteCreateFactory( DWRITE_FACTORY_TYPE_SHARED, __uuidof( IDWriteFactory2 ), reinterpret_cast< IUnknown** >( &factory ) );
	}
	if( !factory )
		return false;

	IDWriteFontFallback* fallback = nullptr;
	if( FAILED( factory->GetSystemFontFallback( &fallback ) ) || !fallback )
		return false;
	const std::wstring wide   = widen( utf8 );
	const std::wstring family = widen( base.family );
	AnalysisSource source( wide );
	UINT32 mappedLength   = 0;
	IDWriteFont* mapped   = nullptr;
	FLOAT scale           = 1.0f;
	fallback->MapCharacters( &source, 0, (UINT32)wide.size(), nullptr, family.empty() ? nullptr : family.c_str(), DWRITE_FONT_WEIGHT_NORMAL,
							 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, &mappedLength, &mapped, &scale );
	releaseCom( fallback );
	if( !mapped )
		return false;

	std::string path;
	UINT32 index                  = 0;
	IDWriteFontFace* face         = nullptr;
	IDWriteFontFile* file         = nullptr;
	IDWriteFontFileLoader* loader = nullptr;
	IDWriteLocalFontFileLoader* local = nullptr;
	if( SUCCEEDED( mapped->CreateFontFace( &face ) ) && face )
	{
		index        = face->GetIndex();
		UINT32 count = 1;
		if( SUCCEEDED( face->GetFiles( &count, &file ) ) && file && SUCCEEDED( file->GetLoader( &loader ) ) && loader &&
			SUCCEEDED( loader->QueryInterface( __uuidof( IDWriteLocalFontFileLoader ), (void**)&local ) ) && local )
		{
			const void* key = nullptr;
			UINT32 keySize  = 0;
			UINT32 length   = 0;
			if( SUCCEEDED( file->GetReferenceKey( &key, &keySize ) ) && SUCCEEDED( local->GetFilePathLengthFromKey( key, keySize, &length ) ) )
			{
				std::wstring p( (size_t)length + 1, L'\0' );
				if( SUCCEEDED( local->GetFilePathFromKey( key, keySize, &p[ 0 ], length + 1 ) ) )
				{
					p.resize( length );
					path = narrow( p );
				}
			}
		}
	}
	releaseCom( local );
	releaseCom( loader );
	releaseCom( file );
	releaseCom( face );
	releaseCom( mapped );
	if( path.empty() )
		return false;

	auto it = scanned.find( path );
	if( it == scanned.end() )
		it = scanned.emplace( path, FontDatabase::scanFile( path ) ).first;
	for( const FontEntry& e : it->second )
		if( e.faceIndex == (int)index && e.instance == 0 )
		{
			out = e;
			return true;
		}
	if( it->second.empty() )
		return false;
	out = it->second.front();
	return true;
}

}//namespace textplus
