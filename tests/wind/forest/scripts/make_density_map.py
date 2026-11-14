# (c) SR VINIX
# SPDX-License-Identifier: MIT

# Paints the foliage density map from the heightmap the terrain actually wears.
#
# The scatter samples this map and the renderer samples that map, so they have to be the same bytes
# from the same run: a density map generated independently puts grass in the sea and on the cliffs.
# Run `terrain generate` first (see lua/main.lua for the exact command), then this.

import numpy as np
from PIL import Image
import pathlib

HEIGHTMAP = pathlib.Path(__file__).resolve().parents[1] / "assets/textures/heightmap_forest.png"
DENSITY = pathlib.Path(__file__).resolve().parents[1] / "assets/textures/foliage_density.png"

TERRAIN_SIZE = 900.0
MAX_HEIGHT = 48.0
WATER_LINE = 0.340          # printed by `terrain generate` for this preset and seed
SHORE_RAISE = 0.6           # metres above the water line before grass starts
MAX_SLOPE_DEGREES = 28.0

def load16(path):
    image = Image.open(path)
    if image.mode not in ("I;16", "I"):
        raise SystemExit("expected a 16-bit grayscale heightmap, got " + image.mode)
    data = np.array(image, dtype=np.uint32)
    return data.astype(np.float32) / 65535.0

def value_noise(size, cell, seed, octaves=4):
    # cheap fbm over a coarse lattice; the same job quantum::Noise does in the engine, kept here
    # because this file runs offline and the map it writes is authored data, not runtime state
    rng = np.random.default_rng(seed)
    total = np.zeros((size, size), dtype=np.float32)
    amplitude = 1.0
    weight = 0.0
    for octave in range(octaves):
        lattice_cell = max(2, cell >> octave)
        gw = max(2, size // lattice_cell)
        lattice = rng.random((gw + 1, gw + 1)).astype(np.float32)
        y = np.linspace(0.0, gw - 1.0, size, dtype=np.float32)
        x = np.linspace(0.0, gw - 1.0, size, dtype=np.float32)
        iy, ix = np.floor(y).astype(np.int32), np.floor(x).astype(np.int32)
        fy, fx = (y - iy)[:, None], (x - ix)[None, :]
        smooth = lambda t: t * t * (3.0 - 2.0 * t)
        sy, sx = smooth(fy), smooth(fx)
        a = lattice[np.ix_(iy, ix)]
        b = lattice[np.ix_(iy + 1, ix)]
        c = lattice[np.ix_(iy, ix + 1)]
        d = lattice[np.ix_(iy + 1, ix + 1)]
        top = a + (b - a) * sy
        bottom = c + (d - c) * sy
        total += amplitude * (top + (bottom - top) * sx)
        weight += amplitude
        amplitude *= 0.5
    return total / weight

def main():
    normalised = load16(HEIGHTMAP)
    size = normalised.shape[0]
    heights = normalised * MAX_HEIGHT
    step = TERRAIN_SIZE / size

    # central differences in metres, so the slope is a real angle and not a texel ratio
    dhx = (np.roll(normalised, -1, axis=1) - np.roll(normalised, 1, axis=1)) * 0.5 * MAX_HEIGHT / step
    dhz = (np.roll(normalised, -1, axis=0) - np.roll(normalised, 1, axis=0)) * 0.5 * MAX_HEIGHT / step
    slope = np.degrees(np.arctan(np.sqrt(dhx * dhx + dhz * dhz)))

    shore = np.clip((heights - (WATER_LINE * MAX_HEIGHT + SHORE_RAISE)) / 1.5, 0.0, 1.0)
    flat = np.clip((MAX_SLOPE_DEGREES - slope) / 8.0, 0.0, 1.0)
    patches = value_noise(size, 32, 20261106)

    # a meadow, not a lawn: clumps with bare ground between them, and nothing under the water
    cover = shore * flat * (0.25 + 0.75 * np.clip((patches - 0.28) / 0.52, 0.0, 1.0))
    image = Image.fromarray((cover * 255.0).astype(np.uint8), mode="L")
    DENSITY.parent.mkdir(parents=True, exist_ok=True)
    image.save(DENSITY)

    print("density map: %dx%d, mean cover %.3f, fully painted %.1f%%, bare %.1f%%" % (
        size, size, cover.mean(), (cover > 0.999).mean() * 100.0, (cover < 0.001).mean() * 100.0))
    print("wrote " + str(DENSITY))

if __name__ == "__main__":
    main()
