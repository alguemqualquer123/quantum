---
name: quantum-terrain
description: Quantum terrain and elevation - CDLOD TerrainComponent, the heightmap vertex-displacement shaders, morph/crack fixes, blend-map texture splatting, terrain shader variant bits, physics heightfield, the instanced foliage chunks, and the editor heightmap generator. Use when working on terrain elevation, heightmaps, painting, LOD cracks/seams, foliage density or why a grassy terrain stutters, terrain textures, or reusing terrain for a large water plane.
---

# Terrain and elevation

Terrain is one of the few genuinely complete advanced features in the engine: it is
**GPU-displaced CDLOD** (instanced patch nodes + morphing), not a static mesh.

## Component

`engine/core/component/TerrainComponent.h:59` - `views[MAX_TERRAIN_VIEWS]` per-view
node selection (`MAX_TERRAIN_NODES = 2000000` at `:19`), `heightMap`, `blendMap`,
`textureDetailRed/Green/Blue`, `autoSetRanges`, `offset`, `ranges`, `nodes` (a
`std::vector` deliberately, per the comment at `:79`: a fixed array blows the stack in
`chkstk.asm`), `grid[MAX_TERRAINGRID]`, `rootGridSize = 2`, `levels = 6`, and the
`u_vs_terrainParams` mirror block (`eyePos`, `terrainSize = 200`, `maxHeight = 5`,
`resolution = 32`, `textureBaseTiles`, `textureDetailTiles`).

> The comment at the uniform block is a hard constraint: **that member order is the
> byte layout uploaded to the GPU.** Reordering it corrupts terrain silently rather
> than failing to compile. Same rule for every `u_vs_*`/`u_fs_*` block in the engine.

Wrapper object `engine/core/object/Terrain.cpp` (`addComponent<TerrainComponent>()`
`:13`, then per-field setters); Lua class `Terrain` derives from `Mesh`
(`engine/core/script/binding/ObjectClassesLua.cpp:525`); height texture upload
`RenderSystem::loadTerrainHeightTexture` (`engine/core/subsystem/RenderSystem.cpp:2672`). A
16-bit grayscale PNG loads as `ColorFormat::RED16` (`TextureData` + `stbi_is_16_bit_from_memory`)
and uploads as `SG_PIXELFORMAT_R16` (`engine/renders/sokol/SokolTexture.cpp:357`); an 8-bit one
reads as `RED`, which is why the generator writes 16-bit.

## Shaders

`engine/shaderlib/includes/terrain_vs.glsl`:
- `getHeight()` samples `u_heightMap.r * terrain.maxHeight`, UV =
  `(pos.xz + size/2)/size` - the heightmap is a world-space projected lookup, so
  elevation edits are just texture writes.
- `getTerrainPosition()` must run **before** `getTerrainNormal()` (it sets
  `morphFactor`), and re-samples height after morphing.
- `morphVertex()` is the crack fix: only odd vertices move, so the grid index is
  recovered with `floor((gridPos + 0.5) * gridDim + 0.5)` - scaling `gridPos` directly
  loses parity in float32. Do not "simplify" that line.
- `getTerrainNormal()` recomputes the normal from three height taps (needs
  `HAS_NORMALS`); the delta scales with `morphFactor` to match the morphed grid.
- `getTerrainTiledTexture()` (guarded `#ifndef DEPTH_SHADER`) produces the base/
  detail UVs.

`engine/shaderlib/includes/terrain_fs.glsl` - `getTerrainColor()`: blend-map
`rgb` = the three detail layers, and `1 - (r+g+b)` is the base layer weight. That is
the splatting convention a paint tool must respect (a blend map that sums over 1.0
over-brightens).

Include sites, all inside `#ifdef HAS_TERRAIN`: `mesh.vert:87`, `depth.vert:28`
(guards `:27`, `:36`), `gbuffer.vert:47` (with an `#ifndef` at `:38`) and fragment
side `mesh.frag:209`.

## Variant bits

`HAS_TERRAIN` is **not the same bit number per shader type**: mesh bit 17
(`editor/shader/ShaderBuilder.cpp:464`), depth bit 5 (`:481`), gbuffer bit 6 (`:493`).
The 3-letter code is 'Ter' and it appears in the `.sdat` cache filename. Always check
the per-type table before adding a bit, and never reuse a bit index across tables.

## Physics

Heightfield collision: `Shape3D` source `ENTITY_HEIGHTFIELD`
(`engine/core/component/Body3DComponent.h:28-34`) built by
`PhysicsSystem::createShape3DForIndex` (`PhysicsSystem.cpp:1259`, heightfield branch
`:1430-1501`): samples span `terrainSize` edge to edge with vertical scale `maxHeight`
(`:1456-1458`), `samplesSize: 0` becomes `getMinNearestPowerOfTwo()` of the loaded heightmap
(`:1449`), and it is refused until `heightMapLoaded` is true (`:1441`) - `isShapeSourceLoading`
(`:1522`) retries per fixed step while the texture lands.

Keep the physics grid and the rendered `resolution` in the same convention or collision will
visibly disagree with the surface. The collider's **entity origin is the bottom** of the elevation
range: a terrain at `y = -5` with `maxHeight 5` has ground anywhere in `[-5, 0]`, so gameplay code
must sample the surface (`Ray:intersects(scene, RayFilter.BODY_3D)`, `quantum-physics`) instead of
reading the transform height.

## Generation (engine-side since FASE 2)

`engine/core/util/TerrainGenerator.{h,cpp}` writes the heightmap a `TerrainComponent` reads -
16-bit single-channel PNG, the same encoding the editor brush uses (`lround(clamp01(h) * 65535)`,
little-endian), so a generated map can be painted over afterwards.

- `TerrainPreset` (`TerrainGenerator.h:21`) is `continents | island | mountains | plains | canyons`;
  a preset is a parameter set, not a different algorithm. `TerrainGenParams` (`:31`) is plain data,
  so a CLI flag list, a Lua table, a plugin schema row and a scene key all fill the same struct.
- `generate` (`TerrainGenerator.cpp:181`) per texel: optional `Noise::warp2`, `fbm`, optional
  `Noise::ridged` at `seed ^ RELIEF_SEED` (`:208`) mixed by `ridgedAmount`, `pow(reliefCurve)`,
  `base + field * amplitude`, sub-sea damping by `SUBSEA_DAMPING` (`:220`), island border mask
  (`:226`), then hydraulic + thermal erosion. Deterministic and non-throwing (`_HAS_EXCEPTIONS=0`).
- `clampedSize` (`:80`) is the size you actually get (8..4096); `encode16` (`:237`),
  `generatePng` (`:246`) and `waterHeight` = `clamp01(seaLevel) * maxHeight` (`:256`) are the other
  four entry points. Place a lake with `waterHeight`, do not guess it.
- Erosion lives in `engine/core/util/TerrainErosion.{h,cpp}` (`hydraulic` / `thermal`, a
  `FalloffFunc` brush mask, `TerrainMapRegion` from `util/TerrainMapRegion.h`) and the encoder in
  `util/PngWriter.{h,cpp}` (`writeGray16Png`, hand-rolled over `stbi_zlib_compress`). All three
  moved out of `editor/util/` so the engine owns one implementation; the editor windows and
  `editor/cli/TerrainCommands.cpp` consume them.

Entry points: `quantum-editor-cmd terrain presets|generate|info MAP.png`
(`editor/cli/TerrainCommands.cpp:336`, dispatched from `editor/main.cpp:81`, no project and no
window needed), and Lua `WorldGen.generate/presets/params/waterHeight`
(`engine/core/script/binding/UtilClassesLua.cpp:269`, raw C functions because the option table needs
the VM). `Noise.ridged/ridged3/warpedFbm/warpedFbm3` are bound next to `fbm` - see
`quantum-procedural-noise`.

Harness: `tests/worldcreator/probe` (project entry `lua/main.lua` builds the scene in Lua - camera
before terrain, `Engine.scene = Scene()` - runs 48 checks and is captured with
`quantum-engine.exe --project tests/worldcreator/probe --screenshot PATH --capture-frame 140`). It
proves CLI bytes == Lua bytes for one seed, and that `Terrain:setHeightMap` + `createTerrain`
still builds a terrain whose heightmap was replaced while live.

Full API reference (params, presets table, heightmap contract, CLI flags, Lua option keys, and what
is explicitly not supported): `WORLD_CREATOR_TERRAIN.md`.

## Foliage

Foliage is **not** one entity per plant. Each layer is a grid of cells
(`TERRAIN_FOLIAGE_CELL_SIZE = 8.0f`) grouped into chunks whose side is
`TERRAIN_FOLIAGE_TARGET_BATCH_RADIUS = 2` cells scaled up to reach the layer's
`drawDistance`, and each chunk is one entity carrying
`Transform + MeshComponent + ModelComponent + InstancedMeshComponent` with
`mergeStaticMeshes = true` and `maxInstances = capacity`
(`MeshSystem::createFoliageEntity` `engine/core/subsystem/MeshSystem.cpp:3391-3405`,
constants `:3333-3337`). Capacity per chunk is `density * cell * cell` clamped to
`TERRAIN_FOLIAGE_MAX_CHUNK_INSTANCES = 8192` (`:3361-3362`), so a lawn of 10k blades is a
few chunk entities and one instanced draw each, not 10k entities - a scene that
"has millions of foliage" is not a scene with millions of entities.

Two things govern how fast a painted area appears: the chunk-mesh load budget, which is a
**wall-clock** budget of 4 ms a frame, re-armed at the top of `MeshSystem::update` and read as
`meshLoadBudget` (`MeshSystem.cpp:3768-3769`, deadline `:3360-3366`; it used to be two loads a
frame, and two grass tufts of 129k triangles in one frame measured a 222 ms hitch), and the
dirty-rect refill - painting sets `TerrainComponent::foliageDirtyLayer` plus a
`TerrainMapRegion` of density texels (`TerrainComponent.h:189-230`) and the refill maps
that rect to a cell range so chunks outside it keep their instances (`MeshSystem.cpp:3740-3766`).
A layer with no readable density pixels falls back to a full refill. Foliage `drawDistance`
is clamped by the scene `renderDistance` (`:3662-3670`) and fades from
`TERRAIN_FOLIAGE_FADE_START = 0.75` of it.

LOD is what makes this run at 60 fps. Measured on the fps-fps terrain with 7 foliage
layers (`tests/scale/fpsprobe`, Release, 400 frames): grass only 22.3 fps with
`meshLodEnabled = false` against 101.9 fps with it true, and grass plus 10,122 props
21.5 fps against 47.9 fps - triangles 141.7M against 22.3M at the same ~5,060 draw calls,
because LOD here drops index buffers, not batches. The default is on
(`SceneSettings::meshLodEnabled = true`, `engine/core/SceneSettings.h:61`) but it does not
migrate old scenes: `editor/Stream.cpp:3037-3038` writes the value always and reads it
whenever the key exists, so a `.scene` saved while the default was false keeps LOD off
until the setting is touched. `templates/voxelworld-main/Main Scene.scene` and both
`templates/charactercontrol-main/*.scene` still carry `meshLodEnabled: false`.

## Editor / tooling

- Inspector: float rows such as `Properties.cpp:7846` `terrainSize`
  (`propertyRow(RowPropertyType::FloatPositive, ...)`).
- The older **editor-side** generator is still there:
  `editor/ai/AiTerrainCapability.cpp` - `makeHeightPixels` `:40` (modes `random_noise` /
  `fractal_noise`, octaves clamped 1-8, `encodeHeight16` `:31`), its own `TerrainNoise::value`,
  output path from `Project::getTerrainMapsDir()`. It now shares `util/PngWriter.h` with the
  engine but not the field: promoting it onto `TerrainGenerator` is an open follow-up.
- `editor/window/TerrainEditWindow.cpp` paints and erodes through the engine headers above.

## Perf notes

Per-frame terrain cost is dominated by node selection and the instance buffer
(`needUpdateNodesBuffer`), not by the vertex shader. Culling uses
`RenderSystem::isInsideCamera` (`RenderSystem.h:675`), and raising `levels` or
`resolution` multiplies vertex load across every view.
