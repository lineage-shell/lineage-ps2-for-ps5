#!/usr/bin/env python3
# laptop-preview (AI-assisted): real PS2 front covers for the sample library, from xlenore/ps2-covers (the app's own default cover
# source, fe_ps5.cpp kCoverUrl), so the preview shelf shows real art. Local only: nothing here goes into git.
#
#   fetch_covers.py <games.txt> <cache dir> <covers dir> [<extra serial|title> ...]
#
# Each serial is fetched once into a fresh temp dir (untrusted), checked with PIL (a real JPEG/PNG of sane size), re-encoded as a plain
# JPEG and kept in <cache dir>/<serial>.jpg; a serial the set has none for is remembered in <cache dir>/<serial>.none. Then every cached
# cover is copied over <covers dir>/<serial>.jpg (fe_covers.cpp's front name). Backs, and games without a cover, keep the painted ones.
import os, shutil, sys, tempfile, urllib.request
from PIL import Image

URL = "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/{}.jpg"
MAX_BYTES = 4 << 20

def serials(games, extra):
    out = []
    for line in open(games, encoding="utf-8"):
        line = line.strip()
        if line and not line.startswith("#"):
            p = line.split("|")
            if len(p) >= 3:
                out.append(p[2].strip())
    out += [e.split("|")[0].strip() for e in extra]
    ok = lambda s: len(s) <= 16 and s.replace("-", "").isalnum()
    return [s for s in dict.fromkeys(out) if ok(s)]

def fetch(serial, cache):
    tmp = tempfile.mkdtemp(prefix="ps2cover-")
    try:
        raw = os.path.join(tmp, "download")
        try:
            with urllib.request.urlopen(URL.format(serial), timeout=20) as r, open(raw, "wb") as f:
                data = r.read(MAX_BYTES + 1)
                if len(data) > MAX_BYTES:
                    return "too big"
                f.write(data)
        except urllib.error.HTTPError as e:
            if e.code == 404:
                open(os.path.join(cache, serial + ".none"), "w").close()
                return "none in the set"
            return f"HTTP {e.code}"
        except OSError as e:
            return f"failed ({e})"
        try:
            with Image.open(raw) as im:
                im.verify()
            with Image.open(raw) as im:
                if im.format not in ("JPEG", "PNG"):
                    return f"not a cover ({im.format})"
                w, h = im.size
                if not (64 <= w <= 4096 and 64 <= h <= 4096 and 1.1 < h / w < 1.8):
                    return f"odd size {w}x{h}"
                im = im.convert("RGB")
                im.load()
        except Exception as e:
            return f"not an image ({e.__class__.__name__})"
        out = os.path.join(cache, serial + ".jpg")
        im.save(out + ".tmp", "JPEG", quality=92)
        os.replace(out + ".tmp", out)
        return "ok"
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__ or "usage: fetch_covers.py <games.txt> <cache dir> <covers dir> [extra ...]")
    games, cache, covers = sys.argv[1:4]
    os.makedirs(cache, exist_ok=True)
    os.makedirs(covers, exist_ok=True)
    got = new = 0
    for s in serials(games, sys.argv[4:]):
        c = os.path.join(cache, s + ".jpg")
        if not os.path.exists(c) and not os.path.exists(os.path.join(cache, s + ".none")):
            res = fetch(s, cache)
            if res == "ok":
                new += 1
            else:
                print(f"[covers] {s}: {res}")
        if os.path.exists(c):
            dst = os.path.join(covers, s + ".jpg")
            if not os.path.exists(dst) or open(c, "rb").read() != open(dst, "rb").read():
                shutil.copyfile(c, dst)
            got += 1
    print(f"[covers] {got} real fronts ({new} new) from xlenore/ps2-covers; the rest stay painted")

if __name__ == "__main__":
    main()
