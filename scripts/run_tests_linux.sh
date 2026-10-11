#!/usr/bin/env bash
# Entwicklungs-Tests ohne Resolume (Linux, headless):
#  1. Unit-Tests für Font-Datenbank, Layout, Animation und Glyphen-SDFs
#  2. Ein Test-Host lädt das Plugin wie Resolume (plugMain), rendert mit Mesa
#     in einem OpenGL-4.1-Core-Kontext und schreibt PNGs nach out/.
#     Er prüft dabei, dass das Plugin den GL-Zustand unverändert zurücklässt
#     und premultiplied Alpha liefert.
#
# Pakete (Ubuntu 24.04): cmake ninja-build libglew-dev libegl-dev libgl1-mesa-dri
#                         fonts-dejavu-core fonts-cantarell fonts-inter
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
$H $P out/slides_beats.png 960 200 "Text=AAAA|BBBB|CCCC" "Font=Inter / Black" "Size=110" "Slides=1" "Advance=2" "Beats per Slide=2" "+A1 Preset=1" "+A1 Drive=1" "+A1 Duration=1" @dt=0.016666667 @frames=76
$H $P out/menus.png 320 180 "Font=Inter / Regular" "+Style=3" @frames=16
echo "Bilder in out/"
