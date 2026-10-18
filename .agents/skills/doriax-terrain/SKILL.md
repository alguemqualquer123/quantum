---
name: doriax-terrain
description: Doriax terrain and elevation - CDLOD TerrainComponent, the heightmap vertex-displacement shaders, morph/crack fixes, blend-map texture splatting, terrain shader variant bits, physics heightfield, and the editor heightmap generator. Use when working on terrain elevation, heightmaps, painting, LOD cracks/seams, terrain textures, or reusing terrain for a large water plane.
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
`RenderSystem::loadTerrainHeightTexture` (`engine/core/subsystem/RenderSystem.cpp:2052`).

## Shaders

`engine/tools/shaderlib/includes/terrain_vs.glsl`:
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

`engine/tools/shaderlib/includes/terrain_fs.glsl` - `getTerrainColor()`: blend-map
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
(`engine/core/component/Body3DComponent.h:28-34`) built in
`PhysicsSystem.cpp:1005`. Keep the physics grid and the rendered `resolution` in the
same convention or collision will visibly disagree with the surface.

## Editor / tooling

- Inspector: float rows such as `Properties.cpp:7846` `terrainSize`
  (`propertyRow(RowPropertyType::FloatPositive, ...)`).
- Procedural heightmap generation is **editor-side only**:
  `editor/ai/AiTerrainCapability.cpp` - `fade` `:42`, `hashNoise` `:43`, `valueNoise`
  `:57`, `makeHeightPixels` `:70` (modes `random_noise` / `fractal_noise`, octaves
  clamped 1-8, height written as **16-bit** via `encodeHeight16`), output path from
  `Project::getTerrainMapsDir()` (`makeTerrainHeightmapPath` `:106`).
  There is no Perlin/simplex in the repo - see `doriax-procedural-noise`.

## Perf notes

Per-frame terrain cost is dominated by node selection and the instance buffer
(`needUpdateNodesBuffer`), not by the vertex shader. Culling uses
`RenderSystem::isInsideCamera` (`RenderSystem.h:675`), and raising `levels` or
`resolution` multiplies vertex load across every view.
