#include "TextLayout.h"
#include "Utf8.h"

#include <algorithm>
#include <cmath>

#include <hb.h>

namespace kontur
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
};

using ShapedPara = std::vector< ShapedGlyph >;

std::vector< ShapedPara > shape( const FontFace& face, const LayoutParams& p )
{
	std::vector< ShapedPara > result;
	const float upem = (float)face.unitsPerEm();

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
	for( const std::string& para : splitParagraphs( p.text, p.pipeIsNewline ) )
	{
		ShapedPara out;
		if( !para.empty() )
		{
			hb_buffer_clear_contents( buf );
			hb_buffer_add_utf8( buf, para.c_str(), (int)para.size(), 0, (int)para.size() );
			hb_buffer_guess_segment_properties( buf );
			hb_shape( face.hbFont(), buf, features.data(), (unsigned)features.size() );

			unsigned n                = 0;
			hb_glyph_info_t* info     = hb_buffer_get_glyph_infos( buf, &n );
			hb_glyph_position_t* pos  = hb_buffer_get_glyph_positions( buf, &n );
			out.reserve( n );
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
				out.push_back( g );
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

Pass breakLines( const std::vector< ShapedPara >& paras, float size, float tracking, float boxWidth )
{
	Pass pass;
	const float eps = 0.01f;
	for( int pi = 0; pi < (int)paras.size(); ++pi )
	{
		const ShapedPara& g = paras[ pi ];
		const int n         = (int)g.size();
		auto adv            = [ & ]( int i ) { return g[ i ].advEm * size + tracking; };

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

LayoutResult place( const FontFace& face, const LayoutParams& p, const std::vector< ShapedPara >& paras, float size, bool allowWrap )
{
	LayoutResult r;
	r.sizePx             = size;
	const float tracking = p.trackingPermille * size / 1000.0f;
	const float boxW     = allowWrap ? p.boxWidth : 0.0f;
	Pass pass            = breakLines( paras, size, tracking, boxW );
	r.overflow           = pass.overflow;
	r.numLines           = (int)pass.lines.size();

	const FontMetrics& m = face.metrics();
	const float lineAdv  = size * p.leadingPercent / 100.0f;
	const float blockW   = p.boxWidth > 0 ? p.boxWidth : pass.maxWidth;
	const float top      = -m.ascender * size;
	const float bottom   = ( r.numLines - 1 ) * lineAdv - m.descender * size;
	if( p.boxHeight > 0 && bottom - top > p.boxHeight + 0.01f )
		r.overflow = true;

	int charIndex = 0, wordIndex = -1;
	bool prevSpace = true;
	for( int li = 0; li < (int)pass.lines.size(); ++li )
	{
		const Line& line    = pass.lines[ li ];
		const ShapedPara& g = paras[ line.para ];

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

		const float y = li * lineAdv;
		for( int k = line.begin; k < line.end; ++k )
		{
			const ShapedGlyph& sg = g[ k ];
			LaidGlyph lg;
			lg.glyphId   = sg.gid;
			lg.x         = x + sg.xOffEm * size;
			lg.y         = y - sg.yOffEm * size;
			lg.lineIndex = li;
			lg.visible   = !sg.space;
			if( lg.visible )
			{
				if( prevSpace )
					++wordIndex;
				lg.charIndex = charIndex++;
				lg.wordIndex = wordIndex;
				r.glyphs.push_back( lg );
			}
			prevSpace = sg.space;
			x += sg.advEm * size + tracking + ( sg.space ? extraSpace : 0.0f );
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
		ay = ( r.numLines - 1 ) * lineAdv;
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

}//namespace

LayoutResult layoutText( const FontFace& face, const LayoutParams& p )
{
	std::vector< ShapedPara > paras = shape( face, p );
	float size                      = std::max( 0.5f, p.sizePx );

	switch( p.fit )
	{
	case FitMode::Off:
		return place( face, p, paras, size, true );

	case FitMode::FitWidth: {
		if( p.boxWidth <= 0 )
			return place( face, p, paras, size, true );
		// Advances and tracking both scale linearly with the size, so one
		// measurement gives the exact size at which the widest line fills the box.
		const float tracking = p.trackingPermille * size / 1000.0f;
		const float widest   = breakLines( paras, size, tracking, 0 ).maxWidth;
		if( widest <= 0 )
			return place( face, p, paras, size, false );
		return place( face, p, paras, size * p.boxWidth / widest, false );
	}

	case FitMode::ShrinkToBox: {
		if( p.boxWidth <= 0 && p.boxHeight <= 0 )
			return place( face, p, paras, size, true );
		LayoutResult full = place( face, p, paras, size, true );
		if( !full.overflow )
			return full;
		float lo = size * 0.02f, hi = size;
		LayoutResult best = place( face, p, paras, lo, true );
		for( int iter = 0; iter < 14; ++iter )
		{
			float mid         = 0.5f * ( lo + hi );
			LayoutResult test = place( face, p, paras, mid, true );
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
	return place( face, p, paras, size, true );
}

}//namespace kontur
