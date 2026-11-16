#!/usr/bin/env python3
# laptop-preview (AI-assisted): paints stand-in PS2 case covers (front and back) for the sample library, so the preview's shelf looks
# full without any real cover art in git. Usage: make_covers.py <games.txt> <covers dir> <font.ttf> [<extra serial|title> ...]
# Writes <serial>.jpg and <serial>_back.jpg (fe_covers.cpp's names); files already there are kept.
import hashlib, os, sys, colorsys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

W, H = 512, 728  # a PS2 DVD case's front, about 0.70

def palette(key):
    h = int(hashlib.sha1(key.encode()).hexdigest(), 16)
    hue = (h % 360) / 360.0
    a = colorsys.hls_to_rgb(hue, 0.42, 0.65)
    b = colorsys.hls_to_rgb((hue + 0.08 + (h >> 9) % 30 / 300.0) % 1, 0.12, 0.6)
    c = colorsys.hls_to_rgb((hue + 0.5) % 1, 0.7, 0.8)
    t = lambda x: tuple(int(v * 255) for v in x)
    return t(a), t(b), t(c), h

def wrap(draw, text, font, width):
    words, lines, cur = text.split(), [], ""
    for w in words:
        nxt = (cur + " " + w).strip()
        if draw.textlength(nxt, font=font) <= width or not cur:
            cur = nxt
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines

def front(title, serial, font_path, out):
    a, b, c, h = palette(serial)
    img = Image.new("RGB", (W, H), b)
    px = img.load()
    for y in range(H):  # a vertical gradient
        f = y / H
        row = tuple(int(a[i] * (1 - f) + b[i] * f) for i in range(3))
        for x in range(W):
            px[x, y] = row
    art = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(art)
    for i in range(7):  # soft shapes, the game's own
        r = 40 + (h >> (i * 5)) % 160
        cx = (h >> (i * 7)) % W
        cy = 180 + (h >> (i * 3)) % (H - 260)
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=c + (40 + (i * 13) % 60,))
    art = art.filter(ImageFilter.GaussianBlur(6))
    img.paste(art, (0, 0), art)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W, 62], fill=(8, 8, 10))  # the black band across the top, with a plain wordmark
    small = ImageFont.truetype(font_path, 30)
    d.text((22, 14), "PS2", font=small, fill=(235, 235, 240))
    d.text((W - 22, 18), serial, font=ImageFont.truetype(font_path, 22), fill=(150, 150, 160), anchor="ra")
    size = 64 if len(title) < 14 else 52 if len(title) < 26 else 42
    big = ImageFont.truetype(font_path, size)
    lines = wrap(d, title.upper(), big, W - 70)
    y = H - 110 - len(lines) * (size + 6)
    for ln in lines:
        d.text((W // 2 + 3, y + 3), ln, font=big, fill=(0, 0, 0), anchor="ma")
        d.text((W // 2, y), ln, font=big, fill=(250, 248, 240), anchor="ma")
        y += size + 6
    d.rectangle([0, H - 54, W, H], fill=(10, 10, 12))
    d.text((22, H - 40), "Preview cover", font=ImageFont.truetype(font_path, 20), fill=(120, 120, 130))
    img.save(out, quality=88)

def back(title, serial, font_path, out):
    a, b, c, h = palette(serial)
    img = Image.new("RGB", (W, H), tuple(int(v * 0.5) for v in b))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W, 62], fill=(8, 8, 10))
    d.text((W // 2, 16), title, font=ImageFont.truetype(font_path, 26), fill=(235, 235, 240), anchor="ma")
    for i in range(3):  # screenshots
        x = 30 + i * 155
        d.rectangle([x, 90, x + 140, 190], fill=tuple(min(255, int(v * (0.8 + i * 0.15))) for v in a))
    body = ImageFont.truetype(font_path, 19)
    text = ("A stand-in back cover painted for the PS2 shelf preview. Real covers are never kept in git; the console reads its own "
            "from /data/PCSX2/covers. ") * 3
    y = 220
    for ln in wrap(d, text, body, W - 60)[:14]:
        d.text((30, y), ln, font=body, fill=(205, 205, 215))
        y += 27
    d.rectangle([0, H - 70, W, H], fill=(10, 10, 12))
    d.text((30, H - 50), serial, font=ImageFont.truetype(font_path, 22), fill=(170, 170, 180))
    img.save(out, quality=85)

def main():
    games, outdir, font = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    items = []
    for line in open(games, encoding="utf-8"):
        line = line.strip()
        if line and not line.startswith("#"):
            f = line.split("|")
            items.append((f[2], f[0]))
    for extra in sys.argv[4:]:
        s, t = extra.split("|", 1)
        items.append((s, t))
    made = 0
    for serial, title in items:
        f, b = os.path.join(outdir, serial + ".jpg"), os.path.join(outdir, serial + "_back.jpg")
        if not os.path.exists(f):
            front(title, serial, font, f)
            made += 1
        if not os.path.exists(b):
            back(title, serial, font, b)
    print(f"covers: {made} painted, {len(items)} games, in {outdir}")

main()
