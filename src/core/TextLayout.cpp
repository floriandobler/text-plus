#include "TextLayout.h"
#include "Utf8.h"

#include <algorithm>
#include <cmath>

#include <hb.h>

namespace textplus
{
namespace
{

struct ShapedGlyph
{
	uint32_t gid  = 0;
	float advEm   = 0;// advance in em
	float xOffEm  = 0;
	float yOffEm  = 0;// up
	bool space    = false;
	bool breakAfter = false;
	int colorIndex = -1;
};

struct ShapedPara
{
	std::vector< ShapedGlyph > glyphs;
	int style          = 0;
	const FontFace* face = nullptr;
	float scale        = 1.0f;// size relative to the main size
	float trackingEm   = 0.0f;// tracking in em of the MAIN size
	float spacingEm    = 0.0f;// extra space above, em of the main size

	size_t size() const { return glyphs.size(); }
	const ShapedGlyph& operator[]( size_t i ) const { return glyphs[ i ]; }
};

std::vector< ShapedPara > shape( const FontFace& mainFace, const LayoutParams& p, std::vector< Rgb >& palette )
{
	std::vector< ShapedPara > result;

	std::vector< hb_feature_t > features;
	auto addFeature = [ & ]( const char* str ) {
		hb_feature_t f;
		if( hb_feature_from_string( str, -1, &f ) )
			features.push_back( f );
	};
	if( !p.kerning )
		addFeature( "kern=0" );
	if( !p.ligatures )
	{
		addFeature( "liga=0" );
		addFeature( "clig=0" );
		addFeature( "dlig=0" );
	}

	hb_buffer_t* buf = hb_buffer_create();
	int paraIndex    = 0;
	for( const MarkedParagraph& marked : parseMarkedText( p.text, p.pipeIsNewline, p.markup, palette ) )
	{
		const std::string& para = marked.text;
		ShapedPara out;
		out.style            = lineStyleOf( paraIndex++, p.lines );
		const LineStyle& ls  = p.lines[ out.style ];
		const bool styled    = out.style > 0;
		const FontFace& face = styled && ls.face ? *ls.face : mainFace;
		out.face             = &face;
		out.scale            = styled ? std::max( 0.01f, ls.sizePercent / 100.0f ) : 1.0f;
		out.trackingEm       = ( p.trackingPermille + ( styled ? ls.trackingPermille : 0.0f ) ) / 1000.0f * out.scale;
		out.spacingEm        = styled ? ls.spacingPercent / 100.0f : 0.0f;
		const float upem     = (float)face.unitsPerEm();
		if( !para.empty() )
		{
			hb_buffer_clear_contents( buf );
			hb_buffer_add_utf8( buf, para.c_str(), (int)para.size(), 0, (int)para.size() );
			hb_buffer_guess_segment_properties( buf );
			hb_shape( face.hbFont(), buf, features.data(), (unsigned)features.size() );

			unsigned n                = 0;
			hb_glyph_info_t* info     = hb_buffer_get_glyph_infos( buf, &n );
			hb_glyph_position_t* pos  = hb_buffer_get_glyph_positions( buf, &n );
			out.glyphs.reserve( n );
			for( unsigned i = 0; i < n; ++i )
			{
				ShapedGlyph g;
				g.gid       = info[ i ].codepoint;
				g.advEm     = pos[ i ].x_advance / upem;
				g.xOffEm    = pos[ i ].x_offset / upem;
				g.yOffEm    = pos[ i ].y_offset / upem;
				uint32_t cp = decodeUtf8At( para, info[ i ].cluster );
				g.space     = isBreakingSpace( cp ) || cp == 0xA0 || cp == 0x202F;
				g.breakAfter = isBreakingSpace( cp ) || cp == '-' || cp == 0x2010 || cp == 0x2013 || cp == 0x2014 || cp == 0x200B;
				if( info[ i ].cluster < marked.colorOfByte.size() )
					g.colorIndex = marked.colorOfByte[ info[ i ].cluster ];
				out.glyphs.push_back( g );
			}
		}
		result.push_back( std::move( out ) );
	}
	hb_buffer_destroy( buf );
	return result;
}

struct Line
{
	int para  = 0;
	int begin = 0;// glyph range in the paragraph
	int end   = 0;
	float inkWidth = 0;// width up to the last non-space glyph, without its trailing tracking
	bool lastOfPara = false;
};

struct Pass
{
	std::vector< Line > lines;
	bool overflow = false;
	float maxWidth = 0;
};

Pass breakLines( const std::vector< ShapedPara >& paras, float mainSize, float boxWidth )
{
	Pass pass;
	const float eps = 0.01f;
	for( int pi = 0; pi < (int)paras.size(); ++pi )
	{
		const ShapedPara& g  = paras[ pi ];
		const int n          = (int)g.size();
		const float size     = mainSize * g.scale;
		const float tracking = mainSize * g.trackingEm;
		auto adv             = [ & ]( int i ) { return g[ i ].advEm * size + tracking; };

		int lineStart     = 0;
		float lineAll     = 0;// sum of advances on the current line, spaces included
		auto closeLine = [ & ]( int end ) {
			Line l;
			l.para  = pi;
			l.begin = lineStart;
			l.end   = end;
			int lastInk = end - 1;
			while( lastInk >= lineStart && g[ lastInk ].space )
				--lastInk;
			float w = 0;
			for( int i = lineStart; i <= lastInk; ++i )
				w += adv( i );
			if( lastInk >= lineStart )
				w -= tracking;
			l.inkWidth    = std::max( 0.0f, w );
			pass.maxWidth = std::max( pass.maxWidth, l.inkWidth );
			pass.lines.push_back( l );
			lineStart = end;
			lineAll   = 0;
		};

		int segStart = 0;
		for( int i = 0; i < n; ++i )
		{
			bool segEnd = g[ i ].breakAfter || i == n - 1;
			if( !segEnd )
				continue;
			// segment [segStart, i]
			float segAll = 0, segInk = 0;
			int lastInk = i;
			while( lastInk >= segStart && g[ lastInk ].space )
				--lastInk;
			for( int k = segStart; k <= i; ++k )
			{
				segAll += adv( k );
				if( k <= lastInk )
					segInk += adv( k );
			}
			if( lastInk >= segStart )
				segInk -= tracking;

			if( boxWidth > 0 )
			{
				if( lineStart < segStart && lineAll + segInk > boxWidth + eps )
					closeLine( segStart );
				if( segInk > boxWidth + eps )
				{
					// A single word wider than the box: break inside the word.
					pass.overflow = true;
					float w       = lineAll;
					for( int k = segStart; k <= i; ++k )
					{
						float a = adv( k );
						if( k > lineStart && w + a - tracking > boxWidth + eps && !g[ k ].space )
						{
							closeLine( k );
							w = 0;
						}
						w += a;
					}
					lineAll  = w;
					segStart = i + 1;
					continue;
				}
			}
			lineAll += segAll;
			segStart = i + 1;
		}
		closeLine( n );
		pass.lines.back().lastOfPara = true;
	}
	return pass;
}

LayoutResult place( const LayoutParams& p, const std::vector< ShapedPara >& paras, float size, bool allowWrap )
{
	LayoutResult r;
	r.sizePx         = size;
	const float boxW = allowWrap ? p.boxWidth : 0.0f;
	Pass pass        = breakLines( paras, size, boxW );
	r.overflow       = pass.overflow;
	r.numLines       = (int)pass.lines.size();
	if( pass.lines.empty() )
		return r;

	// Baselines: every line advances by the leading of its own size. Where the
	// style changes (line 1 -> "Line 2", "Line 2" -> "Line 3+") the new style's
	// spacing is added once.
	std::vector< float > baseline( pass.lines.size(), 0.0f );
	for( size_t li = 1; li < pass.lines.size(); ++li )
	{
		const ShapedPara& para = paras[ pass.lines[ li ].para ];
		const ShapedPara& prev = paras[ pass.lines[ li - 1 ].para ];
		const bool styleStarts = pass.lines[ li ].para != pass.lines[ li - 1 ].para && para.style != prev.style;
		baseline[ li ]         = baseline[ li - 1 ] + size * para.scale * p.leadingPercent / 100.0f + ( styleStarts ? para.spacingEm * size : 0.0f );
	}
	const ShapedPara& firstPara = paras[ pass.lines.front().para ];
	const ShapedPara& lastPara  = paras[ pass.lines.back().para ];
	const float blockW          = p.boxWidth > 0 ? p.boxWidth : pass.maxWidth;
	const float top             = -firstPara.face->metrics().ascender * size * firstPara.scale;
	const float lastBaseline    = baseline.back();
	const float bottom          = lastBaseline - lastPara.face->metrics().descender * size * lastPara.scale;
	if( p.boxHeight > 0 && bottom - top > p.boxHeight + 0.01f )
		r.overflow = true;

	int charIndex = 0, wordIndex = -1;
	bool prevSpace = true;
	for( int li = 0; li < (int)pass.lines.size(); ++li )
	{
		const Line& line     = pass.lines[ li ];
		const ShapedPara& g  = paras[ line.para ];
		const float gSize    = size * g.scale;
		const float tracking = size * g.trackingEm;

		float x          = 0;
		float extraSpace = 0;
		switch( p.align )
		{
		case Align::Left:
			x = 0;
			break;
		case Align::Center:
			x = ( blockW - line.inkWidth ) * 0.5f;
			break;
		case Align::Right:
			x = blockW - line.inkWidth;
			break;
		case Align::Justify: {
			x = 0;
			if( p.boxWidth > 0 && !line.lastOfPara )
			{
				int lastInk = line.end - 1;
				while( lastInk >= line.begin && g[ lastInk ].space )
					--lastInk;
				int spaces = 0;
				for( int k = line.begin; k < lastInk; ++k )
					if( g[ k ].space )
						++spaces;
				if( spaces > 0 )
					extraSpace = std::max( 0.0f, ( p.boxWidth - line.inkWidth ) / spaces );
			}
			break;
		}
		}

		const float y = baseline[ li ];
		for( int k = line.begin; k < line.end; ++k )
		{
			const ShapedGlyph& sg = g[ k ];
			LaidGlyph lg;
			lg.glyphId   = sg.gid;
			lg.x         = x + sg.xOffEm * gSize;
			lg.y         = y - sg.yOffEm * gSize;
			lg.style     = g.style;
			lg.scale     = g.scale;
			lg.lineIndex = li;
			lg.visible   = !sg.space;
			lg.colorIndex = sg.colorIndex;
			if( lg.visible )
			{
				if( prevSpace )
					++wordIndex;
				lg.charIndex = charIndex++;
				lg.wordIndex = wordIndex;
				r.glyphs.push_back( lg );
			}
			prevSpace = sg.space;
			x += sg.advEm * gSize + tracking + ( sg.space ? extraSpace : 0.0f );
		}
		prevSpace = true;// a new line always starts a new word
	}
	r.numChars = charIndex;
	r.numWords = wordIndex + 1;

	// Anchor
	float ax = 0;
	AnchorH ah = p.anchorH;
	if( ah == AnchorH::Auto )
	{
		// Point text: the anchor sits on the alignment side, so lines grow away from it.
		// Area text (fixed box): the box is centred on the anchor.
		if( p.boxWidth > 0 )
			ah = AnchorH::Center;
		else
			ah = p.align == Align::Center ? AnchorH::Center : p.align == Align::Right ? AnchorH::Right : AnchorH::Left;
	}
	switch( ah )
	{
	case AnchorH::Left:
	case AnchorH::Auto:
		ax = 0;
		break;
	case AnchorH::Center:
		ax = blockW * 0.5f;
		break;
	case AnchorH::Right:
		ax = blockW;
		break;
	}
	float ay = 0;
	switch( p.anchorV )
	{
	case AnchorV::Top:
		ay = top;
		break;
	case AnchorV::Middle:
		ay = ( top + bottom ) * 0.5f;
		break;
	case AnchorV::Bottom:
		ay = bottom;
		break;
	case AnchorV::FirstBaseline:
		ay = 0;
		break;
	case AnchorV::LastBaseline:
		ay = lastBaseline;
		break;
	}
	for( LaidGlyph& lg : r.glyphs )
	{
		lg.x -= ax;
		lg.y -= ay;
	}
	r.left   = -ax;
	r.right  = blockW - ax;
	r.top    = top - ay;
	r.bottom = bottom - ay;
	return r;
}

LayoutResult layoutShaped( const LayoutParams& p, const std::vector< ShapedPara >& paras )
{
	float size                      = std::max( 0.5f, p.sizePx );

	switch( p.fit )
	{
	case FitMode::Off:
		return place( p, paras, size, true );

	case FitMode::FitWidth: {
		if( p.boxWidth <= 0 )
			return place( p, paras, size, true );
		// Advances and tracking both scale linearly with the size, so one
		// measurement gives the exact size at which the widest line fills the box.
		const float widest = breakLines( paras, size, 0 ).maxWidth;
		if( widest <= 0 )
			return place( p, paras, size, false );
		return place( p, paras, size * p.boxWidth / widest, false );
	}

	case FitMode::ShrinkToBox: {
		if( p.boxWidth <= 0 && p.boxHeight <= 0 )
			return place( p, paras, size, true );
		LayoutResult full = place( p, paras, size, true );
		if( !full.overflow )
			return full;
		float lo = size * 0.02f, hi = size;
		LayoutResult best = place( p, paras, lo, true );
		for( int iter = 0; iter < 14; ++iter )
		{
			float mid         = 0.5f * ( lo + hi );
			LayoutResult test = place( p, paras, mid, true );
			if( test.overflow )
				hi = mid;
			else
			{
				lo   = mid;
				best = std::move( test );
			}
		}
		return best;
	}
	}
	return place( p, paras, size, true );
}

}//namespace

int lineStyleOf( int index, const LineStyle* styles )
{
	if( index <= 0 )
		return 0;
	if( index == 1 )
		return styles[ 1 ].own ? 1 : 0;
	return styles[ 2 ].own ? 2 : styles[ 1 ].own ? 1 : 0;
}

LayoutResult layoutText( const FontFace& face, const LayoutParams& p )
{
	std::vector< Rgb > palette;
	std::vector< ShapedPara > paras = shape( face, p, palette );
	LayoutResult r                  = layoutShaped( p, paras );
	r.palette                       = std::move( palette );
	return r;
}

}//namespace textplus
