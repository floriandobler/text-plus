#!/usr/bin/env python3
"""Renders the Text+ 2 README images with the headless test host (Linux, Mesa).

Usage: tools/make_showcase_v2.py [out-dir]
Needs Inter installed system-wide (fonts-inter) and Pillow. Run from the
repository root after ./scripts/run_tests_linux.sh (uses build/ffgl_test_host,
build/TextPlus.so and build/TextPlusFX.so).
"""
import glob, json, os, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else 'docs/images'
HOST, PLUGIN, FX = 'build/ffgl_test_host', 'build/TextPlus.so', 'build/TextPlusFX.so'
os.makedirs(OUT, exist_ok=True)
INTER = sorted(glob.glob('/usr/share/fonts/**/Inter-Regular.*', recursive=True) + glob.glob('/usr/share/fonts/**/Inter*.[ot]tf', recursive=True))[0]


def render(w, h, *params, frames=24, dump=None, dt=None, plugin=PLUGIN, bg='none'):
    out = tempfile.mktemp(suffix='.png')
    args = [HOST, plugin, out, str(w), str(h), *params, f'@bg={bg}', f'@frames={frames}']
    if dt:
        args.append(f'@dt={dt}')
    if dump:
        args.append('@dump=' + dump)
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(' '.join(args) + '\n' + r.stdout + r.stderr)
    return Image.open(out).convert('RGBA')


def background(w, h, top=(18, 20, 38), bottom=(40, 24, 58)):
    bg = Image.new('RGB', (w, h))
    px = bg.load()
    for y in range(h):
        t = y / max(1, h - 1)
        for x in range(w):
            s = x / max(1, w - 1)
            c = [int(top[i] * (1 - t) + bottom[i] * t) for i in range(3)]
            v = 1.0 - 0.35 * ((s - 0.5) ** 2 + (t - 0.5) ** 2)
            px[x, y] = tuple(max(0, min(255, int(ch * v))) for ch in c)
    return bg.convert('RGBA')


def label(img, text, xy, size=20, fill=(205, 210, 230)):
    # on a dark, rounded backdrop so it stays readable over video and patterns
    font = ImageFont.truetype(INTER, size)
    d = ImageDraw.Draw(img)
    l, t, r, b = d.textbbox(xy, text, font=font)
    pad = 6
    box = Image.new('RGBA', img.size, (0, 0, 0, 0))
    ImageDraw.Draw(box).rounded_rectangle((l - pad, t - pad, r + pad, b + pad), radius=6, fill=(12, 13, 24, 190))
    img.alpha_composite(box)
    ImageDraw.Draw(img).text(xy, text, font=font, fill=fill)


def grid(cells, cols, cw, ch, name, captions=True, bgcolors=None):
    rows = (len(cells) + cols - 1) // cols
    sheet = background(cols * cw, rows * ch)
    for i, (cap, img) in enumerate(cells):
        x, y = (i % cols) * cw, (i // cols) * ch
        sheet.alpha_composite(img, (x, y))
    for i, (cap, img) in enumerate(cells):
        x, y = (i % cols) * cw, (i // cols) * ch
        if captions:
            label(sheet, cap, (x + 16, y + 12))
    sheet.convert('RGB').save(os.path.join(OUT, name), optimize=True)
    print('wrote', name)


# ---- Looks ----------------------------------------------------------------
looks = [('Clean', 1), ('Outline', 2), ('Double Outline', 3), ('Hollow', 4), ('Soft Shadow', 5), ('Long Shadow', 16),
         ('Comic', 6), ('Neon', 7), ('Chrome', 8), ('Retro 3D', 9), ('Emboss', 10), ('Glass', 12),
         ('Contours', 11), ('Ink', 13), ('Lower Third Bar', 14)]
W, H = 532, 200
cells = [(name, render(W, H, 'Text=Text+', 'Font=Inter / Black', 'Size=104', f'+Look={v}', frames=24)) for name, v in looks]
grid(cells, 3, W, H, 'v2_looks.png')

# ---- Effect stack -----------------------------------------------------------
W, H = 400, 230
cells = [
    ('Bevel: Chisel', render(W, H, 'Text=Bevel', 'Font=Inter / Black', 'Size=110', 'Fill Color=0.11', 'Fill Color_Sat=0.85',
                             '+Effect 1=6', '++Effect 1 Mode=1', '++Effect 1 Size=14')),
    ('Extrude + Outline', render(W, H, 'Text=3D', 'Font=Inter / Black', 'Size=150', 'Fill Mode=1', 'Fill Color=0.55', 'Fill Color_Sat=0.6',
                                 'Fill Color 2=0.75', 'Fill Color 2_Sat=0.8', 'Gradient Angle=-90', '+Effect 1=1', '++Effect 1 Size=4',
                                 '++Effect 1 Color_Bri=0.15', '+Effect 2=7', '++Effect 2 Size=36', '++Effect 2 Color=0.62', '++Effect 2 Color_Sat=0.8',
                                 '++Effect 2 Color_Bri=0.6', '++Effect 2 Color 2=0.7', '++Effect 2 Color 2_Sat=0.9', '++Effect 2 Color 2_Bri=0.2')),
    ('Contours', render(W, H, 'Text=Rings', 'Font=Inter / Black', 'Size=96', '+Effect 1=8', '++Effect 1 Color=0.08',
                        '++Effect 1 Color_Sat=0.9', '++Effect 1 Size=11', '++Effect 1 Distance=62')),
    ('Halftone + Roughen', render(W, H, 'Text=Print', 'Font=Inter / Black', 'Size=110', 'Fill Mode=5', 'Fill Color=0.13', 'Fill Color_Sat=0.9',
                                  'Fill Color 2=0.97', 'Fill Color 2_Sat=0.9', 'Fill Color 2_Bri=0.55', 'Gradient Angle=-90', 'Fill Scale=6',
                                  '+Effect 1=9', '++Effect 1 Distance=2.5', '++Effect 1 Size=8')),
]
grid(cells, 4, W, H, 'v2_effects.png')

# ---- Paths -------------------------------------------------------------------
W, H = 400, 300
cells = [
    ('Arc', render(W, H, 'Text=ON AN ARC', 'Font=Inter / Black', 'Size=48', 'Path=1', 'Path Amount=55', '+Look=2')),
    ('Circle', render(W, H, 'Text=ROUND AND ROUND IT GOES · ', 'Font=Inter / Bold', 'Size=27', 'Path=2', 'Path Radius=95')),
    ('Wave', render(W, H, 'Text=Riding the wave', 'Font=Inter / Black', 'Size=44', 'Path=3', 'Path Amount=35', 'Path Length=220', '+Look=7')),
    ('Ring (3D)', render(W, H, 'Text=SPINNING · RING · OF · TEXT · ', 'Font=Inter / Black', 'Size=40', 'Path=4', 'Path Tilt=22',
                         'Path Offset=0.02', 'Back Opacity=22', 'Fill Color=0.52', 'Fill Color_Sat=0.45')),
]
grid(cells, 4, W, H, 'v2_paths.png')

# ---- 3D animation (GIF) ------------------------------------------------------
def gif(name, w, h, params, frames=48, dt=0.04, plugin=PLUGIN):
    d = tempfile.mkdtemp()
    render(w, h, *params, frames=frames, dt=dt, dump=d, plugin=plugin)
    bg = background(w, h)
    fr = [Image.alpha_composite(bg, Image.open(f).convert('RGBA')).convert('RGB').convert('P', palette=Image.ADAPTIVE)
          for f in sorted(glob.glob(os.path.join(d, 'f*.png')))]
    fr[0].save(os.path.join(OUT, name), save_all=True, append_images=fr[1:], duration=int(dt * 1000), loop=0, optimize=True)
    print('wrote', name)


gif('v2_flip.gif', 800, 220, ['Text=FLIP IN, IN 3D', 'Font=Inter / Black', 'Size=88', 'A1 Mode=1', 'A1 Unit=0', 'A1 Ease=2', 'A1 Spread=4',
                              'A1 Rotation X=90', 'A1 Opacity=0', 'Perspective=900', '@ramp=A1 Progress:-0.1:1.15', '+Look=2'], frames=44)
gif('v2_ring.gif', 640, 240, ['Text=SPINNING RING · TEXT+ 2 · ', 'Font=Inter / Black', 'Size=46', 'Path=4', 'Path Tilt=14',
                              'Back Opacity=25', '@ramp=Path Offset:0:0.2', '+Look=7'], frames=36, dt=0.05)

# ---- Text+ FX ----------------------------------------------------------------
W, H = 532, 260
fx = []
for cap, extra in [('Text over Video', ['+Look=6']), ('Video inside Text', ['Input=1', '+Effect 1=1', '++Effect 1 Size=5', '+Effect 2=3']),
                   ('Video cut by Text', ['Input=2'])]:
    out = tempfile.mktemp(suffix='.png')
    args = [HOST, FX, out, str(W), str(H), '@input=1', 'Text=VIDEO', 'Font=Inter / Black', 'Size=140', *extra, '@bg=none', '@frames=24']
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)
    fx.append((cap, Image.open(out).convert('RGBA')))
grid(fx, 3, W, H, 'v2_fx.png')

# ---- Live data -----------------------------------------------------------------
data = tempfile.mktemp(suffix='.json')
json.dump({'city': 'Ravensburg', 'now': {'temp': 17.4}, 'sky': [{'text': 'light rain'}]}, open(data, 'w'))
W, H = 532, 200
cells = [
    ('Countdown with warning colour', render(W, H, 'Source=2', 'Timer Mode=2', 'Duration=0:42', 'Warn At=60', 'Text=Talk ends in {timer}',
                                             'Font=Inter / Black', 'Size=52', '+Look=2')),
    ('JSON (file or URL) as template', render(W, H, 'Source=1', f'Text File={data}', 'Text={city}', 'Text 2={now.temp} °C · {sky[0].text}',
                                              'Font=Inter / Black', 'Size=50', 'L2 Style=1', 'L2 Size=60', 'L2 Font=Medium', '+Look=5')),
    ('Pattern display', render(W, H, 'Display=6', 'Text=TEXT+ 2', 'Separator=  ✦  ', 'Font=Inter / Black', 'Size=40',
                               'Row Spacing=95', 'Fill Color=0.85', 'Fill Color_Sat=0.5', dt=0.05)),
]
grid(cells, 3, W, H, 'v2_data.png')
