#!/usr/bin/env bash
# Development tests without Resolume (Linux, headless):
#  1. Unit tests: font database, layout, line styles, animation, CSV, glyph SDFs
#  2. A test host loads the plugin like Resolume does (plugMain), renders with
#     Mesa in an OpenGL 4.1 core context and writes PNGs to out/. It checks that
#     the plugin leaves the GL state untouched, and runs regression scenarios
#     (host re-sending values, slide stepping, CSV, line styles, random parameters).
#
# Packages (Ubuntu 24.04): cmake ninja-build libglew-dev libegl-dev libgl1-mesa-dri
#                          fonts-dejavu-core fonts-cantarell fonts-inter python3
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTEXTPLUS_BUILD_TESTS=ON
cmake --build build

./build/layout_test

H=./build/ffgl_test_host
P=build/TextPlus.so
mkdir -p out
$H $P out/default.png 1280 720
$H $P out/outline_outside.png 1280 720 "Text=Text+" "Font=DejaVu Sans / Bold" "Size=220" "Outline Width=40" "Fill Color=0.08" "Fill Color_Sat=0.9" "Outline 2 Width=10"
$H $P out/glow_on_orange.png 1280 720 "Text=Kontur|wefjio Happy" "Font=DejaVu Sans Condensed / Bold" "Size=200" "Fill Color_Alpha=0" "Outline Width=32" "Outline Color=0.25" "Outline Color_Sat=0.65" "Outline 2 Width=17" "Glow Color=0.58" "Glow Color_Sat=0.6" "Glow Radius=69" @bg=0.9,0.25,0.15
$H $P out/fade_merged_outlines.png 1280 720 "Text=Kontur|wefjio Happy" "Font=DejaVu Sans Condensed / Bold" "Size=200" "Fill Color_Alpha=0" "Outline Width=32" "Outline Color=0.25" "Outline Color_Sat=0.65" "Outline 2 Width=17" "Glow Radius=69" "A1 Mode=1" "A1 Opacity=0" "A1 Spread=6" "A1 Progress=0.55" @bg=0.9,0.25,0.15
$H $P out/gradient_and_tags.png 1280 360 "Text=Grad {#ff3366}ient{/} Text" "Font=Inter / Black" "Size=140" "Fill Mode=1" "Fill Color=0.15" "Fill Color_Sat=0.9" "Fill Color 2=0.55" "Fill Color 2_Sat=0.9" "Gradient Angle=-30" "Outline Width=6"
$H $P out/preset_pop.png 1280 360 "Text=POP IN" "Font=Inter / Black" "Size=130" "+A1 Preset=5" "+A1 Progress=0.5" "Outline Width=8" "@print=A1 Preset,A1 Mode"
$H $P out/slides_ramp.png 960 200 "Text=AAAA|BBBB|CCCC" "Font=Inter / Black" "Size=110" "Slides=1" "@ramp=Slide:0:1" "Replay on Slide Change=0.5" "+A1 Preset=1" @dt=0.016666667 @frames=76
# Regression (0.2): a host re-sending the preset menu value must not re-apply the preset
$H $P out/preset_resend.png 640 200 "Text=ABCDEF" "+A1 Preset=3" "++A1 Spread=1" "++A1 Offset Y=-40" "@ramp=A1 Progress:1:0" "@resend=stale:A1 Preset" @dt=0.02 @frames=20 "@print=A1 Spread,A1 Offset Y" | tee out/preset_resend.txt
grep -q "A1 Spread *= 1 " out/preset_resend.txt && grep -q "A1 Offset Y *= -40 " out/preset_resend.txt || { echo "ERROR: preset was applied again"; exit 1; }
# Next at the last slide wraps to the first, also when the host re-sends the old position
$H $P out/slide_wrap.png 640 200 "Text=Eins|Zwei|Drei" "Slides=1" "Slide=0.9" "+Next Slide=1" "@resend=stale:Slide" @print=Slide | tee out/slide_wrap.txt
grep -q "(1 / 3)" out/slide_wrap.txt || { echo "ERROR: slide wrap"; exit 1; }
# CSV: one slide per row, cells become lines, header skipped
$H $P out/csv_slide2.png 960 300 "Text File=tests/data/lower_thirds.csv" "Font=Inter / Bold" "Size=80" "Slide=0.9" @print=Slide | tee out/csv.txt
grep -q "(2 / 2)" out/csv.txt || { echo "ERROR: CSV slides"; exit 1; }
# Wave: Progress is the phase
$H $P out/wave_phase.png 960 240 "Text=Wave Phase" "Font=Inter / Black" "Size=110" "+A1 Preset=9" "++A1 Progress=0.25"
# Extra line fields ("Line 2", "Line 3") add lines below "Text"
$H $P out/line_fields.png 960 400 "Text=Erste Zeile" "Text 2=Zweite Zeile" "Text 3=Dritte" "Size=90"
# Resolume's +/- buttons step the Slide slider by 0.01: an isolated step = next / previous slide
$H $P out/slide_stepper.png 640 200 "Text=A|B|C|D" "Slides=1" "Slide=0.875" "+Slide=0.885" @deferAt=90 @frames=100 @dt=0.016 @print=Slide | tee out/slide_stepper.txt
grep -q "(1 / 4)" out/slide_stepper.txt || { echo "ERROR: slide stepper"; exit 1; }
# Line styles: name / function / company
$H $P out/lower_third.png 1280 400 "Text=Anna Musterfrau" "Text 2=REGIE & LICHTDESIGN" "Text 3=Staatstheater Beispielstadt" "Font=Inter / Black" "Size=110" "Align=0" "Anchor H=2" "L2 Style=1" "L2 Font=Medium" "L2 Size=45" "L2 Color=0.1" "L2 Color_Sat=0.8" "L2 Color_Alpha=1" "L2 Tracking=120" "L2 Spacing=8" "L3 Style=1" "L3 Font=Light Italic" "L3 Size=38" "Outline Width=6" "Outline Color_Bri=0.1" "@print=L2 Font,L3 Font" | tee out/lower_third.txt
grep -q "(Medium)" out/lower_third.txt && grep -q "(Light Italic)" out/lower_third.txt || { echo "ERROR: line styles"; exit 1; }
# Wrap in a box, justified
$H $P out/wrap_box.png 1280 720 "Text=Distance fields keep every outline outside the letters, at any size, with shadows and glow computed in one pass." "Font=Inter / Regular" "Size=64" "Wrap=1" "Box Width=900" "Align=3"
$H $P out/menus.png 320 180 "Font=Inter / Regular" "+Style=3" @frames=16
# Robustness: random parameter combinations (no crash, GL error or state leak)
python3 tests/fuzz_params.py $H $P 20
echo "Images in out/"
