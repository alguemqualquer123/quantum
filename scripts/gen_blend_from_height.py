#!/usr/bin/env python3
# (c) SR VINIX and contributors
# SPDX-License-Identifier: MIT

"""Derive a Terrain blend map from the height map it is painted on.

quantum splits the terrain surface into three detail layers weighted by the blend map's channels and
gives the remainder to the base colour (1 - (r+g+b), engine/shaderlib/includes/terrain_fs.glsl).
Authoring that map by hand means guessing where the slopes are; the slopes are already in the height
map, so this reads one PNG and writes the other.

The assignment is the one a coastline makes: rock where the ground is steep, mud in the band the
water can reach, grass on everything gentle between them. Channels follow the order the component
uses - red, green and blue are textureDetailRed, textureDetailGreen and textureDetailBlue.

Every texel is normalised to --max-weight so the three channels never sum above it, because a texel
over 1.0 does not clamp, it over-brightens.

    python scripts/gen_blend_from_height.py <heightmap.png> <blendmap.png> \
        --terrain-size 900 --max-height 7 --water-level 1.8
"""

import argparse
import sys

from PIL import Image


def load_heights(path):
    """Return (pixels, width, height) with each sample already in metres."""
    image = Image.open(path).convert("L")
    return image.load(), image.width, image.height


def slope_at(pixels, width, height, x, y, step):
    """Central difference in metres per metre.

    The height map is a projected lookup over the whole terrain, so one texel is
    terrain-size / width metres of ground; without that divisor the gradient is in texels and the
    rock layer appears on any relief at all.
    """
    xm, xM = max(x - 1, 0), min(x + 1, width - 1)
    ym, yM = max(y - 1, 0), min(y + 1, height - 1)
    dx = (pixels[xM, y] - pixels[xm, y]) / (xM - xm) * step
    dy = (pixels[x, yM] - pixels[x, ym]) / (yM - ym) * step
    return (dx * dx + dy * dy) ** 0.5


def blend_for(elevation, gradient, water_level, rock_band, mud_band):
    """Weights of the three layers, before normalisation.

    Each band is a smoothstep rather than a switch: a hard threshold draws a contour line around
    the hill at exactly the height where the layer changes.
    """
    steep = smooth(gradient / rock_band)
    wet = smooth(1.0 - (elevation - water_level) / mud_band)
    green = max(0.0, 1.0 - steep - wet)
    return steep, green, wet


def smooth(t):
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    return t * t * (3.0 - 2.0 * t)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("heightmap")
    ap.add_argument("blendmap")
    ap.add_argument("--terrain-size", type=float, default=900.0)
    ap.add_argument("--max-height", type=float, default=7.0)
    ap.add_argument("--water-level", type=float, default=1.8)
    ap.add_argument("--rock-band", type=float, default=0.85, help="gradient that counts as full rock")
    ap.add_argument("--mud-band", type=float, default=2.5, help="metres above water still read as shore")
    ap.add_argument("--max-weight", type=float, default=0.92)
    args = ap.parse_args()

    pixels, width, height = load_heights(args.heightmap)
    step = args.terrain_size / width
    out = Image.new("RGB", (width, height))
    write = out.load()

    for y in range(height):
        for x in range(width):
            elevation = pixels[x, y] / 255.0 * args.max_height
            gradient = slope_at(pixels, width, height, x, y, step)
            r, g, b = blend_for(elevation, gradient, args.water_level, args.rock_band, args.mud_band)
            total = r + g + b
            if total > 1e-6:
                scale = min(1.0, args.max_weight / total)
                r, g, b = r * scale, g * scale, b * scale
            write[x, y] = (int(r * 255.0), int(g * 255.0), int(b * 255.0))

    out.save(args.blendmap)
    print("blend map: {}x{} from {} ({} m/texel, water {} m)".format(
        width, height, args.heightmap, round(step, 3), args.water_level))
    return 0


if __name__ == "__main__":
    sys.exit(main())
