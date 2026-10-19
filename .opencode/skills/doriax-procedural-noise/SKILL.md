---
name: doriax-procedural-noise
description: Procedural noise in doriax - Perlin, simplex, value noise, fBm, turbulence and hash-based randomness. None of it exists in the engine today except an editor-only 2D value/fractal generator, so this is where and how to add it. Use when the user asks for perlin noise, noise textures, fBm, procedural terrain, clouds, detail variation, or seeded generation.
---

# Procedural noise

## Status: nothing in the engine

`grep -rniE "perlin|simplexNoise|fbm\(" engine/ editor/` -> **zero hits**. There is no
noise include in `engine/shaderlib/includes/` (the 19 files there are brdf,
clearcoat, depth_util, fog, ibl, lighting2d, lightprobe, morphtarget, octahedral,
parallax, pbr, pcss, punctual, shadows, skinning, srgb, terrain_fs, terrain_vs,
transmission), no noise utility in `engine/core/util/`, and no Lua-facing noise API.
Procedural content today comes from **pre-baked textures** (the terrain heightmap is a
16-bit R texture; terrain detail is a blend map) and from the editor's one generator.
Do not invent `Noise.perlin(...)`, `math.noise()`, or a `noise.glsl` include - confirm
the absence first.

## The only implementation that exists

`editor/ai/AiTerrainCapability.cpp`, 2D, CPU, editor-only:

- `fade(t) = t*t*(3-2t)` - smoothstep `:42`
- `hashNoise(x, y, seed)` `:43` - integer mixer seeded with the golden-ratio constant
  `0x9E3779B9`, two xor/multiply rounds (`0x85EBCA6B`, `0xC2B2AE35`), then a
  `>>16 * 0x7FEB352D * >>15 * 0x846CA68B` finalizer, mapped to `[0,1]` through
  `h & 0x00FFFFFF`.
- `valueNoise(x, y, seed)` `:57` - bilinear blend of four corner hashes using `fade`.
- `makeHeightPixels(...)` `:70` - `fractal_noise` accumulates up to 8 clamped octaves
  (`weight *= 0.5`, `frequency *= 2`, per-octave seed offset `seed + o*1013`), remaps
  `[0,1] -> [-1,1]`, and encodes 16-bit heights.

Note what this is: **value** noise (hash at lattice points, interpolated), not gradient
noise. It is cheaper and has visible axis-aligned structure; that is fine for a
heightmap and wrong for clouds/water normal detail. It also lives in `editor/`, so
**engine and game code cannot use it** - the dependency direction is editor -> engine.

## Adding noise, by destination

1. **GLSL (the common case: waves, detail, clouds, foam).** New
   `engine/shaderlib/includes/noise.glsl` with an integer-hash gradient noise
   (`uvec`/`bitImages` style, since float bit tricks diverge across drivers) plus an
   `fbm()` octave loop whose count is a loop-bound constant, matching how `ssr.frag:19`
   declares `SSR_MAX_STEPS`. Conventions to follow: `#include "includes/noise.glsl"`
   inside an `#ifdef` guard like `mesh.frag:189-212`; keep the aggregate-initializer
   count in sync with any struct you touch (`doriax-shaders`); and remember the file is
   invisible until `cmake --build "$DORIAX_BUILD_DIR" --target generate_shaders`
   re-embeds it into `shaders.h`. Avoid `fract(sin(p)*43758.5453)`-style hashing on
   large coordinates - precision varies and it tiles visibly.
2. **Engine C++ (world generation, physics heightfields, mesh cooks).** Put it in
   `engine/core/util/` (new file = hand-maintained source lists:
   `engine/CMakeLists.txt`, plus the Xcode `project.pbxproj`), use `std::mt19937`
   seeded explicitly, and never throw - exceptions cannot cross a pooled task here
   (`_HAS_EXCEPTIONS=0` at `engine/libs/joltphysics/CMakeLists.txt:547`).
3. **Project Lua / C++ scripts.** Gameplay noise should be a texture or a shader term;
   if it must be callable from a script, it needs a binding in
   `engine/core/script/binding/MathClassesLua.cpp` and a regenerated API header
   (`doriax-scripting`). Alternatively generate the noise in the editor and ship it as
   an asset - that is what the terrain tool does and it costs nothing at runtime.
4. **Editor tooling.** Extend `AiTerrainCapability.cpp` rather than duplicating it;
   its request struct (`resolution`, `baseHeight`, `amplitude`, `frequency`, `octaves`,
   `seed`, `mode`) is already wired to a heightmap path + a texture write.

## Determinism and quality checklist

- Same seed must give the same map on MSVC, MinGW and GLSL: use integer hashing, not
  `rand()`/`fmod` float mixing, and not `std::hash`.
- Sample noise at world-space coordinates divided by a scale uniform, never by pixel
  index, so LODs and tiling agree.
- Heightmaps are stored 16-bit (`encodeHeight16`) - keep that if terrain must read it.
- A generator with no visual test harness means: emit a PNG/preview asset and ask the
  user to look, since agents must not launch `doriax-editor.exe`.
