#!/usr/bin/env python3
# (c) SR VINIX and contributors
# SPDX-License-Identifier: MIT

"""Paint the voxel project's humanoid skins: a 64x64 map in the layout the model's UVs read.

Nothing here is sampled from a shipped game. Every texel is painted from the palette below with a
seeded per-pixel jitter, in the visual language of a 64x64 humanoid skin (hair on the crown, a face
on the front, a shirt on the torso, sleeves ending in hands, trousers ending in boots). Mojang's
textures are their copyright, so the project owns this file as its source of truth and regenerates
the PNG from it - and a skin somebody already owns can be dropped in instead, because the map areas
below are the ones that skin puts its pixels in.

    python scripts/generate_voxel_skins.py [--project <dir>] [--seed N] [--name default]

Writes, into templates/voxelworld-main by default:
  textures/skins/default.png   64x64 RGBA, the six body parts' map areas painted opaque

face_rect() is a transcription of voxelModelFaceRect() in scripts/VoxelModel.cpp: same packing rule
(top and bottom lying on their sides in a row of depth d, then right/front/left/back in a strip of
height h), same part origins. If that table moves, this one moves with it - and check_areas() below
re-reads every PNG against those same areas before anything is written, so a mismatch fails the run.
"""

import argparse
import hashlib
import os
import sys

from PIL import Image

SKIN_PX = 64

# part -> (mapU, mapV, w, h, d) in texels. The sizes are the vanilla ones (8x8x8 head, 8x12x4 body,
# 4x12x4 limbs); the origins are where a 64x64 skin puts them.
PARTS = {
    "head": (0, 0, 8, 8, 8),
    "body": (16, 16, 8, 12, 4),
    "arm_r": (40, 16, 4, 12, 4),
    "arm_l": (32, 48, 4, 12, 4),
    "leg_r": (0, 16, 4, 12, 4),
    "leg_l": (16, 48, 4, 12, 4),
}

TOP, BOTTOM, RIGHT, FRONT, LEFT, BACK = range(6)

HAIR = (0x3a, 0x2b, 0x1e)
HAIR_DARK = (0x2b, 0x1f, 0x15)
SKIN = (0xc9, 0x9a, 0x6a)
SKIN_DARK = (0xa9, 0x7d, 0x53)
EYE_WHITE = (0xef, 0xef, 0xf2)
PUPIL = (0x3b, 0x3f, 0x6b)
MOUTH = (0x6b, 0x4a, 0x33)
SHIRT = (0x00, 0xa3, 0xa3)
SHIRT_DARK = (0x00, 0x82, 0x82)
PANTS = (0x3f, 0x4c, 0x8f)
PANTS_DARK = (0x32, 0x3c, 0x72)
BOOTS = (0x4a, 0x4a, 0x52)
BELT = (0x2a, 0x2a, 0x33)
# The areas this rig never samples: a real 64x64 skin keeps the hat/jacket overlay in the band above
# and to the right of the head. Left transparent so a viewer knows it is spare, not missing.
UNUSED = (0x00, 0x00, 0x00, 0x00)


def face_rect(part, face):
    """The map area of one face of one box, mirroring the C++ packer."""
    u, v, w, h, d = PARTS[part]
    if face == TOP:
        x0, y0, x1, y1 = u + d, v, u + d + w, v + d
    elif face == BOTTOM:
        x0, y0, x1, y1 = u + d + w, v, u + d + w + w, v + d
    elif face == RIGHT:
        x0, y0, x1, y1 = u, v + d, u + d, v + d + h
    elif face == FRONT:
        x0, y0, x1, y1 = u + d, v + d, u + d + w, v + d + h
    elif face == LEFT:
        x0, y0, x1, y1 = u + d + w, v + d, u + d + w + d, v + d + h
    else:
        x0, y0, x1, y1 = u + d + w + d, v + d, u + d + w + d + w, v + d + h
    return x0, y0, x1, y1


def jitter(seed, x, y, amount):
    """A deterministic per-texel brightness nudge: the flat colours are the palette, the noise is
    what keeps an 8x8 face from reading as a sticker."""
    digest = hashlib.sha1(("%d:%d:%d" % (seed, x, y)).encode()).digest()
    return int(digest[0]) % (2 * amount + 1) - amount


def shade(rgb, delta):
    return tuple(max(0, min(255, c + delta)) for c in rgb)


def paint_head(pixels, x0, y0, face):
    w, h = face_rect("head", face)[2] - x0, face_rect("head", face)[3] - y0
    for py in range(h):
        for px in range(w):
            if face == FRONT:
                rgb = SKIN if py > 1 else HAIR          # the fringe across the top two rows
                col = px
                if py in (3, 4) and col in (1, 2, 5, 6):
                    rgb = EYE_WHITE
                if py in (3, 4) and col in (2, 5):
                    rgb = PUPIL
                if py == 5 and col in (3, 4):
                    rgb = SKIN_DARK                     # the nose
                if py == 6 and 2 <= col <= 5:
                    rgb = MOUTH
            elif face in (TOP, BACK):
                rgb = HAIR
            elif face == BOTTOM:
                rgb = SKIN_DARK
            else:
                rgb = HAIR if py < 4 else SKIN          # hair over the ear, skin below
            pixels[x0 + px, y0 + py] = (*shade(rgb, (px + py) % 2 * -6), 255)


def paint_body(pixels, x0, y0, face):
    w = face_rect("body", face)[2] - x0
    h = face_rect("body", face)[3] - y0
    for py in range(h):
        for px in range(w):
            if py >= h - 2:
                rgb = BELT                              # the last two rows are the waistband
            elif py == 0 and face == FRONT:
                rgb = SKIN_DARK                         # the neckline
            else:
                rgb = SHIRT if face in (FRONT, TOP, BOTTOM) else SHIRT_DARK
            pixels[x0 + px, y0 + py] = (*shade(rgb, (px * 3 + py) % 3 * -5), 255)


def paint_limb(pixels, x0, y0, part, face):
    h = face_rect(part, face)[3] - y0
    cloth, cloth_dark, bare, bare_dark = (
        (SHIRT, SHIRT_DARK, SKIN, SKIN_DARK) if part.startswith("arm")
        else (PANTS, PANTS_DARK, BOOTS, BOOTS))
    hem = 4 if part.startswith("arm") else 4            # the last four rows are hand or boot
    for py in range(h):
        for px in range(face_rect(part, face)[2] - x0):
            if py >= h - hem:
                rgb = bare if face in (FRONT, BOTTOM, TOP) else bare_dark
            else:
                rgb = cloth if face in (FRONT, TOP, BOTTOM) else cloth_dark
            pixels[x0 + px, y0 + py] = (*shade(rgb, (px + py * 2) % 3 * -4), 255)


def make_skin(seed):
    img = Image.new("RGBA", (SKIN_PX, SKIN_PX), UNUSED)
    pixels = img.load()
    for part in PARTS:
        for face in range(6):
            x0, y0, x1, y1 = face_rect(part, face)
            if part == "head":
                paint_head(pixels, x0, y0, face)
            elif part == "body":
                paint_body(pixels, x0, y0, face)
            else:
                paint_limb(pixels, x0, y0, part, face)
    # Everything the rig does not sample stays transparent, so an accidental UV that lands outside
    # the six areas shows up as a hole in the preview instead of as borrowed pixels.
    for y in range(SKIN_PX):
        for x in range(SKIN_PX):
            r, g, b, a = pixels[x, y]
            if a == 0:
                continue
            d = jitter(seed, x, y, 7)
            pixels[x, y] = (*shade((r, g, b), d), 255)
    return img


def check_areas(img):
    """Every texel of the six parts' map areas is painted and opaque; nothing else is. This is the
    one promise a skin has to keep for the rig: a hole in an area is a hole in the body."""
    covered = set()
    for part in PARTS:
        for face in range(6):
            x0, y0, x1, y1 = face_rect(part, face)
            if x1 > SKIN_PX or y1 > SKIN_PX:
                return "%s face %d runs off the map at (%d,%d)" % (part, face, x1, y1)
            for y in range(y0, y1):
                for x in range(x0, x1):
                    covered.add((x, y))
                    if img.getpixel((x, y))[3] != 255:
                        return "%s face %d has an unpainted texel at (%d,%d)" % (part, face, x, y)
    for y in range(SKIN_PX):
        for x in range(SKIN_PX):
            if (x, y) not in covered and img.getpixel((x, y))[3] != 0:
                return "texel (%d,%d) is outside the six areas but painted" % (x, y)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--project", default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "templates", "voxelworld-main"))
    ap.add_argument("--name", default="default")
    ap.add_argument("--seed", type=int, default=20261101)
    args = ap.parse_args()

    img = make_skin(args.seed)
    problem = check_areas(img)
    if problem:
        print("generate_voxel_skins: %s" % problem, file=sys.stderr)
        return 1

    out_dir = os.path.join(args.project, "textures", "skins")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "%s.png" % args.name)
    img.save(out)
    print("generate_voxel_skins: wrote %s (%dx%d, %d areas painted, seed %d)" %
          (out, SKIN_PX, SKIN_PX, len(PARTS) * 6, args.seed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
