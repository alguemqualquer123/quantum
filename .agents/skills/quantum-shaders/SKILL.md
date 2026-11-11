---
name: quantum-shaders
description: Author, compile and cache GLSL in Quantum - engine/shaderlib files, shader variants/ShaderKey bits, the embedded shaderMap, the .sdat disk cache, the async compile path, and forking a built-in shader type through the u_vs_customParams/u_fs_customParams seam. Use when writing or debugging a .vert/.frag/.glsl include, adding a shader variant or a project sky/cloud/atmosphere fork, or when a shader edit appears to have no effect or the editor aborts on import.
---

# Shaders

## The one rule that saves hours

Editing `engine/shaderlib/**` on disk does **nothing** until the shaders are
re-embedded. CMake inlines every file into `$QUANTUM_BUILD_DIR/generated/shaders.h`
as `shaderMap` (`generate_shaders.cmake:36-53`), and the runtime compiler is handed
that map, not the filesystem (`editor/shader/ShaderBuilder.cpp:818,1048`
`args.fileBuffers = editor::shaderMap`). So:

```bash
cmake --build "$QUANTUM_BUILD_DIR" --target generate_shaders
cmake --build "$QUANTUM_BUILD_DIR" --target quantum-editor --config Debug
```

Second masking layer: compiled results are cached on disk at
`<usercache>/quantum/shaders/v40/<shaderStr>_<lang>.sdat` (`App.cpp:3792-3863`,
`ShaderBuilder.cpp:913-925`). Each entry now also carries a
`<cache>.sources.sha1` sidecar signed by `ShaderBuilder::isCacheStale`: a forked variant
signs its own resolved source graph, an engine variant signs the shaderMap graph of its
`ShaderType` (entry files + transitive includes, memoised per type - properties only add
`#defines` and are already in the file name). So **an edit to a shared include
invalidates exactly the variants compiled from that text, with no manual bump**. An
entry with no sidecar is *adopted*, not discarded: it is stamped on first read, which is
what kept a warm cache alive when this landed. The version dir is now the blunt tool,
needed only for a `.sdat` **format** change or for a `#define` the C++ side feeds the
compiler (`setupShaderArgs`: `MAX_LIGHTS`, `SSR_MAX_STEPS`) - `App.cpp:3798-3861` keeps
the manual bump history (v33 was a pbr.glsl change that touched every UV-set variant,
v40 the parallax one). To force the absolute truth, delete the version dir or use
`Clear Shader Cache` in the View menu.

Third masking layer, the one that bites the headless path: `shaderMap` is compiled
into *each editor-target executable* (`editor/shader/ShaderBuilder.h:18` includes
`shaders.h`; nothing under `engine/` does, so `quantum.dll` carries no shader text at
all). `quantum-resource-compiler shaders --out <project>/assets/shaders` therefore
re-emits whatever map was linked into **that exe**: after `generate_shaders`, rebuild
`quantum-resource-compiler` as well, or every bundled `.sdat` keeps drawing the old
pass and nothing prints an error. Prove it on the binary, not on the source -
`grep -c <new-identifier> "$QUANTUM_BUILD_DIR/Debug/quantum-resource-compiler.exe"`.

## Variants

`ShaderKey = customId<<48 | type<<32 | propertyBits` (`engine/core/pool/ShaderPool.cpp:681`).

- Bit -> `#define` tables: `addMeshPropertyDefinitions` `ShaderBuilder.cpp:448-479`
  (bit0 `MATERIAL_UNLIT` 'Ult', bit12 `HAS_FOG` 'Fog', bit17 `HAS_TERRAIN` 'Ter',
  bit22 `USE_LIGHT2D` 'L2d', bit25 `ALPHA_OPAQUE` 'Aop', bit29 `HAS_HEIGHT` 'Hgt'),
  plus depth/gbuffer/ui/points/lines tables at `:481-527`. The 3-letter codes are what
  you see in cache filenames (`getShaderDisplayName` `:1053`). **Mesh bits 1 and 2 are
  the UV sets** (`HAS_UV_SET1` 'Uv1', `HAS_UV_SET2` 'Uv2'): a hand-written `--shader`
  spec must carry them with any map bit, because `pbr.glsl` only defines the
  `get*UV()` helpers inside `#ifdef HAS_UV_SET*`. `mesh:Puc,Nor,Nmp` fails to compile
  for that reason; `mesh:Uv1,Puc,Nor,Nmp` does not, and the runtime never builds the
  difference because every map bit is gated on `p_hasTexture1` (`RenderSystem.cpp:3049`
  for `HAS_HEIGHT`). So this is a tooling trap, not an engine bug.
- Bit 28 `DIRECTIONAL_LIGHTS_ONLY` ('Dlo') is set outside the table, directly on the
  properties (`RenderSystem.cpp:3065-3067`), and `ShaderBuilder` compiles the Dlo
  companion of every spec automatically - a CLI export therefore names one variant and
  writes two `.sdat`.
- `setupShaderArgs` `:678` maps `ShaderType` -> entry files + constants
  (`MAX_LIGHTS=6`, `SSR_MAX_STEPS=128`). **A new ShaderType with no case here never
  compiles**, and the failure surfaces as "Missing shaders" at runtime
  (`RenderSystem.cpp:11165-11188` prints a CLI hint and `exit(1)`).
- Bits are computed per submesh in `RenderSystem.cpp:3020-3067`, from vertex attributes
  rescanned every reload at `:2877-2933` - the `hasTexCoord1: false` a `.scene` file
  carries is a stale mirror and does not select the variant.
- A new bit needs three places, and the name table is the load-bearing one: the
  `#define` row, `getShaderPropertyName` (`ShaderPool.cpp:551`) and
  `getShaderPropertyCount` (`:475`). `getValidPropertyMask` (`:818-828`) derives from
  the names, so a bit with no short name ("?") is masked out of every key and silently
  never compiles.
- `#include "includes/x.glsl"` is resolved by glslang's `BufferIncluder`
  (`shadercompiler/src/glslang.cc:29-107`); project-forked shaders
  resolve project-local first, then `shaderMap` (`ShaderBuilder.cpp:129-141`).

## Two crash classes to know before touching a shader

1. **Aggregate initializers must match the struct exactly.** GLSL has no named
   initializers, so `mesh.frag:339-341` zero-inits `MaterialInfo` positionally; the
   struct (`mesh.frag:190-213`) has 16 members. A schema change that left 21 values in
   the list broke compilation for every `mesh:*` variant and took the editor down.
   When you add/remove a `MaterialInfo`/similar struct member, grep for its
   `{0.0, vec3(0.0), ...}` initializer and fix the count in the same edit.
2. **An exception escaping an async build aborts the process.** Every MSVC target
   inherits `_HAS_EXCEPTIONS=0` (propagated PUBLIC from
   `engine/libs/joltphysics/CMakeLists.txt:547`), so a `std::packaged_task` has no
   handler to store the throw and `std::future::get()` cannot rethrow it - the
   runtime calls `abort()` ("Debug Error! ... abort() has been called"). The build
   lambda therefore `catch(...)`es and records the key in
   `ShaderBuilder::failedBuilds` under its own mutex, and the consumer maps that to
   `ResourceLoadState::Failed`. Copy this pattern for any new threaded compile path;
   never let a throw cross a future boundary in this repo.

## Material uniform blocks are positional, on both sides

`u_fs_pbrParams` is uploaded from the head of the CPU `Material` struct
(`RenderSystem.cpp` `loadPBRTextures`), so the first members' order is the wire format:
`sizeof(float)*12` for a lit variant, `*8` for unlit+`ALPHA_MASK` (two named padding
floats keep the offsets), `*4` for plain unlit. Adding a material field to that block
means editing the struct prefix in the same commit, and a mismatch reads as garbage, not
as a compile error. The parallax tuning went into its own `u_fs_heightParams` vec4
(`Material.h` `heightScale`/`heightMinLayers`/`heightMaxLayers`/pad, uploaded per frame at
`RenderSystem.cpp:4249-4253`, slot resolved at `:3142`) for exactly that reason. `u_fs_texCoordSets` only exists in
a `HAS_UV_SET2` variant and is fed from a local `int32_t[8]`, so a new UV-set selector
costs no layout change (`set1.y` = the height map's set).

## Lighting is forward

`mesh.frag` does all lighting. `SokolGBuffer`/`SokolDeferredLighting` exist but are
never instantiated and load files that do not exist
(`engine/renders/sokol/SokolDeferredLighting.cpp:24` -> `deferred_lighting.{vert,frag}`).
A `gbuffer.vert/frag` pair *is* compiled, but only because SSR consumes the G-buffer.
Do not describe Quantum as a deferred renderer.

## Verify headless (no editor launch)

```bash
"$QUANTUM_BUILD_DIR/Debug/quantum-resource-compiler.exe" shaders --out /tmp/shout \
    --backend opengl --shader "mesh:Uv1,Puc,Nor,Nmp,Aop"
```

Compile every variant you may have affected by changing the variant letters. This is
the only trustworthy GLSL check available; there is no render test harness and the
project rule forbids launching `quantum-editor.exe` from an agent (its autosave has
destroyed project data).

To prove a variant is the one the renderer actually asks for, compile it and run the scene: a
missing `.sdat` is fatal and printed (`Quantum is missing precompiled shaders` + the exact list,
`exit(1)`), so a clean headless run *is* the proof. Files land under
`%LOCALAPPDATA%/quantum/shaders/v40/` only from the editor or `quantum-resource-compiler` —
**`quantum-engine` never compiles**: it installs no `ShaderPool::setShaderBuilder`, so
`ShaderPool::get` only reads `shader://<file>.sdat` = `<project>/assets/shaders/`. A `.sdat` in the
user cache is the editor having built it, not the game.

## Forking a built-in shader type (the `customParams` seam)

A project can replace a built-in shader without touching engine C++. Any fork may declare
`u_vs_customParams` / `u_fs_customParams`; the members are filled **by name** out of
`SkyComponent::shaderUniforms` (a `std::vector<std::pair<std::string, Vector4>>`), plus two
engine-written reserved names, `time` (seconds) and `resolution`. `writeUniform` honours the
reflected std140 offset, so mixing `float` and `vec4` is safe; mixing `int` and `float` uploads
wrong. Lua drives it through `setShaderUniform` / `getShaderUniform` / `removeShaderUniform`
(`addFunction`), and assigns the fork with `sky.customShader = "assets/shaders/<name>"` — that is
an `addProperty` pair, so `setCustomShader(...)` is a nil method.

Two layout constraints, both silent if violated:

- Omitting a uniform block is safe (`SokolObject::applyUniformBlock` guards `slot != -1`), but
  anything the engine applies by size must stay verbatim — `drawSky` pushes `slotVSParams` with
  `sizeof(float)*16`, so a sky fork keeps `u_vs_skyParams { mat4 vpMatrix; }` exactly.
- `envColor = Color::sRGBToLinear(sky.color.rgb) * globalIlluminationIntensity`
  (`RenderSystem.cpp`) is computed from the component regardless of what the dome draws. The water
  sky reflection and the IBL ambient read `SkyComponent::color`, not the fork. A fork therefore
  does not replace the clock that writes the colour — see `engine/components/DayNightCycle.lua`.

`ShaderType::SKYBOX` has **zero property bits**, so a sky fork yields exactly one deterministic
artifact name: `sky_<sanitised-project-relative-path>_glsl410.sdat` (non-alphanumerics become `_`).
Types with bits produce one file per variant instead. Ship the fork's `.sdat` beside its source.

To build one headless, declare the fork in the working project's top-level `shaderKeys:` in the
scene YAML, compile, and copy the artifact out of the user cache:

```bash
"$QUANTUM_BUILD_DIR/Debug/quantum-resource-compiler.exe" shaders \
    --project "C:/abs/path/Projeto" --force --verbose
cp "$LOCALAPPDATA/quantum/shaders/v40/sky_assets_shaders_sky_daynight_glsl410.sdat" \
   Projeto/assets/shaders/
```

`--project` must be **absolute**: a relative path is joined twice and nothing compiles, silently.
Built-in variants take the other form and land straight in the project:
`shaders --out "<abs>/assets/shaders" --backend opengl --shader "mesh:Puc,Nor,Vc4,Fog"`, repeating
`--shader` per variant.
