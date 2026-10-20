// One-shot loaders: animator presets and looks (whole styles).
#include "TextSource.h"

#include <algorithm>
#include <cmath>

namespace textplus
{
namespace
{
// Preset menu: values are stable (scripts and saved compositions), the order is for reading.
enum Preset
{
	PRESET_NONE            = 0,
	PRESET_TYPEWRITER      = 1,
	PRESET_FADE_IN         = 2,
	PRESET_RISE_IN         = 3,
	PRESET_DROP_IN         = 4,
	PRESET_POP_IN          = 5,
	PRESET_SPIN_IN         = 6,
	PRESET_WORDS_SLIDE_IN  = 7,
	PRESET_FADE_OUT        = 8,
	PRESET_WAVE            = 9,
	PRESET_JITTER          = 10,
	PRESET_COLOR_SWEEP     = 11,
	PRESET_RESET           = 12,
	PRESET_WIPE_IN         = 13,
	PRESET_DECODE          = 14,
	PRESET_LOWER_THIRD_IN  = 15,
	PRESET_LOWER_THIRD_OUT = 16,
	PRESET_GLITCH          = 17,
	PRESET_IN_OUT          = 18,
	// 2.0
	PRESET_BLUR_IN         = 19,
	PRESET_TRACKING_IN     = 20,
	PRESET_BOUNCE_IN       = 21,
	PRESET_SKEW_IN         = 22,
	PRESET_WEIGHT_WAVE     = 23,
	PRESET_ELASTIC_POP     = 24,
	PRESET_LAST            = 24
};

enum Look
{
	LOOK_NONE,
	LOOK_CLEAN,
	LOOK_OUTLINE,
	LOOK_HOLLOW,
	LOOK_STICKER,
	LOOK_NEON,
	LOOK_COMIC,
	LOOK_LOWER_THIRD,
	LOOK_NEWS_BAR,
	LOOK_CHROME_3D,
	LOOK_RETRO_3D,
	LOOK_LED_SIGN,
	LOOK_GRADIENT_GLOW,
	LOOK_STRIPES,
	LOOK_LAST = LOOK_STRIPES
};
}//namespace

const std::vector< ffglkit::OptionItem > kPresetItems = {
	{ "Load preset...", PRESET_NONE },
	{ "Typewriter", PRESET_TYPEWRITER, true },
	{ "Fade In", PRESET_FADE_IN },
	{ "Rise In", PRESET_RISE_IN },
	{ "Drop In", PRESET_DROP_IN },
	{ "Bounce In", PRESET_BOUNCE_IN },
	{ "Pop In", PRESET_POP_IN },
	{ "Elastic Pop", PRESET_ELASTIC_POP },
	{ "Spin In", PRESET_SPIN_IN },
	{ "Words Slide In", PRESET_WORDS_SLIDE_IN },
	{ "Skew In", PRESET_SKEW_IN },
	{ "Blur In", PRESET_BLUR_IN },
	{ "Tracking In", PRESET_TRACKING_IN },
	{ "Wipe In", PRESET_WIPE_IN },
	{ "Decode", PRESET_DECODE },
	{ "Fade Out", PRESET_FADE_OUT },
	{ "Fade In, Hold, Out", PRESET_IN_OUT },
	{ "Lower Third In (both animators)", PRESET_LOWER_THIRD_IN, true },
	{ "Lower Third Out (both animators)", PRESET_LOWER_THIRD_OUT },
	{ "Wave", PRESET_WAVE, true },
	{ "Weight Wave", PRESET_WEIGHT_WAVE },
	{ "Jitter", PRESET_JITTER },
	{ "Glitch", PRESET_GLITCH },
	{ "Color Sweep", PRESET_COLOR_SWEEP },
	{ "Reset", PRESET_RESET, true },
};

const std::vector< ffglkit::OptionItem > kLookItems = {
	{ "Load look...", LOOK_NONE },
	{ "Clean", LOOK_CLEAN, true },
	{ "Outline", LOOK_OUTLINE },
	{ "Hollow", LOOK_HOLLOW },
	{ "Sticker", LOOK_STICKER },
	{ "Comic", LOOK_COMIC },
	{ "Neon", LOOK_NEON, true },
	{ "LED Sign", LOOK_LED_SIGN },
	{ "Gradient Glow", LOOK_GRADIENT_GLOW },
	{ "Stripes", LOOK_STRIPES },
	{ "Lower Third", LOOK_LOWER_THIRD, true },
	{ "News Bar", LOOK_NEWS_BAR },
	{ "Chrome 3D", LOOK_CHROME_3D, true },
	{ "Retro 3D", LOOK_RETRO_3D },
};

// ===========================================================================
// Animator presets
// ===========================================================================
void TextSource::applyPreset( int a, int preset )
{
	struct P
	{
		AnimMode mode;
		AnimUnit unit;
		AnimEase ease;
		float spread, offX, offY, scale, rot, opacity, colorAlpha;
		float wipe        = 0;
		WipeFrom wipeFrom = WipeFrom::Left;
		float scramble    = 0;
		Charset charset   = Charset::Letters;
		AnimTarget target = AnimTarget::Text;
		float hold        = 50;
		float skew = 0, spacing = 0, thickness = 0, blur = 0;
		AnimOrder order = AnimOrder::Forward;
	};
	// Each preset describes the "away" state; Progress 0 -> 1 plays it in (or out).
	// Offsets use the UI convention: Offset Y > 0 is up, like Position Y.
	const float size = values[ P_SIZE ];
	auto make = [ & ]( int id ) -> P {
		switch( id )
		{
		case PRESET_TYPEWRITER: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Linear, 0, 0, 0, 100, 0, 0, 0 };
		case PRESET_FADE_IN: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Smooth, 4, 0, 0, 100, 0, 0, 0 };
		case PRESET_RISE_IN: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::EaseOut, 4, 0, -120, 100, 0, 0, 0 };
		case PRESET_DROP_IN: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Overshoot, 3, 0, 160, 100, 0, 0, 0 };
		case PRESET_POP_IN: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Overshoot, 3, 0, 0, 0, 0, 0, 0 };
		case PRESET_SPIN_IN: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::EaseOut, 4, 0, 0, 0, 180, 0, 0 };
		case PRESET_WORDS_SLIDE_IN: return P{ AnimMode::SequenceIn, AnimUnit::Word, AnimEase::EaseOut, 1.5f, -220, 0, 100, 0, 0, 0 };
		case PRESET_WIPE_IN: {
			P p{ AnimMode::SequenceIn, AnimUnit::Line, AnimEase::EaseOut, 1.5f, 0, 0, 100, 0, 100, 0 };
			p.wipe = 100;
			return p;
		}
		case PRESET_DECODE: {
			P p{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Linear, 8, 0, 0, 100, 0, 100, 0 };
			p.scramble = 100;
			return p;
		}
		case PRESET_FADE_OUT: return P{ AnimMode::SequenceOut, AnimUnit::Character, AnimEase::Smooth, 4, 0, 0, 100, 0, 0, 0 };
		case PRESET_IN_OUT: {
			P p{ AnimMode::SequenceInOut, AnimUnit::Character, AnimEase::Smooth, 4, 0, -40, 100, 0, 0, 0 };
			p.hold = 60;
			return p;
		}
		case PRESET_WAVE: return P{ AnimMode::Wave, AnimUnit::Character, AnimEase::Smooth, 8, 0, 30, 100, 0, 100, 0 };
		case PRESET_JITTER: return P{ AnimMode::Random, AnimUnit::Character, AnimEase::Smooth, 8, 6, 6, 100, 8, 100, 0 };
		case PRESET_GLITCH: {
			P p{ AnimMode::Constant, AnimUnit::Character, AnimEase::Smooth, 3, 0, 0, 100, 0, 100, 0 };
			p.scramble = 12;
			p.charset  = Charset::Symbols;
			return p;
		}
		case PRESET_COLOR_SWEEP: return P{ AnimMode::Wave, AnimUnit::Character, AnimEase::Smooth, 12, 0, 0, 100, 0, 100, 1 };
		case PRESET_BLUR_IN: {
			P p{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Smooth, 6, 0, 0, 115, 0, 0, 0 };
			p.blur = 40;
			return p;
		}
		case PRESET_TRACKING_IN: {
			// Cinematic: the letters close up from wide spacing while fading in
			P p{ AnimMode::SequenceIn, AnimUnit::Line, AnimEase::EaseOut, 1, 0, 0, 100, 0, 0, 0 };
			p.spacing = std::round( std::max( 8.0f, size * 0.35f ) );
			return p;
		}
		case PRESET_BOUNCE_IN: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Bounce, 4, 0, 260, 100, 0, 100, 0 };
		case PRESET_SKEW_IN: {
			P p{ AnimMode::SequenceIn, AnimUnit::Word, AnimEase::EaseOut, 1.5f, -260, 0, 100, 0, 0, 0 };
			p.skew = -35;
			return p;
		}
		case PRESET_WEIGHT_WAVE: {
			P p{ AnimMode::Wave, AnimUnit::Character, AnimEase::Smooth, 10, 0, 0, 100, 0, 100, 0 };
			p.thickness = 4;
			return p;
		}
		case PRESET_ELASTIC_POP: return P{ AnimMode::SequenceIn, AnimUnit::Character, AnimEase::Elastic, 3, 0, 0, 0, 0, 0, 0 };
		default: return P{ AnimMode::Off, AnimUnit::Character, AnimEase::Smooth, 3, 0, 0, 100, 0, 100, 0 };
		}
	};
	auto write = [ & ]( int animator, const P& p ) {
		auto id = [ & ]( AnimParam ap ) { return animParam( animator, ap ); };
		setValue( id( AP_MODE ), (float)p.mode );
		setValue( id( AP_TARGET ), (float)p.target );
		setValue( id( AP_UNIT ), (float)p.unit );
		setValue( id( AP_ORDER ), (float)p.order );
		setValue( id( AP_EASE ), (float)p.ease );
		setValue( id( AP_SPREAD ), p.spread );
		setValue( id( AP_HOLD ), p.hold );
		setValue( id( AP_RANGE_START ), 0.0f );
		setValue( id( AP_RANGE_END ), 100.0f );
		setValue( id( AP_OFFSET_X ), p.offX );
		setValue( id( AP_OFFSET_Y ), p.offY );
		setValue( id( AP_SCALE ), p.scale );
		setValue( id( AP_ROTATION ), p.rot );
		setValue( id( AP_SKEW ), p.skew );
		setValue( id( AP_SPACING ), p.spacing );
		setValue( id( AP_OPACITY ), p.opacity );
		setValue( id( AP_THICKNESS ), p.thickness );
		setValue( id( AP_BLUR ), p.blur );
		setValue( id( AP_WIPE ), p.wipe );
		setValue( id( AP_WIPE_FROM ), (float)p.wipeFrom );
		setValue( id( AP_SCRAMBLE ), p.scramble );
		setValue( id( AP_CHARSET ), (float)p.charset );
		setValue( id( AP_COLOR_A ), p.colorAlpha );
		if( p.mode == AnimMode::Wave || p.mode == AnimMode::Random )
			setValue( id( AP_PROGRESS ), 0.0f );// the phase; animate it (e.g. BPM sync) to make it move
		else if( p.mode == AnimMode::Constant )
			setValue( id( AP_PROGRESS ), 1.0f );
		else if( p.mode == AnimMode::Off )
			setValue( id( AP_PROGRESS ), 1.0f );
		refreshShownProperties( animator, false );
	};

	if( preset == PRESET_LOWER_THIRD_IN || preset == PRESET_LOWER_THIRD_OUT )
	{
		// Plate wipes open from the left, the text follows character by character
		// (and on the way out: text first, then the plate). Sets both animators.
		const bool in = preset == PRESET_LOWER_THIRD_IN;
		P plateP{ in ? AnimMode::SequenceIn : AnimMode::SequenceOut, AnimUnit::Line, AnimEase::EaseOut, 1.5f, 0, 0, 100, 0, 100, 0 };
		plateP.wipe   = 100;
		plateP.target = AnimTarget::Plate;
		P textP{ in ? AnimMode::SequenceIn : AnimMode::SequenceOut, AnimUnit::Character, AnimEase::EaseOut, 5, -30, 0, 100, 0, 0, 0 };
		write( in ? 0 : 1, plateP );
		write( in ? 1 : 0, textP );
		if( (int)values[ P_PLATE ] == PLATE_OFF )
			setValue( P_PLATE, (float)PLATE_LINES );
		if( values[ P_REPLAY_STAGGER ] <= 0.0f )
			setValue( P_REPLAY_STAGGER, 35.0f );
		if( !in )
			for( int k = 0; k < 2; ++k )
				setValue( animParam( k, AP_PROGRESS ), 0.0f );
		for( int k = 0; k < 2; ++k )
			setValue( animParam( k, AP_PRESET ), 0.0f );
		updateVisibility();
		return;
	}
	if( preset <= PRESET_NONE || preset > PRESET_LAST )
		return;
	write( a, make( preset ) );
	// The menu is a one-shot loader: back to "Load preset..." so loading a
	// composition never re-applies a preset over tweaked values.
	setValue( animParam( a, AP_PRESET ), 0.0f );
	updateVisibility();
}

// ===========================================================================
// Looks: every style parameter back to its default, then the look's values.
// Sizes follow the font size, so a look works at any size.
// ===========================================================================
void TextSource::applyLook( int look )
{
	if( look <= LOOK_NONE || look > LOOK_LAST )
		return;
	const float size = values[ P_SIZE ];
	auto px          = [ & ]( float share ) { return std::round( std::max( 1.0f, size * share ) ); };
	auto color       = [ & ]( unsigned hue, float h, float s, float b, float a ) {
		setValue( hue, h );
		setValue( hue + 1, s );
		setValue( hue + 2, b );
		setValue( hue + 3, a );
	};
	const unsigned styleParams[] = { P_FILL_MODE, P_FILL_H, P_FILL_S, P_FILL_B, P_FILL_A, P_FILL2_H, P_FILL2_S, P_FILL2_B, P_FILL2_A, P_GRAD_ANGLE,
									 P_PATTERN_SCALE, P_PATTERN_ANGLE, P_PATTERN_OFFSET, P_OUTLINE_MODE, P_OL1_WIDTH, P_OL1_POSITION, P_OL_CORNERS,
									 P_OL1_H, P_OL1_S, P_OL1_B, P_OL1_A, P_OL2_WIDTH, P_OL2_H, P_OL2_S, P_OL2_B, P_OL2_A, P_PLATE, P_PLATE_H, P_PLATE_S,
									 P_PLATE_B, P_PLATE_A, P_PLATE_PAD_X, P_PLATE_PAD_Y, P_PLATE_RADIUS, P_PLATE_SLANT, P_PLATE_WIDTH, P_PLATE_BORDER,
									 P_PLATE_BORDER_H, P_PLATE_BORDER_S, P_PLATE_BORDER_B, P_PLATE_BORDER_A, P_SHADOW_MODE, P_SH_H, P_SH_S, P_SH_B,
									 P_SH_A, P_SH_DISTANCE, P_SH_ANGLE, P_SH_SOFTNESS, P_SH_SPREAD, P_ISH_DISTANCE, P_ISH_SOFTNESS, P_ISH_H, P_ISH_S,
									 P_ISH_B, P_ISH_A, P_GLOW_MODE, P_GL_H, P_GL_S, P_GL_B, P_GL_A, P_GL_RADIUS, P_GL_STRENGTH, P_IGL_RADIUS, P_IGL_H,
									 P_IGL_S, P_IGL_B, P_IGL_A, P_3D, P_TILT_X, P_TILT_Y, P_PERSPECTIVE, P_DEPTH, P_BEVEL, P_SIDE_H, P_SIDE_S, P_SIDE_B,
									 P_SIDE_A, P_LIGHT_ANGLE, P_LIGHT_HEIGHT, P_SHADING, P_GLOSS, P_THICKNESS };
	for( unsigned id : styleParams )
		resetToDefault( id );

	switch( look )
	{
	case LOOK_CLEAN:
		break;
	case LOOK_OUTLINE:
		setValue( P_OUTLINE_MODE, OUTLINE_ONE );
		setValue( P_OL1_WIDTH, px( 0.05f ) );
		break;
	case LOOK_HOLLOW:
		setValue( P_FILL_A, 0.0f );
		setValue( P_OUTLINE_MODE, OUTLINE_ONE );
		setValue( P_OL1_WIDTH, px( 0.03f ) );
		setValue( P_OL1_POSITION, 1.0f );// centre
		color( P_OL1_H, 0.0f, 0.0f, 1.0f, 1.0f );
		break;
	case LOOK_STICKER:
		color( P_FILL_H, 0.97f, 0.75f, 1.0f, 1.0f );
		setValue( P_OUTLINE_MODE, OUTLINE_TWO );
		setValue( P_OL1_WIDTH, px( 0.09f ) );
		color( P_OL1_H, 0.0f, 0.0f, 1.0f, 1.0f );
		setValue( P_OL2_WIDTH, px( 0.015f ) );
		color( P_OL2_H, 0.0f, 0.0f, 0.75f, 1.0f );
		setValue( P_SHADOW_MODE, EFFECT_OUTER );
		setValue( P_SH_DISTANCE, px( 0.04f ) );
		setValue( P_SH_SOFTNESS, px( 0.08f ) );
		color( P_SH_H, 0.0f, 0.0f, 0.0f, 0.45f );
		break;
	case LOOK_COMIC:
		color( P_FILL_H, 0.14f, 0.9f, 1.0f, 1.0f );
		color( P_FILL2_H, 0.08f, 0.95f, 1.0f, 1.0f );
		setValue( P_FILL_MODE, FILL_HALFTONE );
		setValue( P_PATTERN_SCALE, px( 0.07f ) );
		setValue( P_PATTERN_ANGLE, -60.0f );
		setValue( P_OUTLINE_MODE, OUTLINE_ONE );
		setValue( P_OL1_WIDTH, px( 0.045f ) );
		setValue( P_OL_CORNERS, 1.0f );
		setValue( P_SHADOW_MODE, EFFECT_OUTER );
		setValue( P_SH_DISTANCE, px( 0.07f ) );
		setValue( P_SH_SOFTNESS, 0.0f );
		color( P_SH_H, 0.0f, 0.0f, 0.0f, 1.0f );
		break;
	case LOOK_NEON:
		color( P_FILL_H, 0.92f, 0.12f, 1.0f, 1.0f );
		setValue( P_OUTLINE_MODE, OUTLINE_ONE );
		setValue( P_OL1_WIDTH, px( 0.02f ) );
		color( P_OL1_H, 0.92f, 0.85f, 1.0f, 1.0f );
		setValue( P_GLOW_MODE, EFFECT_BOTH );
		color( P_GL_H, 0.92f, 0.9f, 1.0f, 1.0f );
		setValue( P_GL_RADIUS, px( 0.35f ) );
		setValue( P_GL_STRENGTH, 1.2f );
		setValue( P_IGL_RADIUS, px( 0.05f ) );
		color( P_IGL_H, 0.92f, 0.8f, 1.0f, 0.8f );
		break;
	case LOOK_LED_SIGN:
		color( P_FILL_H, 0.07f, 0.9f, 1.0f, 1.0f );
		setValue( P_FILL_MODE, FILL_LED );
		setValue( P_PATTERN_SCALE, px( 0.06f ) );
		setValue( P_GLOW_MODE, EFFECT_OUTER );
		color( P_GL_H, 0.06f, 0.9f, 1.0f, 0.8f );
		setValue( P_GL_RADIUS, px( 0.12f ) );
		setValue( P_GL_STRENGTH, 0.6f );
		break;
	case LOOK_GRADIENT_GLOW:
		setValue( P_FILL_MODE, FILL_GRADIENT_BLOCK );
		color( P_FILL_H, 0.83f, 0.8f, 1.0f, 1.0f );
		color( P_FILL2_H, 0.52f, 0.85f, 1.0f, 1.0f );
		setValue( P_GRAD_ANGLE, -20.0f );
		setValue( P_GLOW_MODE, EFFECT_OUTER );
		color( P_GL_H, 0.7f, 0.8f, 1.0f, 1.0f );
		setValue( P_GL_RADIUS, px( 0.3f ) );
		setValue( P_GL_STRENGTH, 0.8f );
		break;
	case LOOK_STRIPES:
		setValue( P_FILL_MODE, FILL_STRIPES );
		color( P_FILL_H, 0.0f, 0.0f, 1.0f, 1.0f );
		color( P_FILL2_H, 0.0f, 0.85f, 0.95f, 1.0f );
		setValue( P_PATTERN_SCALE, px( 0.12f ) );
		setValue( P_PATTERN_ANGLE, 60.0f );
		setValue( P_OUTLINE_MODE, OUTLINE_ONE );
		setValue( P_OL1_WIDTH, px( 0.03f ) );
		color( P_OL1_H, 0.0f, 0.0f, 0.05f, 1.0f );
		break;
	case LOOK_LOWER_THIRD:
		setValue( P_PLATE, PLATE_LINES );
		color( P_PLATE_H, 0.6f, 0.35f, 0.12f, 0.92f );
		setValue( P_PLATE_PAD_X, 40.0f );
		setValue( P_PLATE_PAD_Y, 12.0f );
		setValue( P_PLATE_RADIUS, 6.0f );
		setValue( P_SHADOW_MODE, EFFECT_OUTER );
		setValue( P_SH_DISTANCE, px( 0.03f ) );
		setValue( P_SH_SOFTNESS, px( 0.12f ) );
		color( P_SH_H, 0.0f, 0.0f, 0.0f, 0.5f );
		break;
	case LOOK_NEWS_BAR:
		setValue( P_PLATE, PLATE_BLOCK );
		color( P_PLATE_H, 0.99f, 0.85f, 0.8f, 1.0f );
		setValue( P_PLATE_PAD_X, 50.0f );
		setValue( P_PLATE_PAD_Y, 14.0f );
		setValue( P_PLATE_SLANT, -12.0f );
		setValue( P_PLATE_BORDER, px( 0.025f ) );
		color( P_PLATE_BORDER_H, 0.0f, 0.0f, 1.0f, 1.0f );
		setValue( P_SHADOW_MODE, EFFECT_OUTER );
		setValue( P_SH_DISTANCE, px( 0.05f ) );
		setValue( P_SH_SOFTNESS, px( 0.06f ) );
		break;
	case LOOK_CHROME_3D:
		setValue( P_FILL_MODE, FILL_GRADIENT_BLOCK );
		color( P_FILL_H, 0.6f, 0.08f, 1.0f, 1.0f );
		color( P_FILL2_H, 0.6f, 0.15f, 0.55f, 1.0f );
		setValue( P_GRAD_ANGLE, -90.0f );
		setValue( P_3D, THREED_EXTRUDE );
		setValue( P_TILT_X, 18.0f );
		setValue( P_TILT_Y, -22.0f );
		setValue( P_DEPTH, px( 0.22f ) );
		setValue( P_BEVEL, px( 0.035f ) );
		color( P_SIDE_H, 0.6f, 0.12f, 0.55f, 1.0f );
		setValue( P_GLOSS, 70.0f );
		setValue( P_SHADING, 80.0f );
		break;
	case LOOK_RETRO_3D:
		color( P_FILL_H, 0.12f, 0.85f, 1.0f, 1.0f );
		setValue( P_OUTLINE_MODE, OUTLINE_ONE );
		setValue( P_OL1_WIDTH, px( 0.025f ) );
		color( P_OL1_H, 0.97f, 0.7f, 0.95f, 1.0f );
		setValue( P_3D, THREED_EXTRUDE );
		setValue( P_TILT_X, 28.0f );
		setValue( P_TILT_Y, 18.0f );
		setValue( P_DEPTH, px( 0.4f ) );
		setValue( P_BEVEL, 0.0f );
		color( P_SIDE_H, 0.92f, 0.75f, 0.55f, 1.0f );
		setValue( P_SHADING, 45.0f );
		setValue( P_GLOSS, 10.0f );
		break;
	default:
		break;
	}
	updateVisibility();
}

}//namespace textplus
