# (c) SR VINIX
# SPDX-License-Identifier: MIT

# Builds a throwaway project that shows the authored terrain with foliage at the million-plant scale,
# using only binaries that already exist.
#
# Why this route: the runtime does not read YAML and the shipped dll has no Lua setter for the scatter,
# so a script-built scene cannot place a single plant. But `quantum-editor-cmd benchmark` decodes a
# .scene through the editor's Stream and captures a PNG of the real frame - the one headless door open
# without a rebuild. The scene is *derived* from templates/fps-fps rather than written from scratch:
# a terrain entity is only half TerrainComponent (the CDLOD patch lives in a MeshComponent submesh with
# its own vertexCount and AABBs), and the authored scene already has both halves plus the glTF models
# the foliage chunks read.
#
# What the patch does, and why each part is needed:
#   - the scene has `mainCamera: 0`, and MeshSystem::updateFoliageLayer returns without placing a single
#     instance when the scene has no render camera, so a camera entity is added and named in mainCamera.
#   - the authored layer's density map is the editor's paint, which is mostly zero; the scatter
#     probability is the map's own value per texel (accept if hash < map), so the map is repainted from
#     the same heightmap the terrain wears - grass cannot grow in the lake because the waterline gates it.
#   - a chunk is 8 m and holds ceil(density * 64) attempts capped at 8192 instances, so `density: 50000`
#     saturates every cell and the count is then set by the map's mean and by the draw distance ring.
#   - the mesh is the sprig variant and the density is a triangle budget, not the authored 50000: the
#     instance *count* is what saturating buys, but 850k x 24.078 triangles is not a frame (measured
#     ~3 s/frame with the full tuft). The printed "satur." line is the million; "planted" is drawable.
#
#   python tests/wind/forest/scripts/make_live_project.py
#   build/Debug/quantum-editor-cmd.exe benchmark --project <dir printed above> \
#       --scene "teste de terreno.scene" --warmup 400 --frames 60 --screenshot /tmp/foliage_scene.png

import pathlib
import shutil
import subprocess

import numpy as np
from PIL import Image

REPO = pathlib.Path(__file__).resolve().parents[4]
SOURCE_PROJECT = REPO / "templates/fps-fps"
SCENE_NAME = "teste de terreno.scene"
OUT = pathlib.Path("C:/Users/Admin/AppData/Local/Temp/foliage_live")

# the authored terrain, read from the scene: 179.5 m of ground rising 5 m, based 2.3175433 m down, so
# the y=0 water plane covers every texel under 0.4635 of the heightmap.
TERRAIN_SIZE = 179.5
MAX_HEIGHT = 5.0
WORLD_BASE_Y = -2.3175433
WATERLINE = -WORLD_BASE_Y / MAX_HEIGHT
HEIGHTMAP = "terrain_maps/terrain_edit_4233178914_1003_height_2.png"
AUTHORED_DENSITY = "terrain_maps/terrain_edit_4233178914_1003_density_3.png"
# the layer's own maxSlope is 35 degrees; the map fades grass out before that, on the crests
SLOPE_CUTOFF = 28.0
MAP_SIZE = 512
CELL = 8.0
# 60 m of ring is 15 cells wide per axis: 225 cells x 8192 attempts, so a mean cover of ~0.55 is the
# million. The authored 50 was one cell narrower for no reason the count needed.
DRAW_DISTANCE = 60.0
CAMERA_ID = 2000
# The asset the scene scatters, and the cost the choice is really made of: meshSystem merges all
# three submeshes, so one instance of the full tuft is 24.078 triangles and of the sprig 26.
AUTHORED_MESH = "TerrainTeste/Foliages/Separate/BuffaloGrass.glb"
FOLIAGE_MESH = "TerrainTeste/Foliages/Separate/Grass_Sprigs.glb"
TRIANGLES_PER_INSTANCE = 96
# density is plants per square metre; the scatter multiplies by the 64 m2 cell and caps at 8192,
# so 128 is where raising it stops planting anything.
DENSITY = 12.0
MAX_DENSITY = 128.0
MAX_CHUNK_INSTANCES = 8192


def scatter_law(density, draw_distance, terrain_size):
    """Mirror of MeshSystem::updateFoliageLayer (:3663-3692) for one saturated-or-not layer.

    Returns what the engine will really build: attempts per 8 m cell, how many cells a chunk entity
    packs, the chunk grid the camera sees, and the instance ceiling. Density above 128/m2 changes
    nothing but the chunk shape, because the attempts clamp forces maxBatchSide down to 1.
    """
    attempts = max(1, int(np.ceil(np.clip(density * CELL * CELL, 0.0, MAX_CHUNK_INSTANCES))))
    max_batch_side = int(np.sqrt(MAX_CHUNK_INSTANCES / attempts))
    batch_side = int(np.clip(np.ceil(draw_distance / (CELL * 2.0)), 1, max_batch_side))
    chunk_size = CELL * batch_side
    placement_radius = max(1, int(np.ceil(terrain_size * 0.5 / CELL)))
    terrain_radius = max(1, int(np.ceil(placement_radius / batch_side)))
    # The window is the square [-D, +D] around the camera, clamped to the terrain, and a partial
    # cell at each edge still counts: floor((p+D)/cs) - floor((p-D)/cs) + 1, worst case.
    side = min(int(np.floor(draw_distance * 2 / chunk_size)) + 2, 2 * terrain_radius)
    chunks = side * side
    capacity = attempts * batch_side * batch_side
    return dict(attempts=attempts, batch_side=batch_side, chunk_size=chunk_size,
                chunks=chunks, capacity=capacity)


def print_scatter(label, law, mean_cover):
    plants = law["chunks"] * law["capacity"] * mean_cover
    print("%s chunks: %d x %d m   attempts/chunk: %d   plants: %.0f   %.1f Mtris" % (
        label, law["chunks"], law["chunk_size"], law["capacity"], plants,
        plants * TRIANGLES_PER_INSTANCE / 1e6))


def junction(name):
    dst = OUT / name
    if dst.exists():
        return
    result = subprocess.run(["cmd", "/c", "mklink", "/J", str(dst), str(SOURCE_PROJECT / name)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"junction {name} failed: {result.stdout}{result.stderr}")


def bilinear(lattice, size):
    """Value noise on a coarse lattice, stretched to `size` - the same bilinear + smoothstep the
    engine's own CPU noise uses, so the patches have soft edges instead of cell blocks."""
    grid = lattice.shape[0]
    coords = np.linspace(0.0, grid - 1.0, size, dtype=np.float32)
    index = np.clip(np.floor(coords).astype(np.int32), 0, grid - 2)
    frac = coords - index
    smooth = frac * frac * (3.0 - 2.0 * frac)
    low = (lattice[np.ix_(index, index)] * (1 - smooth)[:, None] +
           lattice[np.ix_(index + 1, index)] * smooth[:, None])
    high = (lattice[np.ix_(index, index + 1)] * (1 - smooth)[:, None] +
            lattice[np.ix_(index + 1, index + 1)] * smooth[:, None])
    return low * (1 - smooth)[None, :] + high * smooth[None, :]


def paint_density_map():
    height = np.array(Image.open(SOURCE_PROJECT / HEIGHTMAP), dtype=np.float32) / 65535.0
    height = np.asarray(Image.fromarray(height).resize((MAP_SIZE, MAP_SIZE), Image.BILINEAR),
                        dtype=np.float32)
    step = TERRAIN_SIZE / MAP_SIZE
    dhx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5 * MAX_HEIGHT / step
    dhz = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5 * MAX_HEIGHT / step
    slope = np.degrees(np.arctan(np.sqrt(dhx * dhx + dhz * dhz)))

    # a shore is not lawn: the gate ramps in over the 0.3 m above the water the wave spray reaches
    land = np.clip((height - WATERLINE) / 0.06, 0.0, 1.0)
    gentle = np.clip((SLOPE_CUTOFF - slope) / 8.0, 0.0, 1.0)

    rng = np.random.default_rng(20261106)
    meadow = bilinear(rng.random((MAP_SIZE // 32 + 1, MAP_SIZE // 32 + 1)).astype(np.float32), MAP_SIZE)
    tufts = bilinear(rng.random((MAP_SIZE // 8 + 1, MAP_SIZE // 8 + 1)).astype(np.float32), MAP_SIZE)
    patch = np.clip(0.35 + 0.65 * meadow, 0.0, 1.0) * np.clip(tufts * 1.8, 0.0, 1.0)

    cover = land * gentle * (0.25 + 0.75 * patch)
    (OUT / "density").mkdir(parents=True, exist_ok=True)
    Image.fromarray((cover * 255.0).astype(np.uint8), mode="L").save(OUT / "density/foliage_density.png")
    return float(cover.mean()), float(land.mean()), height


def camera_pose(height):
    """Stand on the driest, flattest ground and look 35 m along the direction that keeps the whole
    line above water - the authored lake sits in the middle of this map, so a camera aimed by hand at
    the origin ends up photographing only water."""
    step = TERRAIN_SIZE / MAP_SIZE
    dhx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5 * MAX_HEIGHT / step
    dhz = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5 * MAX_HEIGHT / step
    slope = np.degrees(np.arctan(np.sqrt(dhx * dhx + dhz * dhz)))

    samples = np.linspace(MAP_SIZE * 0.2, MAP_SIZE * 0.8, 25).astype(int)
    angles = np.arange(0, 360, 15)
    reach = int(16.0 / step)

    best = None
    for iz in samples:
        for ix in samples:
            if height[iz, ix] < WATERLINE + 0.12 or slope[iz, ix] > 12.0:
                continue
            for angle in angles:
                dz = int(round(np.sin(np.radians(angle)) * reach))
                dx = int(round(np.cos(np.radians(angle)) * reach))
                fz, fx = iz + dz, ix + dx
                if not (0 <= fz < MAP_SIZE and 0 <= fx < MAP_SIZE):
                    continue
                # the whole line of sight has to be dry, or the photo is a lake again
                path = np.linspace(0, 1, 24)
                zi = np.clip((iz + dz * path).astype(int), 0, MAP_SIZE - 1)
                xi = np.clip((ix + dx * path).astype(int), 0, MAP_SIZE - 1)
                dryness = float(height[zi, xi].min()) - WATERLINE
                if dryness < 0.10:
                    continue
                score = dryness + (height[iz, ix] - WATERLINE) * 0.3
                if best is None or score > best[0]:
                    best = (score, ix, iz, fx, fz)
    if best is None:
        raise SystemExit("no dry line of sight in the authored heightmap")

    _, ix, iz, fx, fz = best
    def world(x, y):
        return ((x + 0.5) / MAP_SIZE - 0.5) * TERRAIN_SIZE, ((y + 0.5) / MAP_SIZE - 0.5) * TERRAIN_SIZE
    cx, cz = world(ix, iz)
    tx, tz = world(fx, fz)
    # a 30 cm tuft only reads from a metre up; the earlier 2 m eye made the field look bare
    eye_y = (WORLD_BASE_Y + MAX_HEIGHT * float(height[iz, ix])) + 0.7
    aim_y = (WORLD_BASE_Y + MAX_HEIGHT * float(height[fz, fx])) + 0.1
    return (cx, eye_y, cz), (tx, aim_y, tz)


def camera_entity(pose, aim):
    """The camera shape main.scene ships, placed by camera_pose and aimed at the land it found."""
    return f"""  - type: Entity
    entity: {CAMERA_ID}
    name: Camera
    components:
      transform:
        position: [{pose[0]:g}, {pose[1]:g}, {pose[2]:g}]
        rotation: [1, 0, 0, 0]
        scale: [1, 1, 1]
        worldPosition: [{pose[0]:g}, {pose[1]:g}, {pose[2]:g}]
        worldRotation: [1, 0, 0, 0]
        worldScale: [1, 1, 1]
        localMatrix: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [{pose[0]:g}, {pose[1]:g}, {pose[2]:g}, 1]]
        modelMatrix: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [{pose[0]:g}, {pose[1]:g}, {pose[2]:g}, 1]]
        visible: true
        billboardRotation: [1, 0, 0, 0]
        billboard: false
        fakeBillboard: false
        cylindricalBillboard: false
      camera:
        type: camera_perspective
        target: [{aim[0]:g}, {aim[1]:g}, {aim[2]:g}]
        up: [0, 1, 0]
        yfov: 0.9
        aspect: 1.7777778
        nearClip: 0.1
        farClip: 400
        renderToTexture: false
        transparentSort: false
        depthTest: true
        useTarget: true
        autoResize: true
        framebufferWidth: 1280
        framebufferHeight: 720
        framebufferFilter: linear
"""



def main():
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)

    shutil.copy(SOURCE_PROJECT / "project.yaml", OUT / "project.yaml")
    for name in ("TerrainTeste", "terrain_maps", "materials", "Model", "Components"):
        junction(name)

    mean_cover, land_fraction, height = paint_density_map()
    pose, aim = camera_pose(height)

    (OUT / "scenes").mkdir(parents=True, exist_ok=True)
    scene = (SOURCE_PROJECT / "scenes" / SCENE_NAME).read_text(encoding="utf-8")
    scene = scene.replace("path: " + AUTHORED_DENSITY, "path: density/foliage_density.png")
    scene = scene.replace("mainCamera: 0", f"mainCamera: {CAMERA_ID}")
    scene = scene.replace("drawDistance: 50", f"drawDistance: {DRAW_DISTANCE:g}")
    scene = scene.replace("entities:\n  - type: Entity\n    entity: 1001",
                          "entities:\n" + camera_entity(pose, aim) + "  - type: Entity\n    entity: 1001")

    # The authored layer 0 scatters BuffaloGrass.glb, which is 24.078 triangles per instance: the
    # million plants are a real instance count but not a drawable frame (measured ~3 s/frame, and
    # that was before shadowing them). The sprig variant of the same plant is 26 triangles, which is
    # what a field of this size actually ships, and DENSITY is then the triangle budget / 26.
    at = scene.index("meshPath: " + AUTHORED_MESH)
    end = scene.index("          - meshPath:", at + 10)
    layer0 = scene[at:end]
    layer0 = layer0.replace(AUTHORED_MESH, FOLIAGE_MESH)
    layer0 = layer0.replace("density: 50000", f"density: {DENSITY:g}")
    # 289 chunks x hundreds of thousands of instances into the directional shadow map costs more
    # than the color pass; the picture wants the field, not the grass shadow.
    layer0 = layer0.replace("castShadows: true", "castShadows: false")
    scene = scene[:at] + layer0 + scene[end:]

    for needle in ("density/foliage_density.png", f"entity: {CAMERA_ID}",
                   f"mainCamera: {CAMERA_ID}", f"drawDistance: {DRAW_DISTANCE:g}",
                   f"meshPath: {FOLIAGE_MESH}", f"density: {DENSITY:g}",
                   "castShadows: false"):
        if needle not in scene:
            raise SystemExit(f"the patch did not land ({needle}) - the authored scene changed shape")
    (OUT / "scenes" / SCENE_NAME).write_text(scene, encoding="utf-8")

    law = scatter_law(DENSITY, DRAW_DISTANCE, TERRAIN_SIZE)
    saturated = scatter_law(MAX_DENSITY, DRAW_DISTANCE, TERRAIN_SIZE)
    print("project: " + OUT.as_posix())
    print("scene: " + SCENE_NAME)
    print("density map mean cover: %.3f  (land above water: %.3f)" % (mean_cover, land_fraction))
    print("%s  %d tris/instance" % (FOLIAGE_MESH.rsplit("/", 1)[1], TRIANGLES_PER_INSTANCE))
    print_scatter("planted ", law, mean_cover)
    print_scatter("satur. ", saturated, mean_cover)
    print("camera at [%.2f, %.2f, %.2f] looking at [%.2f, %.2f, %.2f]" % (*pose, *aim))


if __name__ == "__main__":
    main()
