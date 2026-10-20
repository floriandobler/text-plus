#!/usr/bin/env python3
"""Renders the README images with the headless test host (Linux, Mesa).

Usage: tools/make_showcase.py <fonts-dir> [out-dir] [--v2] [--old-plugin=TextPlus-1.1.so]
  --v2          render only the 2.0 images
  --old-plugin  a Text+ 1.1 build for the interface comparison (v2_ui.png)
<fonts-dir> holds a few Google Fonts (OFL): Anton, Bebas Neue, Space Grotesk,
Archivo Black, Righteous (e.g. a sparse checkout of github.com/google/fonts, ofl/...).
Inter must be installed system-wide. Needs Pillow. Run from the repository root
after ./scripts/run_tests_linux.sh (uses build/ffgl_test_host and build/TextPlus.so).
"""
import glob, os, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ARGS = [a for a in sys.argv[1:] if not a.startswith('--')]
FLAGS = [a for a in sys.argv[1:] if a.startswith('--')]
FONTS = ARGS[0]
OUT = ARGS[1] if len(ARGS) > 1 else 'docs/images'
ONLY_V2 = '--v2' in FLAGS
OLD_PLUGIN = next((f.split('=', 1)[1] for f in FLAGS if f.startswith('--old-plugin=')), None)
HOST, PLUGIN, FILL_PLUGIN = 'build/ffgl_test_host', 'build/TextPlus.so', 'build/TextPlusFill.so'
os.makedirs(OUT, exist_ok=True)


def font_file(name):
    hits = glob.glob(os.path.join(FONTS, '**', name), recursive=True)
    if not hits:
        sys.exit('font not found: ' + name)
    return hits[0]


def render(w, h, *params, frames=24, dump=None, dt=None, plugin=None):
    out = tempfile.mktemp(suffix='.png')
    args = [HOST, plugin or PLUGIN, out, str(w), str(h), *params, '@bg=none', f'@frames={frames}']
    if dt:
        args.append(f'@dt={dt}')
    if dump:
        args.append('@dump=' + dump)
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(r.stdout + r.stderr)
    return Image.open(out).convert('RGBA')


def background(w, h, top=(18, 20, 38), bottom=(52, 22, 64)):
    bg = Image.new('RGB', (w, h))
    px = bg.load()
    for y in range(h):
        t = y / max(1, h - 1)
        for x in range(w):
            s = x / max(1, w - 1)
            c = [int(top[i] * (1 - t) + bottom[i] * t) for i in range(3)]
            v = 1.0 - 0.35 * ((s - 0.5) ** 2 + (t - 0.5) ** 2)  # soft vignette
            px[x, y] = tuple(max(0, min(255, int(ch * v))) for ch in c)
    return bg.convert('RGBA')


def over(bg, fg):
    out = bg.copy()
    out.alpha_composite(fg)
    return out.convert('RGB')


def label(img, text, xy, size=22, fill=(200, 205, 225)):
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(glob.glob('/usr/share/fonts/**/Inter*Regular*.ttf', recursive=True)[0] if glob.glob('/usr/share/fonts/**/Inter*Regular*.ttf', recursive=True) else font_file('SpaceGrotesk*'), size)
    d.text(xy, text, font=f, fill=fill)


anton, bebas = font_file('Anton-Regular.ttf'), font_file('BebasNeue-Regular.ttf')
grotesk, archivo = font_file('SpaceGrotesk*.ttf'), font_file('ArchivoBlack-Regular.ttf')
righteous = font_file('Righteous-Regular.ttf')

# Animated GIFs: dump frames (RGBA), composite on a background
def gif(name, w, h, params, frames, dt=1 / 30, step=1, bgcolors=None):
    d = tempfile.mkdtemp()
    render(w, h, *params, frames=frames, dump=d, dt=dt)
    bg = background(w, h, *(bgcolors or ()))
    imgs = []
    for f in sorted(glob.glob(d + '/f*.png'))[10::step]:
        imgs.append(over(bg, Image.open(f).convert('RGBA')).convert('P', palette=Image.ADAPTIVE, colors=200))
    imgs += [imgs[-1]] * 20  # hold the last frame
    imgs[0].save(f'{OUT}/{name}', save_all=True, append_images=imgs[1:], duration=int(1000 * dt * step), loop=0, optimize=True)



if not ONLY_V2:
    # 1. Hero: thick outside outlines, glow, a styled second line
    W, H = 1600, 700
    fg = render(W, H, 'Text=TEXT+', 'Text 2=TEXT SOURCE FOR RESOLUME', f'Font File={anton}', 'Size=330', 'Position Y=30',
                'Fill Color=0.0', 'Fill Color_Sat=0', 'Fill Color_Bri=1', 'Outline Width=18', 'Outline Color=0.93', 'Outline Color_Sat=0.85',
                'Outline Color_Bri=0.95', 'Outline 2 Width=12', 'Outline 2 Color_Bri=0.12', 'Glow Radius=60', 'Glow Color=0.93', 'Glow Color_Sat=0.8',
                'Glow Strength=0.8', 'Tracking=90', 'L2 Style=1', 'L2 Size=20', 'L2 Tracking=260', 'L2 Spacing=16', 'Leading=105')
    over(background(W, H), fg).save(f'{OUT}/hero.png')

    # 2. Outlines grow outward only
    W, H = 1600, 420
    bg = background(W, H)
    widths = [0, 12, 36, 72]
    for i, ow in enumerate(widths):
        fg = render(400, 360, 'Text=Ra', f'Font File={archivo}', 'Size=150', 'Fill Color_Bri=1', f'Outline Width={ow}', 'Outline Color=0.55',
                    'Outline Color_Sat=0.9', 'Outline Color_Bri=0.9')
        bg.alpha_composite(fg, (i * 400, 0))
    img = bg.convert('RGB')
    for i, ow in enumerate(widths):
        label(img, f'Outline {ow} px', (i * 400 + 140, 370))
    img.save(f'{OUT}/outline_outside.png')

    # 3. Lower third with line styles
    W, H = 1600, 500
    fg = render(W, H, 'Text=Anna Musterfrau', 'Text 2=LIGHTING DESIGN', 'Text 3=Staatstheater Beispielstadt', 'Font=Inter / Black', 'Size=96',
                'Align=0', 'Anchor H=1', 'Position X=-620', 'Position Y=-40', 'L2 Style=1', 'L2 Font=SemiBold', 'L2 Size=42',
                'L2 Color=0.09', 'L2 Color_Sat=0.85', 'L2 Color_Alpha=1', 'L2 Tracking=160', 'L2 Spacing=10', 'L3 Style=1', 'L3 Font=Light Italic',
                'L3 Size=36', 'Shadow Distance=6', 'Shadow Softness=14', 'Shadow Color_Alpha=0.7')
    over(background(W, H, (24, 34, 44), (14, 18, 26)), fg).save(f'{OUT}/lower_third.png')

    # 4. Hollow text with a soft shadow | block gradient
    W, H = 1600, 420
    bg = background(W, H, (30, 30, 36), (60, 40, 30))
    fg = render(800, H, 'Text=HOLLOW', f'Font File={bebas}', 'Size=260', 'Fill Color_Alpha=0', 'Outline Width=8', 'Outline Position=1',
                'Outline Color_Bri=1', 'Shadow Distance=22', 'Shadow Angle=-50', 'Shadow Softness=26', 'Shadow Spread=4', 'Shadow Color_Alpha=0.85')
    fg2 = render(800, H, 'Text=GRADIENT', f'Font File={bebas}', 'Size=220', 'Fill Mode=1', 'Fill Color=0.95', 'Fill Color_Sat=0.8',
                 'Fill Color 2=0.55', 'Fill Color 2_Sat=0.8', 'Gradient Angle=-20', 'Glow Radius=40', 'Glow Color=0.75', 'Glow Strength=0.6')
    bg.alpha_composite(fg, (0, 0))
    bg.alpha_composite(fg2, (800, 0))
    bg.convert('RGB').save(f'{OUT}/hollow_gradient.png')

    # 5. Colour tags and per-character gradient
    W, H = 1600, 380
    fg = render(W, H, 'Text=Colour {#ff4d6d}any{/} {#ffd84d}word{/} {#4dd2ff}inline{/}', f'Font File={righteous}', 'Size=92',
                'Outline Width=5', 'Outline Color_Bri=0.05', 'Glow Radius=24', 'Glow Color=0.6', 'Glow Strength=0.5')
    over(background(W, H), fg).save(f'{OUT}/color_tags.png')


    gif('anim_fade_merge.gif', 960, 300, ['Text=merged outlines', 'Font=Inter / Black', 'Size=110', 'Outline Width=16', 'Outline Color=0.52',
                                          'Outline Color_Sat=0.9', 'Outline Color_Bri=0.9', 'Outline 2 Width=6', 'Glow Radius=30', 'Glow Color=0.52',
                                          'A1 Mode=1', 'A1 Opacity=0', 'A1 Spread=2', 'A1 Ease=1', '@ramp=A1 Progress:0:1'], 80, step=2)
    gif('anim_rise.gif', 960, 300, ['Text=Rise and shine', f'Font File={grotesk}', 'Font=Space Grotesk / Bold', 'Size=120', 'A1 Mode=1',
                                    'A1 Offset Y=-120', 'A1 Opacity=0', 'A1 Spread=4', 'A1 Ease=2', 'Outline Width=4',
                                    '@ramp=A1 Progress:0:1'], 80, step=2)
    gif('anim_wave.gif', 960, 300, ['Text=WAVE OF COLOUR', f'Font File={anton}', 'Size=140', 'A1 Mode=3', 'A1 Offset Y=24', 'A1 Spread=7',
                                    'A1 Color=0.85', 'A1 Color_Sat=0.9', 'A1 Color_Alpha=1', 'Outline Width=6', 'Outline Color_Bri=0.05',
                                    '@ramp=A1 Progress:0:2'], 70, step=2)
    gif('anim_typewriter.gif', 960, 240, ['Text=Typewriter, one character at a time.', 'Font=Inter / Medium', 'Size=56', 'Align=0', 'Anchor H=1',
                                          'Position X=-430', 'A1 Mode=1', 'A1 Opacity=0', 'A1 Spread=0', '@ramp=A1 Progress:0:1'], 80, step=2)

    # --- 1.1 ---
    # Plates: a lower third on melting line plates (slant, border, shadow)
    W, H = 1600, 460
    fg = render(W, H, 'Text=Anna Musterfrau', 'Text 2=LIGHTING DESIGN', 'Font=Inter / Black', 'Size=96', 'Align=0', 'Anchor H=1',
                'Position X=-620', 'Position Y=-20', 'L2 Style=1', 'L2 Font=SemiBold', 'L2 Size=42', 'L2 Tracking=160', 'L2 Spacing=26',
                'Plate=2', 'Plate Color=0.6', 'Plate Color_Sat=0.85', 'Plate Color_Bri=0.6', 'Plate Color_Alpha=0.95', 'Plate Radius=12',
                'Plate Slant=-10', 'Plate Border=4', 'Plate Border Color_Bri=1', 'Shadow Distance=10', 'Shadow Softness=24', 'Shadow Color_Alpha=0.7')
    over(background(W, H, (24, 34, 44), (14, 18, 26)), fg).save(f'{OUT}/plates.png')

    gif('anim_lower_third.gif', 1280, 360, ['Text=Anna Musterfrau', 'Text 2=LIGHTING DESIGN', 'Font=Inter / Black', 'Size=80', 'Align=0',
                                            'Anchor H=1', 'Position X=-560', 'L2 Style=1', 'L2 Font=SemiBold', 'L2 Size=42', 'L2 Tracking=160',
                                            'L2 Spacing=24', 'Plate Color=0.6', 'Plate Color_Sat=0.85', 'Plate Color_Bri=0.6', 'Plate Radius=10',
                                            '+A1 Preset=15', '@ramp=A1 Progress:0:1', '@ramp=A2 Progress:-0.45:1'], 70, step=2,
        bgcolors=((24, 34, 44), (14, 18, 26)))

    gif('anim_ticker.gif', 1280, 160, ['Display=3', 'Text=+++ Text+ 1.1 brings tickers, rolls, clocks and plates', 'Text 2=Doors open at 20:00',
                                       'Font=Inter / Bold', 'Size=56', 'Scroll Speed=260', 'Plate=1', 'Plate Color=0.0', 'Plate Color_Sat=0.85',
                                       'Plate Color_Bri=0.7', 'Plate Padding Y=18', 'Edge Fade=80', 'Separator=   •   '], 90, step=2,
        bgcolors=((20, 22, 30), (30, 30, 40)))

    gif('anim_decode.gif', 960, 240, ['Text=DECODING', f'Font File={grotesk}', 'Font=Space Grotesk / Bold', 'Size=130', 'Outline Width=4',
                                      'Glow Radius=26', 'Glow Color=0.38', 'Glow Strength=0.7', '+A1 Preset=14', '@ramp=A1 Progress:0:1'], 80, step=2)

    # Clock & timer
    W, H = 1600, 360
    bg = background(W, H)
    fg = render(800, H, 'Source=2', 'Timer Mode=2', 'Duration=4:59', 'Text=DOORS OPEN IN', 'Text 2={timer}', f'Font File={anton}', 'Size=70',
                'Tracking=60', 'L2 Style=1', 'L2 Size=260', 'L2 Tracking=-60', 'Leading=105', 'Fill Color_Bri=1', 'Outline Width=6', 'Outline Color_Bri=0.05', frames=6)
    fg2 = render(800, H, 'Source=2', 'Timer Mode=4', 'Count From=0', 'Count To=12840', 'Count=1', 'Number Format=1', 'Text={timer}',
                 'Text 2=VISITORS TODAY', f'Font File={archivo}', 'Size=120', 'L2 Style=1', 'L2 Size=30', 'L2 Tracking=200', 'L2 Spacing=10',
                 'Fill Mode=1', 'Fill Color=0.12', 'Fill Color_Sat=0.85', 'Fill Color 2=0.95', 'Fill Color 2_Sat=0.8', 'Gradient Angle=-90', frames=6)
    bg.alpha_composite(fg, (0, 0))
    bg.alpha_composite(fg2, (800, 0))
    bg.convert('RGB').save(f'{OUT}/clock.png')

    # Inner glow / inner shadow | emoji and other scripts through font fallback
    W, H = 1600, 380
    bg = background(W, H, (30, 30, 36), (60, 40, 30))
    fg = render(800, H, 'Text=INNER', f'Font File={archivo}', 'Size=170', 'Fill Color=0.08', 'Fill Color_Sat=0.9', 'Inner Glow Radius=22',
                'Inner Glow Color=0.14', 'Inner Glow Color_Sat=0.35', 'Inner Shadow Distance=9', 'Inner Shadow Softness=8', 'Shadow Angle=135',
                'Outline Width=5', 'Outline Color_Bri=0.08')
    fg2 = render(800, H, 'Text=Emoji 🎉 👍🏽 ☕', 'Text 2=Ελλάδα · Привет · שלום', 'Font=Inter / Bold', 'Size=72', 'L2 Style=1', 'L2 Size=70',
                 'Outline Width=6', 'Outline Color=0.6', 'Outline Color_Sat=0.8', 'Outline Color_Bri=0.55', frames=30)
    bg.alpha_composite(fg, (0, 0))
    bg.alpha_composite(fg2, (800, 0))
    bg.convert('RGB').save(f'{OUT}/inner_and_fallback.png')


# --- 2.0 ---
def tile_label(img, text, xy, size=20):
    label(img, text, xy, size=size, fill=(170, 176, 200))


# 3D: extruded with bevel, light and gloss
W, H = 1600, 520
bg = background(W, H, (16, 18, 30), (40, 26, 52))
fg = render(800, H, 'Text=CHROME', f'Font File={archivo}', 'Size=150', '+Look=9', '++Tilt Y=-18', '++Tilt X=16', frames=30)
fg2 = render(800, H, 'Text=RETRO', 'Text 2=WAVE', f'Font File={anton}', 'Size=150', 'Leading=95', '+Look=10', frames=30)
bg.alpha_composite(fg, (0, 0))
bg.alpha_composite(fg2, (800, 0))
bg.convert('RGB').save(f'{OUT}/v2_3d.png')

# Looks: one menu sets the whole style
W, H, tw, th = 1600, 680, 533, 340
bg = background(W, H, (20, 20, 30), (34, 24, 44))
tiles = [('Neon', ['Text=NEON', f'Font File={righteous}', 'Size=120', '+Look=5']),
         ('Sticker', ['Text=Sticker', f'Font File={archivo}', 'Size=104', '+Look=4']),
         ('Comic (halftone)', ['Text=POW!', f'Font File={bebas}', 'Size=170', '+Look=6']),
         ('LED Sign', ['Text=OPEN 24/7', 'Font=Inter / Black', 'Size=80', '+Look=11']),
         ('News Bar', ['Text=BREAKING', 'Font=Inter / Black', 'Size=66', '+Look=8']),
         ('Stripes', ['Text=Stripes', f'Font File={anton}', 'Size=140', '+Look=13'])]
img = None
for i, (name, params) in enumerate(tiles):
    fg = render(tw, th, *params, frames=30)
    bg.alpha_composite(fg, ((i % 3) * tw, (i // 3) * th))
img = bg.convert('RGB')
for i, (name, params) in enumerate(tiles):
    tile_label(img, name, ((i % 3) * tw + 24, (i // 3) * th + th - 40))
img.save(f'{OUT}/v2_looks.png')

# Text on a path: a ring around the anchor, a wave
W, H = 1600, 640
bg = background(W, H, (14, 20, 32), (20, 40, 48))
fg = render(800, H, 'Text=TEXT ON A PATH • ROUND AND ROUND • ', 'Text 2=inner ring · second line · smaller', '+ Line=1', 'L2 Style=1',
            'L2 Size=55', 'L2 Color=0.5', 'L2 Color_Sat=0.7', 'L2 Color_Alpha=1', f'Font File={grotesk}', 'Font=Space Grotesk / Bold',
            'Size=52', 'Path=2', 'Path Radius=250', 'Outline=1', 'Outline Width=3', 'Outline Color_Bri=0.05')
fg2 = render(800, H, 'Text=Riding the wave', 'Text 2=of animated text', '+ Line=1', f'Font File={archivo}', 'Size=74', 'Path=3',
             'Wave Height=46', 'Wave Length=620', 'Path Offset=0.15', 'Fill Mode=1', 'Fill Color=0.52', 'Fill Color_Sat=0.8', 'Fill Color 2=0.83',
             'Fill Color 2_Sat=0.7', 'Glow=1', 'Glow Radius=30', 'Glow Color=0.6', 'Glow Strength=0.6')
bg.alpha_composite(fg, (0, 0))
bg.alpha_composite(fg2, (800, 0))
bg.convert('RGB').save(f'{OUT}/v2_path.png')

# Text+ Fill: over the clip, filled with the clip, cut out of the clip
W, H, tw = 1600, 380, 533
bg = background(W, H, (22, 22, 30), (38, 30, 44))
modes = [('Over', 1), ('Fill Text', 2), ('Knockout', 3)]
for i, (name, v) in enumerate(modes):
    fg = render(tw, H - 40, 'Text=VIDEO', f'Font File={anton}', 'Size=190', f'Video={v}', 'Outline=1', 'Outline Width=5',
                'Outline Color_Bri=0.05' if v != 3 else 'Outline Color_Bri=1', '@input=photo', plugin=FILL_PLUGIN)
    bg.alpha_composite(fg, (i * tw, 0))
img = bg.convert('RGB')
for i, (name, v) in enumerate(modes):
    tile_label(img, 'Text+ Fill · ' + name, (i * tw + 24, H - 34))
img.save(f'{OUT}/v2_fill.png')

# Animations
gif('anim_3d.gif', 960, 420, ['Text=TEXT+ 2.0', f'Font File={archivo}', 'Size=130', '+Look=9', '++Tilt X=12', '@ramp=Tilt Y:-38:38'], 80, step=2,
    bgcolors=((16, 18, 30), (40, 26, 52)))
gif('anim_ring.gif', 720, 720, ['Text=ROUND AND ROUND • TEXT+ • ', f'Font File={grotesk}', 'Font=Space Grotesk / Bold', 'Size=58',
                                'Path=2', 'Path Radius=260', 'Fill Mode=8', 'Fill Color 2=0.12', 'Fill Color 2_Sat=0.85',
                                '@ramp=Path Offset:0:0.25'], 70, step=2, bgcolors=((14, 20, 32), (20, 40, 48)))
gif('anim_blur_tracking.gif', 960, 300, ['Text=TRACKING & BLUR', 'Font=Inter / Black', 'Size=100', '+A1 Preset=20', '+A2 Preset=19',
                                         '@ramp=A1 Progress:0:1', '@ramp=A2 Progress:-0.2:1'], 80, step=2)
gif('anim_led.gif', 960, 260, ['Text=LED  SIGN  •  ', 'Display=3', 'Scroll Speed=180', 'Font=Inter / Black', 'Size=120', '+Look=11'], 80, step=2,
    bgcolors=((8, 8, 10), (16, 12, 14)))


# The interface: rows Resolume shows by default, 1.1 against 2.0
def ui_rows(plugin):
    r = subprocess.run([HOST, plugin, tempfile.mktemp(suffix='.png'), '320', '180', '@ui=rows', '@frames=12'], capture_output=True, text=True)
    groups = []
    for line in r.stdout.splitlines():
        if line.startswith('  [') and line.strip().endswith(']'):
            groups.append((line.strip()[1:-1], []))
        elif line.startswith('      ') and groups:
            groups[-1][1].append(line.strip().split('  ')[0])
    return groups


if OLD_PLUGIN:
    cols = [('Text+ 1.1', ui_rows(OLD_PLUGIN)), ('Text+ 2.0', ui_rows(PLUGIN))]
    rh, gh, top = 19, 26, 84
    hmax = max(sum(gh + rh * len(rows) for _, rows in g) for _, g in cols) + top + 30
    img = Image.new('RGB', (1600, hmax), (26, 27, 32))
    d = ImageDraw.Draw(img)
    def fnt(size, weight='Medium'):
        hits = glob.glob(f'/usr/share/fonts/**/Inter-{weight}.*', recursive=True)
        return ImageFont.truetype(hits[0], size) if hits else ImageFont.truetype(font_file('SpaceGrotesk*'), size)
    for c, (title, groups) in enumerate(cols):
        x0 = 60 + c * 800
        n = sum(len(r) for _, r in groups)
        d.text((x0, 24), f'{title}: {n} rows by default', font=fnt(32, 'Bold'), fill=(236, 238, 246))
        y = top
        for name, rows in groups:
            d.rectangle([x0, y, x0 + 640, y + gh - 4], fill=(48, 50, 60))
            d.text((x0 + 10, y + 3), name, font=fnt(16, 'SemiBold'), fill=(235, 236, 245))
            y += gh
            for row in rows:
                d.text((x0 + 22, y), row, font=fnt(15), fill=(178, 183, 198))
                d.rectangle([x0 + 300, y + 7, x0 + 620, y + 12], fill=(70, 74, 88))
                y += rh
    img.save(f'{OUT}/v2_ui.png')

print('images in', OUT)
