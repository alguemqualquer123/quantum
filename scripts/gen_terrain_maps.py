#!/usr/bin/env python3
# (c) SR VINIX and contributors
# SPDX-License-Identifier: MIT

"""Emit the height map and the blend map of a cliff-and-basin profile, as the two PNGs a
quantum Terrain reads.

Both maps come out of ONE height function on purpose: WaterSystem::bakeFloor samples the
height map CPU-side to build the water bed, so the relief the water sees and the relief the
shader displaces have to be the same pixels. Writing them from separate descriptions is how
a stream ends up running downhill into a wall.

The water bed is the terrain surface, and the terrain surface is
    height = maxHeight * value / 255
(sampleTerrainSurface, engine/core/subsystem/MeshSystem.cpp:3455-3464). So the profile is
authored in METRES and normalised by --max-height at the end; a value of 0 is y=0, never
"below sea".

Height map: 8-bit grayscale, one sample per texel. It stays 8-bit because that is the format
the working kit map ships (engine/components/assets/textures/heightmap_hills.png) and because
8 bit over 16 m is a 6 cm step - below the water cell size of any resolution the sim allows.

Blend map: RGB, 8 bit. The shader reads the three channels as the weights of the three detail
layers and gives everything they do not claim to the base colour
(1 - (r+g+b), includes/terrain_fs.glsl:146-165), so a texel whose channels sum over 1.0
over-brightens. This script normalises every texel to --max-weight.

    python scripts/gen_terrain_maps.py templates/waterfall/assets/terrain \
        --size 256 --terrain-size 90 --max-height 16
"""

import argparse
import math
import os
import struct
import zlib

try:
    from PIL import Image
except ImportError:  # stdlib fallback: a PNG is a zlib stream plus a CRC, nothing more
    Image = None


# --------------------------------------------------------------------------- profile ----
# Every function returns metres of elevation for a terrain-local (x, z) inside
# [-half, +half]. The scene reads north as -z: the plateau is up there, the cliff faces
# south, and the valley mouth lets the lake drain off the bottom of the field.


def ridge(x, z, p):
    """Back wall of the plateau: a high, broken crest so rain collects on the shelf
    instead of leaking off the north border. It meets the shelf exactly at shelf_high."""
    t = (z - p["ridge_start"]) / (-(p["half"]) - p["ridge_start"])
    base = p["shelf_high"] + p["ridge_rise"] * min(max(t, 0.0), 1.0)
    return base + p["noise"](x, z, 5.0, 0.6)


def shelf(x, z, p):
    """The sloped top the water runs down. Grade is constant so the stream has one speed,
    and a shallow channel carved toward x = 0 gathers it at the lip instead of letting it
    spread over the whole cliff edge."""
    t = (p["ridge_start"] - z) / (p["ridge_start"] - p["lip_z"])
    base = p["shelf_high"] + (p["lip_top"] - p["shelf_high"]) * t
    # The channel deepens as it approaches the lip, which is what makes a fall rather than a
    # wide sheet: 0 at the back of the shelf, 1 at the lip. It stays under the noise the whole
    # way down, or the stream breaks into puddles in the pits and reads as a wet rock.
    carve = (0.85 + 0.15 * t) * channel_carve(x, p)
    # A bump inside the chute dams the whole stream a metre above the lip: the grade is only
    # 0.13 m per metre, so one 3 m noise cell can exceed it. The roughness is therefore faded
    # out where the channel is cut and kept everywhere else, which is also what a scoured rock
    # channel actually looks like.
    rough = p["noise"](x, z, 3.2, 0.22) * (1.0 - 0.92 * min(carve / p["channel"], 1.0))
    return base - carve + rough


def channel_carve(x, p):
    return p["channel"] * math.exp(-((x / p["channel_width"]) ** 2))


def cliff(x, z, p):
    """The lip face. The profile is concave (`t ** 0.62`), not a smoothstep, on purpose: a
    smoothstep leaves the crest flat, so the rock barely drops in the first metre and a sheet of
    water falling from the lip goes *into* the face instead of off it. With the exponent the
    surface plunges immediately, which is both what a plunge pool cuts and what lets the fall
    stay clear of the rock. The foot is close in z so the face is near-vertical rather than a
    45 degree ramp: water running down a ramp is the shelf, water falling needs a cliff."""
    t = (z - p["lip_z"]) / (p["cliff_foot_z"] - p["lip_z"])
    s = min(max(t, 0.0), 1.0) ** 0.62
    bulge = math.sin(math.pi * t) * p["plunge"]
    gully = channel_carve(x, p) * (1.0 - min(t / 0.55, 1.0))
    return p["lip_top"] + (p["cliff_foot"] - p["lip_top"]) * s - bulge - gully \
        + p["noise"](x, z, 2.4, 0.5)


def basin(x, z, p):
    """Elliptical bowl around the impact, and a valley that falls away to the south so the
    lake reaches a sill and then drains out of the field instead of flooding the shelf. It
    starts at the cliff foot height and blends into the bowl, so the fall lands on a ramp.

    The rim is what decides where the lake stops: it rises on both sides of the outlet
    corridor but not along it, so the water level is the bowl's own rim height and the trench
    carries the surplus away. A rim that closed over the corridor would dam the lake a metre
    above the sill and drown the cliff foot."""
    dx = x / p["bowl_w"]
    dz = (z - p["bowl_z"]) / p["bowl_d"]
    r = math.hypot(dx, dz)
    floor = p["bowl_floor"] + p["bowl_rise"] * min(r, 1.0) ** 1.7
    # the outlet: a trench along x = 0 south of the bowl, cut down to the field border
    mouth = math.exp(-((x / p["mouth_w"]) ** 2)) * min(max((z - p["mouth_z"]) /
                (p["half"] - p["mouth_z"]), 0.0), 1.0)
    if r > 1.0:
        floor += (r - 1.0) * p["rim_rise"] * (1.0 - math.exp(-((x / (p["mouth_w"] * 1.6)) ** 2)))
    floor -= mouth * p["mouth_cut"]
    t = min(max((z - p["cliff_foot_z"]) / p["foot_blend"], 0.0), 1.0)
    blended = p["cliff_foot"] + (floor - p["cliff_foot"]) * (t * t * (3.0 - 2.0 * t))
    return blended + p["noise"](x, z, 4.0, 0.45)


def profile(x, z, p):
    if z <= p["ridge_start"]:
        return ridge(x, z, p)
    if z <= p["lip_z"]:
        return shelf(x, z, p)
    if z <= p["cliff_foot_z"]:
        return cliff(x, z, p)
    return basin(x, z, p)


# ----------------------------------------------------------------------------- noise ----

def _hash(ix, iz, seed):
    """ xorshift-style integer mix: the same (x, z) always gives the same relief, which is
    what makes a regenerated map reviewable in a diff rather than a new accident."""
    h = (ix * 374761393 + iz * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) & 0xFFFFFFFF
    h = (h * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFFFFFF) / 4294967295.0


def make_noise(seed):
    def value_noise(x, z, cell, amplitude):
        gx, gz = x / cell + 0.5, z / cell + 0.5
        ix, iz = math.floor(gx), math.floor(gz)
        fx, fz = gx - ix, gz - iz
        sx, sz = fx * fx * (3.0 - 2.0 * fx), fz * fz * (3.0 - 2.0 * fz)
        a = _hash(ix, iz, seed)
        b = _hash(ix + 1, iz, seed)
        c = _hash(ix, iz + 1, seed)
        d = _hash(ix + 1, iz + 1, seed)
        top, bottom = a + (b - a) * sx, c + (d - c) * sx
        return (top + (bottom - top) * sz - 0.5) * 2.0 * amplitude

    return value_noise


# -------------------------------------------------------------------------- blending ----

def blend_weights(h, slope, p):
    """(rock, grass, mud) as a fraction of the texel. Slope is |dh/dn| in metres per metre,
    so the bands follow the relief rather than a fixed height: a steep patch is rock, the
    ground the water can reach is mud, and grass takes exactly what the other two leave.
    That last one is the point: when each layer is an independent ramp, every texel where all
    three come out low is painted with the terrain's own base colour, and a base colour of flat
    grey is what made the cliff look patchy. Rock and mud are the claims, grass is the remainder."""
    rock = _ramp(slope, 0.28, 0.75)
    mud = _ramp(p["mud_line"] - h, 0.0, 1.4) * (1.0 - _ramp(slope, 0.20, 0.50))
    grass = (1.0 - rock) * (1.0 - mud)
    return rock, grass, mud


def _ramp(v, lo, hi):
    if hi == lo:
        return 1.0 if v >= lo else 0.0
    return min(max((v - lo) / (hi - lo), 0.0), 1.0)


# ---------------------------------------------------------------------------- output ----

def write_png(path, width, height, channels, rows):
    """channels is 1 (grayscale) or 3 (RGB). Written by hand so the script has no
    dependency it could be missing on a fresh checkout; PIL is only used to check the
    IHDR of the file it just made."""
    depth = 0 if channels == 1 else 2  # colour type 0 = gray, 2 = RGB

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = bytearray()
    stride = width * channels
    for y in range(height):
        raw.append(0)  # filter type 0 per scanline
        raw.extend(rows[y * stride:(y + 1) * stride])

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, depth, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as handle:
        handle.write(png)
    return len(png)


def build(params, cells):
    heights = [[0.0] * cells for _ in range(cells)]
    for row in range(cells):
        z = (row + 0.5) / cells * params["terrain_size"] - params["half"]
        for col in range(cells):
            x = (col + 0.5) / cells * params["terrain_size"] - params["half"]
            heights[row][col] = max(min(profile(x, z, params), params["max_height"]), 0.0)
    return heights


def slope_at(heights, params, col, row, cells):
    """|grad h| in metres per metre, sampled with the same texel-centre convention the
    engine's CPU sampler uses."""
    step = params["terrain_size"] / cells
    xm, xp = max(col - 1, 0), min(col + 1, cells - 1)
    zm, zp = max(row - 1, 0), min(row + 1, cells - 1)
    dx = (heights[row][xp] - heights[row][xm]) / ((xp - xm) * step)
    dz = (heights[zp][col] - heights[zm][col]) / ((zp - zm) * step)
    return math.hypot(dx, dz)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out_dir", help="folder the two PNGs are written to")
    ap.add_argument("--size", type=int, default=256, help="map edge in texels")
    ap.add_argument("--terrain-size", type=float, default=90.0, help="Terrain size in metres")
    ap.add_argument("--max-height", type=float, default=16.0, help="Terrain maxHeight in metres")
    ap.add_argument("--seed", type=int, default=20261031)
    ap.add_argument("--max-weight", type=float, default=0.92,
                    help="cap on r+g+b per blend texel; the rest goes to the base colour")
    ap.add_argument("--height-name", default="heightmap_cliff.png")
    ap.add_argument("--blend-name", default="blendmap_cliff.png")
    args = ap.parse_args()

    half = args.terrain_size * 0.5
    params = {
        "half": half, "terrain_size": args.terrain_size, "max_height": args.max_height,
        "noise": make_noise(args.seed),
        # the authored profile, in metres, north (-z) to south (+z)
        "ridge_start": -half + 6.0,   # the shelf runs from here down to the lip
        "ridge_rise": args.max_height - 15.0 - 0.3,
        "shelf_high": 15.0,
        "lip_z": -10.0,
        "lip_top": 11.6,
        # The face is 5 m of z for 9 m of drop: steep enough that a drop leaving the lip at
        # walking pace stays ahead of the rock all the way down. At the old 9 m the face was a
        # 45 degree ramp and the whole sheet fell inside the cliff.
        "cliff_foot_z": -5.0,
        "cliff_foot": 0.9,
        "foot_blend": 5.0,
        "channel": 1.15,
        "channel_width": 6.5,
        "plunge": 0.55,
        "bowl_z": 4.0, "bowl_w": 22.0, "bowl_d": 14.0,
        # The bowl rim height IS the lake level: the pool spills through the outlet corridor and
        # cannot rise above the lowest point of its own basin. 2.35 m of rise over a 0.15 m floor
        # puts the surface just under the cliff foot so the fall lands on rock, not in a flood.
        "bowl_floor": 0.15, "bowl_rise": 2.35, "rim_rise": 3.4,
        "mouth_z": 24.0, "mouth_w": 7.0, "mouth_cut": 6.6,
        "mud_line": 2.3,
    }

    os.makedirs(args.out_dir, exist_ok=True)
    cells = args.size
    heights = build(params, cells)

    hrows = bytearray(cells * cells)
    for row in range(cells):
        for col in range(cells):
            hrows[row * cells + col] = int(round(heights[row][col] / args.max_height * 255.0))
    height_path = os.path.join(args.out_dir, args.height_name)
    hsize = write_png(height_path, cells, cells, 1, hrows)

    brows = bytearray(cells * cells * 3)
    for row in range(cells):
        for col in range(cells):
            h = heights[row][col]
            slope = slope_at(heights, params, col, row, cells)
            rock, grass, mud = blend_weights(h, slope, params)
            total = rock + grass + mud
            if total > args.max_weight:
                scale = args.max_weight / total
                rock, grass, mud = rock * scale, grass * scale, mud * scale
            i = (row * cells + col) * 3
            brows[i], brows[i + 1], brows[i + 2] = (int(round(rock * 255)),
                                                    int(round(grass * 255)), int(round(mud * 255)))
    blend_path = os.path.join(args.out_dir, args.blend_name)
    bsize = write_png(blend_path, cells, cells, 3, brows)

    # ---- the proof: the profile the water will be baked from
    print(f"{height_path} ({hsize} bytes), {blend_path} ({bsize} bytes)")
    print(f"terrain size {args.terrain_size} m, maxHeight {args.max_height} m, "
          f"1 texel = {args.max_height / 255.0:.3f} m")
    lo = min(min(r) for r in heights)
    hi = max(max(r) for r in heights)
    print(f"elevation range {lo:.2f} .. {hi:.2f} m")

    centre = cells // 2
    print("\nnorth-south section through x = 0 (metres):")
    for row in range(0, cells, max(cells // 30, 1)):
        z = (row + 0.5) / cells * args.terrain_size - half
        print(f"  z = {z:7.1f}  h = {heights[row][centre]:6.2f}")

    def row_of(z):
        return int((z + half) / args.terrain_size * cells)

    lip_row, foot_row, bowl_row = row_of(params["lip_z"]), row_of(params["cliff_foot_z"]), \
        row_of(params["bowl_z"])
    print(f"\nlip at z={params['lip_z']:.1f} -> texel row {lip_row}, h={heights[lip_row][centre]:.2f} m")
    print(f"cliff foot at z={params['cliff_foot_z']:.1f} -> texel row {foot_row}, "
          f"h={heights[foot_row][centre]:.2f} m")
    print(f"bowl centre at z={params['bowl_z']:.1f} -> texel row {bowl_row}, "
          f"h={heights[bowl_row][centre]:.2f} m")
    drop = heights[lip_row][centre] - heights[bowl_row][centre]
    print(f"the fall is {drop:.2f} m")

    # ---- the property the visible column depends on: a drop leaving the lip must clear the rock
    # for its whole flight. This is the check that was never made when the face was a 45 degree
    # ramp, and the reason the sheet rendered as a wet stain *inside* the cliff.
    #
    # A ballistic drop gets steeper as it accelerates, and a heightfield face can only get gentler
    # going down, so the crest has to throw the water far enough that it passes the foot before it
    # passes the surface. The 0.4 m head start is the emitter offset Waterfall.lua applies.
    def surface(z):
        col = centre
        row = min(max(int((z + half) / args.terrain_size * cells), 0), cells - 1)
        return heights[row][col]

    def clearance(v):
        y, zz, vy, t = heights[lip_row][centre], params["lip_z"] + 0.4, 1.4, 0.0
        worst, land = 1e9, zz
        while y > params["cliff_foot"] and t < 4.0:
            dt = 0.02
            vy += 9.81 * dt
            y -= vy * dt
            zz += v * dt
            t += dt
            worst = min(worst, y - surface(zz))
            land = zz
        return worst, land

    for v in (0.0, 2.0, 4.0, 5.0, 6.0, 8.0):
        worst, land = clearance(v)
        print(f"a sheet leaving the lip at {v:.1f} m/s outward keeps {worst:6.2f} m of clearance "
              f"and reaches the foot at z = {land:.1f}")

    # ---- the two properties the water depends on, checked rather than assumed
    shoulder = row_of(-25.0)
    off = int((18.0 + half) / args.terrain_size * cells)
    notch, side = heights[lip_row][centre], heights[lip_row][off]
    print(f"\nchannel gathers: at the lip the bed is {notch:.2f} m and 18 m to the east it is "
          f"{side:.2f} m -> {side - notch:.2f} m of wall funnels the stream")
    print(f"shelf grade: {heights[shoulder][centre]:.2f} m at z=-25 down to {notch:.2f} m at "
          f"z={params['lip_z']:.1f} -> runs {(heights[shoulder][centre] - notch) / 15.0:.3f} m per m")
    mouth_row = row_of(params["half"] - 1.0)
    # The lake cannot rise above the high point of the path out: that is the sill it spills at.
    divide = max((heights[r][centre] for r in range(row_of(params["bowl_z"]), cells - 1)),
                 default=0.0)
    print(f"outlet: {heights[mouth_row][centre]:.2f} m at the south border against "
          f"{heights[row_of(30.0)][off]:.2f} m 18 m east of it -> the lake drains through the trench")
    print(f"the lake settles at {divide:.2f} m, the height of the divide along x = 0; the pool "
          f"bed under the fall is {heights[bowl_row][centre]:.2f} m")

    wet = sum(1 for r in range(cells) for c in range(cells)
              if heights[r][c] < params["mud_line"])
    print(f"blend: {wet * 100.0 / (cells * cells):.1f}% of the map is below the mud line, "
          f"{hi - lo:.1f} m of relief over {args.terrain_size:.0f} m")
    if Image is not None:
        for path in (height_path, blend_path):
            with Image.open(path) as im:
                print(f"read back {os.path.basename(path)}: mode {im.mode}, size {im.size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
