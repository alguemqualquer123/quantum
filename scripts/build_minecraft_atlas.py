#!/usr/bin/env python3
# (c) SR VINIX and contributors
# SPDX-License-Identifier: MIT

"""Stitch a real block atlas out of the user's own Minecraft asset dump.

    python scripts/build_minecraft_atlas.py [--pack <dir>] [--project <dir>] [--tile 16]

Reads `minecraft-assets-26.3/` (Mojang's copyright, gitignored, supplied by the user) and writes
`textures/voxel/atlas_minecraft.png` + `.txt` **in the slot order of generate_voxel_atlas.py**, so
the mesher can switch atlases without one number in `VoxelTiles.h` moving. Both outputs are
gitignored: a derivative of Mojang's pixels carries Mojang's copyright.

The pack is not a pile of ready-made tiles, and three of its conventions have to be resolved here:

  * Grass and foliage textures are **grayscale** and are multiplied by `colormap/grass.png` /
    `colormap/foliage.png` at draw time. `models/block/grass_block.json` says which faces carry
    `"tintindex": 0` (the top and the side overlay, and *not* the plain side, which already ships
    colored) so the tint table below follows the model files rather than a guess.
  * Animated textures are **vertical strips** (`water_still.png` is 16x512, `water_flow.png` is
    32x1024) with a `.mcmeta` `{"animation": {"frametime": 2}}`. Frame 0 is the top square.
  * `destroy_stage_N.png` is white RGB with a **soft alpha**, while this project draws the break
    overlay as an alpha-tested cutout (MASK): a fade carried by alpha would pop to solid black the
    moment it crossed the cutoff. So the crack is quantised here, exactly like the generated atlas
    tapers it by dropping whole pixels.

Exit codes: 0 wrote an atlas (a slot the pack cannot fill borrows the generated atlas's pixels and
is listed - the runtime swaps whole files, never tiles, so it has to be complete), 1 with --strict if
any slot had no pack source, or if the slot order drifted, 2 if the pack is not on disk.
"""

import argparse
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generate_voxel_atlas import CUTOUT, LIQUID, OVERLAY, SLOTS, TILES  # noqa: E402

# slot -> list of layers, painted in order over the previous one. A layer is (pack path, tint).
# Tint is "" (use as authored), "grass", "foliage" or "water". Water has no colour map in the pack,
# so it takes the flat ocean colour below.
PLAIN = ""
MAP = {
    "grass_top":         [("grass_block_top.png", "grass")],
    "grass_side":        [("grass_block_side.png", PLAIN),
                          ("grass_block_side_overlay.png", "grass")],
    "dirt":              [("dirt.png", PLAIN)],
    "stone":             [("stone.png", PLAIN)],
    "cobblestone":       [("cobblestone.png", PLAIN)],
    "mossy_cobblestone": [("mossy_cobblestone.png", PLAIN)],
    "bedrock":           [("bedrock.png", PLAIN)],
    "sand":              [("sand.png", PLAIN)],
    "sandstone_top":     [("sandstone_top.png", PLAIN)],
    "sandstone_side":    [("sandstone.png", PLAIN)],
    "gravel":            [("gravel.png", PLAIN)],
    "clay":              [("clay.png", PLAIN)],
    "log_oak_side":      [("oak_log.png", PLAIN)],
    "log_oak_top":       [("oak_log_top.png", PLAIN)],
    "log_birch_side":    [("birch_log.png", PLAIN)],
    "log_birch_top":     [("birch_log_top.png", PLAIN)],
    "log_spruce_side":   [("spruce_log.png", PLAIN)],
    "log_spruce_top":    [("spruce_log_top.png", PLAIN)],
    "leaves_oak":        [("oak_leaves.png", "foliage")],
    "leaves_birch":      [("birch_leaves.png", "foliage")],
    "planks_oak":        [("oak_planks.png", PLAIN)],
    "planks_birch":      [("birch_planks.png", PLAIN)],
    "planks_spruce":     [("spruce_planks.png", PLAIN)],
    "brick":             [("bricks.png", PLAIN)],
    "stone_bricks":      [("stone_bricks.png", PLAIN)],
    "glass":             [("glass.png", PLAIN)],
    "water":             [("water_still.png", "water")],
    "ice":               [("ice.png", PLAIN)],
    "snow":              [("snow.png", PLAIN)],
    "coal_ore":          [("coal_ore.png", PLAIN)],
    "iron_ore":          [("iron_ore.png", PLAIN)],
    "gold_ore":          [("gold_ore.png", PLAIN)],
    "diamond_ore":       [("diamond_ore.png", PLAIN)],
    "redstone_ore":      [("redstone_ore.png", PLAIN)],
    "lapis_ore":         [("lapis_ore.png", PLAIN)],
    "emerald_ore":       [("emerald_ore.png", PLAIN)],
    "tnt_side":          [("tnt_side.png", PLAIN)],
    "tnt_top":           [("tnt_top.png", PLAIN)],
    "glowstone":         [("glowstone.png", PLAIN)],
    "obsidian":          [("obsidian.png", PLAIN)],
    "netherrack":        [("netherrack.png", PLAIN)],
    "crafting_table_top":  [("crafting_table_top.png", PLAIN)],
    "crafting_table_side": [("crafting_table_side.png", PLAIN)],
    "furnace_front":     [("furnace_front.png", PLAIN)],
    "furnace_side":      [("furnace_side.png", PLAIN)],
    "furnace_top":       [("furnace_top.png", PLAIN)],
    "bookshelf":         [("bookshelf.png", PLAIN)],
    "wool_white":        [("white_wool.png", PLAIN)],
    "wool_red":          [("red_wool.png", PLAIN)],
    "wool_green":        [("green_wool.png", PLAIN)],
    "wool_blue":         [("blue_wool.png", PLAIN)],
    "wool_yellow":       [("yellow_wool.png", PLAIN)],
    "wool_black":        [("black_wool.png", PLAIN)],
    "flower_poppy":      [("poppy.png", PLAIN)],
    "flower_dandelion":  [("dandelion.png", PLAIN)],
    "tall_grass":        [("short_grass.png", "grass")],
    "dead_bush":         [("dead_bush.png", PLAIN)],
    "cactus_side":       [("cactus_side.png", PLAIN)],
    "cactus_top":        [("cactus_top.png", PLAIN)],
    "melon_side":        [("melon_side.png", PLAIN)],
    "melon_top":         [("melon_top.png", PLAIN)],
    "pumpkin_side":      [("pumpkin_side.png", PLAIN)],
    "pumpkin_top":       [("pumpkin_top.png", PLAIN)],
    "lava":              [("lava_still.png", PLAIN)],
    "mud":               [("mud.png", PLAIN)],
    "packed_ice":        [("packed_ice.png", PLAIN)],
    "snowy_grass_side":  [("grass_block_side.png", PLAIN),
                          ("grass_block_snow.png", PLAIN)],
    "slab_stone":        [("stone.png", PLAIN)],
    "torch":             [("torch.png", PLAIN)],
}
for _stage in range(10):
    MAP["destroy_stage_%d" % _stage] = [("destroy_stage_%d.png" % _stage, "crack")]

# The vanilla default ocean colour. Not a guess from a screenshot: the pack ships grass/foliage/dry
# colour maps and no water one, because water is tinted by a biome constant rather than a texture.
WATER_RGB = (68, 117, 167)
# Where a temperate plain sits in the 256x256 colour maps, measured: the valid band of grass.png runs
# from a bright (71,205,51) at (0,0) and this point reads (151,187,95), which is the green the
# generated atlas paints with. Move it for a desert or a tundra.
GRASS_XY = (64, 192)
FOLIAGE_XY = (64, 192)
# The break overlay is drawn alpha-tested, so a crack pixel is either ink or nothing.
CRACK_INK = (14, 14, 16)
CRACK_CUTOFF = 128


def load_frame0(path, tile):
    """The first frame of a strip, at atlas resolution, with its alpha intact."""
    with Image.open(path) as src:
        img = src.convert("RGBA")
        w, h = img.size
        side = min(w, h)
        if h > side or w > side:                 # a strip: frame 0 is the first square
            img = img.crop((0, 0, side, side))
        if img.size != (tile, tile):             # a 32px pack tile still lands in a 16px slot
            img = img.resize((tile, tile), Image.NEAREST)
        return [img.getpixel((x, y)) for y in range(tile) for x in range(tile)]


def colour_of(colormap, xy):
    with Image.open(colormap) as src:
        img = src.convert("RGB")
        x = min(img.size[0] - 1, max(0, xy[0]))
        y = min(img.size[1] - 1, max(0, xy[1]))
        return img.getpixel((x, y))


def tint_pixels(px, rgb):
    """Multiply, don't replace: the grayscale texture carries the shading and the colour the biome."""
    return [(p[0] * rgb[0] // 255, p[1] * rgb[1] // 255, p[2] * rgb[2] // 255, p[3]) for p in px]


def crack_pixels(px):
    out = []
    for p in px:
        out.append(CRACK_INK + (255,) if p[3] >= CRACK_CUTOFF else (0, 0, 0, 0))
    return out


def over(dst, src):
    """Source-over, the way the game draws a side overlay on top of a side."""
    out = []
    for d, s in zip(dst, src):
        sa = s[3]
        if sa == 0:
            out.append(d)
            continue
        if sa == 255 or d[3] == 0:
            out.append(s)
            continue
        da = d[3]
        oa = sa + da * (255 - sa) // 255
        mix = lambda cs, cd: (cs * sa + cd * da * (255 - sa) // 255) // max(1, oa)  # noqa: E731
        out.append((mix(s[0], d[0]), mix(s[1], d[1]), mix(s[2], d[2]), oa))
    return out


def stitch_slot(name, tile, block_dir, grass_rgb, foliage_rgb):
    """The pixels of one slot, or (None, why it has none)."""
    layers = MAP.get(name)
    if not layers:
        return None, "no mapping in this script"
    px = [(0, 0, 0, 0)] * (tile * tile)
    used = []
    for rel, role in layers:
        path = os.path.join(block_dir, rel)
        if not os.path.exists(path):
            # A layer that is not there is not a layer to skip: an overlay drawn over nothing is
            # a half-drawn tile. The whole slot falls back, and the report says which file.
            return None, "%s MISSING" % rel
        layer = load_frame0(path, tile)
        if role == "grass":
            layer = tint_pixels(layer, grass_rgb)
        elif role == "foliage":
            layer = tint_pixels(layer, foliage_rgb)
        elif role == "water":
            layer = tint_pixels(layer, WATER_RGB)
        elif role == "crack":
            layer = crack_pixels(layer)
        px = over(px, layer) if used else layer
        used.append(rel + (" x tint" if role in ("grass", "foliage", "water") else
                           (" -> cutout" if role == "crack" else "")))
    return px, " + ".join(used)


def paint(atlas, col, row, tile, px):
    for y in range(tile):
        for x in range(tile):
            atlas.putpixel((col * tile + x, row * tile + y), px[y * tile + x])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", default=os.path.join("templates", "voxelworld-main"),
                    help="project folder that owns the atlas")
    ap.add_argument("--pack", default=None,
                    help="the Mojang dump (default: <project>/minecraft-assets-26.3)")
    ap.add_argument("--tile", type=int, default=16, help="edge of one tile, as the base atlas")
    ap.add_argument("--grid", type=int, default=0,
                    help="tiles per row (default: the same rule as generate_voxel_atlas.py)")
    ap.add_argument("--out", default="atlas_minecraft",
                    help="stem of the .png/.txt written into textures/voxel")
    ap.add_argument("--strict", action="store_true",
                    help="fail if any slot has no source in the pack")
    args = ap.parse_args()

    pack = args.pack or os.path.join(args.project, "minecraft-assets-26.3")
    block_dir = os.path.join(pack, "assets", "minecraft", "textures", "block")
    colormap_dir = os.path.join(pack, "assets", "minecraft", "textures", "colormap")
    if not os.path.isdir(block_dir):
        print("PACK NOT FOUND: %s\n"
              "The dump is the user's own files and is gitignored, so a checkout without it is\n"
              "normal. The shipped art stays the generated atlas: run generate_voxel_atlas.py."
              % block_dir)
        return 2

    grass_rgb = colour_of(os.path.join(colormap_dir, "grass.png"), GRASS_XY)
    foliage_rgb = colour_of(os.path.join(colormap_dir, "foliage.png"), FOLIAGE_XY)

    grid = args.grid or 1
    while grid * grid < SLOTS:
        grid *= 2

    # The runtime swaps one whole atlas for the other - it cannot paste a tile in at draw time - so
    # a slot the pack cannot fill takes the generated atlas's pixels here. That is what lets
    # voxelAtlasPath() fall back per file and nothing complain about an invisible block later.
    tex_dir = os.path.join(args.project, "textures", "voxel")
    base_png = os.path.join(tex_dir, "atlas.png")
    base = Image.open(base_png).convert("RGBA") if os.path.exists(base_png) else None
    if base is not None and base.size != (grid * args.tile, grid * args.tile):
        print("NOTE %s is %dx%d, not %dx%d: its cells would not line up with these slots, so a slot "
              "with no pack source stays empty instead of borrowing the wrong pixels."
              % (base_png, base.size[0], base.size[1], grid * args.tile, grid * args.tile))
        base = None

    atlas = Image.new("RGBA", (grid * args.tile, grid * args.tile), (0, 0, 0, 0))
    filled, borrowed, missing = 0, 0, []
    for index, name in enumerate(TILES):
        col, row = index % grid, index // grid
        px, note = stitch_slot(name, args.tile, block_dir, grass_rgb, foliage_rgb)
        if px is not None:
            paint(atlas, col, row, args.tile, px)
            filled += 1
            print("%3d %-20s %d,%d  %s" % (index, name, col, row, note))
            continue
        if base is not None:
            box = (col * args.tile, row * args.tile,
                   col * args.tile + args.tile, row * args.tile + args.tile)
            atlas.paste(base.crop(box), box)
            borrowed += 1
            print("%3d %-20s %d,%d  %s -> taken from atlas.png" % (index, name, col, row, note))
            missing.append("%s (%s, borrowed from the generated atlas)" % (name, note))
        else:
            print("%3d %-20s %d,%d  %s -> NOTHING TO FALL BACK TO" % (index, name, col, row, note))
            missing.append("%s (%s, and no generated atlas to borrow from)" % (name, note))

    os.makedirs(tex_dir, exist_ok=True)
    png = os.path.join(tex_dir, "%s.png" % args.out)
    atlas.save(png)

    txt = os.path.join(tex_dir, "%s.txt" % args.out)
    with open(txt, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# name index col row cutout liquid  (tile %dpx, grid %dx%d)\n"
                     "# Stitched from the user's own Mojang dump at %s. Their copyright, their file:\n"
                     "# this PNG and every atlas derived from it are gitignored. voxelAtlasPath()\n"
                     "# binds this file when it is on disk and textures/voxel/atlas.png otherwise;\n"
                     "# a slot the pack cannot fill already carries that atlas's pixels, because the\n"
                     "# runtime swaps whole files, not tiles.\n"
                     % (args.tile, grid, grid, pack))
        for index, name in enumerate(TILES):
            handle.write("%s %d %d %d %d %d\n" % (name, index, index % grid, index // grid,
                                                  int(name in CUTOUT), int(name in LIQUID)))

    print()
    print("atlas:    %s  %dx%d  tile %dpx  grid %dx%d  %d/%d slots from the pack, %d borrowed"
          % (png, atlas.size[0], atlas.size[1], args.tile, grid, grid, filled, SLOTS, borrowed))
    print("manifest: %s" % txt)
    print("tints:    grass %s @ %s   foliage %s @ %s   water %s (flat, no colour map in the pack)"
          % (grass_rgb, GRASS_XY, foliage_rgb, FOLIAGE_XY, WATER_RGB))
    print("overlay:  crack alpha >= %d becomes %s opaque, the rest transparent (MASK cutout)"
          % (CRACK_CUTOFF, CRACK_INK))

    # The one invariant the whole swap rests on: a saved world stores slot numbers, so an atlas that
    # is not in the base atlas's order would repaint every block in the game. Checked against the
    # file the other script wrote, not against TILES again - the point is that the two agree.
    base = os.path.join(tex_dir, "atlas.txt")
    if os.path.exists(base):
        with open(base, encoding="utf-8") as handle:
            rows = [line.split() for line in handle if line and line[0] != "#"]
        with open(txt, encoding="utf-8") as handle:
            mine = [line.split() for line in handle if line and line[0] != "#"]
        drift = [a[0] for a, b in zip(rows, mine) if a != b]
        if len(rows) != len(mine):
            print("slot order: DIFFERS in length (%d vs %d rows)" % (len(rows), len(mine)))
            return 1
        print("slot order: %s %s atlas.txt (%d rows, name index col row cutout liquid)"
              % ("identical to" if not drift else "DIFFERS FROM", base, len(rows)))
        if drift:
            print("  drifting slots: %s" % ", ".join(drift[:12]))
            return 1
    else:
        print("slot order: not checked - %s has not been generated yet" % base)

    # Every filled slot must have brought pixels: a tile that stitched to nothing would draw the
    # black of an empty atlas cell and look like a texture bug rather than a build bug.
    empty = []
    for index, name in enumerate(TILES):
        col, row = index % grid, index // grid
        if not any(atlas.getpixel((col * args.tile + x, row * args.tile + y))[3] > 0
                   for y in range(args.tile) for x in range(args.tile)):
            empty.append(name)
    if empty:
        print("EMPTY %d slot(s) stitched to nothing: %s" % (len(empty), ", ".join(empty)))
        return 1
    print("pixels:   every stitched slot has at least one non-transparent texel")

    if missing:
        print("MISSING %d slot(s) with no pack source:" % len(missing))
        for line in missing:
            print("  " + line)
    return 1 if (missing and args.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
