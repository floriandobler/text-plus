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
# Interface: the default shows 29 rows; effect groups are one row until switched on
$H $P out/ui_default.png 320 180 @ui=count @frames=12 | tee out/ui_default.txt
grep -q "UI: 29 visible rows" out/ui_default.txt || { echo "ERROR: default interface is not 29 rows"; exit 1; }
# 1.x compositions: the Outline / Shadow / Glow menus follow the values (no menu sent)
$H $P out/migrate.png 640 200 "Text=Old" "Outline Width=8" "Outline 2 Width=3" "Shadow Distance=6" "Inner Glow Radius=10" "@print=Outline,Shadow,Glow" | tee out/migrate.txt
grep -q "Outline *= 2 " out/migrate.txt && grep -q "Shadow *= 1 " out/migrate.txt && grep -q "Glow *= 2 " out/migrate.txt || { echo "ERROR: 1.x migration of the effect menus"; exit 1; }
# 2.x compositions: a menu that is off stays off, even with a width set
$H $P out/migrate2.png 640 200 "Text=New" "Composition Format=2" "Outline=0" "Outline Width=8" "@print=Outline" | tee out/migrate2.txt
grep -q "Outline *= 0 " out/migrate2.txt || { echo "ERROR: an Outline menu from the composition was overridden"; exit 1; }
# A host that also sends the default of the new menus (Outline=0) to a 1.x composition
$H $P out/migrate3.png 640 200 "Text=Old" "Outline=0" "Outline Width=8" "@print=Outline,Composition Format" | tee out/migrate3.txt
grep -q "Outline *= 1 " out/migrate3.txt && grep -q "Composition Format *= 2 " out/migrate3.txt || { echo "ERROR: 1.x migration with re-sent defaults"; exit 1; }
# Switching an effect on gives it a visible size
$H $P out/switch_on.png 640 200 "Text=On" "+Glow=1" "@print=Glow Radius" | tee out/switch_on.txt
grep -q "Glow Radius *= [1-9]" out/switch_on.txt || { echo "ERROR: switching Glow on left the radius at 0"; exit 1; }
# Fill patterns, LED, alternating colours
$H $P out/fill_stripes.png 960 260 "Text=STRIPES" "Font=Inter / Black" "Size=170" "Fill Mode=3" "Fill Color 2_Sat=0.9" "Outline=1" "Outline Width=5" @expectInk=1 @frames=12
$H $P out/fill_halftone.png 960 260 "Text=HALFTONE" "Font=Inter / Black" "Size=150" "Fill Mode=4" "Pattern Scale=14" "Fill Color=0.14" "Fill Color_Sat=0.9" "Fill Color 2_Sat=0.9" @expectInk=1 @frames=12
$H $P out/fill_led.png 960 260 "Text=LED SIGN" "Font=Inter / Black" "Size=150" "Fill Mode=7" "Pattern Scale=10" "Fill Color=0.07" "Fill Color_Sat=0.9" "Glow=1" "Glow Radius=20" "Glow Color=0.07" "Glow Strength=0.6" @bg=black @expectInk=1 @frames=12
$H $P out/fill_noise_alt.png 960 260 "Text=Noise|Alternate Words Here" "Font=Inter / Black" "Size=90" "Fill Mode=9" @expectInk=1 @frames=12
# Looks (one-shot menu, applied live)
for L in 4 5 6 8 9 10; do $H $P out/look_$L.png 960 300 "Text=Look $L" "Font=Inter / Black" "Size=150" "+Look=$L" @expectInk=1 @frames=20 > /dev/null || { echo "ERROR: look $L"; exit 1; }; done
$H $P out/look_reset.png 640 200 "Text=Reset" "Glow=1" "Glow Radius=40" "+Look=1" "@print=Glow,Look" | tee out/look_reset.txt
grep -q "Glow *= 0 " out/look_reset.txt && grep -q "Look *= 0 " out/look_reset.txt || { echo "ERROR: look Clean / one-shot reset"; exit 1; }
# A host re-sending the one-shot Look menu must not apply the look again over a later change
$H $P out/look_resend.png 640 200 "Text=Resend" "+Look=5" "++Glow Radius=7" "@resend=stale:Look" @dt=0.02 @frames=24 "@print=Glow Radius" | tee out/look_resend.txt
grep -q "Glow Radius *= 7 " out/look_resend.txt || { echo "ERROR: look applied again"; exit 1; }
# Slides: "Out, then In" keeps the old slide while it plays out
$H $P out/replay_out.png 640 200 "Text=AAAA|BBBB" "Display=1" "Slide=0.1" "Replay on Slide Change=1" "Replay=1" "+A1 Preset=3" "++Slide=0.9" @deferAt=20 @dt=0.05 @frames=26 @expectInk=1 @print=Slide | tee out/replay_out.txt
# 3D: tilt and extrude
$H $P out/3d_tilt.png 1280 720 "Text=STAR CRAWL" "Text 2=far far away" "Font=Inter / Black" "Size=120" "3D=1" "Tilt X=55" "Perspective=60" "Shadow=1" "Shadow Distance=12" @expectInk=1 @frames=12
$H $P out/3d_extrude.png 1280 720 "Text=EXTRUDE" "Font=Inter / Black" "Size=220" "3D=2" "Tilt X=20" "Tilt Y=-25" "Depth=50" "Bevel=6" "Fill Color=0.08" "Fill Color_Sat=0.85" "Side Color=0.05" "Side Color_Sat=0.8" "Side Color_Bri=0.6" "Plate=1" "Plate Color_Alpha=0.6" @expectInk=1 @frames=12
$H $P out/3d_emboss.png 960 300 "Text=EMBOSS" "Font=Inter / Black" "Size=200" "3D=2" "Depth=30" "Bevel=10" "Perspective=0" "Fill Color=0.6" "Fill Color_Sat=0.5" @expectInk=1 @frames=12
$H $P out/3d_fade.png 960 300 "Text=FADING 3D" "Font=Inter / Black" "Size=150" "3D=2" "Tilt Y=30" "+A1 Preset=2" "++A1 Progress=0.5" @expectInk=1 @frames=16
# Paths
$H $P out/path_arc.png 800 500 "Text=RAINBOW ARC TEXT" "Font=Inter / Black" "Size=70" "Path=1" "Path Radius=380" "Outline=1" "Outline Width=4" @expectInk=1 @frames=12
$H $P out/path_circle.png 800 800 "Text=TEXT AROUND A CIRCLE • " "Font=Inter / Bold" "Size=64" "Path=2" "Path Radius=300" "+A1 Preset=3" "++A1 Progress=0.6" @expectInk=1 @frames=16
$H $P out/path_wave.png 1200 400 "Text=Riding the wave of text" "Font=Inter / Black" "Size=90" "Path=3" "Wave Height=50" "Wave Length=700" "@ramp=Path Offset:0:1" @dt=0.033 @frames=30 @expectInk=1
# Animators: new properties, presets, four animators
$H $P out/anim_blur.png 960 220 "Text=BLUR IN EFFECT" "Font=Inter / Black" "Size=110" "+A1 Preset=19" "++A1 Progress=0.5" @expectInk=1 @frames=16
$H $P out/anim_tracking.png 960 220 "Text=TRACKING IN" "Font=Inter / Black" "Size=90" "+A1 Preset=20" "++A1 Progress=0.12" @expectInk=1 @frames=16
$H $P out/anim_skew_weight.png 960 220 "Text=Faux Italic Weight" "Font=Inter / Medium" "Size=100" "A1 Mode=6" "A1 Skew=20" "A1 Progress=1" "A2 Mode=4" "A2 Thickness=4" "A2 Progress=0.3" "Outline=1" "Outline Width=5" @expectInk=1 @frames=12
$H $P out/anim_four.png 960 300 "Text=FOUR ANIMATORS" "Font=Inter / Black" "Size=100" "+A1 Preset=3" "++A1 Progress=0.7" "+A2 Preset=9" "++A2 Progress=0.2" "+A3 Preset=11" "++A3 Progress=0.4" "+A4 Preset=21" "++A4 Progress=0.8" "@ui=count" @expectInk=1 @frames=16 | tee out/anim_four.txt
grep -q "Animator 4" out/anim_four.txt || { echo "ERROR: animator 4 did not appear"; exit 1; }
$H $P out/anim_props.png 640 200 "Text=Props" "+A1 Preset=3" "++A1 + Property=10" "@ui=rows" @frames=16 | tee out/anim_props.txt
grep -q "A1 Blur" out/anim_props.txt && grep -q "A1 Offset Y" out/anim_props.txt && ! grep -q "A1 Skew" out/anim_props.txt || { echo "ERROR: + Property / property rows"; exit 1; }
# Content: CSV row templates, words as slides, shuffle, fit each line, countdown to a date
printf 'Name;Role\nAnna Muster;Director\nBen Beispiel;Sound\n' > out/people.csv
$H $P out/csv_template.png 960 260 "Text File=out/people.csv" "Row Template={Name}|{Role} · {slide}/{slides}" "Font=Inter / Bold" "Size=70" "Slide=0.9" @expectInk=1 @frames=14
$H $P out/slides_words.png 960 200 "Text=One word at a time" "Display=5" "Slide=0.6" "Font=Inter / Black" "Size=120" @frames=14 @print=Slide | tee out/slides_words.txt
grep -q "(4 / 5)" out/slides_words.txt || { echo "ERROR: Slides: Words"; exit 1; }
$H $P out/fit_lines.png 960 700 "Text=BIG|small line here|MEDIUM" "Wrap=1" "Box Width=700" "Auto Fit=3" "Font=Inter / Black" "Size=60" "Leading=100" @expectInk=1 @frames=14
$H $P out/countdown_date.png 960 200 "Source=2" "Timer Mode=1" "Target Time=2099-12-31 23:59" "Text=Noch {timer}" "Time Format=6" "Font=Inter / Bold" "Size=80" @expectInk=1 @frames=10
# Live data: OSC values in the text
python3 plugins/textplus/tests/osc_send.py 39873 /team "Füchse" 4 & python3 plugins/textplus/tests/osc_send.py 39873 /score 3 4 &
sleep 0.3
$H $P out/osc.png 960 200 "Text={osc:/team} {osc:/score} : 1" "OSC Port=39873" "Font=Inter / Black" "Size=90" @dt=0.05 @frames=40 @expectInk=1
wait
# Text+ Fill (effect): over, fill, knockout, with 3D
F=build/TextPlusFill.so
for V in 1 2 3; do $H $F out/fill_video_$V.png 960 300 "Text=VIDEO" "Font=Inter / Black" "Size=240" "Video=$V" "Outline=1" "Outline Width=6" @input=photo @expectInk=1 @frames=12; done
$H $F out/fill_video_3d.png 960 300 "Text=VIDEO 3D" "Font=Inter / Black" "Size=200" "Video=2" "3D=2" "Tilt X=15" "Tilt Y=-20" "Depth=40" @input=photo @expectInk=1 @frames=12
$H $F out/fill_video_empty.png 640 200 "Text=" "Video=1" @input=photo @expectInk=1 @frames=8

# Robustness: random parameter combinations (no crash, GL error or state leak)
python3 plugins/textplus/tests/fuzz_params.py $H $P 30
python3 plugins/textplus/tests/fuzz_params.py $H build/TextPlusFill.so 10
echo "Images in out/"
