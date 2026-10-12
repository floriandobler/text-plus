# Changelog

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
