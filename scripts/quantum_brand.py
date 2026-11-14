#!/usr/bin/env python3
# (c) SR VINIX and contributors
# SPDX-License-Identifier: MIT

"""Derive every Quantum brand artifact from the two masters under brand/.

The masters are raster exports of the approved mark (nucleus + orbital rings) and the
mark + QUANTUM wordmark lockup, both painted on a near-white field. Everything the
engine, the editor and the docs site consume is generated from them here so a logo
change is one reproducible command instead of a pile of hand-saved files:

    python scripts/quantum_brand.py --report   # what would be written, writes nothing
    python scripts/quantum_brand.py --apply    # write it

Outputs: the transparent/ recolored PNGs under brand/, the Windows .ico pair and the
embedded _png.h headers under editor/resources/icons/, the hicolor sizes under
editor/resources/icons/linux/, and the site art under <docs>/docs/assets/logo/.

The _png.h files are hand-maintained sources, not build output: the editor embeds the
logo bytes so the About box and the splash work before any asset VFS exists. They are
rewritten in the exact byte-array layout the previous generator emitted (12 bytes per
line) so the diff of a logo swap is the bytes and nothing else.
"""

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps

REPO = Path(__file__).resolve().parent.parent
BRAND = REPO / "brand"
ICONS = REPO / "editor" / "resources" / "icons"
LINUX_ICONS = ICONS / "linux"
DOCS = Path("C:/quantum-docs")

MARK_MASTER = BRAND / "quantum-mark-master.png"
LOCKUP_MASTER = BRAND / "quantum-lockup-master.png"

# The mark's own palette, sampled from the master so the recolored variants match.
INK_DARK = (18, 19, 26)
WHITE_TEXT = (245, 246, 250)

ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
LINUX_SIZES = (16, 32, 48, 64, 128, 256, 512)

EMBEDDED_LOGO = "quantum_logo_embedded.png"
EMBEDDED_FAVICON = "quantum_favicon_embedded.png"

# A pixel this close to the master's background is fully transparent; the ramp in
# between is un-blended so the anti-aliased edges do not keep a white halo.
BG_FLOOR = 6
BG_RAMP = 110.0


def cutout(im: Image.Image) -> Image.Image:
    """Drop the flat near-white background, un-blending the edge pixels."""
    rgb = im.convert("RGB")
    w, h = rgb.size
    src = rgb.load()
    out = Image.new("RGBA", (w, h))
    dst = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b = src[x, y]
            # Distance from white in the channel that is furthest from it.
            d = 255 - min(r, g, b)
            if d <= BG_FLOOR:
                dst[x, y] = (0, 0, 0, 0)
                continue
            a = min(1.0, d / BG_RAMP)
            inv = 1.0 - a
            cr = min(255, max(0, round((r - inv * 255) / a)))
            cg = min(255, max(0, round((g - inv * 255) / a)))
            cb = min(255, max(0, round((b - inv * 255) / a)))
            dst[x, y] = (cr, cg, cb, round(a * 255))
    return out


def trim(im: Image.Image, pad: int) -> Image.Image:
    """Crop to the opaque content and re-pad symmetrically."""
    bbox = im.getchannel("A").getbbox()
    if bbox is None:
        raise SystemExit("master has no visible content after background removal")
    im = im.crop(bbox)
    w, h = im.size
    side = max(w, h) + 2 * pad
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(im, ((side - w) // 2, (side - h) // 2), im)
    return canvas


def square(im: Image.Image, size: int, pad_ratio: float = 0.06) -> Image.Image:
    """Content-centered square at `size`, with the mark occupying ~the inner 60%."""
    im = trim(im, int(size * pad_ratio))
    return im.resize((size, size), Image.LANCZOS)


def resize_to_width(im: Image.Image, width: int) -> Image.Image:
    """Lanczos downscale keeping the aspect ratio."""
    return im.resize((width, max(1, round(im.height * width / im.width))), Image.LANCZOS)


def quantize(im: Image.Image) -> Image.Image:
    """Palette-mode the art so the embedded header stays a few KB, not hundreds.

    The About box and the splash draw the logo 150 logical px wide, so the embedded
    copy carries no visual information past a 256-colour palette at that size; the
    header it produces is the same order of magnitude as the 6.7 KB one it replaces.
    """
    return im.quantize(colors=256, method=Image.Quantize.FASTOCTREE,
                       dither=Image.Dither.NONE)


def recolor_text(im: Image.Image, target) -> Image.Image:
    """Swap the dark wordmark ink for `target`, leaving the gradient mark alone."""
    out = im.copy()
    px = out.load()
    w, h = out.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            # Dark ink is the only near-black thing in the lockup.
            if max(r, g, b) < 90:
                px[x, y] = (*target, a)
    return out


def png_h(path: Path, symbol: str, data: bytes) -> None:
    """Emit an embedded-bytes header in the layout the editor already reads."""
    lines = ["// (c) SR VINIX and contributors",
             "// SPDX-License-Identifier: MIT",
             "",
             "#pragma once",
             "",
             f"inline unsigned char {symbol}[] = {{"]
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        body = ", ".join(f"0x{b:02x}" for b in chunk)
        tail = "," if i + 12 < len(data) else ""
        lines.append(f"  {body}{tail}")
    lines.append("};")
    lines.append(f"inline unsigned int {symbol}_len = {len(data)};")
    lines.append("")
    write(path, "\n".join(lines))


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, newline="\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--report", action="store_true", help="list what would be written")
    mode.add_argument("--apply", action="store_true", help="write the artifacts")
    ap.add_argument("--docs", default=str(DOCS), help="path to the quantum-docs checkout")
    args = ap.parse_args()

    for master in (MARK_MASTER, LOCKUP_MASTER):
        if not master.exists():
            raise SystemExit(f"missing master: {master}")

    mark = cutout(Image.open(MARK_MASTER))
    lockup = cutout(Image.open(LOCKUP_MASTER))
    mark_1024 = square(mark, 1024)
    mark_512 = square(mark, 512)

    docs = Path(args.docs)
    targets = []

    def plan(path: Path, kind: str, payload):
        targets.append((path, kind, payload))

    # --- brand/ : the transparent and theme-variant PNGs, plus the icon base -------
    plan(BRAND / "quantum-mark.png", "png", (mark_1024, None))
    plan(BRAND / "quantum-mark-512.png", "png", (mark_512, None))
    plan(BRAND / "quantum-lockup.png", "png", (lockup, None))
    plan(BRAND / "quantum_logo_transparent.png", "png",
         (recolor_text(lockup, WHITE_TEXT), None))
    plan(BRAND / "quantum_logo_dark.png", "png", (lockup, None))
    plan(BRAND / "quantum-favicon.png", "png", (square(mark, 64), None))

    # --- editor/resources/icons/ : Windows icons and the embedded headers ---------
    plan(ICONS / "quantum-editor.ico", "ico", (mark_512, ICO_SIZES))
    plan(ICONS / "quantum-editor-cmd.ico", "ico", (mark_512, ICO_SIZES))
    plan(ICONS / "quantum-editor.png", "png", (mark_512, None))

    # --- embedded headers ----------------------------------------------------------
    # The About box and the splash draw the logo 150 logical px wide, so the embedded
    # copy is capped well below the master and quantised: the header it produces is
    # the same order of magnitude as the one it replaces (~7 KB) instead of 100 KB+.
    embedded_logo = quantize(resize_to_width(lockup, 600))
    embedded_fav = quantize(square(mark, 64))
    plan(BRAND / EMBEDDED_LOGO, "png", (embedded_logo, None))
    plan(BRAND / EMBEDDED_FAVICON, "png", (embedded_fav, None))
    plan(ICONS / "quantum-logo_png.h", "h", ("quantum_logo_png", BRAND / EMBEDDED_LOGO))
    plan(ICONS / "quantum-favicon_png.h", "h",
         ("quantum_favicon_png", BRAND / EMBEDDED_FAVICON))

    for size in LINUX_SIZES:
        plan(LINUX_ICONS / f"quantum-editor-{size}.png", "png", (square(mark, size), None))

    # --- quantum-docs : site logo, favicon and the README banner ------------------
    logo_dir = docs / "docs" / "assets" / "logo"
    plan(logo_dir / "quantum_mark.png", "png", (mark_512, None))
    plan(logo_dir / "quantum_wordmark.png", "png", (lockup, None))
    plan(logo_dir / "quantum_logo_light.png", "png", (lockup, None))
    plan(logo_dir / "quantum_logo_dark.png", "png", (recolor_text(lockup, WHITE_TEXT), None))
    plan(logo_dir / "quantum_favicon.png", "png", (square(mark, 64), None))

    if args.report:
        for path, kind, _ in targets:
            state = "exists" if path.exists() else "new"
            print(f"{kind:<4} {state:<6} {path}")
        print(f"\n{len(targets)} artifacts, masters {MARK_MASTER.name} + "
              f"{LOCKUP_MASTER.name}")
        if not docs.exists():
            print(f"WARNING docs checkout not found at {docs}", file=sys.stderr)
        return 0

    # Emit the images first: the headers embed the bytes of the files just written.
    for path, kind, payload in targets:
        if kind != "h":
            emit(path, kind, payload)
    for path, kind, payload in targets:
        if kind == "h":
            symbol, source = payload
            data = source.read_bytes()
            png_h(path, symbol, data)
            print(f"h    wrote   {path} ({len(data)} bytes from {source.name})")
    return 0


def emit(path: Path, kind: str, payload) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if kind == "png":
        im, _ = payload
        im.save(path)
    elif kind == "ico":
        im, sizes = payload
        im.save(path, format="ICO", sizes=[(s, s) for s in sizes])
    print(f"{kind:<4} wrote   {path}")


if __name__ == "__main__":
    sys.exit(main())
