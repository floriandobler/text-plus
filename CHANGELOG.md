# Changelog

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
