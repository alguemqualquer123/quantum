#!/usr/bin/env python3
# (c) SR VINIX and contributors
# SPDX-License-Identifier: MIT

"""Pack six sky faces into the single-file cubemap quantum reads, or unpack one back.

The engine loads a cube from one image when only paths[0] is set
(engine/core/pool/TextureDataPool.cpp:207 -> TextureData::loadCubeMapFromSingleFile), and that
loader accepts exactly two layouts: a horizontal cross (4x3 tiles, so width*100/height == 133) or
a vertical one (3x4). It slices the tiles by name, so the tile a face occupies is the whole
contract -- front is +Z, back is -Z, right is +X, left is -X.

Rows are the other half of the contract, and they are measured, not guessed: a horizon view of a
loaded cube comes back pixel-for-pixel as the tile that was stored there, with the image's top
edge pointing at world +Y. So the faces this script packs have to be captured that way -- which is
what engine/components/demos/demo_cubemap.lua does -- and a cross assembled from faces rendered
with -Y as up shows every sky upside down while still round-tripping perfectly.

    python scripts/assemble_sky_cubemap.py assemble <dir> <out.png> [--tile 512] [--vertical]
    python scripts/assemble_sky_cubemap.py split <cross.png> <dir>

Faces are read as <dir>/px.png, nx.png, py.png, ny.png, pz.png, nz.png.
"""

import argparse
import os
import sys

from PIL import Image

# tile = (column, row) inside the atlas, per TextureData::loadCubeMapFromSingleFile
HORIZONTAL = {"px": (2, 1), "nx": (0, 1), "py": (1, 0), "ny": (1, 2), "pz": (1, 1), "nz": (3, 1)}
VERTICAL = {"px": (2, 1), "nx": (0, 1), "py": (1, 0), "ny": (1, 2), "pz": (1, 1), "nz": (1, 3)}
FACES = ("px", "nx", "py", "ny", "pz", "nz")


def assemble(args):
    layout = VERTICAL if args.vertical else HORIZONTAL
    cols, rows = (3, 4) if args.vertical else (4, 3)

    images = {}
    for face in FACES:
        path = os.path.join(args.source, face + ".png")
        if not os.path.isfile(path):
            sys.exit("error: missing face %s" % path)
        image = Image.open(path).convert("RGB")
        if image.width != image.height:
            sys.exit("error: %s is %dx%d, a cube face must be square" % (face, image.width, image.height))
        if args.tile and image.width != args.tile:
            image = image.resize((args.tile, args.tile), Image.LANCZOS)
        images[face] = image

    tile = sorted({image.width for image in images.values()})
    if len(tile) != 1:
        sys.exit("error: faces of different sizes (%s); pass --tile to resample" % ", ".join(str(t) for t in tile))
    tile = tile[0]

    atlas = Image.new("RGB", (cols * tile, rows * tile))
    for face, (col, row) in layout.items():
        atlas.paste(images[face], (col * tile, row * tile))

    ratio = atlas.width * 100 // atlas.height
    if ratio not in (133, 75):
        sys.exit("error: %dx%d is not a layout the loader recognises (want 4:3 or 3:4)" % (atlas.width, atlas.height))
    atlas.save(args.out)
    print("cubemap: %s  %dx%d  tile %d  %s cross" % (
        args.out, atlas.width, atlas.height, tile, "vertical" if args.vertical else "horizontal"))
    print("faces:   " + ",  ".join("%s=(%d,%d)" % (face, *layout[face]) for face in FACES))


def split(args):
    image = Image.open(args.cross).convert("RGB")
    ratio = image.width * 100 // image.height
    if ratio == 133:
        layout, cols, rows = HORIZONTAL, 4, 3
    elif ratio == 75:
        layout, cols, rows = VERTICAL, 3, 4
    else:
        sys.exit("error: %dx%d is not a cross the loader recognises (want 4:3 or 3:4)" % (image.width, image.height))

    tile = image.width // cols
    if tile * rows != image.height:
        sys.exit("error: %dx%d does not divide into %dx%d equal tiles" % (image.width, image.height, cols, rows))
    os.makedirs(args.dest, exist_ok=True)
    for face, (col, row) in layout.items():
        box = (col * tile, row * tile, (col + 1) * tile, (row + 1) * tile)
        path = os.path.join(args.dest, face + ".png")
        image.crop(box).save(path)
    print("split:   %s -> %s  (%d tiles of %dpx)" % (args.cross, args.dest, len(FACES), tile))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    up = sub.add_parser("assemble", help="six face PNGs -> one cross")
    up.add_argument("source", help="folder holding px/nx/py/ny/pz/nz.png")
    up.add_argument("out", help="cross PNG to write")
    up.add_argument("--tile", type=int, default=0, help="resample every face to this edge")
    up.add_argument("--vertical", action="store_true", help="write the 3x4 cross instead of 4x3")
    up.set_defaults(func=assemble)

    down = sub.add_parser("split", help="one cross -> six face PNGs")
    down.add_argument("cross", help="cross PNG read")
    down.add_argument("dest", help="folder to write the faces into")
    down.set_defaults(func=split)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
