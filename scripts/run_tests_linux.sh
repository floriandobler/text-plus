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

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKONTUR_BUILD_TESTS=ON
cmake --build build

./build/layout_test

H=./build/ffgl_test_host
P=build/KonturText.so
mkdir -p out
$H $P out/default.png 1280 720
$H $P out/outline_outside.png 1280 720 "Text=Kontur" "Font=DejaVu Sans / Bold" "Size=220" "Outline Width=40" "Fill Color=0.08" "Fill Color_Sat=0.9" "Outline 2 Width=10"
$H $P out/outline_center.png 1280 720 "Text=Kontur" "Font=DejaVu Sans / Bold" "Size=220" "Outline Width=40" "Outline Position=1" "Fill Color=0.08" "Fill Color_Sat=0.9"
$H $P out/hollow_shadow.png 1280 720 "Text=HOLLOW" "Font=DejaVu Sans / Bold" "Size=220" "Outline Width=12" "Outline Color_Bri=1" "Fill Color_Alpha=0" "Shadow Distance=24" "Shadow Softness=20" "Shadow Color_Alpha=0.8"
$H $P out/neon.png 1280 720 "Text=NEON" "Font=Inter / Thin" "Size=300" "Outline Width=6" "Outline Color=0.85" "Outline Color_Sat=0.9" "Outline Color_Bri=1" "Glow Radius=60" "Glow Strength=1.4" "Glow Color=0.85" @bg=black
$H $P out/box_justify.png 1280 720 "Text=Distance fields keep every edge sharp at any size, and outlines grow only outward so thick strokes never eat into the letters." "Font=DejaVu Serif / Book" "Size=44" "Box Width=900" "Align=3" "Leading=130" "Outline Width=3"
$H $P out/rise.png 1280 360 "Text=RISE BY CHARACTER" "Font=Inter / Black" "Size=110" "Animation=3" "Progress=0.55" "Spread=4" "Amount=120" "Outline Width=8"
$H $P out/menus.png 320 180 "Font=Inter / Regular" "+Style=3" @frames=16
echo "Bilder in out/"
