# Changelog

## 2.0.0

**A clearer panel**
- The empty panel shows 22 rows in 7 groups instead of 52 in 13; a typical lower third needs 63 rows instead of 100. Rows appear when a menu asks for them, never through slider values.
- Display names (FFGL 2.3, Resolume 7.4+): rows read "Offset X" instead of "A1 Offset X". The stored names are unchanged, so compositions and OSC addresses keep working.
- *Style* replaces the groups *Fill*, *Outline*, *Shadow* and *Glow*: a *Look* menu, the fill, and up to six **effect slots**. A slot's type sets what its rows are called and which ones show. The next empty slot appears once the previous one is used.
- Animators show only the properties you add with *+ / - Property*. A property that does something (from a composition, OSC or MIDI) shows itself.
- *Placement*: Center, Top, Bottom, Left, Right, the four corners and Lower Third, with a margin that adapts to any resolution. *Custom* shows position, rotation and anchors.
- *Align*, *Leading* and the *Line 2* / *Line 3+* groups appear when the text has more than one line. *Options* sit behind *Show Options*.

**Look**
- **Looks**: Clean, Outline, Double Outline, Hollow, Soft Shadow, Long Shadow, Comic, Neon, Chrome, Retro 3D, Emboss, Glass, Contours, Ink, Lower Third Bar. Their sizes follow the text size.
- **Effect stack**: Outline (several, stacking outwards), Glow (several), Shadow (up to three), Inner Glow, Inner Shadow (up to two, each with its own angle), **Bevel** (Smooth, Chisel, Chrome), **Extrude** (2.5D depth with shading, also long shadows), **Contours** (rings around or inside the text), **Roughen** (noisy edges, loopable).
- **Weight**: makes any font bolder or thinner through the distance field.
- New fills: radial gradient, stripes, halftone.

**3D and paths**
- Characters turn in 3D (*Rotation X*, *Rotation Y*), move in depth (*Depth*) and slant (*Skew*), with a perspective (*Camera Distance*).
- *Path*: Arc (bend up or down), Circle, Wave and a **Ring** that spins in 3D; *Offset* / *Spin* / *Phase* move the text along.

**Content and live data**
- Placeholders in every source: `{bpm}`, `{beat}`, `{bar}`, `{slide}`, `{slides}`, `{time}`, `{time:s}`, `{date}`, `{date:iso}`, `{weekday}`.
- **OSC receiver** (*Options → OSC Port*): every address becomes a variable, shown with `{/score/home}`. Shared by all Text+ instances.
- **Web (URL)** source: JSON (the text fields are the template, e.g. `{main.temp} °C`), CSV (rows become slides, works with published Google Sheets) or plain text, refreshed in the background. Without placeholders the JSON's fields are listed with their paths.
- JSON files work like JSON from the web.
- Countdowns: *Warn At* switches to a warning colour (optionally blinking) when time runs out.
- Display **Slides: Words** (one word after another) and **Pattern** (rows of text filling the screen, alternating directions).
- Equal-width digits for clocks, web data and OSC values.

**Animation**
- A third animator; each appears once the one before it runs.
- New properties: *Depth*, *Rotation X*, *Rotation Y*, *Skew*, *Tracking*, *Blur*, *Weight*.
- Easing: *Ease In*, *Bounce*, *Elastic*.
- Presets: Flip In / Out, Swing In, Depth In (3D), Blur In, Track In, Bounce In, Elastic Pop, Skew In, Weight Pulse.

**Text+ FX** (new plugin, effect)
- Everything of Text+ as an effect on a clip: text over the video, the video inside the text, or the video cut by the text.

**Compatibility**
- Compositions from 1.1 open with the same look: the old Outline, Shadow and Glow settings are translated into effect slots (tested pixel-identical). Position, rotation or anchors that are not at their defaults set *Placement* to *Custom*.

**Under the hood**
- The composite comes in a small variant (the 1.1 feature set) and a full one that is compiled on first use of bevel, extrude, contours, roughen or Text+ FX.
- Glyphs in 3D measure their distances in screen space; flat glyphs keep the exact 1.1 path.
- New unit tests (JSON, placeholders, OSC decoding, easing, 3D properties), panel dumps in the test host (`@ui=`), input textures for effects (`@input=1`); ASan/UBSan clean, OSC fuzzed with 320 000 random packets.

## 1.1.0

**Content**
- *Source* (Input / File / Clock & Timer) and *Display* (Static / Slides: Lines / Slides: Blocks / Ticker / Roll) replace *Slides*. Compositions from 1.0 open with the matching settings.
- Clock & Timer: clock, countdown to a time of day, countdown, stopwatch and counter, with *Start / Stop / Reset*, *Start with Clip*, time and number formats, *At Zero* (hold, hide, count up). The text fields are the template: `{timer}`, `{time}`, `{date}`. Digits have equal widths, so running numbers don't jitter.
- Ticker: endless horizontal crawl in px/s with a separator and soft edges, as wide as the output or the wrap box. Roll: vertical roll, once or looping.
- A CSV file switches a static display to slides once; CSV rows also feed tickers and rolls.

**Look**
- Plates behind the block or every line: colour, padding, corner radius, slant, width (fit text / equal / box), border. Overlapping line plates melt into one shape; the drop shadow is cast by plates and text together.
- Inner glow and inner shadow (same light direction as the drop shadow).
- *Case*: Tt / TT / tt (Latin, Greek, Cyrillic; ß becomes SS).

**Animation**
- *Target*: text, plates or both.
- *Sequence In/Out* with *Hold*.
- *Wipe* (0–100 %) with *Wipe From* left, right, centre, top or bottom: masks text, cuts plates (they grow with their border and shadow).
- *Scramble* with *Characters*: letters, digits, symbols, binary or the text's own characters; random characters are centred on the real ones.
- *Replay Stagger*: Animator 2 starts later when sequences replay on slide changes.
- Presets: Wipe In, Decode, Fade In-Hold-Out, Lower Third In / Out (both animators), Glitch.

**Fonts**
- Font fallback: missing characters come from the font the OS would use (CoreText on macOS, DirectWrite on Windows), emoji from the built-in monochrome Noto Emoji (with outlines and glow like text), incl. flags, keycaps and ZWJ sequences.

**Platform**
- Windows x64 build (MSVC), tested automatically in a software renderer; not yet tested in Resolume.
- UTF-8 file paths on every platform (font and text files in folders with umlauts).
- Monorepo: the renderer (`sdfkit`) and the FFGL plumbing (`ffglkit`) are libraries for future plugins.

**Under the hood**
- The composite runs as one incremental pass over the opacity levels (smaller shader, faster first use).
- New unit tests (clock, case, animator, fallback) and regression scenes; ASan/UBSan clean.

## 1.0.1

Bug fix release.

- Fixed flicker and jagged notches in merged outlines when *Opacity* and *Offset* are animated together and characters or lines slide over each other. The renderer keeps up to four glyphs per pixel for the opacity integration; it used to pick the four *nearest* ones, so in dense overlaps a relevant glyph could drop out and come back from frame to frame. It now picks them along the opacity order (most opaque first) and ignores glyphs that are too far away to contribute to any effect.
- Plugin version reported to the host is now 1.0 (was a leftover 0.2).

## 1.0.0

First public release. Everything below was developed in pre-releases 0.1–0.6, tested in Resolume Arena 7.23 on macOS.

- Distance-field renderer: outlines grow outward only, merged contours, two outline bands, shadow, glow, gradients, hollow text.
- Opacity integrated as a threshold over the four nearest glyphs per pixel: fading characters leave merged outlines without holes or popping.
- Line styles *Line 2* and *Line 3+*: weight or font, size, colour, tracking, spacing.
- Text fields *Text 2 … Text 10* (*+ Line* / *− Line*), `|` as line break, text files, CSV (rows = slides, cells = lines).
- Slides with a 0–1 *Slide* position for BPM sync, *Next / Previous*, Resolume's −/+ step whole slides, *Replay on Slide Change*.
- Two animators (Sequence In/Out, Wave, Random, Constant) driven by *Progress*; presets.
- Font menus that survive font changes on the system, font files, variable-font named instances.
- Straight-alpha output (Resolume's convention); shaders and scratch buffers shared per GL context.
- Robust against hosts re-sending unchanged parameter values.
