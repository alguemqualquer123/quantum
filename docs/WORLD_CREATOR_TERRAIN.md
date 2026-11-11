# Quantum procedural terrain

A heightmap is an image, and the engine already knows how to read one: `TerrainComponent` uploads it
and the vertex shader lifts each texel by `u_heightMap.r * terrain.maxHeight`. What FASE 2 added is the
other half — a generator that runs inside the engine, so a Lua script, a plugin, a CLI verb and the
editor brush all produce the same bytes for the same seed. Nothing here needs ImGui, the editor or a
downloaded asset.

Source: [engine/core/util/TerrainGenerator.h](engine/core/util/TerrainGenerator.h) ·
noise: [engine/core/util/Noise.h](engine/core/util/Noise.h) ·
Lua binding: [engine/core/script/binding/UtilClassesLua.cpp](engine/core/script/binding/UtilClassesLua.cpp) ·
CLI: [editor/cli/TerrainCommands.cpp](editor/cli/TerrainCommands.cpp) ·
acceptance probe: [tests/worldcreator/probe](tests/worldcreator/probe) ·
decisions: [WORLD_CREATOR_PLAN.md](WORLD_CREATOR_PLAN.md)

## 1. The format a terrain reads

| Step | What happens | Where |
|---|---|---|
| write | `[0,1]` height → `lround(h * 65535)`, little-endian pairs | `TerrainGenerator::encode16` |
| file | 16-bit single-channel PNG, adaptive row filtering, DEFLATE | `writeGray16Png`, [engine/core/util/PngWriter.h](engine/core/util/PngWriter.h) |
| load | `stbi_is_16_bit_from_memory` → `ColorFormat::RED16` | `TextureData::loadTexture` |
| upload | `RED16` → `SG_PIXELFORMAT_R16` | [engine/renders/sokol/SokolTexture.cpp](engine/renders/sokol/SokolTexture.cpp) |
| shade | `u_heightMap.r * terrain.maxHeight` above the entity plane | [engine/shaderlib/includes/terrain_vs.glsl](engine/shaderlib/includes/terrain_vs.glsl) |

Two consequences a caller owns, not the loader:

- **The size you ask for is not always the size you get.** `clampedSize` maps the request into
  `[MIN_SIZE 8, MAX_SIZE 4096]`; anything that writes a texture or a PNG must use the returned side.
- **`resolution` is the CDLOD grid, not the image.** Keep it in the same convention as the physics
  heightfield (`samplesSize: 0` takes `getMinNearestPowerOfTwo()` of the loaded map), or collision
  visibly disagrees with the surface. See `quantum-terrain`.

The quantization matches the editor brush, so a generated map and a painted one are stored
alike and a hand-edit of a generated map survives a re-read.

## 2. `quantum::Noise`

Pure functions of `(coordinates, seed)`: no global state, no allocation, nothing that throws, so a
pooled generation task and the main thread can sample the same field. Hashing is integer-only — that
is why a seed baked into a scene returns the same map on MSVC and MinGW. Coordinates are lattice units,
one cell per unit: sample at world position divided by a scale, never at a pixel index.

| Family | Functions | Range |
|---|---|---|
| bases | `perlin2/3`, `simplex2/3`, `value2/3` | `[-1, 1]` |
| white | `hash2/3` | `[0, 1]`, discontinuous by design |
| dispatch | `sample`, `sample3` (by `NoiseType`) | the base's range |
| fractal | `fbm`/`fbm3`, `turbulence`/`turbulence3` | `[-1,1]` / `[0,1]` |
| ridged | `ridged`/`ridged3` | `[0, 1]`, sharp crests where the field crosses zero |
| warped | `warp2`/`warp3` (in-place coordinate displacement), `warpedFbm`/`warpedFbm3` | `[-1, 1]` |

`MAX_OCTAVES = 16` is a hard clamp, and `octaves`, `gain` and `lacunarity` are clamped inside every
fractal call: these run in per-pixel loops fed by inspector rows, so a bad number must cost detail,
not the frame. The warp displacement is always a 3-octave simplex sum read from seeds derived from
`seed`, whatever the caller samples afterwards — a grid bias in the warp field prints a grid bias into
the result.

## 3. `TerrainGenerator`

`TerrainGenParams` is plain data, which is what lets one struct be filled by a schema row, a scene key,
a CLI argument list or a Lua table.

| Field | Default | Meaning |
|---|---|---|
| `seed` / `erosionSeed` | 1 / 1 | two separate fields: re-rolling the map seed must not be the only way to move the rivers |
| `size` | 256 | square side, clamped |
| `type`, `octaves`, `lacunarity`, `gain` | SIMPLEX, 6, 2.0, 0.5 | the fractal sum |
| `frequency` | 0.012 | lattice units per texel, so one value works at any size |
| `baseHeight`, `amplitude` | 0.12, 0.72 | where the field's zero lands and how tall it reaches |
| `ridgedAmount` | 0 | mix between the smooth sum and the ridged one (a second lattice, `seed ^ RELIEF_SEED`) |
| `warpAmplitude`, `warpFrequency` | 0, 0.4 | 0 skips domain warping entirely |
| `reliefCurve` | 1.0 | power on the normalized field: >1 flattens valleys, <1 flattens peaks |
| `seaLevel` | 0.28 | normalized; below it the bed is **damped** to 25% of its range, not walled off |
| `islandFalloff` | 0 | pulls the border to the map floor (not to sea level — a shelf at the water line reads as a swimming pool) |
| `erosionDroplets`, `erosionHeightScale` | 0, 100 | 0 skips the hydraulic pass |
| `thermalTalus`, `thermalRate` | 0, 0.5 | 0 skips the thermal pass |

Per texel, in this order ([TerrainGenerator.cpp:196-234](engine/core/util/TerrainGenerator.cpp)):
warp the coordinates → fbm → mix in the ridged relief → `pow` by the relief curve → scale by
`baseHeight + field * amplitude` → damp what fell under sea level → apply the border mask → clamp.
Then `applyErosion` runs hydraulic and thermal over the whole map.

| Method | Contract |
|---|---|
| `clampedSize(size)` | the side a request actually produces |
| `preset(TerrainPreset)` | a parameter set, not a different algorithm |
| `presetFromName` / `presetName` | names are what a manifest, a CLI argument and a schema dropdown carry; an unknown name reads as `CONTINENTS` |
| `generate(params, heights)` | `size*size` values in `[0,1]`, row-major from the map's north edge |
| `encode16(heights, pixels)` | the same field as 16-bit little-endian samples |
| `generatePng(params, path)` | generate + encode + write; the parent directory must already exist |
| `waterHeight(params, maxHeight)` | the world `y` a water plane needs, i.e. `seaLevel * maxHeight` |

The five presets and what distinguishes them:

| Preset | Noise | octaves | freq | ridged | warp | sea | droplets |
|---|---|---|---|---|---|---|---|
| `continents` | simplex | 6 | 0.008 | 0.20 | 0.8 | 0.30 | 4000 |
| `island` | simplex | 6 | 0.010 | 0.35 | 1.2 | 0.34 | 6000 (+falloff 1.0) |
| `mountains` | simplex | 7 | 0.016 | 0.85 | 0.6 | 0.18 | 8000 |
| `plains` | perlin | 4 | 0.010 | 0 | 0 | 0.16 | 1500 |
| `canyons` | simplex | 6 | 0.014 | 0.60 | 2.5 | 0.22 | 9000 |

## 4. Erosion, regions and the writer

Moved out of `editor/util/` so the generator can call them:

- `TerrainErosion::hydraulic` — droplets carry sediment downhill and drop it where the slope eases.
  `heightScale` converts a height difference into texel steps, so any resolution erodes alike.
- `TerrainErosion::thermal` — slopes steeper than `talus` shed onto lower neighbours (angle of repose).
- Both take a `FalloffFunc` (a `std::function<float(int,int)>` weight per texel). The terrain brush
  passes its stamp weight so the effect stays inside the brush; the generator passes a constant 1 over
  the region. Same code, two callers.
- `TerrainMapRegion` — inclusive texel bounds of what one pass wrote, shared with the editor's undo
  patches (`fitsIn`, `merge`, `empty`).
- `writeGray16Png` — the only writer for the heightmap format; stb cannot emit 16-bit PNGs, so the
  encoder does the swap and the filtering itself.

## 5. Lua

```lua
-- read a preset, change one field, write the map relative to the running project
local params = WorldGen.params("island")
params.seed = 7313
params.size = 512

local ok, err, size = WorldGen.generate({
    out = "assets/textures/heightmap.png",  -- required, or generate refuses
    preset = "island",                      -- fills every field; the keys below override it
    seed = 7313, size = 512,
})

WorldGen.presets()                                  -- 5 names
WorldGen.waterHeight(params, terrain.maxHeight)      -- where to put the Water plane
Noise.fbm(NoiseType.SIMPLEX, x / 40, y / 40, seed, 5, 2.0, 0.5)
```

`generate` returns `ok, error, size` and never throws — a missing `out` or a non-table argument comes
back as `false` plus the message. Options table keys are exactly the `TerrainGenParams` names as read
by the binding: `preset`, `size`, `seed`, `noise` (`"perlin"/"simplex"/"value"`), `octaves`,
`lacunarity`, `gain`, `frequency`, `base`, `amplitude`, `curve`, `ridged`, `warp`, `warpFrequency`,
`seaLevel`, `island`, `droplets`, `erosionScale`, `erosionSeed`, `talus`, `thermalRate`.
`WorldGen.params()` returns a table with those same keys, so a preset round-trips.

Every one of these names is in the generated reference — `build/generated/lua_api.md` and the
`Types.lua` a project gets in `.quantum/engine-api/`. That required a reflector fix: a `beginNamespace`
block was only mined for `addVariable` (enums), so a namespace of plain functions (`WorldGen`,
`Plugins`) was silently missing from the docs
([tools/reflector/parse_bindings.cpp:160-189](tools/reflector/parse_bindings.cpp)).

## 6. CLI

```bash
quantum-editor-cmd terrain presets
quantum-editor-cmd terrain generate --out <path> --preset island --size 512 --seed 7313 --erosion-seed 99
quantum-editor-cmd terrain info <path> [--sea-level 0.34]
```

`generate` prints the parameters it used and the map statistics (min / mean / max, percentage below the
water line); `info` re-reads a PNG and prints the same statistics, which is what proves a round trip.
Every `TerrainGenParams` field has a `--flag` (`--perlin|--simplex|--value` for `--noise`), and both
`--key value` and `--key=value` parse.

## 7. Wiring a generated map into a scene

The entity origin is the **bottom** of the elevation range: a terrain at `y = -5` with `maxHeight 5`
puts ground anywhere in `[-5, 0]`, so gameplay samples the surface with a ray instead of reading the
transform.

```lua
-- camera BEFORE terrain, or MeshSystem::resetTerrainToUnbuilt clears needUpdateTerrain for good
local camera = Camera(scene)
camera:setPerspective(55, 1280 / 720, 0.5, 3000)
scene:setCamera(camera)

local terrain = Terrain(scene)
terrain.size = 400.0
terrain.maxHeight = 60.0
terrain.resolution = 64
terrain.levels = 8
terrain:setHeightMap("textures/heightmap.png")
terrain:createTerrain()

local water = Water(scene)
water.position = Vector3(0, WorldGen.waterHeight(params, 60.0), 0)
```

Note the two path spellings: `WorldGen.generate` resolves against the project directory
(`assets/textures/…`), while a component texture path is relative to the asset root (`textures/…`).

With no blend map, `getTerrainColor()` paints the unclaimed weight (`1 - (r+g+b)`) with `terrain.color`
— which is why the probe shows real relief with no third-party art anywhere in it.

## 8. Determinism rules

- Integer hashing only. `rand()`, `std::hash` and float bit tricks differ across compilers, and a seed
  that is not reproducible is not a save-game field.
- Sample at world-space coordinates divided by a scale, never by pixel index, so LODs and tiling agree.
- Heightmaps stay 16-bit. An 8-bit map quantizes into terraces you can see on a gentle slope.
- `erosionSeed` is separate from `seed` on purpose: hydraulic erosion is a stochastic walk, and a
  designer must be able to re-cut the rivers without re-rolling the relief.

## 9. Not supported yet

- **Materials.** The surface is the mesh colour; slope/altitude-driven layer selection and a PBR
  texture set are FASE 4. A blend map is read by the shader and by the editor brush, not produced here.
- **One map per terrain.** Chunked worlds with seams, a generation cache, and partial regeneration are
  FASE 7; `TerrainMapRegion` exists because those passes need it, not because they run.
- **Rivers as water.** Erosion carves beds; nothing places a `Water` per basin yet (FASE 5).
- **Vegetation.** No biome classification and no foliage instancing (FASE 6).
- **3D output.** `generate` writes a heightmap. An editable vertex mesh is FASE 8's export, and the
  plan explicitly rejects shipping only an image.
- **Generation is synchronous.** Calling `TerrainGenerator::generate` at 4096² on the main thread will
  drop frames; the async path belongs to FASE 7, and nothing may throw across a task boundary
  (`_HAS_EXCEPTIONS=0`).
- **Preset ceilings.** `island` at 512² with seed 7313 tops out at 0.741, not 1.0 — `baseHeight +
  amplitude` is the ceiling, and `amplitude` is not a guarantee that the field reaches it.
- **The old editor generator** (`editor/ai/AiTerrainCapability.cpp`) still runs its own value-noise
  field. Promoting it onto `TerrainGenerator` is an open follow-up, not done.
