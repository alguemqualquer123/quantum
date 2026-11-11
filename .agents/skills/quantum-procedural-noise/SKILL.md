---
name: quantum-procedural-noise
description: Procedural noise in Quantum - the engine's own Perlin/simplex/value CPU library (engine/core/util/Noise.{h,cpp}), its Lua surface (Noise.perlin2, fbm, turbulence, NoiseType), determinism rules and the GLSL noise that still does not exist. Use when the user asks for perlin noise, noise textures, fBm, procedural terrain, clouds, detail variation, or seeded generation.
---

# Procedural noise

## Status: CPU noise exists, GLSL noise does not

`engine/core/util/Noise.{h,cpp}` (`quantum::Noise`, `QUANTUM_API`) is pure functions of
`(coordinates, seed)` - no global state, no allocation, no exceptions, so a pooled world-gen
task and the main thread can sample the same field. Bases: `perlin2/3`, `simplex2/3`,
`value2/3`, all in `[-1,1]`; `hash2/3` in `[0,1]` and discontinuous by design; `sample` /
`sample3` dispatch on `NoiseType`; `fbm`/`fbm3` and `turbulence`/`turbulence3` take
`(type, coords..., seed, octaves, lacunarity, gain)` with `MAX_OCTAVES = 16` as a hard clamp
(it runs inside per-pixel loops, so a bad octave count must cost detail, not the frame);
`ridged`/`ridged3` (`Noise.h:76`) are the same octaves of `(1 - |sample|)^2` - sharp divides, [0, 1];
`warp2`/`warp3` (`:87`) displace the coordinates themselves by a 3-octave simplex field read from
seeds `seed ^ 0x9E3779B9 / 0x85EBCA6B / 0xC2B2AE35`, and `warpedFbm`/`warpedFbm3` (`:92`) are the fbm
of a warped point. `warpAmplitude = 0` is the identity, so an unused warp costs nothing.
Coordinates are lattice units (one cell per unit) - sample at world position / scale, never
at a pixel index. NaN or out-of-int coordinates read flat (0).

It is callable from Lua (`engine/core/script/binding/UtilClassesLua.cpp`: `Noise.perlin2(...)`,
`NoiseType.PERLIN`, `Noise.MAX_OCTAVES`; `NoiseType` marshals as a plain number through an
`EnumWrapper` specialisation). Proof: `engine/components/checks/probe_instances.lua`-style
sibling `checks/probe_noise.lua`, 41 checks, all PASS through the real runner.

What still does **not** exist: any noise in `engine/shaderlib/includes/` (the 19 files there
are brdf, clearcoat, depth_util, fog, ibl, lighting2d, lightprobe, morphtarget, octahedral,
parallax, pbr, pcss, punctual, shadows, skinning, srgb, terrain_fs, terrain_vs, transmission)
- so per-pixel noise in a shader is still written by hand. The Lua surface is documented: `Noise.*`
and the `WorldGen` namespace reach `build/generated/lua_api.md` and `Types.lua` because the
reflector now parses a `beginNamespace` block of `addFunction` calls as a table of statics
(`tools/reflector/parse_bindings.cpp:160`) - before that, a function namespace was silently dropped
from the reference. Signatures print as `WorldGen.generate()` only: a raw `int f(lua_State*)` has no
header to read types from, so the hand-written chapter (`tools/reflector/lua_usage.lua`, section 23)
carries the argument list and a runnable example.

## The heightmap generator (engine-side since FASE 2)

`engine/core/util/TerrainGenerator.{h,cpp}` is the field a World Creator uses: presets
(`continents | island | mountains | plains | canyons`), fBm + optional ridged relief + optional
domain warp, a relief curve, sub-sea damping, an island border mask, then hydraulic and thermal
erosion (`util/TerrainErosion.{h,cpp}`, `util/TerrainMapRegion.h`) written out as a 16-bit
grayscale PNG (`util/PngWriter.{h,cpp}`). All three moved out of `editor/util/` so there is one
implementation; the same seed from the CLI (`quantum-editor-cmd terrain generate`), from Lua
(`WorldGen.generate`) and from the editor brush produce byte-identical files - asserted, not
assumed, by `tests/worldcreator/probe` (48 checks + a captured PNG). See `quantum-terrain`.

## The older editor-side generator

`editor/ai/AiTerrainCapability.cpp`, 2D, CPU, still its own field:

- `TerrainNoise::hash(x, y, seed)` (`editor/util/TerrainNoise.h:16`) - integer mixer seeded with the
  golden-ratio constant `0x9E3779B9`, two xor/multiply rounds (`0x85EBCA6B`, `0xC2B2AE35`), then a
  `>>16 * 0x7FEB352D * >>15 * 0x846CA68B` finalizer, mapped to `[0,1]` through `h & 0x00FFFFFF`.
- `TerrainNoise::value(x, y, seed)` (`:28`) - bilinear blend of four corner hashes with
  `fade(t) = t*t*(3-2t)`, and its octave sum at `:52`.
- `makeHeightPixels(...)` (`AiTerrainCapability.cpp:40`) - modes `random_noise` / `fractal_noise`
  (up to 8 clamped octaves, per-octave seed offset `seed + o*1013`), remapping `[0,1] -> [-1,1]` and
  encoding through `encodeHeight16` (`:32`).

`Noise::value2` reuses these exact hash constants, so the two agree bit for bit - the heightmap
tool and a runtime field do not diverge. New heightmap work goes through `TerrainGenerator` /
`WorldGen` (section above), not this file: `AiTerrainCapability` keeps its own field only because
its request shape (`resolution`, `baseHeight`, `amplitude`, `frequency`, `octaves`, `seed`, `mode`)
is already wired to an AI capability and a texture write. Promoting it onto `TerrainGenerator` is
an open follow-up, not a blocker.

## Adding noise, by destination

1. **GLSL (waves, detail, clouds, foam).** Still a new
   `engine/shaderlib/includes/noise.glsl` with an integer-hash gradient noise
   (`uvec`/`bitImages` style, since float bit tricks diverge across drivers) plus an
   `fbm()` octave loop whose count is a loop-bound constant, matching how `ssr.frag:19`
   declares `SSR_MAX_STEPS`. Conventions: `#include "includes/noise.glsl"` inside an
   `#ifdef` guard like `mesh.frag:189-212`; keep aggregate-initializer counts in sync with
   any struct you touch (`quantum-shaders`); the file is invisible until
   `cmake --build "$QUANTUM_BUILD_DIR" --target generate_shaders` re-embeds it. Avoid
   `fract(sin(p)*43758.5453)` hashing on large coordinates - precision varies and it tiles.
   If the shader and the CPU field must match, port the `Noise.cpp` integer mixer, not a
   textbook one.
2. **Engine C++ / Lua** - use `quantum::Noise`; extend it rather than adding a second
   generator (a new file there means the hand-maintained lists: `engine/CMakeLists.txt`
   source list, and note the Xcode `project.pbxproj` already lags several engine files -
   `WaterSystem.cpp`, the slate and voxel trees are absent from it, so a per-file edit
   there is a judgement call, not a checkbox). New binding = regenerate the API surface
   (`quantum-lua-api`, `quantum-build-verify`).
3. **Project scripts.** `Noise.fbm(NoiseType.SIMPLEX, x/scale, y/scale, seed, 5, 2.0, 0.5)`
   is enough for procedural fields, and a whole heightmap is one call -
   `WorldGen.generate({out = "assets/textures/map.png", preset = "mountains", seed = 42})`. For
   anything drawn per-pixel, generate a texture at load or ship one as an asset; the runtime never
   compiles a shader, so a shader-side field is not an option (see `quantum-shaders`).

## Determinism and quality checklist

- Same seed must give the same map on MSVC, MinGW and GLSL: `Noise` hashes with integers
  only for that reason - never `rand()`, `std::hash` or float bit tricks.
- Sample noise at world-space coordinates divided by a scale, never by pixel index, so LODs
  and tiling agree.
- Heightmaps are stored 16-bit (`encodeHeight16`) - keep that if terrain must read it.
- A generator with no visual test harness means: emit a PNG/preview asset and ask the user
  to look, since agents must not launch `quantum-editor.exe`. `probe_noise.lua` is the
  headless half of that contract (range, purity, continuity, seed, octave identity).
