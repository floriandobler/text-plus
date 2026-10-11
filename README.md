# Text+

Text-Source für Resolume Arena/Avenue 7 als FFGL-Plugin. Rendert Text über ein
Distanzfeld: Outlines wachsen nur nach außen und fressen nie in die Buchstaben,
Schatten und Glow entstehen ohne Blur-Pässe, die Kanten bleiben in jeder Größe
scharf, und zwei Animatoren bewegen, skalieren, drehen, blenden und färben den
Text pro Zeichen, Wort oder Zeile.

Stand: Version 0.6. macOS zuerst (Universal Binary), Windows vorbereitet.

## Bauen und installieren (macOS)

Voraussetzungen: Xcode Command Line Tools und CMake 3.20 oder neuer.

```sh
xcode-select --install      # falls noch nicht vorhanden
brew install cmake
./scripts/build_mac.sh --install
```

Das Skript lädt beim ersten Lauf FFGL SDK, FreeType, HarfBuzz und msdfgen per git
(ein paar Minuten), baut `TextPlus.bundle` für arm64 und x86_64, signiert ad hoc
und kopiert es nach `~/Documents/Resolume Arena/Extra Effects`. Ein altes
`KonturText.bundle` (Version 0.1) entfernt es dabei, denn beide tragen dieselbe
Plugin-ID. Danach Resolume neu starten; das Plugin heißt **Text+** und liegt bei
den Sources direkt neben Text Animator und Text Block.

Nutzt du einen anderen Plugin-Ordner (Resolume → Preferences → Video → FFGL),
kopiere `build-mac/TextPlus.bundle` dorthin.

## Parameter

| Gruppe | Parameter | Werte |
| --- | --- | --- |
| Text | Text | Zeilenumbruch: `\|` (bei „\| = New Line“), `\n` oder echte Umbrüche |
| Text | + Line, - Line, Line 2 … Line 10 | weitere Textfelder, jedes eine eigene Zeile unter „Text“; „+ Line“ blendet das nächste ein, „- Line“ leert und entfernt das letzte |
| Text | Color Tags | `{#ff8800}farbig{/}` färbt Textstellen; `{{` für ein echtes `{` |
| Text | Text File | .txt oder .csv als Textquelle, wird bei Änderung automatisch neu geladen |
| Text | CSV: Skip First Row | CSV: jede Zeile eine Slide, die Zellen werden ihre Zeilen (z. B. Name, Funktion). Erste Zeile = Kopfzeile. Trennzeichen `;` `,` oder Tab wird erkannt, Excel-Export (Windows-1252) wird umgewandelt |
| Text | Slides | Off, Each Line, Blocks (`\|\|` oder Leerzeile trennt) |
| Text | Slide | Position 0–1, gleichmäßig auf alle Slides verteilt (Anzeige „2 / 5“). Mit BPM Sync animiert läuft sie durch alle Slides und beginnt wieder bei der ersten. Resolumes −/+ neben dem Regler schalten eine ganze Slide weiter (ein einzelner Schritt von 0,01 wird als „vorige/nächste Slide“ erkannt) |
| Text | Next Slide, Previous Slide | einzeln mappbare Buttons; springen am Ende zurück auf die erste bzw. letzte Slide |
| Font | Font | `Familie / Stil`, z. B. `Helvetica Neue / Bold`. Tippen geht: `din bold` |
| Font | Family, Style | Auswahllisten, schreiben ins Feld „Font“ |
| Font | Font File | .ttf/.otf/.ttc direkt laden |
| Font | Size | px |
| Layout | Position X / Y, Rotation | px ab Bildmitte (Y nach oben), Grad |
| Layout | Align, Anchor H, Anchor V | Ausrichtung und Ankerpunkt, getrennt einstellbar |
| Layout | Box Width / Height, Auto Fit | Umbruch in einer Box, Verkleinern oder Einpassen |
| Layout | Tracking, Leading, Kerning, Ligatures | 1/1000 em, % der Größe |
| Fill | Fill Mode | Solid, Gradient (Block), Gradient (Characters) |
| Fill | Fill Color, Fill Color 2, Gradient Angle | Alpha 0 = hohle Schrift |
| Outline | Width, Position, Corners, Color | px; Outside/Center/Inside; Round/Sharp |
| Outline 2 | Width, Color | zweite Kontur außen um die erste |
| Shadow | Color, Distance, Angle, Softness, Spread | Distance 0 = aus |
| Glow | Color, Radius, Strength | Radius 0 = aus |
| Animator 1, 2 | Preset | lädt Werte, springt danach zurück auf „Load preset…“ |
| Animator 1, 2 | Mode | Off, Sequence In, Sequence Out, Wave, Random, Constant |
| Animator 1, 2 | Unit, Order | Character/Word/Line; Forward, Reverse, From Center, From Edges, Random |
| Text | Replay on Slide Change | Beats; > 0 spielt Sequence-Animatoren bei jedem Slide-Wechsel neu ab (ersetzt dann deren Progress) |
| Animator 1, 2 | Progress | Sequence: Position; Wave: Phase (0–1 = eine Welle); Random: Position im Zufalls-Loop; Constant: Stärke. Bewegung entsteht durch Animieren in Resolume (BPM Sync, Timeline) |
| Animator 1, 2 | Spread, Ease | gleichzeitig bewegte Einheiten (Sequence), Wellenlänge (Wave), Anzahl Zufallszustände (Random); Linear/Smooth/Ease Out/Overshoot |
| Animator 1, 2 | Offset X/Y, Scale, Rotation, Opacity, Color | der „Weg“-Zustand; Color-Alpha = Stärke der Färbung |
| Quality | Edge Softness | Anti-Aliasing-Breite in px |

Der Animator beschreibt, wo eine Einheit „weg“ ist (versetzt, verkleinert,
gedreht, unsichtbar, eingefärbt). Sequence In bringt die Einheiten mit Progress
0 → 1 nacheinander in die Ruhelage, Sequence Out umgekehrt. Bei Wave ist
Progress die Phase, bei Random die Position in einer Schleife von
Zufallszuständen; beide laufen nahtlos, wenn Progress per BPM Sync von 0 bis 1
loopt. Constant wendet den Zustand mit der Stärke Progress auf alle an. Zwei
Animatoren addieren sich. Im Plugin läuft keine eigene Uhr: Bewegung entsteht
durch Resolumes Parameter-Animation (Ausnahme: Replay on Slide Change).

Die Schrift wird als Text gespeichert, nicht als Listen-Index. Kompositionen laden
deshalb auch dann die richtige Schrift, wenn inzwischen Fonts installiert oder
entfernt wurden. Fehlt eine Schrift, bleibt ihr Name erhalten und das Plugin nutzt
vorübergehend die Standardschrift.

## Architektur

```
src/core/     plattformneutraler Kern, ohne FFGL und OpenGL
  FontDatabase   Font-Suche (CoreText-URLs auf dem Mac) + FreeType-Scan, Cache auf Platte
  FontFace       Datei-Bytes + HarfBuzz-Font + Metriken
  TextLayout     Farb-Tags, HarfBuzz-Shaping, Umbruch, Ausrichtung, Anker, Auto-Fit
  GlyphCache     MTSDF-Erzeugung mit msdfgen in Worker-Threads, sichtbare Glyphen zuerst
  Animator       Glyph-Zustände als reine Funktion von Progress/Beat
src/render/   OpenGL 4.1 Core
  GlyphAtlas     RGBA16F-Atlas pro Instanz, wächst bis 4096²
  TextRenderer   Feld-Pässe: Distanzfeld der Vereinigung (Depth = min. Abstand); bei
                 unterschiedlicher Deckkraft die 4 nächsten Glyphen pro Pixel (Depth Peeling)
                 Composite: Fill, 2 Outlines, Schatten, Glow; Deckkraft als Schwellen-
                 Integral, damit ausblendende Zeichen nahtlos aus verschmolzenen Konturen
                 verschwinden; nur in der Text-Bounding-Box
src/plugin/   FFGL-Adapter (Parameter, Presets, Slides, Events, dynamische Listen)
tests/        Unit-Tests + Headless-FFGL-Host (Linux/EGL)
```

Shader und Zwischenpuffer gibt es einmal pro GL-Kontext, alle Instanzen teilen sie.
Die Ausgabe ist straight alpha, so wie Resolume Sources verrechnet.

Menüs, die etwas auslösen (Presets, Family, Style), und die Slide-Position
reagieren nur auf Werte, die sich gegenüber dem zuletzt vom Host gesendeten
Wert geändert haben. Schickt Resolume unveränderte Werte erneut, wird nichts
noch einmal angewendet.

Ausführlich: Dokument „Resolume Text-Plugin: Recherche & Architektur“.

## Tests ohne Resolume (Linux)

`./scripts/run_tests_linux.sh` baut Kern, Plugin und Test-Host, führt die
Unit-Tests aus und rendert Beispielbilder nach `out/`. Der Test-Host lädt das
Plugin wie Resolume über `plugMain`, rendert in einem OpenGL-4.1-Core-Kontext
(Mesa, ohne GPU) und meldet, ob das Plugin GL-Zustand verändert zurücklässt.

```sh
./build/ffgl_test_host build/TextPlus.so --list                 # Parameterliste
./build/ffgl_test_host build/TextPlus.so out/x.png 1280 720 \
    "Text=Hallo" "Outline Width=20" "Glow Radius=40" @bg=0.9,0.3,0.1
# "+Name=Wert" setzt einen Wert erst nach einigen Frames (wie eine Nutzeraktion),
# "@dt=0.0166 @frames=90" lässt die Zeit laufen, "@print=A1 Mode" gibt Werte aus,
# "@ramp=Slide:0:1" animiert einen Parameter, "@resend=stale" schickt wie ein Host
# jeden Frame alle Werte erneut, "@trace=…" und "@dump=dir" zeigen jeden Frame.
```

## Bekannte Grenzen (Version 0.6)

- Resolumes Textfeld für Plugins ist einzeilig: Zeilen per `|` trennen oder eine
  Text-Datei verwenden.
- Effekte reichen bis 1 em um die Glyphe (Distanzfeld-Spread). Darüber hinaus
  werden Outline, Glow und Schatten begrenzt.
- Rechts-nach-links-Text wird geshaped, aber nicht bidirektional umgebrochen.
- Kein Font-Fallback: Zeichen, die in der Schrift fehlen, erscheinen als .notdef.
- Neue Glyphen erscheinen einen Frame später (Erzeugung im Hintergrund).
- Kompositionen mit Text+ 0.2/0.3: Slides und Animatoren neu einstellen (Parameter geändert).
- Resolume entfernt Zeilenumbrüche beim Einfügen ins Text-Feld: Zeilenfelder („+ Line“), `|` oder eine Datei verwenden.
- Resolume zeigt die internen Parameternamen („A1 Mode“), keine Anzeigenamen.
- Kurzer Hänger beim Reinziehen oder Kopieren eines Clips: tritt bei Resolumes
  eigenen Text-Sources genauso auf, liegt also bei Resolume.
- Windows: Code ist vorbereitet (Font-Ordner-Scan), Build noch nicht eingerichtet.

## Lizenzen der Abhängigkeiten

FFGL SDK (Resolume, BSD-Stil), FreeType (FreeType License), HarfBuzz (MIT),
msdfgen (MIT). Alle statisch gelinkt.

Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved.
