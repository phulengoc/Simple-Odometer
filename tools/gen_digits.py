#!/usr/bin/env python3
"""Generate the big-digit font header (main/digits_font.h) for the Tach Ring UI.

Self-contained author-time tool — NOT part of the firmware build. It extracts the
JetBrains Mono font embedded in the design mockup, rasterizes the digits 0-9 at
two sizes with anti-aliased (8-bit coverage) glyphs, and emits a C header plus a
preview PNG sheet for review.

Run from the firmware project root:
    .fontvenv/bin/python tools/gen_digits.py

Deps (already in .fontvenv): Pillow, fonttools, brotli.
The generated main/digits_font.h is committed, so the firmware build needs none
of this.
"""
import base64
import io
import re
import sys
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
HTML = ROOT.parent / "2 _ Tach Ring.html"   # design mockup in the workspace
OUT_HEADER = ROOT / "main" / "digits_font.h"
OUT_TTF = ROOT / "tools" / "JetBrainsMono-digits.ttf"
OUT_PREVIEW = ROOT / "tools" / "digits_preview.png"

DIGITS = "0123456789"

# Pixel font sizes (em). Tuned for the 466 px panel: speed ~135 px (BIG),
# gear ~43 px (MID, gear-only), odo ~33 px (SMALL).
SIZES = {"BIG": 178, "MID": 56, "SMALL": 44}


def extract_jetbrains_ttf() -> bytes:
    """Pull the embedded JetBrains Mono subset that contains the digits and
    return it as TTF bytes."""
    html = HTML.read_text(encoding="utf-8", errors="replace")
    srcs = re.findall(r'src:\s*url\("data:font/woff2;base64,([A-Za-z0-9+/=]+)"', html)
    if not srcs:
        sys.exit(f"No embedded woff2 fonts found in {HTML}")
    for b64 in srcs:
        raw = base64.b64decode(b64)
        try:
            font = TTFont(io.BytesIO(raw))   # decodes woff2 (needs brotli)
        except Exception:
            continue
        cmap = font.getBestCmap()
        if all(ord(d) in cmap for d in DIGITS):
            font.flavor = None               # woff2 -> plain TTF
            buf = io.BytesIO()
            font.save(buf)
            print(f"Found digit-bearing subset ({len(raw)} B woff2)")
            return buf.getvalue()
    sys.exit("No embedded font subset contains all digits 0-9")


def rasterize(ttf_bytes: bytes, px: int):
    """Render digits 0-9 into a uniform monospace cell of 8-bit coverage.

    Returns (cell_w, cell_h, [bytes-per-glyph...], font, top) where each glyph is
    cell_w*cell_h coverage bytes, row-major, 0=bg 255=ink. `font` and `top` let
    callers rasterize extra glyphs (e.g. 'N') into the same cell/baseline."""
    font = ImageFont.truetype(io.BytesIO(ttf_bytes), px)

    # Uniform cell: max ink bbox across all digits (monospace, but measure to be safe).
    boxes = [font.getbbox(d) for d in DIGITS]   # (l, t, r, b) ink box
    left = min(b[0] for b in boxes)
    top = min(b[1] for b in boxes)
    right = max(b[2] for b in boxes)
    bottom = max(b[3] for b in boxes)
    cell_w = right - left
    cell_h = bottom - top

    glyphs = []
    for d in DIGITS:
        img = Image.new("L", (cell_w, cell_h), 0)
        draw = ImageDraw.Draw(img)
        # offset so the common ink box maps to (0,0)
        draw.text((-left, -top), d, fill=255, font=font)
        glyphs.append(img.tobytes())
    return cell_w, cell_h, glyphs, font, top


def rasterize_glyph(font, ch, cell_w, cell_h, top):
    """Render a single character centred horizontally in (cell_w, cell_h), using
    the digit baseline (`top`) so it lines up with the digit set."""
    bbox = font.getbbox(ch)
    ink_w = bbox[2] - bbox[0]
    xoff = (cell_w - ink_w) // 2 - bbox[0]
    img = Image.new("L", (cell_w, cell_h), 0)
    ImageDraw.Draw(img).text((xoff, -top), ch, fill=255, font=font)
    return img.tobytes()


def emit_array(out, name, cell_w, cell_h, glyphs):
    out.write(f"#define {name}_W {cell_w}\n")
    out.write(f"#define {name}_H {cell_h}\n")
    out.write(f"// 10 glyphs, {cell_w}x{cell_h} 8-bit coverage each, row-major.\n")
    out.write(f"static const uint8_t {name}[10][{cell_w} * {cell_h}] = {{\n")
    for d, g in zip(DIGITS, glyphs):
        out.write(f"  {{ // '{d}'\n    ")
        for i, b in enumerate(g):
            out.write(f"{b},")
            if (i + 1) % 24 == 0:
                out.write("\n    ")
        out.write("\n  },\n")
    out.write("};\n\n")


def emit_glyph(out, name, cell_w, cell_h, glyph):
    out.write(f"// '{name}' glyph, {cell_w}x{cell_h} 8-bit coverage, row-major.\n")
    out.write(f"static const uint8_t {name}[{cell_w} * {cell_h}] = {{\n    ")
    for i, b in enumerate(glyph):
        out.write(f"{b},")
        if (i + 1) % 24 == 0:
            out.write("\n    ")
    out.write("\n};\n\n")


def make_preview(sets):
    pad = 8
    bw = sets["BIG"][0]
    rows = list(sets.values())
    sheet_h = sum(r[1] for r in rows) + pad * (len(rows) + 1)
    sheet = Image.new("RGB", (10 * (bw + pad) + pad, sheet_h), (0, 0, 0))
    y = pad
    for cw, ch, glyphs, _, _ in rows:
        for i, g in enumerate(glyphs):
            glyph = Image.frombytes("L", (cw, ch), g)
            sheet.paste(glyph.convert("RGB"), (pad + i * (bw + pad), y))
        y += ch + pad
    sheet.save(OUT_PREVIEW)


def main():
    ttf = extract_jetbrains_ttf()
    OUT_TTF.write_bytes(ttf)

    sets = {name: rasterize(ttf, pt) for name, pt in SIZES.items()}
    for name, (cw, ch, _, _, _) in sets.items():
        print(f"{name+' cell:':12s} {cw}x{ch}  ({cw*ch} B/glyph, {cw*ch*10} B total)")

    # Neutral gear letter 'N' at the gear (MID) size.
    mcw, mch, _, mfont, mtop = sets["MID"]
    n_glyph = rasterize_glyph(mfont, "N", mcw, mch, mtop)

    with OUT_HEADER.open("w") as out:
        out.write("// Auto-generated by tools/gen_digits.py — do not edit by hand.\n")
        out.write("// JetBrains Mono digits, 8-bit anti-aliased coverage (0=bg, 255=ink).\n")
        out.write("#pragma once\n#include <stdint.h>\n\n")
        for name, s in sets.items():
            emit_array(out, "DIGITS_" + name, s[0], s[1], s[2])
        emit_glyph(out, "GEAR_N", mcw, mch, n_glyph)
    print(f"Wrote {OUT_HEADER}")

    make_preview(sets)
    print(f"Wrote {OUT_PREVIEW}")


if __name__ == "__main__":
    main()
