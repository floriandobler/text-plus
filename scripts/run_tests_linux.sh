#!/usr/bin/env bash
# Development tests without Resolume (Linux, headless):
#  1. Unit tests: font database, layout, line styles, animation, CSV, glyph SDFs
#  2. A test host loads the plugin like Resolume does (plugMain), renders with
#     Mesa in an OpenGL 4.1 core context and writes PNGs to out/. It checks that
#     the plugin leaves the GL state untouched, and runs regression scenarios
#     (host re-sending values, slide stepping, CSV, line styles, random parameters).
#
# Packages (Ubuntu 24.04): cmake ninja-build libglew-dev libegl-dev libgl1-mesa-dri
#                          fonts-dejavu-core fonts-cantarell fonts-inter python3 (libcurl4-openssl-dev for https)
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPLUS_BUILD_TESTS=ON
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
$H $P out/csv_slide2.png 960 300 "Text File=plugins/textplus/tests/data/lower_thirds.csv" "Font=Inter / Bold" "Size=80" "Slide=0.9" @print=Slide | tee out/csv.txt
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
# 1.1: clock & counter, ticker, roll, plates with the lower-third preset, scramble, inner effects, fallback
$H $P out/counter.png 960 200 "Source=2" "Timer Mode=4" "Count From=0" "Count To=12840" "Count=0.5" "Text={timer} Besucher" "Number Format=2" "Font=Inter / Black" "Size=80" @expectInk=1 @frames=10
$H $P out/countdown.png 960 200 "Source=2" "Timer Mode=2" "Duration=1:30" "Text=Start in {timer}" "Font=Inter / Bold" "Size=80" @expectInk=1 @frames=10
$H $P out/ticker.png 1280 200 "Display=3" "Text=Breaking: Text+ 1.1" "Text 2=Second item" "Font=Inter / Bold" "Size=64" "Scroll Speed=300" "Plate=1" @dt=0.05 @frames=30 @expectInk=1
$H $P out/roll.png 640 480 "Display=4" "Text=CREDITS" "Text 2=Direction: Anna" "Text 3=Sound: Ben" "Font=Inter / Regular" "Size=48" "Scroll Speed=120" "Roll Loop=0" "Plate=2" "Plate Radius=20" @dt=0.05 @frames=60 @expectInk=1
$H $P out/lower_third_preset.png 1280 400 "Text=Anna Musterfrau" "Text 2=REGIE & LICHTDESIGN" "Font=Inter / Black" "Size=90" "Align=0" "Anchor H=2" "L2 Style=1" "L2 Size=45" "Plate Radius=10" "Plate Slant=-10" "Plate Border=3" "Shadow Distance=8" "+A1 Preset=15" "@ramp=A1 Progress:0:1" "@ramp=A2 Progress:-0.4:1" @dt=0.033 @frames=40 @expectInk=1 "@print=Plate,A1 Target,A2 Mode" | tee out/lower_third_preset.txt
grep -q "Plate *= 2 " out/lower_third_preset.txt && grep -q "A1 Target *= 1 " out/lower_third_preset.txt || { echo "ERROR: lower third preset"; exit 1; }
$H $P out/decode.png 960 160 "Text=DECODING" "Font=Inter / Black" "Size=80" "+A1 Preset=14" "++A1 Progress=0.5" @expectInk=1 @frames=14
$H $P out/inner_effects.png 960 260 "Text=INNER" "Font=Inter / Black" "Size=200" "Fill Color=0.08" "Fill Color_Sat=0.9" "Inner Glow Radius=25" "Inner Shadow Distance=10" "Shadow Angle=135" "Outline Width=4" @expectInk=1 @frames=8
$H $P out/fallback.png 1100 300 "Text=Emoji 🎉 👍🏽 1️⃣ 🇩🇪" "Text 2=שלום · Привет · Ελλάδα" "Case=1" "Font=Inter / Bold" "Size=70" "Outline Width=4" @expectInk=1 @frames=20
# 1.0 compositions: "Slides" and a text file become Display / Source
$H $P out/legacy.png 640 200 "Slides=1" "Text=A|B|C" @print=Display | tee out/legacy.txt
grep -q "Display *= 1 " out/legacy.txt || { echo "ERROR: legacy Slides -> Display"; exit 1; }
# ---- 2.0 ----
# The empty panel stays short (rows as Resolume shows them)
$H $P out/panel.png 640 360 "@ui=out/panel.txt" @frames=4
grep -q "^22 rows in 7 groups" out/panel.txt || { echo "ERROR: empty panel is not 22 rows / 7 groups:"; tail -1 out/panel.txt; exit 1; }
# 1.1 compositions: outline / shadow / glow become effect slots, the legacy rows stay hidden
$H $P out/migrate.png 960 300 "Text=Migrated" "Font=Inter / Black" "Size=120" "Outline Width=12" "Outline 2 Width=6" "Shadow Distance=10" "Glow Radius=30" @frames=12 @expectInk=1 "@print=Effect 1,Effect 2,Effect 3,Effect 4,Outline Width" "@ui=out/migrate_ui.txt" | tee out/migrate.txt
grep -q "Effect 1 *= 1 " out/migrate.txt && grep -q "Effect 2 *= 1 " out/migrate.txt && grep -q "Effect 3 *= 2 " out/migrate.txt && grep -q "Effect 4 *= 3 " out/migrate.txt && grep -q "Outline Width *= 0 " out/migrate.txt || { echo "ERROR: 1.1 effects not migrated"; exit 1; }
! grep -q "Shadow Distance\|Glow Radius" out/migrate_ui.txt || { echo "ERROR: legacy rows visible"; exit 1; }
# Looks, effects, fills
for L in 6 7 8 9 11 13 16; do $H $P out/look_$L.png 640 240 "Text=Look $L" "Font=Inter / Black" "Size=100" "+Look=$L" @frames=16 @expectInk=1 > /dev/null; done
$H $P out/bevel.png 640 240 "Text=Bevel" "Font=Inter / Black" "Size=120" "+Effect 1=6" "++Effect 1 Mode=1" @frames=16 @expectInk=1
$H $P out/fills.png 960 240 "Text=Halftone" "Font=Inter / Black" "Size=120" "Fill Mode=5" "Fill Color 2=0.95" "Gradient Angle=-90" @frames=16 @expectInk=1
# 3D and paths
$H $P out/flip.png 960 240 "Text=FLIP IN 3D" "Font=Inter / Black" "Size=100" "+A1 Preset=19" "++A1 Progress=0.5" @frames=16 @expectInk=1 "@print=A1 Rotation X,Perspective" | tee out/flip.txt
grep -q "A1 Rotation X *= 90 " out/flip.txt || { echo "ERROR: flip preset"; exit 1; }
for PA in 1 2 3 4; do $H $P out/path_$PA.png 800 320 "Text=TEXT ON A PATH · " "Font=Inter / Black" "Size=48" "Path=$PA" @frames=16 @expectInk=1 > /dev/null; done
# Displays: words, pattern
$H $P out/words.png 640 200 "Display=5" "Text=one two three" "Slide=0.5" @print=Slide @frames=8 | tee out/words.txt
grep -q "(2 / 3)" out/words.txt || { echo "ERROR: word slides"; exit 1; }
$H $P out/pattern.png 960 400 "Display=6" "Text=PATTERN" "Font=Inter / Black" "Size=50" @dt=0.05 @frames=20 @expectInk=1
# Live data: placeholders, JSON file as template, countdown warning
printf '{"city":"Ravensburg","now":{"temp":17.4}}' > out/data.json
$H $P out/json.png 960 200 "Source=1" "Text File=out/data.json" "Text={city} {now.temp}" "Font=Inter / Bold" "Size=60" @frames=12 @expectInk=1
$H $P out/warn.png 960 200 "Source=2" "Timer Mode=2" "Duration=0:30" "Warn At=60" "Text={timer} · {weekday}" "Font=Inter / Black" "Size=70" @frames=10 @expectInk=1
# Text+ FX on an input texture
F=build/TextPlusFX.so
for IM in 0 1 2; do $H $F out/fx_$IM.png 960 300 @input=1 "Input=$IM" "Text=FX" "Font=Inter / Black" "Size=160" "+Effect 1=1" @frames=12 @expectInk=1 > /dev/null; done
# Robustness: random parameter combinations (no crash, GL error or state leak)
python3 plugins/textplus/tests/fuzz_params.py $H $P 20
python3 plugins/textplus/tests/fuzz_params.py $H $F 5
echo "Images in out/"
