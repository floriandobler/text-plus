// Parameters of Text+: definition, visibility, values from the host,
// migration of older compositions and the animators' property rows.
#include "TextSource.h"

#include "core/Clock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace textplus
{
namespace
{
const char* kGroupText     = "Text";
const char* kGroupFont     = "Font";
const char* kGroupLines    = "Lines";
const char* kGroupLayout   = "Layout";
const char* kGroupLook     = "Look";
const char* kGroupFill     = "Fill";
const char* kGroupOutline  = "Outline";
const char* kGroupPlate    = "Plate";
const char* kGroupShadow   = "Shadow";
const char* kGroupGlow     = "Glow";
const char* kGroup3D       = "3D";
const char* kGroupAnim[ kNumAnimators ] = { "Animator 1", "Animator 2", "Animator 3", "Animator 4" };
const char* kGroupAdvanced = "Advanced";

bool isSequence( AnimMode m )
{
	return m == AnimMode::SequenceIn || m == AnimMode::SequenceOut || m == AnimMode::SequenceInOut;
}
}//namespace


// ===========================================================================
// Definition
// ===========================================================================
void TextSource::defineParameters()
{
	using ffglkit::OptionItem;

	// ---- Text: where it comes from, how it is shown ----
	defOption( P_SOURCE, "Source", kGroupText, std::vector< OptionItem >{ { "Input", SOURCE_INPUT }, { "File", SOURCE_FILE }, { "Clock & Timer", SOURCE_CLOCK } },
			   SOURCE_INPUT );
	defOption( P_DISPLAY, "Display", kGroupText,
			   std::vector< OptionItem >{ { "Static", DISPLAY_STATIC },
										  { "Slides: Lines", DISPLAY_SLIDES_LINES, true },
										  { "Slides: Blocks (|| or empty line)", DISPLAY_SLIDES_BLOCKS },
										  { "Slides: Words", DISPLAY_SLIDES_WORDS },
										  { "Ticker", DISPLAY_TICKER, true },
										  { "Roll", DISPLAY_ROLL } },
			   DISPLAY_STATIC );
	text = "Text+";
	defText( P_TEXT, "Text", kGroupText, text );
	// Resolume's text fields are single-line: further lines get their own fields,
	// revealed one by one with "+ Line" (hidden fields keep their saved values).
	for( int i = 0; i < kExtraLines; ++i )
		defText( P_LINE_FIRST + (unsigned)i, "Text " + std::to_string( i + 2 ), kGroupText, "" );
	defEvent( P_ADD_LINE, "+ Line", kGroupText );
	defEvent( P_REMOVE_LINE, "- Line", kGroupText );
	defFile( P_TEXT_FILE, "Text File", kGroupText, { "txt", "text", "md", "csv", "tsv" } );
	defBool( P_CSV_HEADER, "CSV: Skip First Row", kGroupText, true );
	defText( P_ROW_TEMPLATE, "Row Template", kGroupText, "" );

	// Clock & Timer: the text fields are the template ("Doors open in {timer}")
	defOption( P_TIMER_MODE, "Timer Mode", kGroupText,
			   std::vector< OptionItem >{ { "Clock", TIMER_CLOCK },
										  { "Countdown to Time", TIMER_COUNTDOWN_TO },
										  { "Countdown", TIMER_COUNTDOWN },
										  { "Stopwatch", TIMER_STOPWATCH },
										  { "Counter", TIMER_COUNTER, true } },
			   TIMER_CLOCK );
	defText( P_TIMER_TARGET, "Target Time", kGroupText, timerTarget );
	defText( P_TIMER_DURATION, "Duration", kGroupText, timerDuration );
	defEvent( P_TIMER_START, "Start", kGroupText );
	defEvent( P_TIMER_STOP, "Stop", kGroupText );
	defEvent( P_TIMER_RESET, "Reset", kGroupText );
	defBool( P_TIMER_AUTOSTART, "Start with Clip", kGroupText, true );
	defOption( P_TIMER_FORMAT, "Time Format", kGroupText, { "Auto", "HH:MM:SS", "HH:MM", "MM:SS", "MM:SS.f", "Seconds", "Days" }, 0 );
	defOption( P_TIMER_AT_ZERO, "At Zero", kGroupText, { "Hold 0", "Hide", "Count Up" }, 0 );
	defRange( P_COUNT_FROM, "Count From", kGroupText, 0.0f, -1000000.0f, 1000000.0f );
	defRange( P_COUNT_TO, "Count To", kGroupText, 1000.0f, -1000000.0f, 1000000.0f );
	defRange( P_COUNT_PROGRESS, "Count", kGroupText, 1.0f, 0.0f, 1.0f );
	defOption( P_COUNT_DECIMALS, "Decimals", kGroupText, { "0", "1", "2", "3" }, 0 );
	defOption( P_NUMBER_FORMAT, "Number Format", kGroupText, { "1234.5", "1,234.5", "1.234,5", "1 234,5", "1'234.5" }, 1 );

	// Slides: 0..1 split into equal shares, so BPM sync on the slider cycles through
	// all slides and wraps back to the first, whatever the number of slides.
	defRange( P_SLIDE, "Slide", kGroupText, 0.0f, 0.0f, 1.0f );
	defEvent( P_NEXT_SLIDE, "Next Slide", kGroupText );
	defEvent( P_PREV_SLIDE, "Previous Slide", kGroupText );
	defOption( P_SLIDE_ORDER, "Slide Order", kGroupText, { "In Order", "Shuffle" }, 0 );
	defRange( P_REPLAY, "Replay on Slide Change", kGroupText, 0.0f, 0.0f, 16.0f );
	defOption( P_REPLAY_MODE, "Replay", kGroupText, { "In", "Out, then In" }, 0 );
	defRange( P_REPLAY_STAGGER, "Replay Stagger", kGroupText, 0.0f, 0.0f, 100.0f );

	// Ticker / Roll
	defRange( P_SCROLL_SPEED, "Scroll Speed", kGroupText, 150.0f, -2000.0f, 2000.0f );
	defRange( P_SCROLL_POSITION, "Scroll Position", kGroupText, 0.0f, 0.0f, 1.0f );
	defText( P_SEPARATOR, "Separator", kGroupText, separator );
	defBool( P_ROLL_LOOP, "Roll Loop", kGroupText, true );
	defRange( P_EDGE_FADE, "Edge Fade", kGroupText, 60.0f, 0.0f, 1000.0f );
	// Text+ 1.0 had "Slides" (Off / Each Line / Blocks); kept hidden so old
	// compositions open with the matching Display.
	defOption( P_SLIDES_LEGACY, "Slides", kGroupText, { "Off", "Each Line", "Blocks" }, 0 );

	// ---- Font: the menus; "Font" (Advanced) is the stored, typeable name the menus write into ----
	defOption( P_FAMILY, "Family", kGroupFont, std::vector< const char* >{ "(scanning fonts...)" }, 0 );
	defOption( P_STYLE, "Style", kGroupFont, std::vector< const char* >{ "-" }, 0 );
	defRange( P_SIZE, "Size", kGroupFont, 160.0f, 4.0f, 2000.0f );
	defRange( P_TRACKING, "Tracking", kGroupFont, 0.0f, -300.0f, 1000.0f );
	defOption( P_CASE, "Case", kGroupFont, { "Tt", "TT", "tt" }, 0 );

	// ---- Lines: leading, "Line 2" and "Line 3+" (every line from the third on) ----
	defRange( P_LEADING, "Leading", kGroupLines, 120.0f, 50.0f, 300.0f );
	defOption( P_L2_MODE, "L2 Style", kGroupLines, std::vector< const char* >{ "Same as Line 1", "Own" }, 0 );
	defOption( P_L3_MODE, "L3 Style", kGroupLines, std::vector< const char* >{ "Same as Line 2", "Own" }, 0 );
	defOption( P_LINE_EDIT, "Edit Line", kGroupLines, std::vector< const char* >{ "Line 2", "Line 3+" }, 0 );
	for( int l = 0; l < kNumLineStyles; ++l )
	{
		const std::string pre = l == 0 ? "L2 " : "L3 ";
		auto id               = [ & ]( LineParam p ) { return lineParam( l, p ); };
		defOption( id( LP_WEIGHT ), pre + "Weight", kGroupLines, std::vector< const char* >{ "-" }, 0 );
		defText( id( LP_FONT ), pre + "Font", kGroupLines, "" );
		defRange( id( LP_SIZE ), pre + "Size", kGroupLines, 100.0f, 5.0f, 400.0f );
		defColor( id( LP_COLOR_H ), pre + "Color", kGroupLines, 0.0f, 0.0f, 1.0f, 0.0f );
		defRange( id( LP_TRACKING ), pre + "Tracking", kGroupLines, 0.0f, -300.0f, 1000.0f );
		defRange( id( LP_SPACING ), pre + "Spacing", kGroupLines, 0.0f, -100.0f, 200.0f );
	}

	// ---- Layout ----
	defRange( P_POS_X, "Position X", kGroupLayout, 0.0f, -4000.0f, 4000.0f );
	defRange( P_POS_Y, "Position Y", kGroupLayout, 0.0f, -4000.0f, 4000.0f );
	defRange( P_ROTATION, "Rotation", kGroupLayout, 0.0f, -360.0f, 360.0f );
	defOption( P_ALIGN, "Align", kGroupLayout, { "Left", "Center", "Right", "Justify" }, 1 );
	defOption( P_WRAP, "Wrap", kGroupLayout, { "Off", "Box" }, 0 );
	defRange( P_BOX_WIDTH, "Box Width", kGroupLayout, 1000.0f, 10.0f, 8000.0f );
	defRange( P_BOX_HEIGHT, "Box Height", kGroupLayout, 0.0f, 0.0f, 8000.0f );
	defOption( P_FIT, "Auto Fit", kGroupLayout, { "Off", "Shrink to Box", "Fit Box Width", "Fit Each Line" }, 0 );
	defOption( P_PATH, "Path", kGroupLayout, { "Off", "Arc", "Circle", "Wave" }, 0 );
	defRange( P_PATH_RADIUS, "Path Radius", kGroupLayout, 500.0f, -4000.0f, 4000.0f );
	defRange( P_WAVE_HEIGHT, "Wave Height", kGroupLayout, 60.0f, -1000.0f, 1000.0f );
	defRange( P_WAVE_LENGTH, "Wave Length", kGroupLayout, 800.0f, 10.0f, 8000.0f );
	defRange( P_PATH_OFFSET, "Path Offset", kGroupLayout, 0.0f, 0.0f, 1.0f );

	// ---- Look: a one-shot loader for the whole style ----
	defOption( P_LOOK, "Look", kGroupLook, kLookItems, 0 );

	// ---- Fill ----
	defOption( P_VIDEO, "Video", kGroupFill, { "Off", "Over", "Fill Text", "Knockout" }, 2 );
	defOption( P_FILL_MODE, "Fill Mode", kGroupFill,
			   std::vector< OptionItem >{ { "Solid", FILL_SOLID },
										  { "Gradient (Block)", FILL_GRADIENT_BLOCK },
										  { "Gradient (Characters)", FILL_GRADIENT_CHARS },
										  { "Stripes", FILL_STRIPES, true },
										  { "Halftone", FILL_HALFTONE },
										  { "Checker", FILL_CHECKER },
										  { "Noise", FILL_NOISE },
										  { "LED", FILL_LED },
										  { "Alternate: Characters", FILL_ALT_CHARS, true },
										  { "Alternate: Words", FILL_ALT_WORDS },
										  { "Alternate: Lines", FILL_ALT_LINES } },
			   FILL_SOLID );
	defColor( P_FILL_H, "Fill Color", kGroupFill, 0.0f, 0.0f, 1.0f, 1.0f );
	defColor( P_FILL2_H, "Fill Color 2", kGroupFill, 0.58f, 0.7f, 1.0f, 1.0f );
	defRange( P_GRAD_ANGLE, "Gradient Angle", kGroupFill, 0.0f, -180.0f, 180.0f );
	defRange( P_PATTERN_SCALE, "Pattern Scale", kGroupFill, 24.0f, 2.0f, 1000.0f );
	defRange( P_PATTERN_ANGLE, "Pattern Angle", kGroupFill, 45.0f, -180.0f, 180.0f );
	defRange( P_PATTERN_OFFSET, "Pattern Offset", kGroupFill, 0.0f, 0.0f, 1.0f );

	// ---- Outline: band 1 and, outside it, band 2 ----
	defOption( P_OUTLINE_MODE, "Outline", kGroupOutline, { "Off", "One Band", "Two Bands" }, OUTLINE_OFF );
	defRange( P_OL1_WIDTH, "Outline Width", kGroupOutline, 0.0f, 0.0f, 400.0f );
	defOption( P_OL1_POSITION, "Outline Position", kGroupOutline, { "Outside", "Center", "Inside" }, 0 );
	defOption( P_OL_CORNERS, "Outline Corners", kGroupOutline, { "Round", "Sharp" }, 0 );
	defColor( P_OL1_H, "Outline Color", kGroupOutline, 0.0f, 0.0f, 0.0f, 1.0f );
	defRange( P_OL2_WIDTH, "Outline 2 Width", kGroupOutline, 0.0f, 0.0f, 400.0f );
	defColor( P_OL2_H, "Outline 2 Color", kGroupOutline, 0.0f, 0.0f, 1.0f, 1.0f );

	// ---- Plate: areas behind the block or behind each line ----
	defOption( P_PLATE, "Plate", kGroupPlate, { "Off", "Block", "Lines" }, PLATE_OFF );
	defColor( P_PLATE_H, "Plate Color", kGroupPlate, 0.0f, 0.0f, 0.08f, 0.85f );
	defRange( P_PLATE_PAD_X, "Plate Padding X", kGroupPlate, 35.0f, -50.0f, 300.0f );
	defRange( P_PLATE_PAD_Y, "Plate Padding Y", kGroupPlate, 10.0f, -50.0f, 300.0f );
	defRange( P_PLATE_RADIUS, "Plate Radius", kGroupPlate, 0.0f, 0.0f, 100.0f );
	defRange( P_PLATE_SLANT, "Plate Slant", kGroupPlate, 0.0f, -45.0f, 45.0f );
	defOption( P_PLATE_WIDTH, "Plate Width", kGroupPlate, { "Fit Text", "Equal", "Box" }, 0 );
	defRange( P_PLATE_BORDER, "Plate Border", kGroupPlate, 0.0f, 0.0f, 200.0f );
	defColor( P_PLATE_BORDER_H, "Plate Border Color", kGroupPlate, 0.0f, 0.0f, 1.0f, 1.0f );

	// ---- Shadow ----
	defOption( P_SHADOW_MODE, "Shadow", kGroupShadow, { "Off", "Drop", "Inner", "Drop + Inner" }, EFFECT_OFF );
	defColor( P_SH_H, "Shadow Color", kGroupShadow, 0.0f, 0.0f, 0.0f, 0.6f );
	defRange( P_SH_DISTANCE, "Shadow Distance", kGroupShadow, 0.0f, 0.0f, 400.0f );
	defRange( P_SH_ANGLE, "Shadow Angle", kGroupShadow, -45.0f, -180.0f, 180.0f );
	defRange( P_SH_SOFTNESS, "Shadow Softness", kGroupShadow, 8.0f, 0.0f, 200.0f );
	defRange( P_SH_SPREAD, "Shadow Spread", kGroupShadow, 0.0f, 0.0f, 200.0f );
	defRange( P_ISH_DISTANCE, "Inner Shadow Distance", kGroupShadow, 0.0f, 0.0f, 200.0f );
	defRange( P_ISH_SOFTNESS, "Inner Shadow Softness", kGroupShadow, 6.0f, 0.0f, 200.0f );
	defColor( P_ISH_H, "Inner Shadow Color", kGroupShadow, 0.0f, 0.0f, 0.0f, 0.6f );

	// ---- Glow ----
	defOption( P_GLOW_MODE, "Glow", kGroupGlow, { "Off", "Outer", "Inner", "Outer + Inner" }, EFFECT_OFF );
	defColor( P_GL_H, "Glow Color", kGroupGlow, 0.55f, 0.8f, 1.0f, 1.0f );
	defRange( P_GL_RADIUS, "Glow Radius", kGroupGlow, 0.0f, 0.0f, 400.0f );
	defRange( P_GL_STRENGTH, "Glow Strength", kGroupGlow, 1.0f, 0.0f, 2.0f );
	defRange( P_IGL_RADIUS, "Inner Glow Radius", kGroupGlow, 0.0f, 0.0f, 200.0f );
	defColor( P_IGL_H, "Inner Glow Color", kGroupGlow, 0.14f, 0.6f, 1.0f, 1.0f );

	// ---- 3D ----
	defOption( P_3D, "3D", kGroup3D, { "Off", "Tilt", "Extrude" }, THREED_OFF );
	defRange( P_TILT_X, "Tilt X", kGroup3D, 0.0f, -180.0f, 180.0f );
	defRange( P_TILT_Y, "Tilt Y", kGroup3D, 0.0f, -180.0f, 180.0f );
	defRange( P_PERSPECTIVE, "Perspective", kGroup3D, 40.0f, 0.0f, 100.0f );
	defRange( P_DEPTH, "Depth", kGroup3D, 40.0f, 0.0f, 2000.0f );
	defRange( P_BEVEL, "Bevel", kGroup3D, 4.0f, 0.0f, 200.0f );
	defColor( P_SIDE_H, "Side Color", kGroup3D, 0.0f, 0.0f, 0.4f, 1.0f );
	defRange( P_LIGHT_ANGLE, "Light Angle", kGroup3D, 135.0f, -180.0f, 180.0f );
	defRange( P_LIGHT_HEIGHT, "Light Height", kGroup3D, 45.0f, 0.0f, 90.0f );
	defRange( P_SHADING, "Shading", kGroup3D, 70.0f, 0.0f, 100.0f );
	defRange( P_GLOSS, "Gloss", kGroup3D, 30.0f, 0.0f, 100.0f );

	// ---- Animators. Resolume shows the internal names, so they carry the "A1 " prefix. ----
	const std::vector< OptionItem > addItems = { { "+ Property...", 0 },
												 { "Offset X", PROP_OFFSET_X, true },
												 { "Offset Y", PROP_OFFSET_Y },
												 { "Scale", PROP_SCALE },
												 { "Rotation", PROP_ROTATION },
												 { "Skew", PROP_SKEW },
												 { "Spacing", PROP_SPACING },
												 { "Opacity", PROP_OPACITY, true },
												 { "Color", PROP_COLOR },
												 { "Thickness", PROP_THICKNESS },
												 { "Blur", PROP_BLUR },
												 { "Wipe", PROP_WIPE, true },
												 { "Scramble", PROP_SCRAMBLE },
												 { "Range", PROP_RANGE, true },
												 { "Show All", PROP_SHOW_ALL, true },
												 { "Hide Unused", PROP_HIDE_UNUSED } };
	for( int a = 0; a < kNumAnimators; ++a )
	{
		const std::string pre = "A" + std::to_string( a + 1 ) + " ";
		const char* g         = kGroupAnim[ a ];
		auto id               = [ & ]( AnimParam p ) { return animParam( a, p ); };
		defOption( id( AP_PRESET ), pre + "Preset", g, kPresetItems, 0 );
		defOption( id( AP_MODE ), pre + "Mode", g,
				   std::vector< OptionItem >{ { "Off", (float)AnimMode::Off },
											  { "Sequence In", (float)AnimMode::SequenceIn, true },
											  { "Sequence Out", (float)AnimMode::SequenceOut },
											  { "Sequence In/Out", (float)AnimMode::SequenceInOut },
											  { "Wave", (float)AnimMode::Wave, true },
											  { "Random", (float)AnimMode::Random },
											  { "Constant", (float)AnimMode::Constant } },
				   (float)AnimMode::Off );
		defOption( id( AP_ADD ), pre + "+ Property", g, addItems, 0 );
		defRange( id( AP_PROGRESS ), pre + "Progress", g, 1.0f, 0.0f, 1.0f );
		defOption( id( AP_TARGET ), pre + "Target", g, { "Text", "Plate", "Text + Plate" }, 0 );
		defOption( id( AP_UNIT ), pre + "Unit", g, { "Character", "Word", "Line" }, 0 );
		defOption( id( AP_ORDER ), pre + "Order", g, { "Forward", "Reverse", "From Center", "From Edges", "Random", "Left to Right", "Top to Bottom" }, 0 );
		defRange( id( AP_SPREAD ), pre + "Spread", g, 3.0f, 0.0f, 50.0f );
		defRange( id( AP_HOLD ), pre + "Hold", g, 50.0f, 0.0f, 100.0f );
		defOption( id( AP_EASE ), pre + "Ease", g, { "Linear", "Smooth", "Ease Out", "Overshoot", "Ease In", "Ease In-Out", "Bounce", "Elastic" }, 1 );
		defRange( id( AP_RANGE_START ), pre + "Range Start", g, 0.0f, 0.0f, 100.0f );
		defRange( id( AP_RANGE_END ), pre + "Range End", g, 100.0f, 0.0f, 100.0f );
		defRange( id( AP_OFFSET_X ), pre + "Offset X", g, 0.0f, -2000.0f, 2000.0f );
		defRange( id( AP_OFFSET_Y ), pre + "Offset Y", g, 0.0f, -2000.0f, 2000.0f );
		defRange( id( AP_SCALE ), pre + "Scale", g, 100.0f, 0.0f, 400.0f );
		defRange( id( AP_ROTATION ), pre + "Rotation", g, 0.0f, -720.0f, 720.0f );
		defRange( id( AP_SKEW ), pre + "Skew", g, 0.0f, -75.0f, 75.0f );
		defRange( id( AP_SPACING ), pre + "Spacing", g, 0.0f, -500.0f, 2000.0f );
		defRange( id( AP_OPACITY ), pre + "Opacity", g, 100.0f, 0.0f, 100.0f );
		defColor( id( AP_COLOR_H ), pre + "Color", g, 0.0f, 0.8f, 1.0f, 0.0f );
		defRange( id( AP_THICKNESS ), pre + "Thickness", g, 0.0f, -20.0f, 20.0f );
		defRange( id( AP_BLUR ), pre + "Blur", g, 0.0f, 0.0f, 64.0f );
		defRange( id( AP_WIPE ), pre + "Wipe", g, 0.0f, 0.0f, 100.0f );
		defOption( id( AP_WIPE_FROM ), pre + "Wipe From", g, { "Left", "Right", "Center", "Top", "Bottom" }, 0 );
		defRange( id( AP_SCRAMBLE ), pre + "Scramble", g, 0.0f, 0.0f, 100.0f );
		defOption( id( AP_CHARSET ), pre + "Characters", g, { "Letters", "Digits", "Symbols", "Binary", "Same as Text" }, 0 );
	}

	// ---- Advanced: set once, rarely touched ----
	defOption( P_ADVANCED, "Advanced", kGroupAdvanced, { "Hide", "Show" }, 0 );
	defText( P_FONT, "Font", kGroupAdvanced, "" );
	defFile( P_FONT_FILE, "Font File", kGroupAdvanced, { "ttf", "otf", "ttc", "otc" } );
	defRange( P_THICKNESS, "Thickness", kGroupAdvanced, 0.0f, -10.0f, 10.0f );
	defOption( P_ANCHOR_H, "Anchor H", kGroupAdvanced, { "Auto", "Left", "Center", "Right" }, 0 );
	defOption( P_ANCHOR_V, "Anchor V", kGroupAdvanced, { "Top", "Middle", "Bottom", "First Baseline", "Last Baseline" }, 1 );
	defBool( P_KERNING, "Kerning", kGroupAdvanced, true );
	defBool( P_LIGATURES, "Ligatures", kGroupAdvanced, true );
	defText( P_FEATURES, "OpenType Features", kGroupAdvanced, "" );
	defBool( P_PIPE_NEWLINE, "| = New Line", kGroupAdvanced, true );
	defBool( P_MARKUP, "Color Tags", kGroupAdvanced, true );
	defText( P_OSC_PORT, "OSC Port", kGroupAdvanced, "" );
	defRange( P_EDGE, "Edge Softness", kGroupAdvanced, 1.0f, 0.25f, 4.0f );

	raiseVisibilityEvents = false;
	updateVisibility();// part of the initial parameter info
	raiseVisibilityEvents = true;
}

// ===========================================================================
// Visibility
// ===========================================================================
void TextSource::updateVisibility()
{
	// Rows appear only through explicit switches (menus, buttons, loading), never
	// through slider values, so the list never jumps under the mouse while dragging.
	const int src       = source();
	const int disp      = display();
	const bool typed    = src == SOURCE_INPUT || src == SOURCE_CLOCK;// the clock uses the fields as template
	const bool fromFile = src == SOURCE_FILE;
	show( P_TEXT, typed );
	for( int i = 0; i < kExtraLines; ++i )
		show( P_LINE_FIRST + (unsigned)i, typed && i < shownLines );
	show( P_ADD_LINE, typed && shownLines < kExtraLines );
	show( P_REMOVE_LINE, typed && shownLines > 0 );
	show( P_TEXT_FILE, fromFile );
	show( P_CSV_HEADER, fromFile && csvActive );
	show( P_ROW_TEMPLATE, fromFile && csvActive );

	const bool clock = src == SOURCE_CLOCK;
	const int tm     = (int)values[ P_TIMER_MODE ];
	const bool timed = tm == TIMER_COUNTDOWN || tm == TIMER_STOPWATCH;
	show( P_TIMER_MODE, clock );
	show( P_TIMER_TARGET, clock && tm == TIMER_COUNTDOWN_TO );
	show( P_TIMER_DURATION, clock && tm == TIMER_COUNTDOWN );
	show( P_TIMER_START, clock && timed );
	show( P_TIMER_STOP, clock && timed );
	show( P_TIMER_RESET, clock && timed );
	show( P_TIMER_AUTOSTART, clock && timed );
	show( P_TIMER_FORMAT, clock && tm != TIMER_COUNTER );
	show( P_TIMER_AT_ZERO, clock && ( tm == TIMER_COUNTDOWN || tm == TIMER_COUNTDOWN_TO ) );
	for( unsigned id : { P_COUNT_FROM, P_COUNT_TO, P_COUNT_PROGRESS, P_COUNT_DECIMALS, P_NUMBER_FORMAT } )
		show( id, clock && tm == TIMER_COUNTER );

	const bool slides = slidesOn();
	const bool replay = slides && values[ P_REPLAY ] > 0.0f;
	show( P_SLIDE, slides );
	show( P_NEXT_SLIDE, slides );
	show( P_PREV_SLIDE, slides );
	show( P_SLIDE_ORDER, slides );
	show( P_REPLAY, slides );
	show( P_REPLAY_MODE, slides );
	show( P_REPLAY_STAGGER, slides );

	const bool ticker = disp == DISPLAY_TICKER, roll = disp == DISPLAY_ROLL;
	show( P_SCROLL_SPEED, ticker || roll );
	show( P_SCROLL_POSITION, ticker || roll );
	show( P_SEPARATOR, ticker );
	show( P_ROLL_LOOP, roll );
	show( P_EDGE_FADE, ticker || roll );
	show( P_SLIDES_LEGACY, false );

	// Lines: with both styles own, "Edit Line" picks which six rows show
	const bool own2 = (int)values[ P_L2_MODE ] == 1, own3 = (int)values[ P_L3_MODE ] == 1;
	const bool both = own2 && own3;
	const int edit  = (int)values[ P_LINE_EDIT ];
	show( P_LINE_EDIT, both );
	for( int l = 0; l < kNumLineStyles; ++l )
	{
		const bool visible = ( l == 0 ? own2 : own3 ) && ( !both || edit == l );
		for( LineParam p : { LP_WEIGHT, LP_FONT, LP_SIZE, LP_COLOR_H, LP_TRACKING, LP_SPACING } )
			show( lineParam( l, p ), visible );
	}

	const bool box = (int)values[ P_WRAP ] == 1;
	show( P_BOX_WIDTH, box );
	show( P_BOX_HEIGHT, box );
	show( P_FIT, box );
	const int path = (int)values[ P_PATH ];
	show( P_PATH_RADIUS, path == (int)PathMode::Arc || path == (int)PathMode::Circle );
	show( P_WAVE_HEIGHT, path == (int)PathMode::Wave );
	show( P_WAVE_LENGTH, path == (int)PathMode::Wave );
	show( P_PATH_OFFSET, path != (int)PathMode::Off );

	show( P_VIDEO, isEffect() );
	const int fillMode   = (int)values[ P_FILL_MODE ];
	const bool pattern   = fillMode >= FILL_STRIPES && fillMode <= FILL_LED;
	show( P_FILL2_H, fillMode != FILL_SOLID && fillMode != FILL_LED );
	show( P_GRAD_ANGLE, fillMode == FILL_GRADIENT_BLOCK );
	show( P_PATTERN_SCALE, pattern );
	show( P_PATTERN_ANGLE, fillMode == FILL_STRIPES || fillMode == FILL_HALFTONE || fillMode == FILL_CHECKER );
	show( P_PATTERN_OFFSET, pattern && fillMode != FILL_LED );

	const int outline = (int)values[ P_OUTLINE_MODE ];
	for( unsigned id : { P_OL1_WIDTH, P_OL1_POSITION, P_OL_CORNERS, P_OL1_H } )
		show( id, outline != OUTLINE_OFF );
	show( P_OL2_WIDTH, outline == OUTLINE_TWO );
	show( P_OL2_H, outline == OUTLINE_TWO );

	const bool plate = (int)values[ P_PLATE ] != PLATE_OFF;
	for( unsigned id : { P_PLATE_H, P_PLATE_PAD_X, P_PLATE_PAD_Y, P_PLATE_RADIUS, P_PLATE_SLANT, P_PLATE_WIDTH, P_PLATE_BORDER, P_PLATE_BORDER_H } )
		show( id, plate );

	const int shadow = (int)values[ P_SHADOW_MODE ];
	const bool drop = ( shadow & EFFECT_OUTER ) != 0, inner = ( shadow & EFFECT_INNER ) != 0;
	for( unsigned id : { P_SH_H, P_SH_DISTANCE, P_SH_SOFTNESS, P_SH_SPREAD } )
		show( id, drop );
	show( P_SH_ANGLE, drop || inner );
	for( unsigned id : { P_ISH_DISTANCE, P_ISH_SOFTNESS, P_ISH_H } )
		show( id, inner );

	const int glow = (int)values[ P_GLOW_MODE ];
	for( unsigned id : { P_GL_H, P_GL_RADIUS, P_GL_STRENGTH } )
		show( id, ( glow & EFFECT_OUTER ) != 0 );
	show( P_IGL_RADIUS, ( glow & EFFECT_INNER ) != 0 );
	show( P_IGL_H, ( glow & EFFECT_INNER ) != 0 );

	const int view = (int)values[ P_3D ];
	for( unsigned id : { P_TILT_X, P_TILT_Y, P_PERSPECTIVE } )
		show( id, view != THREED_OFF );
	for( unsigned id : { P_DEPTH, P_BEVEL, P_SIDE_H, P_LIGHT_ANGLE, P_LIGHT_HEIGHT, P_SHADING, P_GLOSS } )
		show( id, view == THREED_EXTRUDE );

	for( int a = 0; a < kNumAnimators; ++a )
	{
		const auto mode    = (AnimMode)(int)values[ animParam( a, AP_MODE ) ];
		const bool on      = mode != AnimMode::Off;
		// Animator k appears once animator k-1 runs (or when it runs itself)
		const bool visible = a == 0 || on || animatorOn( a - 1 );
		const bool seq     = isSequence( mode );
		const bool waveR   = mode == AnimMode::Wave || mode == AnimMode::Random;
		auto prop          = [ & ]( int p ) { return on && ( shownProps[ a ] & ( 1u << p ) ) != 0; };
		show( animParam( a, AP_PRESET ), visible );
		show( animParam( a, AP_MODE ), visible );
		show( animParam( a, AP_ADD ), on );
		show( animParam( a, AP_PROGRESS ), on && !( seq && replay ) );// replay drives sequences itself
		show( animParam( a, AP_TARGET ), on && plate );
		show( animParam( a, AP_UNIT ), on );
		show( animParam( a, AP_ORDER ), seq || mode == AnimMode::Wave );
		show( animParam( a, AP_SPREAD ), seq || waveR );
		show( animParam( a, AP_HOLD ), mode == AnimMode::SequenceInOut );
		show( animParam( a, AP_EASE ), seq );
		show( animParam( a, AP_RANGE_START ), prop( PROP_RANGE ) );
		show( animParam( a, AP_RANGE_END ), prop( PROP_RANGE ) );
		show( animParam( a, AP_OFFSET_X ), prop( PROP_OFFSET_X ) );
		show( animParam( a, AP_OFFSET_Y ), prop( PROP_OFFSET_Y ) );
		show( animParam( a, AP_SCALE ), prop( PROP_SCALE ) );
		show( animParam( a, AP_ROTATION ), prop( PROP_ROTATION ) );
		show( animParam( a, AP_SKEW ), prop( PROP_SKEW ) );
		show( animParam( a, AP_SPACING ), prop( PROP_SPACING ) );
		show( animParam( a, AP_OPACITY ), prop( PROP_OPACITY ) );
		show( animParam( a, AP_COLOR_H ), prop( PROP_COLOR ) );
		show( animParam( a, AP_THICKNESS ), prop( PROP_THICKNESS ) );
		show( animParam( a, AP_BLUR ), prop( PROP_BLUR ) );
		show( animParam( a, AP_WIPE ), prop( PROP_WIPE ) );
		show( animParam( a, AP_WIPE_FROM ), prop( PROP_WIPE ) );
		show( animParam( a, AP_SCRAMBLE ), prop( PROP_SCRAMBLE ) );
		show( animParam( a, AP_CHARSET ), prop( PROP_SCRAMBLE ) );
	}

	const bool adv = (int)values[ P_ADVANCED ] == 1;
	for( unsigned id = P_ADVANCED + 1; id < P_COUNT; ++id )
		show( id, adv );
}

// ===========================================================================
// Animator property rows
// ===========================================================================
bool TextSource::propertyUsed( int a, int prop ) const
{
	auto v = [ & ]( AnimParam p ) { return values[ animParam( a, p ) ]; };
	switch( prop )
	{
	case PROP_OFFSET_X: return std::fabs( v( AP_OFFSET_X ) ) > 1e-4f;
	case PROP_OFFSET_Y: return std::fabs( v( AP_OFFSET_Y ) ) > 1e-4f;
	case PROP_SCALE: return std::fabs( v( AP_SCALE ) - 100.0f ) > 1e-4f;
	case PROP_ROTATION: return std::fabs( v( AP_ROTATION ) ) > 1e-4f;
	case PROP_OPACITY: return std::fabs( v( AP_OPACITY ) - 100.0f ) > 1e-4f;
	case PROP_COLOR: return v( AP_COLOR_A ) > 1e-4f;
	case PROP_WIPE: return v( AP_WIPE ) > 1e-4f;
	case PROP_SCRAMBLE: return v( AP_SCRAMBLE ) > 1e-4f;
	case PROP_SKEW: return std::fabs( v( AP_SKEW ) ) > 1e-4f;
	case PROP_BLUR: return v( AP_BLUR ) > 1e-4f;
	case PROP_THICKNESS: return std::fabs( v( AP_THICKNESS ) ) > 1e-4f;
	case PROP_SPACING: return std::fabs( v( AP_SPACING ) ) > 1e-4f;
	case PROP_RANGE: return v( AP_RANGE_START ) > 1e-4f || v( AP_RANGE_END ) < 100.0f - 1e-4f;
	default: return false;
	}
}

void TextSource::refreshShownProperties( int a, bool keepAdded )
{
	unsigned bits = keepAdded ? shownProps[ a ] : 0u;
	for( int p = PROP_OFFSET_X; p < PROP_COUNT; ++p )
		if( propertyUsed( a, p ) )
			bits |= 1u << p;
	shownProps[ a ] = bits;
}

void TextSource::onAddProperty( int a, int item )
{
	if( item == PROP_SHOW_ALL )
		for( int p = PROP_OFFSET_X; p < PROP_COUNT; ++p )
			shownProps[ a ] |= 1u << p;
	else if( item == PROP_HIDE_UNUSED )
		refreshShownProperties( a, false );
	else if( item >= PROP_OFFSET_X && item < PROP_COUNT )
		shownProps[ a ] |= 1u << item;
	updateVisibility();
}

// ===========================================================================
// Mode menus: migration and switching on
// ===========================================================================
bool TextSource::isModeParam( unsigned index ) const
{
	return index == P_OUTLINE_MODE || index == P_SHADOW_MODE || index == P_GLOW_MODE;
}

void TextSource::deriveModesFromValues()
{
	// A composition from 1.x has no Outline / Shadow / Glow menus: switch on what
	// its values use, so it looks exactly as before. (A 2.x composition sends them.)
	if( !modeFromHost[ 0 ] )
	{
		const int m = values[ P_OL2_WIDTH ] > 0.0f ? OUTLINE_TWO : values[ P_OL1_WIDTH ] > 0.0f ? OUTLINE_ONE : OUTLINE_OFF;
		setValue( P_OUTLINE_MODE, (float)m );
	}
	if( !modeFromHost[ 1 ] )
	{
		const bool drop  = values[ P_SH_A ] > 0.0f && ( values[ P_SH_DISTANCE ] > 0.0f || values[ P_SH_SPREAD ] > 0.0f );
		const bool inner = values[ P_ISH_A ] > 0.0f && values[ P_ISH_DISTANCE ] > 0.0f;
		setValue( P_SHADOW_MODE, (float)( ( drop ? EFFECT_OUTER : 0 ) | ( inner ? EFFECT_INNER : 0 ) ) );
	}
	if( !modeFromHost[ 2 ] )
	{
		const bool outer = values[ P_GL_RADIUS ] > 0.0f && values[ P_GL_STRENGTH ] > 0.0f && values[ P_GL_A ] > 0.0f;
		const bool inner = values[ P_IGL_RADIUS ] > 0.0f && values[ P_IGL_A ] > 0.0f;
		setValue( P_GLOW_MODE, (float)( ( outer ? EFFECT_OUTER : 0 ) | ( inner ? EFFECT_INNER : 0 ) ) );
	}
}

void TextSource::onModePicked( unsigned index, int previous )
{
	// Switching an effect on gives it a visible size if it has none yet, relative
	// to the font size, so the first click shows something.
	const float size = values[ P_SIZE ];
	const int now    = (int)values[ index ];
	auto atLeast     = [ & ]( unsigned id, float v ) {
		if( values[ id ] <= 0.0f )
			setValue( id, std::round( v ) );
	};
	switch( index )
	{
	case P_OUTLINE_MODE:
		if( now >= OUTLINE_ONE )
			atLeast( P_OL1_WIDTH, std::max( 2.0f, size * 0.04f ) );
		if( now == OUTLINE_TWO )
			atLeast( P_OL2_WIDTH, std::max( 2.0f, size * 0.03f ) );
		break;
	case P_SHADOW_MODE:
		if( ( now & EFFECT_OUTER ) && values[ P_SH_DISTANCE ] <= 0.0f && values[ P_SH_SPREAD ] <= 0.0f )
			atLeast( P_SH_DISTANCE, std::max( 2.0f, size * 0.05f ) );
		if( now & EFFECT_INNER )
			atLeast( P_ISH_DISTANCE, std::max( 2.0f, size * 0.03f ) );
		break;
	case P_GLOW_MODE:
		if( now & EFFECT_OUTER )
			atLeast( P_GL_RADIUS, std::max( 4.0f, size * 0.25f ) );
		if( now & EFFECT_INNER )
			atLeast( P_IGL_RADIUS, std::max( 3.0f, size * 0.12f ) );
		break;
	case P_3D:
		if( previous == THREED_OFF && now != THREED_OFF && values[ P_TILT_X ] == 0.0f && values[ P_TILT_Y ] == 0.0f )
		{
			setValue( P_TILT_X, now == THREED_TILT ? 35.0f : 20.0f );
			setValue( P_TILT_Y, now == THREED_TILT ? 0.0f : -25.0f );
		}
		if( now == THREED_EXTRUDE && values[ P_DEPTH ] == 40.0f )
			setValue( P_DEPTH, std::round( std::max( 4.0f, size * 0.2f ) ) );
		break;
	default:
		break;
	}
}

// ===========================================================================
// Values from the host
// ===========================================================================
FFResult TextSource::SetFloatParameter( unsigned int index, float value )
{
	if( index >= P_COUNT )
		return FF_FAIL;

	// Choices and positions the plugin also changes itself: only a value the host
	// did not know before is a new user action. Hosts may re-send unchanged
	// values at any time (e.g. while animating other parameters).
	if( index == P_COUNT - 1 && !defaultsDone )
	{
		defaultsDone = true;// the last parameter of the SDK's default pass
		values[ index ] = value;
		hostKnown[ index ] = value;
		return FF_SUCCESS;
	}
	const float previousHost = hostKnown[ index ];
	const float previous     = values[ index ];
	const bool changed       = hostChanged( index, value );

	if( index == P_FAMILY || index == P_STYLE )
	{
		// Before the menus are filled the stored index means nothing (the font list
		// may have changed since the composition was saved); "Font" decides.
		if( !menusPopulated || !changed )
			return FF_SUCCESS;
		values[ index ] = value;
		if( live )
			index == P_FAMILY ? onFamilyPicked( (int)value ) : onStylePicked( (int)value );
		return FF_SUCCESS;
	}
	for( int l = 0; l < kNumLineStyles; ++l )
		if( index == lineParam( l, LP_WEIGHT ) )
		{
			if( !menusPopulated || !changed )
				return FF_SUCCESS;
			values[ index ] = value;
			if( live )
				onWeightPicked( l, (int)value );
			return FF_SUCCESS;
		}
	for( int a = 0; a < kNumAnimators; ++a )
	{
		if( index == animParam( a, AP_PRESET ) )
		{
			if( !changed )
				return FF_SUCCESS;
			values[ index ] = value;
			if( live && (int)value > 0 )
				applyPreset( a, (int)value );
			return FF_SUCCESS;
		}
		if( index == animParam( a, AP_ADD ) )
		{
			if( !changed )
				return FF_SUCCESS;
			values[ index ] = value;
			if( (int)value > 0 )
			{
				if( live )
					onAddProperty( a, (int)value );
				setValue( index, 0.0f );// a one-shot menu: back to "+ Property..."
			}
			return FF_SUCCESS;
		}
		if( index == animParam( a, AP_MODE ) )
		{
			values[ index ] = value;
			if( changed && live )
				refreshShownProperties( a, true );
			updateVisibility();
			return FF_SUCCESS;
		}
	}
	switch( index )
	{
	case P_LOOK:
		if( !changed )
			return FF_SUCCESS;
		values[ index ] = value;
		if( (int)value > 0 )
		{
			if( live )
				applyLook( (int)value );
			setValue( P_LOOK, 0.0f );
		}
		return FF_SUCCESS;
	case P_ADD_LINE:
	case P_REMOVE_LINE:
		if( pressed( index, value ) )
		{
			if( index == P_ADD_LINE )
				showLines( shownLines + 1 );
			else if( shownLines > 0 )
			{
				// The removed field is emptied, so it stays hidden after reloading.
				extraLines[ shownLines - 1 ].clear();
				RaiseParamEvent( P_LINE_FIRST + (unsigned)( shownLines - 1 ), FF_EVENT_FLAG_VALUE );
				showLines( shownLines - 1 );
			}
		}
		return FF_SUCCESS;
	case P_NEXT_SLIDE:
	case P_PREV_SLIDE:
		if( pressed( index, value ) )
			stepSlide( index == P_NEXT_SLIDE ? 1 : -1 );
		return FF_SUCCESS;
	case P_TIMER_START:
		if( pressed( index, value ) )
			timerStart();
		return FF_SUCCESS;
	case P_TIMER_STOP:
		if( pressed( index, value ) )
			timerStop();
		return FF_SUCCESS;
	case P_TIMER_RESET:
		if( pressed( index, value ) )
			timerReset();
		return FF_SUCCESS;
	case P_SLIDE: {
		if( !changed )
			return FF_SUCCESS;// keep a position set by the Next/Previous buttons
		// Resolume's own - / + buttons next to the slider step by 0.01. A single,
		// isolated step of about that size means "previous / next slide"; host
		// animations and dragging send a stream of values and are taken as they are.
		const double now    = wallSeconds();
		const bool isolated = lastSlideHostChange < 0.0 || std::fabs( now - lastSlideHostChange ) > 0.3;
		lastSlideHostChange = now;
		const float delta   = value - previousHost;
		if( isolated && std::isfinite( previousHost ) && std::fabs( delta ) > 0.004f && std::fabs( delta ) < 0.016f && slideCount > 1 )
		{
			values[ P_SLIDE ] = previousHost;
			slideTarget       = slideFromPosition( previousHost, slideCount );
			stepSlide( delta > 0.0f ? 1 : -1 );
			return FF_SUCCESS;
		}
		break;
	}
	case P_SOURCE:
		sourceFromHost = sourceFromHost || defaultsDone;
		break;
	case P_SLIDES_LEGACY:
		// A composition from Text+ 1.0: "Slides" becomes the matching Display.
		values[ index ] = value;
		if( changed && (int)value > 0 && display() == DISPLAY_STATIC )
		{
			setValue( P_DISPLAY, (int)value == 1 ? (float)DISPLAY_SLIDES_LINES : (float)DISPLAY_SLIDES_BLOCKS );
			updateVisibility();
		}
		return FF_SUCCESS;
	default:
		break;
	}

	if( isModeParam( index ) && defaultsDone )
		modeFromHost[ index == P_OUTLINE_MODE ? 0 : index == P_SHADOW_MODE ? 1 : 2 ] = true;
	values[ index ] = value;
	if( live && changed && ( isModeParam( index ) || index == P_3D ) )
		onModePicked( index, (int)previous );
	updateVisibility();
	return FF_SUCCESS;
}

FFResult TextSource::SetTextParameter( unsigned int index, const char* value )
{
	std::string v = value ? value : "";
	for( int l = 0; l < kNumLineStyles; ++l )
		if( index == lineParam( l, LP_FONT ) )
		{
			setLineFontText( l, v, false );
			return FF_SUCCESS;
		}
	if( index >= P_LINE_FIRST && index <= P_LINE_LAST )
	{
		const int i     = (int)( index - P_LINE_FIRST );
		extraLines[ i ] = v;
		if( !v.empty() && i + 1 > shownLines )
			showLines( i + 1 );// a saved composition brings its lines back
		return FF_SUCCESS;
	}
	switch( index )
	{
	case P_TEXT:
		text = v;
		return FF_SUCCESS;
	case P_FONT:
		if( v != fontText )
		{
			fontText  = v;
			fontDirty = true;
		}
		return FF_SUCCESS;
	case P_FONT_FILE:
		if( v != fontFile )
		{
			fontFile  = v;
			fontDirty = true;
		}
		return FF_SUCCESS;
	case P_TEXT_FILE:
		if( v != textFile )
		{
			textFile            = v;
			textFileStamp       = -1;
			textFilePollCounter = 0;
			textFileContent.clear();
			// Text+ 1.0 compositions have no "Source": a file there means "show the file".
			if( !v.empty() && !live && !sourceFromHost && source() == SOURCE_INPUT )
			{
				values[ P_SOURCE ] = (float)SOURCE_FILE;
				RaiseParamEvent( P_SOURCE, FF_EVENT_FLAG_VALUE );
				updateVisibility();
			}
		}
		return FF_SUCCESS;
	case P_ROW_TEMPLATE:
		rowTemplate = v;
		return FF_SUCCESS;
	case P_TIMER_TARGET:
		timerTarget = v;
		return FF_SUCCESS;
	case P_TIMER_DURATION:
		timerDuration = v;
		return FF_SUCCESS;
	case P_SEPARATOR:
		separator = v;
		return FF_SUCCESS;
	case P_FEATURES:
		features = v;
		return FF_SUCCESS;
	case P_OSC_PORT:
		oscPortText = v;
		return FF_SUCCESS;
	default:
		return FF_FAIL;
	}
}

char* TextSource::GetTextParameter( unsigned int index )
{
	if( index >= P_COUNT )
		return nullptr;
	if( index >= P_LINE_FIRST && index <= P_LINE_LAST )
		return returnText( index, extraLines[ index - P_LINE_FIRST ] );
	for( int l = 0; l < kNumLineStyles; ++l )
		if( index == lineParam( l, LP_FONT ) )
			return returnText( index, lineFonts[ l ].text );
	switch( index )
	{
	case P_TEXT: return returnText( index, text );
	case P_FONT: return returnText( index, fontText );
	case P_FONT_FILE: return returnText( index, fontFile );
	case P_TEXT_FILE: return returnText( index, textFile );
	case P_ROW_TEMPLATE: return returnText( index, rowTemplate );
	case P_TIMER_TARGET: return returnText( index, timerTarget );
	case P_TIMER_DURATION: return returnText( index, timerDuration );
	case P_SEPARATOR: return returnText( index, separator );
	case P_FEATURES: return returnText( index, features );
	case P_OSC_PORT: return returnText( index, oscPortText );
	default: return returnText( index, "" );
	}
}

char* TextSource::GetParameterDisplay( unsigned int index )
{
	if( index >= P_COUNT )
		return nullptr;
	char buf[ 96 ];
	const float v = values[ index ];
	int animator  = -1;
	AnimParam ap  = AP_COUNT;
	if( index >= P_A1 && index < P_A_END )
	{
		animator = (int)( ( index - P_A1 ) / AP_COUNT );
		ap       = (AnimParam)( ( index - P_A1 ) % AP_COUNT );
	}
	auto isLine = [ & ]( LineParam p ) { return index == lineParam( 0, p ) || index == lineParam( 1, p ); };
	if( ( index >= P_LINE_FIRST && index <= P_LINE_LAST ) || isLine( LP_FONT ) )
		return GetTextParameter( index );
	switch( index )
	{
	case P_TEXT:
	case P_FONT:
	case P_FONT_FILE:
	case P_TEXT_FILE:
	case P_ROW_TEMPLATE:
	case P_TIMER_TARGET:
	case P_TIMER_DURATION:
	case P_SEPARATOR:
	case P_FEATURES:
	case P_OSC_PORT:
		return GetTextParameter( index );
	case P_SIZE:
	case P_POS_X:
	case P_POS_Y:
	case P_BOX_WIDTH:
	case P_BOX_HEIGHT:
	case P_OL1_WIDTH:
	case P_OL2_WIDTH:
	case P_SH_DISTANCE:
	case P_SH_SOFTNESS:
	case P_SH_SPREAD:
	case P_ISH_DISTANCE:
	case P_ISH_SOFTNESS:
	case P_GL_RADIUS:
	case P_IGL_RADIUS:
	case P_PLATE_BORDER:
	case P_EDGE_FADE:
	case P_PATH_RADIUS:
	case P_WAVE_HEIGHT:
	case P_WAVE_LENGTH:
	case P_PATTERN_SCALE:
	case P_DEPTH:
	case P_BEVEL:
		std::snprintf( buf, sizeof( buf ), "%.0f px", v );
		break;
	case P_SCROLL_SPEED:
		std::snprintf( buf, sizeof( buf ), "%.0f px/s", v );
		break;
	case P_EDGE:
		std::snprintf( buf, sizeof( buf ), "%.2f px", v );
		break;
	case P_ROTATION:
	case P_SH_ANGLE:
	case P_GRAD_ANGLE:
	case P_PATTERN_ANGLE:
	case P_PLATE_SLANT:
	case P_TILT_X:
	case P_TILT_Y:
	case P_LIGHT_ANGLE:
	case P_LIGHT_HEIGHT:
		std::snprintf( buf, sizeof( buf ), "%.0f\xC2\xB0", v );
		break;
	case P_TRACKING:
		std::snprintf( buf, sizeof( buf ), "%.0f /1000 em", v );
		break;
	case P_THICKNESS:
		std::snprintf( buf, sizeof( buf ), "%+.1f %% of size", v );
		break;
	case P_LEADING:
	case P_PLATE_PAD_X:
	case P_PLATE_PAD_Y:
	case P_PLATE_RADIUS:
	case P_REPLAY_STAGGER:
	case P_PERSPECTIVE:
	case P_SHADING:
	case P_GLOSS:
		std::snprintf( buf, sizeof( buf ), "%.0f %%", v );
		break;
	case P_SCROLL_POSITION:
	case P_COUNT_PROGRESS:
	case P_PATH_OFFSET:
	case P_PATTERN_OFFSET:
		std::snprintf( buf, sizeof( buf ), "%.0f %%", v * 100.0f );
		break;
	case P_COUNT_FROM:
	case P_COUNT_TO:
		std::snprintf( buf, sizeof( buf ), "%s", formatNumber( v, (int)values[ P_COUNT_DECIMALS ], (NumberStyle)(int)values[ P_NUMBER_FORMAT ] ).c_str() );
		break;
	case P_SLIDE:
		std::snprintf( buf, sizeof( buf ), "%d / %d", slideFromPosition( v, slideCount ) + 1, slideCount );
		break;
	case P_REPLAY:
		if( v <= 0.0f )
			std::snprintf( buf, sizeof( buf ), "Off" );
		else
			std::snprintf( buf, sizeof( buf ), "%.2g beats", v );
		break;
	default:
		if( isLine( LP_SIZE ) || isLine( LP_SPACING ) )
			std::snprintf( buf, sizeof( buf ), "%.0f %%", v );
		else if( isLine( LP_TRACKING ) )
			std::snprintf( buf, sizeof( buf ), "%+.0f /1000 em", v );
		else if( ap == AP_PROGRESS )
			std::snprintf( buf, sizeof( buf ), "%.0f %%", v * 100.0f );
		else if( ap == AP_OFFSET_X || ap == AP_OFFSET_Y || ap == AP_SPACING || ap == AP_BLUR )
			std::snprintf( buf, sizeof( buf ), "%.0f px", v );
		else if( ap == AP_SCALE || ap == AP_OPACITY || ap == AP_WIPE || ap == AP_SCRAMBLE || ap == AP_HOLD || ap == AP_RANGE_START || ap == AP_RANGE_END )
			std::snprintf( buf, sizeof( buf ), "%.0f %%", v );
		else if( ap == AP_THICKNESS )
			std::snprintf( buf, sizeof( buf ), "%+.1f %% of size", v );
		else if( ap == AP_ROTATION || ap == AP_SKEW )
			std::snprintf( buf, sizeof( buf ), "%.0f\xC2\xB0", v );
		else if( ap == AP_SPREAD )
		{
			const auto mode = (AnimMode)(int)values[ animParam( animator, AP_MODE ) ];
			if( mode == AnimMode::Random )
				std::snprintf( buf, sizeof( buf ), "%d states", std::max( 2, (int)std::lround( v ) ) );
			else if( mode == AnimMode::Wave )
				std::snprintf( buf, sizeof( buf ), "%.1f units / wave", v );
			else
				std::snprintf( buf, sizeof( buf ), "%.1f units", v );
		}
		else
		{
			const char* d = CFFGLPlugin::GetParameterDisplay( index );
			std::snprintf( buf, sizeof( buf ), "%s", d ? d : "" );
		}
	}
	return returnDisplay( index, buf );
}

}//namespace textplus
