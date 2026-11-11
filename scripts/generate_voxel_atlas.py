#!/usr/bin/env python3
# (c) SR VINIX and contributors
# SPDX-License-Identifier: MIT

"""Draw the voxel block atlas, its manifest, and the C++ tile enum the mesher reads.

Nothing here is sampled from a shipped game: every tile is painted pixel by pixel from a seeded
hash, in the visual language of a 16x16 block atlas (base colour + a few quantised shades + the
structure that makes a log read as a log). Mojang's textures are their copyright, so the project
owns this file as its source of truth and regenerates the PNG from it.

    python scripts/generate_voxel_atlas.py [--tile 16] [--project <dir>] [--seed N]

Writes, into templates/voxelworld-main by default:
  textures/voxel/atlas.png   grid*grid tiles of TILE px, RGBA, slot order = TILES below
  textures/voxel/atlas.txt   "name index col row cutout liquid", for tools and for humans
  scripts/VoxelTiles.h              the enum + voxelTileRect() the chunk mesher includes

TILES is the single source of truth: the enum, the manifest and the pixels all come from its
order. Append, never insert - a saved world stores slot numbers, not names.
"""

import argparse
import math
import os
import random
import sys

from PIL import Image

AIR = -1

# Atlas slot order, which is also the generated enum order.
TILES = [
    "grass_top", "grass_side", "dirt", "stone", "cobblestone", "mossy_cobblestone",
    "bedrock", "sand", "sandstone_top", "sandstone_side", "gravel", "clay",
    "log_oak_side", "log_oak_top", "log_birch_side", "log_birch_top",
    "log_spruce_side", "log_spruce_top", "leaves_oak", "leaves_birch",
    "planks_oak", "planks_birch", "planks_spruce", "brick", "stone_bricks",
    "glass", "water", "ice", "snow", "coal_ore", "iron_ore", "gold_ore",
    "diamond_ore", "redstone_ore", "lapis_ore", "emerald_ore", "tnt_side", "tnt_top",
    "glowstone", "obsidian", "netherrack", "crafting_table_top", "crafting_table_side",
    "furnace_front", "furnace_side", "furnace_top", "bookshelf", "wool_white",
    "wool_red", "wool_green", "wool_blue", "wool_yellow", "wool_black",
    "flower_poppy", "flower_dandelion", "tall_grass", "dead_bush", "cactus_side",
    "cactus_top", "melon_side", "melon_top", "pumpkin_side", "pumpkin_top",
    "lava", "mud", "packed_ice", "snowy_grass_side", "slab_stone",
    "destroy_stage_0", "destroy_stage_1", "destroy_stage_2", "destroy_stage_3",
    "destroy_stage_4", "destroy_stage_5", "destroy_stage_6", "destroy_stage_7",
    "destroy_stage_8", "destroy_stage_9", "torch",
]

# Cut-out tiles are meshed as two crossed quads with alpha, not as a solid cube.
CUTOUT = {"glass", "leaves_oak", "leaves_birch", "flower_poppy", "flower_dandelion",
          "tall_grass", "dead_bush", "torch"}
# Liquid tiles go to the transparent submesh and are drawn one level below the block above.
LIQUID = {"water", "lava"}
# The break overlay: drawn over an existing face, so everything that is not a crack must be
# genuinely transparent or the block under it disappears.
OVERLAY = {"destroy_stage_%d" % i for i in range(10)}
# Tiles whose alpha channel matters at all: everything else is written fully opaque.
ALPHA = CUTOUT | LIQUID | OVERLAY | {"ice"}

SLOTS = len(TILES)


# ============================================================================
# Painting primitives
# ============================================================================

def clamp(value):
    return 0 if value < 0 else (255 if value > 255 else int(round(value)))


def shade(rgb, amount):
    """Multiply a colour by a brightness delta, keeping it in 8-bit range."""
    return (clamp(rgb[0] * amount), clamp(rgb[1] * amount), clamp(rgb[2] * amount))


def mix(a, b, t):
    return (clamp(a[0] + (b[0] - a[0]) * t),
            clamp(a[1] + (b[1] - a[1]) * t),
            clamp(a[2] + (b[2] - a[2]) * t))


def name_salt(name):
    salt = 0
    for ch in name.encode():
        salt = (salt * 131 + ch) & 0xFFFFFFFF
    return salt


class Tile:
    """A TILE x TILE pixel buffer a painter writes into, with hash-noise helpers.

    Noise is a per-pixel integer hash of (tile name, x, y, channel) rather than a shared RNG, so a
    tile keeps its pixels when another tile is added and a rebuild is byte-for-byte reproducible.
    """

    def __init__(self, name, size, seed):
        self.name = name
        self.size = size
        self.seed = seed
        self.salt = name_salt(name)
        self.px = [(0, 0, 0, 0) for _ in range(size * size)]

    def noise(self, x, y, channel=0):
        h = self.seed ^ self.salt
        h = (h + x * 0x1F1F1F1F + y * 0x85EBCA6B + channel * 0x9E3779B9) & 0xFFFFFFFF
        h = ((h ^ (h >> 13)) * 0xC2B2AE35) & 0xFFFFFFFF
        h = ((h ^ (h >> 15)) * 0x27D4EB2F) & 0xFFFFFFFF
        h = (h ^ (h >> 16)) & 0xFFFFFFFF
        return h / 4294967295.0

    def get(self, x, y):
        return self.px[y * self.size + x]

    def set(self, x, y, rgba):
        if 0 <= x < self.size and 0 <= y < self.size:
            alpha = rgba[3] if len(rgba) > 3 else 255
            self.px[y * self.size + x] = (rgba[0], rgba[1], rgba[2], alpha)

    def set_rgb(self, x, y, rgb, alpha=255):
        """Write a colour without touching the tile's alpha - the painters keep opacity separate."""
        self.set(x, y, (rgb[0], rgb[1], rgb[2], alpha))

    def grain(self, base, spread, steps=4):
        """Blocky brightness noise: the pixel texture every dirt-like tile shares.

        Quantising to `steps` levels is what makes a 16x16 tile read as a hand-authored pack
        instead of as smooth photographic noise.
        """
        alpha = 255
        for y in range(self.size):
            for x in range(self.size):
                level = int(self.noise(x, y, 0) * steps) / max(1, steps - 1)
                self.set_rgb(x, y, shade(base, 1.0 - spread + spread * 2 * level), alpha)

    def clear(self, rgba=(0, 0, 0, 0)):
        self.px = [rgba for _ in range(self.size * self.size)]

    def rng(self, tag):
        return random.Random((self.seed ^ self.salt ^ (tag * 0x51ED)) & 0xFFFFFFFF)


def blobs(tile, rgb, count, min_r, max_r, tag=0, alpha=255):
    """Draw `count` round blobs at seeded positions: the stone/ore/pebble vocabulary."""
    rng = tile.rng(0xB10B ^ tag)
    for _ in range(count):
        cx = rng.random() * tile.size
        cy = rng.random() * tile.size
        radius = rng.uniform(min_r, max_r)
        for y in range(int(cy - radius) - 1, int(cy + radius) + 2):
            for x in range(int(cx - radius) - 1, int(cx + radius) + 2):
                if math.hypot(x - cx, y - cy) > radius:
                    continue
                wx, wy = x % tile.size, y % tile.size
                cur = tile.get(wx, wy)
                out = mix((cur[0], cur[1], cur[2]), rgb, 1.0)
                tile.set(wx, wy, out + (max(cur[3], alpha),))


def streak_column(tile, x, tone, base):
    for y in range(tile.size):
        cur = tile.get(x, y)
        tile.set(x, y, shade(tone, 0.85 + 0.3 * tile.noise(x, y, 17)) + (cur[3],))


# ============================================================================
# Painters, one per visual family
# ============================================================================

def paint_dirt(tile):
    tile.grain((134, 96, 67), 0.16, 5)
    blobs(tile, (108, 76, 52), 5, 1.2, 2.6, 1)
    blobs(tile, (158, 118, 84), 4, 1.0, 2.2, 2)


def paint_grass_top(tile):
    tile.grain((107, 154, 62), 0.13, 5)
    blobs(tile, (86, 132, 48), 6, 1.0, 2.4, 3)
    blobs(tile, (128, 174, 76), 5, 0.8, 1.8, 4)


def paint_grass_side(tile, snow=False):
    paint_dirt(tile)
    green = (232, 240, 245) if snow else (107, 154, 62)
    edge_dark = (198, 210, 216) if snow else (74, 110, 42)
    # The fringe is not a straight line: ragged rows of green over the dirt, the way a 16px tile
    # reads as grass rather than as a painted band.
    for x in range(tile.size):
        depth = 3 + int(tile.noise(x, 0, 5) * 3.0) - 1
        for y in range(depth):
            tile.set_rgb(x, y, shade(green, 0.90 + 0.20 * tile.noise(x, y, 6)))
        tile.set_rgb(x, depth, shade(edge_dark, 0.95 + 0.1 * tile.noise(x, depth, 7)))


def paint_stone(tile):
    tile.grain((128, 128, 128), 0.10, 4)
    blobs(tile, (106, 106, 106), 5, 1.4, 3.0, 5)
    blobs(tile, (150, 150, 150), 4, 1.0, 2.2, 6)


def paint_cobble(tile, mossy=False):
    tile.grain((90, 90, 90), 0.08, 3)
    # Rounded stones with dark gaps between them: a grid of jittered cells, each one a blob.
    cell = tile.size / 4.0
    rng = tile.rng(0xC0BB1E)
    base = mix((150, 150, 150), (110, 150, 105), 0.45) if mossy else (150, 150, 150)
    for gy in range(4):
        for gx in range(4):
            cx = (gx + 0.5) * cell + rng.uniform(-0.18, 0.18) * cell
            cy = (gy + 0.5) * cell + rng.uniform(-0.18, 0.18) * cell
            tone = shade(base, 0.82 + rng.random() * 0.36)
            for y in range(int(cy - cell * 0.55), int(cy + cell * 0.55) + 1):
                for x in range(int(cx - cell * 0.55), int(cx + cell * 0.55) + 1):
                    if math.hypot(x - cx, y - cy) > cell * 0.52:
                        continue
                    wx, wy = (x + tile.size) % tile.size, (y + tile.size) % tile.size
                    cur = tile.get(wx, wy)
                    tile.set(wx, wy, shade(tone, 0.92 + 0.16 * tile.noise(wx, wy, 9)) + (cur[3],))
    if mossy:
        blobs(tile, (96, 128, 84), 4, 1.2, 2.6, 11)


def paint_bedrock(tile):
    tile.grain((85, 85, 85), 0.2, 6)
    blobs(tile, (35, 35, 35), 7, 1.0, 3.0, 7)
    blobs(tile, (130, 130, 130), 5, 0.8, 2.0, 8)


def paint_sand(tile):
    tile.grain((219, 210, 156), 0.07, 4)
    blobs(tile, (236, 228, 180), 6, 0.7, 1.6, 9)


def paint_sandstone(tile, top=False):
    if top:
        paint_sand(tile)
        blobs(tile, (204, 195, 140), 5, 1.0, 2.2, 10)
        return
    tile.grain((212, 203, 150), 0.05, 3)
    # Three stacked beds with a darker seam each: the side face of a sandstone block.
    for y in range(tile.size):
        if y % 5 == 4:
            for x in range(tile.size):
                tile.set_rgb(x, y, shade((186, 176, 124), 0.92 + 0.16 * tile.noise(x, y, 12)))
    blobs(tile, (232, 224, 176), 4, 0.8, 1.8, 11)


def paint_gravel(tile):
    tile.grain((126, 122, 120), 0.12, 5)
    for tone, off in (((92, 88, 86), 12), ((160, 156, 152), 13), ((70, 68, 66), 14)):
        blobs(tile, tone, 5, 0.9, 2.0, off)


def paint_clay(tile):
    tile.grain((160, 164, 170), 0.07, 4)
    blobs(tile, (140, 145, 152), 5, 1.0, 2.0, 15)


BARK = {
    "oak": ((102, 81, 50), (78, 61, 38)),
    "birch": ((214, 210, 201), (150, 148, 142)),
    "spruce": ((74, 58, 36), (52, 40, 26)),
}
HEART = {
    "oak": ((164, 129, 79), (122, 95, 56)),
    "birch": ((200, 173, 121), (166, 141, 96)),
    "spruce": ((140, 104, 60), (104, 76, 42)),
}


def paint_log_side(tile, kind="oak"):
    bark, dark = BARK[kind]
    tile.grain(bark, 0.08, 4)
    # Vertical streaks, plus the knots birch is known for.
    for x in range(tile.size):
        strength = tile.noise(x, 0, 16)
        if strength > 0.62:
            streak_column(tile, x, dark, bark)
        elif strength < 0.18:
            for y in range(tile.size):
                cur = tile.get(x, y)
                tile.set(x, y, shade(bark, 1.12) + (cur[3],))
    if kind == "birch":
        rng = tile.rng(0xB17C4)
        for _ in range(2):
            cx, cy = rng.randrange(1, tile.size - 2), rng.randrange(1, tile.size - 2)
            blobs(tile, (60, 58, 54), 1, 1.0, 1.8, cx * 31 + cy)
    else:
        blobs(tile, shade(bark, 0.72), 3, 1.0, 1.8, 19)


def paint_log_top(tile, kind="oak"):
    heart, ring = HEART[kind]
    cx = cy = (tile.size - 1) / 2.0
    for y in range(tile.size):
        for x in range(tile.size):
            band = int(math.hypot(x - cx, y - cy) / 1.7) % 2
            tone = heart if band == 0 else ring
            tile.set_rgb(x, y, shade(tone, 0.94 + 0.12 * tile.noise(x, y, 20)))
    blobs(tile, shade(ring, 0.8), 1, 0.6, 1.2, 21)


def paint_leaves(tile, birch=False):
    """Cut-out foliage: a dense green field with punched holes so the silhouette is ragged."""
    base = (62, 122, 44) if not birch else (96, 148, 66)
    tile.grain(base, 0.16, 5)
    blobs(tile, shade(base, 1.25), 5, 1.0, 2.2, 60)
    blobs(tile, shade(base, 0.68), 5, 1.0, 2.0, 61)
    holes = tile.size * tile.size // 5
    for y in range(tile.size):
        for x in range(tile.size):
            if tile.noise(x, y, 62) < float(holes) / (tile.size * tile.size):
                tile.set(x, y, (0, 0, 0, 0))


def paint_planks(tile, kind="oak"):
    base = {"oak": (162, 130, 78), "birch": (192, 170, 110), "spruce": (114, 87, 52)}[kind]
    tile.grain(base, 0.07, 4)
    # Four boards with a dark seam, and a nail dot on the ends of two of them.
    for y in range(tile.size):
        if y % 4 == 3:
            for x in range(tile.size):
                tile.set_rgb(x, y, shade(base, 0.62))
    for (nx, ny) in ((2, 1), (13, 5), (2, 9), (13, 13)):
        tile.set_rgb(nx, ny, shade(base, 0.55))
    for x in range(tile.size):
        if tile.noise(x, 0, 22) > 0.7:
            y = int(tile.noise(x, 1, 23) * (tile.size - 1))
            tile.set_rgb(x, y, shade(base, 0.78))


def paint_bricks(tile):
    tile.grain((154, 99, 88), 0.08, 4)
    mortar = (188, 176, 164)
    for y in range(tile.size):
        offset = 0 if (y // 4) % 2 == 0 else 4
        for x in range(tile.size):
            if y % 4 == 3 or (x + offset) % 8 == 7:
                tile.set_rgb(x, y, shade(mortar, 0.94 + 0.1 * tile.noise(x, y, 24)))


def paint_stone_bricks(tile):
    paint_stone(tile)
    mortar = (86, 86, 86)
    for y in range(tile.size):
        offset = 0 if (y // 4) % 2 == 0 else 4
        for x in range(tile.size):
            if y % 4 == 3 or (x + offset) % 8 == 7:
                tile.set_rgb(x, y, shade(mortar, 0.9 + 0.1 * tile.noise(x, y, 25)))


def paint_glass(tile):
    for y in range(tile.size):
        for x in range(tile.size):
            border = x in (0, tile.size - 1) or y in (0, tile.size - 1)
            tile.set(x, y, (208, 226, 232, 210) if border else (200, 222, 230, 40))
    # Highlight streaks so the pane is not a flat wash of alpha.
    for i in range(2, tile.size - 2):
        tile.set(i, i, (236, 246, 250, 120))
        tile.set(tile.size - 1 - i, i, (236, 246, 250, 90))


def paint_water(tile, lava=False):
    base = (206, 92, 22) if lava else (52, 108, 201)
    alpha = 235 if lava else 168
    for y in range(tile.size):
        for x in range(tile.size):
            wave = 0.5 + 0.5 * math.sin((x + y * 0.6) * 0.9)
            tone = mix(base, shade(base, 1.25), 0.25 * wave + 0.35 * tile.noise(x, y, 26))
            tile.set(x, y, tone + (alpha,))
    if lava:
        blobs(tile, (252, 190, 60), 3, 1.0, 2.2, 27, alpha)
        blobs(tile, (120, 40, 10), 3, 1.0, 2.0, 28, alpha)


def paint_ice(tile, packed=False):
    base = (100, 150, 220) if packed else (140, 180, 240)
    alpha = 255 if packed else 200
    for y in range(tile.size):
        for x in range(tile.size):
            tile.set(x, y, mix(base, shade(base, 1.2), tile.noise(x, y, 29)) + (alpha,))
    rng = tile.rng(0x1CE)
    for _ in range(4):
        x, y = rng.randrange(tile.size), rng.randrange(max(1, tile.size - 3))
        for s in range(3):
            if x + s < tile.size:
                tile.set(x + s, y, shade(base, 1.3) + (alpha,))


def paint_snow(tile):
    tile.grain((238, 244, 248), 0.04, 3)
    blobs(tile, (216, 228, 238), 5, 0.8, 1.8, 31)


def paint_ore(tile, ore_rgb, count=4):
    paint_stone(tile)
    rng = tile.rng(0x0DE)
    for _ in range(count):
        cx = rng.randrange(2, tile.size - 2)
        cy = rng.randrange(2, tile.size - 2)
        radius = rng.uniform(1.2, 2.2)
        for y in range(int(cy - radius) - 1, int(cy + radius) + 2):
            for x in range(int(cx - radius) - 1, int(cx + radius) + 2):
                if not (0 <= x < tile.size and 0 <= y < tile.size):
                    continue
                d = math.hypot(x - cx, y - cy)
                if d > radius:
                    continue
                tile.set_rgb(x, y, shade(ore_rgb, 0.72 if d > radius - 0.6 else 1.05))


def paint_tnt(tile, top=False):
    if top:
        tile.grain((176, 62, 48), 0.08, 4)
        blobs(tile, (60, 58, 58), 1, 2.6, 3.2, 32)
        tile.set_rgb(7, 7, (222, 214, 200))
        tile.set_rgb(8, 7, (222, 214, 200))
        return
    tile.grain((196, 70, 54), 0.07, 4)
    for y in range(5, 11):
        for x in range(tile.size):
            tile.set_rgb(x, y, (236, 232, 226) if tile.noise(x, y, 33) > 0.35
                         else (214, 208, 200))
    for x in range(3, 13):
        tile.set_rgb(x, 7, (40, 38, 38))
        tile.set_rgb(x, 8, (40, 38, 38))


def paint_glowstone(tile):
    tile.grain((200, 168, 92), 0.10, 4)
    blobs(tile, (252, 232, 152), 6, 1.0, 2.2, 34)
    blobs(tile, (150, 116, 60), 5, 0.8, 1.6, 35)


def paint_obsidian(tile):
    tile.grain((24, 20, 34), 0.14, 5)
    blobs(tile, (56, 44, 78), 5, 0.8, 1.8, 36)
    blobs(tile, (12, 8, 18), 4, 0.7, 1.5, 37)


def paint_netherrack(tile):
    tile.grain((98, 34, 34), 0.16, 6)
    blobs(tile, (60, 20, 20), 6, 1.0, 2.2, 38)
    blobs(tile, (132, 62, 56), 4, 0.8, 1.6, 39)


def paint_crafting_top(tile):
    tile.grain((162, 130, 78), 0.06, 4)
    for y in range(tile.size):
        for x in range(tile.size):
            inside = 2 <= x < tile.size - 2 and 2 <= y < tile.size - 2
            if inside:
                tile.set_rgb(x, y, (196, 176, 132) if (x + y) % 3 else (178, 158, 116, 255))
            if x in (2, tile.size - 3) or y in (2, tile.size - 3):
                tile.set_rgb(x, y, (96, 74, 44))


def paint_crafting_side(tile):
    paint_planks(tile)
    for y in range(4, 8):
        for x in range(2, tile.size - 2):
            if x in (2, 6, 10, 13) or y == 4:
                tile.set_rgb(x, y, (98, 76, 46))


def paint_furnace(tile, front=False, top=False):
    paint_stone(tile)
    dark = (60, 60, 60)
    if top:
        blobs(tile, (86, 86, 86), 4, 1.4, 2.4, 40)
        return
    if front:
        for y in range(4, 12):
            for x in range(3, 13):
                inner = 5 <= y < 10 and 5 <= x < 11
                tile.set_rgb(x, y, (34, 30, 28) if inner else dark)
        for x in range(5, 11):
            tile.set_rgb(x, 9, (128, 74, 32))
    else:
        for y in range(3, 13):
            for x in range(3, 13):
                if x in (3, 12) or y in (3, 12):
                    tile.set_rgb(x, y, dark)


def paint_bookshelf(tile):
    paint_planks(tile)
    books = [(150, 60, 54), (70, 110, 160), (180, 160, 80), (100, 150, 90)]
    for band in (4, 10):
        for y in range(band, band + 5):
            for x in range(1, tile.size - 1):
                tile.set_rgb(x, y, (64, 48, 28))
        x = 1
        while x < tile.size - 1:
            w = 1 + int(tile.noise(x, band, 41) * 2)
            tone = books[int(tile.noise(x, band + 1, 42) * 4) % 4]
            for dx in range(w):
                if x + dx < tile.size - 1:
                    for y in range(band + 1, band + 4):
                        tile.set_rgb(x + dx, y, tone)
            x += w + 1


def paint_wool(tile, rgb):
    tile.grain(rgb, 0.06, 3)
    blobs(tile, shade(rgb, 1.12), 6, 0.7, 1.5, 43)
    blobs(tile, shade(rgb, 0.86), 5, 0.7, 1.4, 44)


def paint_crossflower(tile, petal, center, stem):
    tile.clear()
    x = tile.size // 2
    # Stem: a crooked green line up the middle, then the head as a soft disc.
    for y in range(tile.size - 1, 5, -1):
        jx = x + (1 if tile.noise(x, y, 45) > 0.72 else 0) - (1 if tile.noise(x, y, 46) > 0.66 else 0)
        tile.set_rgb(jx, y, stem)
        tile.set_rgb(jx + 1, y, shade(stem, 0.82))
    for dx in (-3, 2):
        for y in (10, 11):
            tile.set_rgb(x + dx, y, shade(stem, 0.9))
    cx, cy = tile.size // 2, 4
    for dy in range(-3, 4):
        for dx in range(-3, 4):
            d = math.hypot(dx, dy)
            if d > 3.1:
                continue
            tone = center if d < 1.2 else petal
            tile.set_rgb(cx + dx, cy + dy, shade(tone, 0.9 + 0.2 * tile.noise(cx + dx, cy + dy, 47)))


def paint_tall_grass(tile):
    tile.clear()
    rng = tile.rng(0x9A55)
    for _ in range(7):
        base_x = rng.randrange(1, tile.size - 1)
        lean = rng.choice((-1, 0, 1))
        height = rng.randrange(7, 13)
        tone = (86, 132, 48) if rng.random() > 0.5 else (112, 162, 66)
        for i in range(height):
            x = max(0, min(tile.size - 1, base_x + lean * (i // 4)))
            tile.set_rgb(x, tile.size - 1 - i, shade(tone, 0.85 + 0.3 * rng.random()))


def paint_dead_bush(tile):
    tile.clear()
    rng = tile.rng(0xB054)
    cx = tile.size // 2
    for _ in range(6):
        lean = rng.choice((-2, -1, 0, 1, 2))
        height = rng.randrange(6, 12)
        for i in range(height):
            x = max(0, min(tile.size - 1, cx + lean * (i // 3)))
            tile.set_rgb(x, tile.size - 1 - i, shade((136, 100, 52), 0.8 + 0.4 * rng.random()))


def paint_torch(tile):
    """A stick with a flame on top, centred in the tile.

    A torch is meshed as two crossed quads over the whole cell, so the air around it has to stay
    transparent or those quads paint a wall, and the stick has to sit on the tile's centre line or
    the torch reads as leaning out of its own block.
    """
    tile.clear()
    rng = tile.rng(0x70A1)
    size = tile.size
    cx = size // 2
    flame_bottom = 5                    # rows 0..5 are the flame, the rest is the stick

    for y in range(flame_bottom + 1, size):
        # Two pixels wide, the lit edge facing the flame's left, with enough grain that it is not a
        # flat brown bar at 16 px. Straight: the quad it lands on already leans with the camera.
        for dx in (0, 1):
            x = max(0, min(size - 1, cx - 1 + dx))
            base = (150, 110, 66) if dx == 0 else (112, 80, 46)
            tile.set_rgb(x, y, shade(base, 0.88 + 0.24 * rng.random()))

    # Half-width per row, tip to base. The flame is brightest where it leaves the stick, so the two
    # bottom rows drop the orange skin and paint the white-hot core.
    widths = (0, 1, 1, 2, 2, 1)
    for y, half in enumerate(widths):
        for dx in range(-half, half + 1):
            x = max(0, min(size - 1, cx + dx))
            edge = half > 0 and abs(dx) == half
            if y >= flame_bottom - 1 and not edge:
                col = (255, 240, 176) if y == flame_bottom else (255, 208, 96)
            elif edge:
                col = (226, 116, 34)
            else:
                col = (255, 158, 48)
            tile.set_rgb(x, y, shade(col, 0.92 + 0.16 * rng.random()))


def paint_cactus(tile, top=False):
    base = (86, 156, 76) if top else (58, 128, 58)
    tile.grain(base, 0.08, 4)
    if top:
        blobs(tile, (110, 172, 92), 3, 1.2, 2.2, 48)
        return
    for x in (1, tile.size - 2):
        for y in range(tile.size):
            tile.set_rgb(x, y, shade(base, 0.62))
    for y in range(2, tile.size, 4):
        for x in range(3, tile.size - 3, 4):
            tile.set_rgb(x, y, (222, 222, 190))
            tile.set_rgb(x + 1, y, (200, 200, 170))


def paint_melon(tile, top=False):
    base = (96, 158, 62)
    tile.grain(base, 0.08, 4)
    if top:
        blobs(tile, (70, 122, 46), 4, 1.2, 2.2, 49)
        return
    for y in range(tile.size):
        for x in range(tile.size):
            if (x // 3) % 2 == 0:
                tile.set_rgb(x, y, shade(base, 0.72))


def paint_pumpkin(tile, top=False):
    base = (214, 130, 32)
    tile.grain(base, 0.07, 4)
    if top:
        blobs(tile, (150, 92, 28), 3, 1.4, 2.4, 50)
        for y in range(3, tile.size - 3):
            tile.set_rgb(tile.size // 2, y, shade((120, 76, 24), 1.0))
        return
    for x in range(0, tile.size, 4):
        for y in range(tile.size):
            tile.set_rgb(x, y, shade(base, 0.66))


def paint_mud(tile):
    tile.grain((88, 74, 64), 0.10, 4)
    blobs(tile, (66, 54, 46), 5, 1.0, 2.2, 51)


def paint_slab(tile):
    paint_stone(tile)
    for x in range(tile.size):
        tile.set_rgb(x, 3, shade((128, 128, 128), 0.66))


CRACK_SPOKES = 16
_CRACK_CACHE = {}


def crack_network(size):
    """One crack network cut into CRACK_SPOKES growing spokes, cached per tile edge.

    Each destroy stage draws a *prefix* of the same list instead of its own random layout: stage 5
    has to contain stage 4, or the cracks jump around under the crosshair and the block never reads
    as breaking progressively. The seed is a constant rather than the per-tile one for the same
    reason - the ten tiles are ten slices of one network, not ten independent tiles.
    """
    cached = _CRACK_CACHE.get(size)
    if cached is not None:
        return cached

    rng = random.Random(0xC0FFEE ^ size)
    spokes = []
    for _ in range(CRACK_SPOKES):
        x = rng.uniform(2.0, size - 3.0)
        y = rng.uniform(2.0, size - 3.0)
        angle = rng.uniform(0.0, math.tau * 2.0)
        points = [(x, y)]
        for _ in range(rng.randint(4, 9)):
            angle += rng.uniform(-0.85, 0.85)
            x += math.cos(angle)
            y += math.sin(angle)
            if not (0.0 <= x < size and 0.0 <= y < size):
                break
            points.append((x, y))
        if len(points) > 2:
            spokes.append(points)

    _CRACK_CACHE[size] = spokes
    return spokes


def paint_destroy(tile, stage):
    """Progressive break overlay: cracks only, transparent everywhere else."""
    size = tile.size
    spokes = crack_network(size)
    visible = len(spokes) * (stage + 1) // 10
    dark = (14, 14, 16)
    lift = (96, 96, 100)

    for pts in spokes[:visible]:
        for i in range(len(pts) - 1):
            x0, y0 = pts[i]
            x1, y1 = pts[i + 1]
            steps = max(abs(x1 - x0), abs(y1 - y0), 1.0)
            for s in range(int(steps) + 1):
                t = s / (steps + 1.0)
                px = int(round(x0 + (x1 - x0) * t))
                py = int(round(y0 + (y1 - y0) * t))
                if not (0 <= px < size and 0 <= py < size):
                    continue
                # The overlay is drawn as an alpha-tested cutout (alphaMode MASK), so a pixel is
                # either gone or fully opaque: a fade carried by the alpha channel would pop to
                # solid black the moment it crossed the cutoff. The taper is therefore carried by
                # which pixels exist - the head of each spoke is every pixel, the tail every other
                # one, which reads as a crack that thins as it spreads.
                weight = 1.0 - 0.45 * (i / max(1, len(pts) - 2))
                if weight >= 0.85 or s % 2 == 0:
                    tile.set(px, py, dark + (255,))
                if stage >= 6 and s % 3 == 0:
                    tile.set((px + 1) % size, py, lift + (255,))
        # The origin of a late spoke spalls: one wider pixel so the network thickens as it grows.
        if stage >= 3:
            ox, oy = int(round(pts[0][0])), int(round(pts[0][1]))
            tile.set(ox, (oy + 1) % size, dark + (255,))


PAINTERS = {
    "grass_top": paint_grass_top,
    "grass_side": paint_grass_side,
    "snowy_grass_side": lambda t: paint_grass_side(t, snow=True),
    "dirt": paint_dirt,
    "stone": paint_stone,
    "cobblestone": paint_cobble,
    "mossy_cobblestone": lambda t: paint_cobble(t, mossy=True),
    "bedrock": paint_bedrock,
    "sand": paint_sand,
    "sandstone_top": lambda t: paint_sandstone(t, top=True),
    "sandstone_side": lambda t: paint_sandstone(t),
    "gravel": paint_gravel,
    "clay": paint_clay,
    "log_oak_side": lambda t: paint_log_side(t, "oak"),
    "log_oak_top": lambda t: paint_log_top(t, "oak"),
    "log_birch_side": lambda t: paint_log_side(t, "birch"),
    "log_birch_top": lambda t: paint_log_top(t, "birch"),
    "log_spruce_side": lambda t: paint_log_side(t, "spruce"),
    "log_spruce_top": lambda t: paint_log_top(t, "spruce"),
    "leaves_oak": lambda t: paint_leaves(t),
    "leaves_birch": lambda t: paint_leaves(t, birch=True),
    "planks_oak": lambda t: paint_planks(t, "oak"),
    "planks_birch": lambda t: paint_planks(t, "birch"),
    "planks_spruce": lambda t: paint_planks(t, "spruce"),
    "brick": paint_bricks,
    "stone_bricks": paint_stone_bricks,
    "glass": paint_glass,
    "water": paint_water,
    "lava": lambda t: paint_water(t, lava=True),
    "ice": paint_ice,
    "packed_ice": lambda t: paint_ice(t, packed=True),
    "snow": paint_snow,
    "coal_ore": lambda t: paint_ore(t, (32, 32, 32)),
    "iron_ore": lambda t: paint_ore(t, (216, 169, 130)),
    "gold_ore": lambda t: paint_ore(t, (246, 208, 66)),
    "diamond_ore": lambda t: paint_ore(t, (110, 222, 224)),
    "redstone_ore": lambda t: paint_ore(t, (190, 32, 28)),
    "lapis_ore": lambda t: paint_ore(t, (36, 80, 190)),
    "emerald_ore": lambda t: paint_ore(t, (40, 190, 110)),
    "tnt_side": lambda t: paint_tnt(t),
    "tnt_top": lambda t: paint_tnt(t, top=True),
    "glowstone": paint_glowstone,
    "obsidian": paint_obsidian,
    "netherrack": paint_netherrack,
    "crafting_table_top": paint_crafting_top,
    "crafting_table_side": paint_crafting_side,
    "furnace_front": lambda t: paint_furnace(t, front=True),
    "furnace_side": lambda t: paint_furnace(t),
    "furnace_top": lambda t: paint_furnace(t, top=True),
    "bookshelf": paint_bookshelf,
    "wool_white": lambda t: paint_wool(t, (232, 236, 236)),
    "wool_red": lambda t: paint_wool(t, (164, 46, 42)),
    "wool_green": lambda t: paint_wool(t, (64, 134, 54)),
    "wool_blue": lambda t: paint_wool(t, (46, 74, 164)),
    "wool_yellow": lambda t: paint_wool(t, (222, 196, 62)),
    "wool_black": lambda t: paint_wool(t, (26, 26, 26)),
    "flower_poppy": lambda t: paint_crossflower(t, (196, 46, 40), (252, 214, 84), (64, 116, 48)),
    "flower_dandelion": lambda t: paint_crossflower(t, (244, 214, 66), (252, 240, 160), (72, 124, 52)),
    "tall_grass": paint_tall_grass,
    "dead_bush": paint_dead_bush,
    "cactus_side": lambda t: paint_cactus(t),
    "cactus_top": lambda t: paint_cactus(t, top=True),
    "melon_side": lambda t: paint_melon(t),
    "melon_top": lambda t: paint_melon(t, top=True),
    "pumpkin_side": lambda t: paint_pumpkin(t),
    "pumpkin_top": lambda t: paint_pumpkin(t, top=True),
    "mud": paint_mud,
    "slab_stone": paint_slab,
    "torch": paint_torch,
}
for _stage in range(10):
    PAINTERS["destroy_stage_%d" % _stage] = (
        lambda t, s=_stage: paint_destroy(t, s))


def build_tile(name, size, seed):
    tile = Tile(name, size, seed)
    painter = PAINTERS.get(name)
    if painter is None:
        sys.exit("error: tile %r has no painter" % name)
    tile.clear()
    painter(tile)
    # Only the declared cut-out/liquid tiles keep holes: a stray transparent pixel on a solid
    # block would show the inside of the chunk through its face.
    if name not in ALPHA:
        for i in range(size * size):
            r, g, b, a = tile.px[i]
            tile.px[i] = (r, g, b, 255)
    return tile


HEADER_BANNER = """// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

// GENERATED by scripts/generate_voxel_atlas.py - do not edit by hand.
// tile %d px, grid %d x %d, atlas edge %d px.

#pragma once

namespace quantum {

"""


def write_header(path, tile_px, grid, manifest):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(HEADER_BANNER % (tile_px, grid, grid, tile_px * grid))
        handle.write("// Atlas slot per tile. The order is the atlas layout: append at the end and\n"
                     "// never renumber, because a saved world stores block ids, not names.\n"
                     "enum VoxelTile {\n")
        for entry in manifest:
            handle.write("    TILE_%s = %d,\n" % (entry["name"].upper(), entry["index"]))
        handle.write("    TILE_COUNT = %d,\n" % len(manifest))
        handle.write("};\n\n")
        handle.write("struct VoxelTileRect {\n    float u0, v0, u1, v1;\n};\n\n")
        handle.write("// One tile of the atlas as the corner UVs of its quad. The inset is half a texel:\n"
                     "// sampling exactly on the border is what makes the neighbour tile bleed into\n"
                     "// a face. quantum loads images unflipped and divides pixel-Y-from-the-top straight\n"
                     "// into v (MeshSystem::normalizeTileRect), so v = 0 is the atlas's first row: v0 is\n"
                     "// the tile's TOP edge and v1 its bottom edge, and a face must map its own up to v0.\n"
                     "inline VoxelTileRect voxelTileRect(int tile, float inset = 0.5f) {\n")
        handle.write("    const float uv = 1.0f / %d.0f;\n" % grid)
        handle.write("    const float per = uv / %d.0f;\n" % tile_px)
        handle.write("    const float col = (float)(tile %% %d);\n" % grid)
        handle.write("    const float row = (float)(tile / %d);\n" % grid)
        handle.write("    VoxelTileRect r;\n")
        handle.write("    r.u0 = col * uv + per * inset;\n")
        handle.write("    r.v0 = row * uv + per * inset;\n")
        handle.write("    r.u1 = (col + 1.0f) * uv - per * inset;\n")
        handle.write("    r.v1 = (row + 1.0f) * uv - per * inset;\n")
        handle.write("    return r;\n}\n\n")
        handle.write("inline bool voxelTileIsCutout(int tile) {\n    switch (tile) {\n")
        for entry in manifest:
            if entry["cutout"]:
                handle.write("        case TILE_%s: return true;\n" % entry["name"].upper())
        handle.write("        default: return false;\n    }\n}\n\n")
        handle.write("inline bool voxelTileIsLiquid(int tile) {\n    switch (tile) {\n")
        for entry in manifest:
            if entry["liquid"]:
                handle.write("        case TILE_%s: return true;\n" % entry["name"].upper())
        handle.write("        default: return false;\n    }\n}\n\n")
        handle.write("inline const char* voxelTileName(int tile) {\n"
                     "    static const char* names[%d] = {\n" % len(manifest))
        for entry in manifest:
            handle.write('        "%s",\n' % entry["name"])
        handle.write("    };\n"
                     "    return (tile >= 0 && tile < %d) ? names[tile] : \"missing\";\n}\n\n"
                     % len(manifest))
        handle.write("} // namespace quantum\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", default=os.path.join("templates", "voxelworld-main"),
                        help="project folder that owns the atlas")
    parser.add_argument("--tile", type=int, default=16, help="edge of one tile in pixels")
    parser.add_argument("--grid", type=int, default=0,
                        help="tiles per row (default: the smallest power of two that holds them)")
    parser.add_argument("--seed", type=int, default=20261101, help="noise seed for the painters")
    args = parser.parse_args()

    grid = args.grid or 1
    while grid * grid < SLOTS:
        grid *= 2

    atlas_w = grid * args.tile
    image = Image.new("RGBA", (atlas_w, atlas_w), (0, 0, 0, 0))
    manifest = []
    for index, name in enumerate(TILES):
        tile = build_tile(name, args.tile, (args.seed + index * 7919) & 0xFFFFFFFF)
        col, row = index % grid, index // grid
        for y in range(args.tile):
            for x in range(args.tile):
                image.putpixel((col * args.tile + x, row * args.tile + y), tile.get(x, y))
        manifest.append({"name": name, "index": index, "col": col, "row": row,
                         "cutout": name in CUTOUT, "liquid": name in LIQUID})

    tex_dir = os.path.join(args.project, "textures", "voxel")
    os.makedirs(tex_dir, exist_ok=True)
    png = os.path.join(tex_dir, "atlas.png")
    image.save(png)

    # The HUD backing: a flat white square the widgets multiply by their own colour, so a
    # translucent rect costs one texture instead of a shipped GUI atlas.
    panel = os.path.join(tex_dir, "panel.png")
    Image.new("RGBA", (4, 4), (255, 255, 255, 255)).save(panel)

    txt = os.path.join(tex_dir, "atlas.txt")
    with open(txt, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# name index col row cutout liquid  (tile %dpx, grid %dx%d)\n"
                     % (args.tile, grid, grid))
        for entry in manifest:
            handle.write("%s %d %d %d %d %d\n" % (entry["name"], entry["index"], entry["col"],
                                                  entry["row"], int(entry["cutout"]),
                                                  int(entry["liquid"])))

    scripts_dir = os.path.join(args.project, "scripts")
    os.makedirs(scripts_dir, exist_ok=True)
    header = os.path.join(scripts_dir, "VoxelTiles.h")
    write_header(header, args.tile, grid, manifest)

    holes = sum(1 for i in range(atlas_w * atlas_w) if image.getpixel((i % atlas_w, i // atlas_w))[3] < 255)
    print("atlas:    %s  %dx%d  tile %dpx  grid %dx%d  %d slots  %d transparent px"
          % (png, atlas_w, atlas_w, args.tile, grid, grid, SLOTS, holes))
    print("manifest: %s" % txt)
    print("panel:    %s" % panel)
    print("header:   %s" % header)


if __name__ == "__main__":
    main()
