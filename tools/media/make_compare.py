#!/usr/bin/env python3
"""KCD DLSS: Nexus comparison images from two 4K screenshots of the same view (left = A, right = B).

usage: make_compare.py A.jpg LABEL_A B.jpg LABEL_B FONTLIB.gfx OUTDIR [zoom regions: name=x,y,w,h ...]
Writes OUTDIR/compare_split_1920x1080.jpg (left half of A, right half of B, labels, divider) and one
OUTDIR/compare_zoom_<name>.jpg per region (the region from both shots, KCD_ZOOM x nearest-neighbour (default 6), side by side, labels).
Text is drawn in WHS Champ Fleury Pro (the game's menu font) by gfx_font_text.py, from the player's own font library.
"""
import os, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFilter

a_path, la, b_path, lb, fontlib, out = sys.argv[1:7]
regions = dict((r.split('=')[0], tuple(int(v) for v in r.split('=')[1].split(','))) for r in sys.argv[7:])
here = os.path.dirname(os.path.abspath(__file__))
FONT = 'WHS Champ Fleury Pro'
GOLD, INK = (200, 170, 110), (240, 228, 200)
os.makedirs(out, exist_ok=True)
A, B = Image.open(a_path).convert('RGB'), Image.open(b_path).convert('RGB')
SUB_A, SUB_B = os.environ.get('KCD_SUB_A', ''), os.environ.get('KCD_SUB_B', '')   # resolution line under/next to each label, e.g. '2227x1253 internal, 3840x2160 output'


def text(t, em):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 't.png')
        subprocess.run([sys.executable, '-I', f'{here}/gfx_font_text.py', fontlib, FONT, t, str(em), p], check=True, stdout=subprocess.DEVNULL)
        m = Image.open(p).getchannel('A'); return m.crop(m.getbbox())


def label(im, t, x, y, em, right=False):
    """text with a soft shadow; x is the left edge, or the right edge when right=True"""
    m = text(t, em)
    if right: x -= m.width
    sh = Image.new('L', im.size, 0); sh.paste(m, (x + 3, y + 4), m)
    sh = sh.filter(ImageFilter.GaussianBlur(em / 12)).point(lambda v: v * 220 // 255)
    im.alpha_composite(Image.merge('RGBA', (*[Image.new('L', im.size, 0)] * 3, sh)))
    im.paste(Image.new('RGBA', m.size, INK + (255,)), (x, y), m)
    return m


# 1. split: left half of A, right half of B (same view, so the halves join into one picture)
W, H = (int(v) for v in os.environ.get('KCD_SPLIT_SIZE', '1920x1080').split('x'))  # e.g. 1280x800 for Steam Deck shots
S = H / 1080  # text scale
a, b = A.resize((W, H), Image.LANCZOS), B.resize((W, H), Image.LANCZOS)
im = a.copy(); im.paste(b.crop((W // 2, 0, W, H)), (W // 2, 0)); im = im.convert('RGBA')
ImageDraw.Draw(im).line([(W // 2, 0), (W // 2, H)], fill=GOLD, width=4)
label(im, la, int(40 * S), H - int(110 * S), int(64 * S)); label(im, lb, W - int(40 * S), H - int(110 * S), int(64 * S), right=True)
if SUB_A: label(im, SUB_A, int(40 * S), H - int(110 * S) - int(46 * S), int(32 * S))
if SUB_B: label(im, SUB_B, W - int(40 * S), H - int(110 * S) - int(46 * S), int(32 * S), right=True)
im.convert('RGB').save(f'{out}/compare_split_{W}x{H}.jpg', quality=92); print('wrote split', W, H)

# 2. zooms: the same region from both, 3x nearest (shows the pixels as they are), side by side
for name, (x, y, w, h) in regions.items():
    Z = int(os.environ.get('KCD_ZOOM', '6')); gap = 12
    ca, cb = A.crop((x, y, x + w, y + h)).resize((w * Z, h * Z), Image.NEAREST), B.crop((x, y, x + w, y + h)).resize((w * Z, h * Z), Image.NEAREST)
    im = Image.new('RGBA', (w * Z * 2 + gap, h * Z + 90), (8, 8, 6, 255))
    im.paste(ca, (0, 90)); im.paste(cb, (w * Z + gap, 90))
    ma = label(im, la, 20, 18, 52); mb = label(im, lb, w * Z + gap + 20, 18, 52)
    if SUB_A: label(im, SUB_A, 20 + ma.width + 30, 34, 28)
    if SUB_B: label(im, SUB_B, w * Z + gap + 20 + mb.width + 30, 34, 28)
    label(im, f'{Z}x zoom', im.width - 20, 30, 30, right=True)
    im.convert('RGB').save(f'{out}/compare_zoom_{name}.jpg', quality=92); print('wrote zoom', name, im.size)
