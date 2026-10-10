# Kontur Text

Text-Source für Resolume Arena/Avenue 7 als FFGL-Plugin. Rendert Text über ein
Distanzfeld: Outlines wachsen nur nach außen und fressen nie in die Buchstaben,
Schatten und Glow entstehen ohne Blur-Pässe, und die Kanten bleiben in jeder
Größe scharf.

Stand: Version 0.1, Basis. macOS zuerst (Universal Binary), Windows vorbereitet.

## Bauen und installieren (macOS)

Voraussetzungen: Xcode Command Line Tools und CMake 3.20 oder neuer.

```sh
xcode-select --install      # falls noch nicht vorhanden
brew install cmake
./scripts/build_mac.sh --install
```

Das Skript lädt beim ersten Lauf FFGL SDK, FreeType, HarfBuzz und msdfgen per git
(ein paar Minuten), baut `KonturText.bundle` für arm64 und x86_64, signiert ad hoc
und kopiert es nach `~/Documents/Resolume Arena/Extra Effects`. Danach Resolume neu
starten; das Plugin heißt **Kontur Text** und liegt bei den Sources.

Nutzt du einen anderen Plugin-Ordner (Resolume → Preferences → Video → FFGL),
kopiere `build-mac/KonturText.bundle` dorthin.

## Parameter

| Gruppe | Parameter | Werte |
| --- | --- | --- |
| Text | Text | Zeilenumbruch: echte Umbrüche, `\n` oder `\|` |
| Text | \| = New Line | `\|` als Zeilentrenner an/aus |
| Font | Font | `Familie / Stil`, z. B. `Helvetica Neue / Bold`. Tippen geht: `din bold`, `inter semibold italic` |
| Font | Family, Style | Auswahllisten, schreiben ins Feld „Font“ |
| Font | Font File | .ttf/.otf/.ttc direkt laden (z. B. Show-Fonts, die nicht installiert sind) |
| Font | Size | px |
| Layout | Position X / Y | px ab Bildmitte, Y nach oben |
| Layout | Rotation | Grad, gegen den Uhrzeigersinn |
| Layout | Align | Left, Center, Right, Justify |
| Layout | Anchor H | Auto, Left, Center, Right. Auto: bei freiem Text auf der Ausrichtungsseite, bei Box-Text Box-Mitte |
| Layout | Anchor V | Top, Middle, Bottom, First Baseline, Last Baseline |
| Layout | Box Width / Height | px, 0 = kein Umbruch |
| Layout | Auto Fit | Off, Shrink to Box, Fit Box Width |
| Layout | Tracking | 1/1000 em |
| Layout | Leading | % der Schriftgröße |
| Layout | Kerning, Ligatures | an/aus |
| Fill | Fill Color | Farbe mit Alpha. Alpha 0 = hohle Schrift |
| Outline | Width, Position, Corners, Color | px; Outside/Center/Inside; Round/Sharp |
| Outline 2 | Width, Color | zweite Kontur außen um die erste |
| Shadow | Color, Distance, Angle, Softness, Spread | Distance 0 = aus |
| Glow | Color, Radius, Strength | Radius 0 = aus |
| Animation | Animation, Unit, Progress, Spread, Amount, Speed | Off/Typewriter/Fade/Rise/Wave; Zeichen/Wort/Zeile |
| Quality | Edge Softness | Anti-Aliasing-Breite in px |

Die Schrift wird als Text gespeichert, nicht als Listen-Index. Kompositionen laden
deshalb auch dann die richtige Schrift, wenn inzwischen Fonts installiert oder
entfernt wurden. Fehlt eine Schrift, bleibt ihr Name erhalten und das Plugin nutzt
vorübergehend die Standardschrift.

## Architektur

```
src/core/     plattformneutraler Kern, ohne FFGL und OpenGL
  FontDatabase   Font-Suche (CoreText-URLs auf dem Mac) + FreeType-Scan, Cache auf Platte
  FontFace       Datei-Bytes + HarfBuzz-Font + Metriken
  TextLayout     HarfBuzz-Shaping, Umbruch, Ausrichtung, Anker, Auto-Fit
  GlyphCache     MTSDF-Erzeugung mit msdfgen in Worker-Threads, geteilt
  Animator       Glyph-Zustände als reine Funktion von Progress/Beat
src/render/   OpenGL 4.1 Core
  GlyphAtlas     RGBA16F-Atlas pro Instanz, wächst bis 4096²
  TextRenderer   Pass 1: Distanzfeld der Vereinigung (Depth = min. Abstand)
                 Pass 2: Fill, 2 Outlines, Schatten, Glow, premultiplied
src/plugin/   FFGL-Adapter (Parameter, Events, dynamische Listen)
tests/        Unit-Tests + Headless-FFGL-Host (Linux/EGL)
```

Ausführlich: Dokument „Resolume Text-Plugin: Recherche & Architektur“.

## Tests ohne Resolume (Linux)

`./scripts/run_tests_linux.sh` baut Kern, Plugin und Test-Host, führt die
Unit-Tests aus und rendert Beispielbilder nach `out/`. Der Test-Host lädt das
Plugin wie Resolume über `plugMain`, rendert in einem OpenGL-4.1-Core-Kontext
(Mesa, ohne GPU) und meldet, ob das Plugin GL-Zustand verändert zurücklässt oder
die Premultiplied-Alpha-Regel verletzt.

```sh
./build/ffgl_test_host build/KonturText.so --list                 # Parameterliste
./build/ffgl_test_host build/KonturText.so out/x.png 1280 720 \
    "Text=Hallo" "Outline Width=20" "Glow Radius=40"
```

## Bekannte Grenzen (Version 0.1)

- Effekte reichen bis 1 em um die Glyphe (Distanzfeld-Spread). Darüber hinaus
  werden Outline, Glow und Schatten begrenzt.
- Rechts-nach-links-Text wird geshaped, aber nicht bidirektional umgebrochen.
- Kein Font-Fallback: Zeichen, die in der Schrift fehlen, erscheinen als .notdef.
- Neue Glyphen erscheinen einen Frame später (Erzeugung im Hintergrund).
- Windows: Code ist vorbereitet (Font-Ordner-Scan), Build noch nicht eingerichtet.

## Lizenzen der Abhängigkeiten

FFGL SDK (Resolume, BSD-Stil), FreeType (FreeType License), HarfBuzz (MIT),
msdfgen (MIT). Alle statisch gelinkt.

Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved.
