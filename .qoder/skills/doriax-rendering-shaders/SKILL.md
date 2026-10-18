---
name: doriax-rendering-shaders
description: Work on doriax rendering - the sokol backend, deferred/forward passes, the post-process chain, shadows, GLSL shaderlib files, shader compilation and the ShaderPool cache. Use when adding or changing a render pass, post-process effect, shader, material/shader variant, or shadow/lighting behaviour in engine/.
---

# Rendering and shaders

## Layering

- `engine/core/render/*` are backend-agnostic facades; each holds one concrete
  backend object (e.g. `ObjectRender.h:19` `SokolObject backend`). Keep public
  signatures free of sokol types.
- `engine/core/render/Render.h` is the pure vocabulary header: `ShaderType`
  (`GBUFFER` :47, `POSTPROCESS` :80, SSAO/SSR/bloom/TAA/DOF), `UniformBlockType`
  (`GBUFFER_VS_PARAMS` :148...), `PipelineType` bit flags. New passes need an entry
  here **and** a matching path in `ShaderPool`.
- `engine/renders/sokol/*` implements it (`SokolSystem` queue/setup/commit,
  `SokolGBuffer`, `SokolDeferredLighting`, `SokolObject/Shader/Texture/Buffer/
  Framebuffer/Camera`). Compiled into `doriax` (`engine/CMakeLists.txt:872+`);
  sokol itself is vendored at `engine/libs/sokol`.
- Pass orchestration lives in `engine/core/subsystem/RenderSystem.{h,cpp}`: depth
  pre-pass, `renderGBufferPass` (:577), SSAO/SSR, bloom/HDR/exposure/tonemap, and the
  user post-process ping-pong chain (`postProcessFramebuffer[2]` :440,
  `loadPostProcess()` :593, `needReloadPostProcess()` :683).

## Adding a post-process effect

1. Author GLSL in `engine/tools/shaderlib/` (`.frag`/`.vert`, shared code in
   `includes/*.glsl`). 53 files today; match the existing naming
   (`bloom_threshold.frag`, `dof_coc.frag`).
2. Add the `ShaderType`/`UniformBlockType` entries in `Render.h` if it is a new kind
   of pass, and the matching case in `engine/core/pool/ShaderPool.cpp`.
3. Wire the pass in `RenderSystem` (framebuffer allocation, bind order, and the
   `needReload*` flag that rebuilds it when the setting changes).
4. Expose data as a field in `engine/core/SceneSettings.h` (defaults live only
   there), a `Scene` setter that calls the `needReload*` hook, and a
   `PostProcessPass{shader, enabled, uniforms}` entry (:36-43) if it is user-chain
   selectable.
5. Editor: the YAML key in `editor/Stream.cpp` and the row in
   `editor/window/Properties.cpp`.

## Shader pipeline (how a .frag reaches the GPU)

- CMake embeds every `engine/tools/shaderlib/**` file verbatim into
  `$DORIAX_BUILD_DIR/generated/shaders.h` as `shaderMap` via `generate_shaders.cmake`
  (root `CMakeLists.txt:337-356`, `CONFIGURE_DEPENDS` glob so new files are picked up
  on reconfigure; target `generate_shaders`).
- Backend-specific compilation happens at runtime through
  `editor/shader/ShaderBuilder` (glslang + SPIRV-Cross from `shadercompiler/`),
  installed into the engine with `ShaderPool::setShaderBuilder`
  (`engine/core/pool/ShaderPool.h:106`). Headless: `doriax-resource-compiler shaders`.
- Without a builder, `ShaderPool` falls back to embedded base64
  (`engine/shaders/*.h`, mostly stubs) or on-disk `.sdat`
  (`ShaderPool.cpp:699-715`). Custom/forked shaders get unique `.sdat` basenames
  (:625) so project edits do not collide with engine shaders.

## Verification

There is no render test harness. Minimum bar: the `doriax` target compiles, the
editor launches, and the effect is visible in a scene - which requires actually
running it. Type-only checks (`g++ -fsyntax-only`, `cl /Zs`) catch GLSL string
breakage only indirectly; GLSL errors surface at runtime through the shader compiler,
so check the editor's `Output` log / `Notification` toasts after enabling a new pass.
See `doriax-build-verify`.

## Known open work (TODO_ENGINE.md, RENDERING_UPGRADE_PLAN.md)

Luminance-histogram exposure (256 bins + temporal smoothing), 5-6 mip bloom
ping-pong, 3D LUT generation shader, clustered forward, SSGI (`ssgi.frag` exists but
is not wired), procedural sky, GTAO, VSM/better PCF, velocity MRT + material ID,
decals and auto-LOD. Each one follows the 5-step recipe above.
