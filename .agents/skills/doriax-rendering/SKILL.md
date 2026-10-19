---
name: doriax-rendering
description: Orientation for doriax rendering as a whole - the sokol backend layering, Render.h vocabulary, the actual pass order in RenderSystem::draw, where the deferred code is and is not, and which specialist skill to open for shaders, lighting, post-process, bloom, volumetrics, water, terrain or noise. Use when starting any engine/ rendering task.
---

# Rendering map

Open the specialist skill for the area: `doriax-shaders`, `doriax-lighting`,
`doriax-postprocess`, `doriax-bloom`, `doriax-volumetrics`, `doriax-water`,
`doriax-terrain`, `doriax-procedural-noise`. Build/verify: `doriax-build-verify`.

## Layering

- `engine/core/render/*` - backend-agnostic facades, each holding one concrete backend
  object (e.g. `ObjectRender.h:19` `SokolObject backend`). Keep sokol types out of
  public signatures.
- `engine/core/render/Render.h` - the pure vocabulary: `ShaderType` (`GBUFFER` `:47`,
  `POSTPROCESS` `:80`, the bloom/DOF/TAA entries `:57-78`), `UniformBlockType`
  (`GBUFFER_VS_PARAMS` `:148`, `BLOOM*_FS_PARAMS` `:170-173`, `BLOOMTEXTURE0/1` slots
  `:232+`), `PipelineType` bit flags. A new pass needs an entry here **and** a case in
  `ShaderBuilder::setupShaderArgs`.
- `engine/renders/sokol/*` - the implementation (`SokolSystem` queue/setup/commit,
  `SokolObject/Shader/Texture/Buffer/Framebuffer/Camera`, `SokolCmdQueue` for
  cross-thread uploads). Compiled into `doriax`; sokol itself is vendored at
  `engine/libs/sokol`.
- Orchestration: `engine/core/subsystem/RenderSystem.{h,cpp}`.

## Pass order (all from `RenderSystem::draw()`, `RenderSystem.cpp:6812`)

shadow atlas `:6886-6919` (`ensureShadowAtlas:1526`) -> point-cube atlas `:6921-6952`
-> 2D polar shadow rows `:6955-7019` (`ensureShadow2DAtlas:911`,
`loadOccluder2DPass:1882`) -> reflection-probe capture `:7024`
(`renderReflectionProbeCapture:1347`) -> per camera: optional G-buffer
(`renderGBufferPass:3550`) + SSR (`renderSSR:3709`, gated `isSSREnabled():7081-7084`),
optional depth pre-pass (`renderDepthPrePass:3396`), SSAO (`renderSSAO:3329`), colour
pass (sky `drawSky:4899` -> opaque `drawMesh:2939` -> UI/points/lines -> sorted
transparents `:7366`) -> SSR composite -> `renderPostProcess:4087` -> fixed-res
`renderBlit:3881`.

## Two statements to repeat verbatim when they matter

**Lighting is forward.** `mesh.frag` lights the scene. `SokolGBuffer`/
`SokolDeferredLighting` are never instantiated and `SokolDeferredLighting.cpp:24`
loads `deferred_lighting.{vert,frag}`, which do not exist.

**Declared is not implemented.** Many effects ship as a `.frag` file + enum entries +
header declarations with zero definitions and zero call sites (bloom, tonemap,
exposure, colour grading, DOF, TAA, FXAA, motion blur, velocity, volumetric/height
fog, light shafts, VSM, SSGI). Before answering "does doriax have X", check for a
`RenderSystem::renderX` **definition** and a call in `draw()`.

## Verification

No render test harness exists. Minimum bar: `generate_shaders` re-run, the `doriax`
target compiles with MSVC, and the affected variants compile headless via
`doriax-resource-compiler.exe shaders --out <dir> --backend opengl --shader "<type>:<bits>"`.
GLSL errors otherwise surface at runtime, and the visual check belongs to the user
(`doriax-safety-rules`).

## Planned work worth citing

`RENDERING_UPGRADE_PLAN.md` and `TODO_ENGINE.md` at the repo root: histogram
luminance exposure, 5-6 mip bloom ping-pong, 3D LUT generation, clustered forward,
SSGI, procedural sky, GTAO, VSM/better PCF, velocity MRT + material IDs, decals,
auto-LOD. Each follows the 6-step built-in recipe in `doriax-postprocess`.
