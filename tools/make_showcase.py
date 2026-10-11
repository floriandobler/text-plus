#!/usr/bin/env python3
"""Renders the README images with the headless test host (Linux, Mesa).

Usage: tools/make_showcase.py <fonts-dir> [out-dir]
<fonts-dir> holds a few Google Fonts (OFL): Anton, Bebas Neue, Space Grotesk,
Archivo Black, Righteous (e.g. a sparse checkout of github.com/google/fonts, ofl/...).
Inter must be installed system-wide. Needs Pillow.
"""
import glob, os, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont, ImageFilter

FONTS = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else 'docs/images'
HOST, PLUGIN = 'build/ffgl_test_host', 'build/TextPlus.so'
os.makedirs(OUT, exist_ok=True)


def font_file(name):
    hits = glob.glob(os.path.join(FONTS, '**', name), recursive=True)
    if not hits:
        sys.exit('font not found: ' + name)
    return hits[0]


def render(w, h, *params, frames=24, dump=None, dt=None):
    out = tempfile.mktemp(suffix='.png')
    args = [HOST, PLUGIN, out, str(w), str(h), *params, '@bg=none', f'@frames={frames}']
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
print('images in', OUT)
