---
name: quantum-rendering
description: Orientation for Quantum rendering as a whole - the sokol backend layering, Render.h vocabulary, the actual pass order in RenderSystem::draw, where the deferred code is and is not, and which specialist skill to open for shaders, lighting, post-process, bloom, volumetrics, water, terrain or noise. Use when starting any engine/ rendering task.
---

# Rendering map

Open the specialist skill for the area: `quantum-shaders`, `quantum-lighting`,
`quantum-postprocess`, `quantum-bloom`, `quantum-volumetrics`, `quantum-water`,
`quantum-terrain`, `quantum-procedural-noise`. Build/verify: `quantum-build-verify`.

## Layering

- `engine/core/render/*` - backend-agnostic facades, each holding one concrete backend
  object (e.g. `ObjectRender.h:19` `SokolObject backend`). Keep sokol types out of
  public signatures.
- `engine/core/render/Render.h` - the pure vocabulary: `ShaderType` (`GBUFFER` `:47`,
  `POSTPROCESS` `:80`, the HDR passes `:57-63`, the anti-aliasing passes `:64-67`),
  `UniformBlockType` (`GBUFFER_VS_PARAMS` `:152`, the HDR params `:179-185`),
  `BLOOMTEXTURE0..4` slots `:246+`, `EXPOSURETEXTURE` `:245`. A new pass needs an entry here
  **and** a case in
  `ShaderBuilder::setupShaderArgs`.
- `engine/renders/sokol/*` - the implementation (`SokolSystem` queue/setup/commit,
  `SokolObject/Shader/Texture/Buffer/Framebuffer/Camera`, `SokolCmdQueue` for
  cross-thread uploads). Compiled into `quantum`; sokol itself is vendored at
  `engine/libs/sokol`.
  sokol allocates its GPU objects from pools sized **once** at `sg_setup`
  (`SokolSystem.cpp:68-73`) and they never grow, so an exhausted pool is a hard ceiling
  that shows up as `*_POOL_EXHAUSTED` plus a mesh that never loads. `SokolObject` does
  **not** own its pipelines: `endLoad()` takes each of the up-to-11 handles from a
  ref-counted cache keyed by the whole `sg_pipeline_desc` (vertex layout + depth/stencil/
  blend state), so meshes that share a material state share one `sg_pipeline`. Anything
  touching those handles must go through `acquirePipeline`/`releasePipeline` - copying a
  `SokolObject` calls `retainAll()`, and `beginLoad()` releases a half-built attempt
  before rebuilding it. Under async rendering the make/destroy calls are queued through
  `SokolCmdQueue`, hence the cache is mutex-guarded and `SokolSystem::shutdown()` clears
  it before `sg_shutdown()` (ids are reused by the next `sg_setup`).
  Vertex and index buffers are still per mesh - two slots each - which is what limits
  the mesh count of a scene today; sharing them per imported model is the open work.
- Orchestration: `engine/core/subsystem/RenderSystem.{h,cpp}`.

## Pass order (all from `RenderSystem::draw()`, `RenderSystem.cpp:10090`)

directional shadow atlas (`ensureShadowAtlas:1847`, its pass starts at `:10119`) -> 2D polar
shadow rows (`ensureShadow2DAtlas:1229`, `loadOccluder2DPass:2203` called at `:10271`)
-> reflection-probe capture (`:10336`, `renderReflectionProbeCapture:1668`) -> per camera:
optional G-buffer (`:10415`, `renderGBufferPass:4741`) + optional depth pre-pass
(`:10450`/`:10503`, `renderDepthPrePass:4576`), SSAO (`:10484`, `renderSSAO:4509`), colour
pass (sky `:10776`, `drawSky:6684` -> opaque `drawMesh:4074` at `:10782` -> UI/points/lines ->
sorted transparents, the `drawTransparent` lambda at `:10940`) -> SSR composite (`:11029`,
`renderSSR:4910`) -> `renderPostProcess:5300` (`:11045`) -> **`renderHDR:5608`** (`:11050`)
-> fixed-res `renderFixedResolutionBlit:5114` (`:11057`) or `renderBlit:5084`
-> **`renderFXAA:5838`** (`:11063`), which presents whatever the previous stage produced.

Two consequences worth internalising: the HDR chain reads the *scene colour* target and is
skipped whenever SSR is on (`:10462`, `updateSwapchainRedirect:9218` - `quantum-bloom`), and an
offscreen pass must declare the pixel format of the target it writes
(`ObjectRender::setRenderTargetColorFormat`), because sokol rejects a pipeline whose
`colors[0]` differs from the pass attachment.

## Two statements to repeat verbatim when they matter

**Lighting is forward.** `mesh.frag` lights the scene. `SokolGBuffer`/
`SokolDeferredLighting` are never instantiated and `SokolDeferredLighting.cpp:24`
loads `deferred_lighting.{vert,frag}`, which do not exist.

**Declared is not implemented.** Many effects ship as a `.frag` file + enum entries +
header declarations with zero definitions and zero call sites (TAA, motion
blur, velocity, volumetric/height
fog, light shafts, VSM, SSGI). Before answering "does Quantum have X", check for a
`RenderSystem::renderX` **definition** and a call in `draw()`. DOF left that list: `renderDOF`
runs CoC -> a half-res separable blur -> composite -> blit back into the scene colour
(`quantum-postprocess`).

**The HDR chain is the exception, and it runs.** `renderHDR` (`RenderSystem.cpp:5608`)
draws bloom threshold -> the mip down/upsample chain -> composite (with the lens dirt, driven
by `Scene::getBloomDirtIntensity`) -> the 1x1 eye-adaptation
meter -> the tone map, and colour grading is folded into that last pass rather than being a
pass of its own (`quantum-bloom`). The 3D LUT is still dead data inside it, and FXAA runs
after the whole chain as the frame's last stage (`quantum-postprocess`).

## Verification

No render test harness exists. Minimum bar: `generate_shaders` re-run, the `quantum`
target compiles with MSVC, and the affected variants compile headless via
`quantum-resource-compiler.exe shaders --out <dir> --backend opengl --shader "<type>:<bits>"`.
GLSL errors otherwise surface at runtime.
For the picture itself there is now a real instrument: `quantum-engine.exe --project <dir>
--window 1280x720 --screenshot out.png --capture-frame 120` writes a PNG of the presented frame
(`engine/core/util/FrameCapture.{h,cpp}`, documented in `quantum-build-verify`). Read it back with
an image tool before claiming a look — "compiles" and "renders what was intended" are separate
proofs. A frame that only the GUI can show is still the user's check (`quantum-safety-rules`).

## Planned work worth citing

`RENDERING_UPGRADE_PLAN.md` and `TODO_ENGINE.md` at the repo root: 3D LUT generation,
clustered forward,
SSGI, procedural sky, GTAO, VSM/better PCF, velocity MRT + material IDs, decals,
auto-LOD. Each follows the 6-step built-in recipe in `quantum-postprocess`. (Histogram
luminance exposure and the mip-chain bloom were on this list and now run - see
`quantum-bloom`.)
