---
name: quantum-postprocess
description: Add or change a Quantum post-process effect - the working user postProcess chain (forked postprocess.frag + u_fs_postParams uniforms), the SSAO/SSR/DOF passes, the built-in pass recipe, and the FXAA pass that closes the frame. Use when the user asks for a screen effect, color grading, depth of field, anti-aliasing, SSR/SSAO changes, or wonders why TAA/volumetric files do nothing.
---

# Post-processing

## Reality check first (read this before promising an effect)

**Running today**: SSAO (`ssao.frag`, `renderSSAO:4561`), SSR + glossy blur +
composite (`ssr.frag`/`ssr_blur.frag`/`composite.frag`, `renderSSR:4962`,
`renderGBufferPass`), depth pre-pass (`renderDepthPrePass`), **DOF**
(`dof_coc.frag`/`dof_blur.frag`/`dof_composite.frag`, `loadDOF:5920`,
`ensureDOFFramebuffers:5992`, `renderDOF:6040` - see the DOF section), fixed-res blit
(`renderFixedResolutionBlit:5166`), per-pixel fog, the **generic user chain**
(`renderPostProcess:5352`), the **HDR chain** (`renderHDR:5660` - bloom mip chain, bloom
lens dirt, eye adaptation, tone map and colour grading; see `quantum-bloom`), and
**FXAA** (`renderFXAA:5890`, the last stage of the frame - see below).

**Not wired**: TAA, motion blur,
velocity, volumetric fog, height fog, light shafts, VSM, SSGI, and the 3D LUT path. For those, the
`shaderlib/*.frag` files (`taa`,
`motionblur`, `lightshafts`, `volumetric_fog_composite`, ...), the `ShaderType` entries
(`engine/core/render/Render.h:57-67`), the `UniformBlockType` entries (`:179-192`), the
texture slots (`:245+`), and the
`SceneSettings.h:91-116` fields (bloom, tone map, exposure, grading) exist - but there is
**no `RenderSystem` declaration, definition or call site for any of them** (every
`render*` in `RenderSystem.h` now has a body: `renderBlit`, `renderDOF`, `renderDepthPrePass`,
`renderFXAA`, `renderFixedResolutionBlit`, `renderGBufferPass`, `renderHDR`,
`renderPostProcess`, `renderReflectionProbeCapture`, `renderSceneCopy`, `renderSSAO`,
`renderSSR`), and
`setupShaderArgs` maps no case to them. The half-written versions are preserved in commits `2db29aa6`
and `5e7e2a50` - read those before writing a pass from scratch. Do not tell the user
an effect "exists"; say it is declared-but-unwired and quote this file.

Order matters and is fixed by `RenderSystem::draw`: scene colour -> **DOF** (`:11372`) -> the
SSR composite (`:11384`) -> the scene-target blit (`:11391`/`:11394`) -> the **user
chain** (`:11400`) -> the HDR chain (`:11405`, only when `Scene::isHdrEnabled()` and SSR is
off) -> fixed-res blit (`:11412`) -> FXAA (`:11418`). A user pass therefore runs *after* DOF
but *before* bloom, adaptation and
tone mapping, on the still-linear-ish scene target: anything written here that assumes
display-referred pixels (an exposure multiply, a curve, a grade) is applied twice or lands in
the wrong space. Put colour work in the tone map instead.

## FXAA: what a "last stage of the frame" pass costs

`scene.fxaaEnabled` is independent of `scene.hdrEnabled` - anti-aliasing the finished image
needs the image in a texture, so the pass also forces the swapchain redirect
(`updateSwapchainRedirect`: it calls `loadBlit()` + `loadFXAA()` + both framebuffers, because
a scene with no bloom, no tone map and no post chain still has to move its colour buffer into
the AA target). The precedence is decided once, in `draw()`: whoever *would have presented*
is redirected into `fxaaFramebuffer`, and `renderFXAA(fxaaSource)` presents instead. Two
traps found while wiring it:

- **Fixed resolution owns the last slot.** With `useFixedRes` the upscale is the last stage,
  so the chain keeps writing the fixed-size buffer and the blit runs *into* the AA target
  (`renderFixedResolutionBlit(&fxaaFramebuffer.getRender())`); overriding the destination
  unconditionally makes the tone map write a display-sized target the blit then overwrites.
- **The AA target must be LINEAR-filtered.** The along-edge taps land between texels, so with
  NEAREST they collapse onto the very pixels the pass is trying to blend.
- It reads RGBA8, so it is the one quality pass with **no pipeline-format hazard**: the
  `SokolObject::rttColorFormat` single-format constraint (see `quantum-bloom`) does not apply.

Headless proof: `Tools/lua/probe_fxaa.lua` (8 checks + `FXAA ready` in the log, and it flips
the redirect twice - on, off, on - to exercise both branches).

## DOF: what a depth-reading pass costs

`scene.dofEnabled` runs CoC -> separable blur H -> blur V -> composite (`renderDOF:6040`). The
CoC and both blur axes run at **half** the scene resolution (`ensureDOFFramebuffers:5992`,
`DOF_BLUR_TAPS 9` in `editor/shader/ShaderBuilder.cpp:803`); the composite mixes the blurred copy
over the sharp scene at full size and the result is blitted back into the buffer the colour pass
was drawing into, so every stage after it reads the same buffer it always did. The radius travels
in the tap *spacing*, so `dofMaxBlur` is free and the tap count is the whole cost knob.

Three traps found while wiring it:

- **A pass that draws with `PIP_DEPTH` needs `needDepthPass` to say so.** `loadMesh` only builds
  the depth fork for `(hasShadows && castShadows) || isSSAOEnabled() || needPrepass ||
  hasWaterDepth || isDOFEnabled()` (`RenderSystem.cpp:3086`). Leave a consumer out and
  `SokolObject::beginDraw` answers false (`depth_pip` is still `{0}`), `drawMeshDepth` sets
  `mesh.needReload`, and the scene tears down and rebuilds **every mesh every frame while drawing
  none of them**: measured `meshLoadsPerFrame`/`meshDropsPerFrame` 16.00 with `avgDrawCalls` 7
  (the fullscreen passes only) against 39 once DOF joined the list. `Scene::setDOFEnabled`
  therefore calls `needReloadMeshes()`, like `setSSAOEnabled`/`setSSREnabled`.
- **The depth has to exist before the CoC reads it**, and the DOF gate is the last of the pre-pass
  decisions (`RenderSystem.cpp:10814-10842`): it reuses `depthPrePassRendered` when SSAO, the post
  chain or water already drew one, otherwise it runs its own. SSR is excluded - its composite
  writes the destination, not the scene colour. Like FXAA it forces the swapchain redirect
  (`updateSwapchainRedirect:9530`), so an exported build with nothing else offscreen still
  gets it.
- **A new `ShaderType` needs three name tables, not one.** `ShaderPool::getShaderTypeName` (the
  `.sdat` stem), `ShaderPool::parseShaderTypeToken` (the runtime reverse map) and
  `editor/cli/CommandLine.cpp:160-185` (the `--shader` token) are independent. A stem must contain
  **no underscore**: `getShaderCliSpec` splits `"name_properties"` on the first one, so DOF
  compiles as `dofcoc`, `dofblur`, `dofcomposite`.

Headless proof: `%TEMP%/DofLive/lua/dof_probe.lua` (9 checks, `DOF ready` in the log, and a `dof`
row in `slowestSystems` that is absent when `dofEnabled` is off). 16 cubes at 640x360, Debug,
`--frames 120 --warmup 30`: off 18 draws / 194 tris, on 39 / 391 (`dof` 0.070 ms), on plus the
scene's own pre-pass 55 / 583. The blur itself is **not** pixel-verified: the runner has no
framebuffer readback (`GraphicUtils::saveFramebufferImage` is editor-only), so the image needs the
editor or a generated game build.

## The cheap path: the user chain (no engine change)

`SceneSettings::PostProcessPass { shader, enabled, uniforms }`
(`engine/core/SceneSettings.h:36-40`) is a list of forked fullscreen shaders run
after scene colour, ping-ponged between two framebuffers.

- Uniforms are **reflected from the shader's `u_fs_postParams` block**
  (`RenderSystem.cpp:3998-4033`; `getUniformBlockMembers` `:4009`,
  `getUniformBlockIndexByName` `:4011`). Renamed/removed members drop silently and
  unset members stay zero - so a pass can be edited without touching C++.
- Built-in `postprocess.frag:29` is a passthrough; copy it into the project as a fork.
- YAML key `postProcess` (`editor/Stream.cpp:2181-2188` write, `:2297-2311` read);
  UI `drawScenePostProcess` (`editor/window/Properties.cpp:5215`, undo key
  `"post_process"` `:5210`); scene-factory export `Factory.cpp:2384-2396`;
  precompile keys `editor/Project.cpp:2654-2670`.
- Framebuffers are recreated on demand (`RenderSystem.cpp:4051-4077`) and the chain
  never targets the swapchain - it blits (`:3993`).

This is the right place to prototype a new look: author GLSL, add a pass in the
editor's Post Process panel, tune it, and only promote it to a built-in pass if it
needs depth, multiple RTs, or several sub-passes.

## Driving the chain from Lua (what the kit components hit)

`Scene:setPostProcessUniform(index, name, value)` has four overloads
(`binding/CoreClassesLua.cpp:1009-1013`: Vector4/Vector3/Vector2/float) and all of them funnel into
the Vector4 one, `Scene::setPostProcessUniform` (`engine/core/Scene.cpp:663-671`). Three
consequences, measured while writing `engine/components/ScreenFade.lua`:

- **It returns `void` and logs an error per call** when `index >= settings.postProcess.size()`:
  `Post-process pass %u does not exist, cannot set uniform '%s'` (`:665`). There is no way to ask
  first from the call itself, so gate on `#scene.postProcessPasses` (bound at `:1008`) or you get
  one `(ERROR)` line every frame.
- **A name that is not in the pass's reflected `u_fs_postParams` block drops silently** -
  `ShaderUniforms::set` returns false at `:669` and nothing is logged. A misspelled uniform looks
  exactly like an effect that does nothing.
- **The scalar overloads zero the other components**: `float` becomes `(v,0,0,0)`, `Vector2`
  becomes `(x,y,0,0)`. Reading back with `Scene:getPostProcessUniform` (`:685-689`) is safe - it
  answers `Vector4::ZERO` for a missing pass instead of erroring - but a shader member declared
  `vec4` will get zeroes in `yzw`, so a colour/fade uniform needs the Vector4 overload.

`engine/components/` has `ScreenFade` (fade/blackout/flash by an **Image** entity, or by a float
uniform) and `PostEffectPulse` (breathes a float uniform, or follows a `Health`) built on exactly
this. Both are float-only by design: the user chain cannot be extended with new passes from Lua, so
nothing here is a TAA/volumetric promise - those remain declared-but-unwired, while bloom,
tone mapping, adaptation, grading, DOF and FXAA are engine passes documented in `quantum-bloom`
and above.

## The built-in path: 6 steps, copy SSR

An effect becomes a real pass only when all six are done:

1. Field(s) in `engine/core/SceneSettings.h` (defaults live **only** there).
2. `Scene` delegating setter that calls the `needReload*` hook when the change
   requires a rebuild (`needReloadPostProcess:3948` is the model).
3. `ShaderType` + `UniformBlockType` entries in `Render.h` **and** a matching case in
   `ShaderBuilder::setupShaderArgs` (`:661-740`) that names the entry files and
   constants, plus variant bits in the bit->define tables (`:446-515`).
4. Framebuffer allocation + `load*()`/`render*()` **definitions** in
   `RenderSystem.cpp` and a call in `draw()` at the right point in the chain
   (`:6812+`). Declarations in `RenderSystem.h` are not enough - that is exactly how
   the unwired effects got left behind.
5. YAML key + editor row (`editor/Stream.cpp`, `editor/window/Properties.cpp`).
6. Bump the shader cache version dir (`App.cpp:2768`, history `:2732-2767`) and
   re-run `generate_shaders` (`quantum-shaders`).

Then verify the variants compile headless:
`quantum-resource-compiler.exe shaders --out <dir> --backend opengl --shader "..."`.
