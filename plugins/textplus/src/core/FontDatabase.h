#pragma once
// FontDatabase: knows every font installed on the system (families, styles,
// named instances of variable fonts) and resolves user queries like
// "Helvetica Neue / Bold" or "din bold" to a concrete font file + face.
//
// Scanning happens once per process on a background thread and is shared by
// all plugin instances. Results are cached on disk, keyed by file path +
// modification time, so the second Resolume start is instant.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace textplus
{

struct FontEntry
{
	std::string path;      // font file on disk
	int faceIndex = 0;     // face inside a .ttc/.otc collection
	int instance  = 0;     // 0 = default/static, n = n-th named instance (1-based) of a variable font
	std::string family;    // "Helvetica Neue"
	std::string style;     // "Bold Italic"
	std::string postscript;// "HelveticaNeue-BoldItalic"
	int weight  = 400;     // 100..900, for sorting styles
	bool italic = false;

	std::string displayName() const { return family + " / " + style; }
	bool sameFace( const FontEntry& o ) const { return path == o.path && faceIndex == o.faceIndex && instance == o.instance; }
};

// Platform hooks (FontPaths_<platform>): list every font file the OS knows about,
// including fonts activated by font managers where the OS exposes them.
std::vector< std::string > platformFontFiles();
// Directory for small cache files (created if missing). Empty if unavailable.
std::string platformCacheDirectory();

class FontDatabase
{
public:
	static FontDatabase& instance();

	// Starts the background scan if it has not been started yet. Cheap to call often.
	void ensureScanning();
	bool isReady() const { return ready.load(); }
	// Blocks until the scan has finished or the timeout elapsed. Returns isReady().
	bool waitUntilReady( int timeoutMs );

	// Sorted, de-duplicated family names (hidden system families removed).
	std::vector< std::string > families() const;
	// Styles of one family, sorted by weight then upright before italic.
	std::vector< FontEntry > stylesOf( const std::string& family ) const;

	// Resolves "Family / Style", "Family Style" (prefix match) or partial names.
	// Falls back to a sensible default font when nothing matches.
	// `matched` (optional) tells whether the query found a family; false means
	// the result is only the fallback default font.
	std::optional< FontEntry > resolve( const std::string& query, bool* matched = nullptr ) const;
	std::optional< FontEntry > defaultFont() const;

	// Reads the faces of a single font file (for the "Font File" parameter). Synchronous.
	static std::vector< FontEntry > scanFile( const std::string& path );

	// Used by tests: replace the platform file list.
	void setFileListOverride( std::vector< std::string > files );

private:
	FontDatabase() = default;
	~FontDatabase();
	void scanThreadMain();

	mutable std::mutex mutex;
	std::vector< FontEntry > entries;           // guarded by mutex
	std::vector< std::string > familyNames;     // guarded by mutex
	std::thread scanThread;
	std::atomic< bool > started{ false };
	std::atomic< bool > ready{ false };
	std::optional< std::vector< std::string > > fileOverride;
};

// Lower-case, strip spaces/dashes/underscores. Used for fuzzy matching.
std::string normalizeFontName( const std::string& s );

}//namespace textplus
