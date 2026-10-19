---
name: doriax-shaders
description: Author, compile and cache GLSL in doriax - engine/tools/shaderlib files, shader variants/ShaderKey bits, the embedded shaderMap, the .sdat disk cache, and the async compile path. Use when writing or debugging a .vert/.frag/.glsl include, adding a shader variant, or when a shader edit appears to have no effect or the editor aborts on import.
---

# Shaders

## The one rule that saves hours

Editing `engine/tools/shaderlib/**` on disk does **nothing** until the shaders are
re-embedded. CMake inlines every file into `$DORIAX_BUILD_DIR/generated/shaders.h`
as `shaderMap` (`generate_shaders.cmake:36-53`), and the runtime compiler is handed
that map, not the filesystem (`editor/shader/ShaderBuilder.cpp:818,1048`
`args.fileBuffers = editor::shaderMap`). So:

```bash
cmake --build "$DORIAX_BUILD_DIR" --target generate_shaders
cmake --build "$DORIAX_BUILD_DIR" --target doriax-editor --config Debug
```

Second masking layer: compiled results are cached on disk at
`<usercache>/doriax/shaders/v23/<shaderStr>_<lang>.sdat` (`App.cpp:2768`,
`ShaderBuilder.cpp:913-925`). A stale cache makes a **broken** shader look fine - it
only recompiles for a variant not yet on disk. To force the truth, delete that
version dir or bump it (`App.cpp:2732-2767` records the manual bump history).

## Variants

`ShaderKey = customId<<48 | type<<32 | propertyBits` (`engine/core/pool/ShaderPool.cpp:648-650`).

- Bit -> `#define` tables: `addMeshPropertyDefinitions` `ShaderBuilder.cpp:446-473`
  (bit0 `MATERIAL_UNLIT` 'Ult', bit12 `HAS_FOG` 'Fog', bit17 `HAS_TERRAIN` 'Ter',
  bit22 `USE_LIGHT2D` 'L2d', bit25 `ALPHA_OPAQUE` 'Aop'), plus depth/gbuffer/ui/
  points/lines tables at `:475-515`. The 3-letter codes are what you see in cache
  filenames (`getShaderDisplayName` `:997`).
- `setupShaderArgs` `:661-740` maps `ShaderType` -> entry files + constants
  (`MAX_LIGHTS=6`, `SSR_MAX_STEPS=128`). **A new ShaderType with no case here never
  compiles**, and the failure surfaces as "shader missing" at runtime
  (`RenderSystem.cpp:7401-7431` prints a CLI hint and `exit(1)`).
- Bits are computed per submesh in `RenderSystem.cpp:2422-2455`.
- `#include "includes/x.glsl"` is resolved by glslang's `BufferIncluder`
  (`engine/tools/shadercompiler/src/glslang.cc:29-107`); project-forked shaders
  resolve project-local first, then `shaderMap` (`ShaderBuilder.cpp:129-141`).

## Two crash classes to know before touching a shader

1. **Aggregate initializers must match the struct exactly.** GLSL has no named
   initializers, so `mesh.frag:259-261` zero-inits `MaterialInfo` positionally; the
   struct (`mesh.frag:152`) has 16 members. A schema change that left 21 values in
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

## Lighting is forward

`mesh.frag` does all lighting. `SokolGBuffer`/`SokolDeferredLighting` exist but are
never instantiated and load files that do not exist
(`engine/renders/sokol/SokolDeferredLighting.cpp:24` -> `deferred_lighting.{vert,frag}`).
A `gbuffer.vert/frag` pair *is* compiled, but only because SSR consumes the G-buffer.
Do not describe doriax as a deferred renderer.

## Verify headless (no editor launch)

```bash
"$DORIAX_BUILD_DIR/Debug/doriax-resource-compiler.exe" shaders --out /tmp/shout \
    --backend opengl --shader "mesh:Puc,Nor,Aop"
```

Compile every variant you may have affected by changing the variant letters. This is
the only trustworthy GLSL check available; there is no render test harness and the
project rule forbids launching `doriax-editor.exe` from an agent (its autosave has
destroyed project data).
