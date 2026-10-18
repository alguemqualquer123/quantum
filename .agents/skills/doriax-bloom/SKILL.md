---
name: doriax-bloom
description: Implement or debug bloom in doriax - currently declared but not wired. Use when the user asks for bloom, glow, HDR emission, light bleeding, bright-pixel thresholding, mip downsample/upsample chains, or asks why bloom_*.frag files have no effect.
---

# Bloom

## Status: nothing runs

Four shader files exist (`engine/tools/shaderlib/bloom_threshold.frag`,
`bloom_downsample.frag`, `bloom_upsample.frag`, `bloom_composite.frag`), as do the
enum entries (`Render.h:57-60` `BLOOM_THRESHOLD/DOWNSAMPLE/UPSAMPLE/COMPOSITE`),
the uniform blocks (`:170-173`), the layer slots (`:232+` `BLOOMTEXTURE0/1/...`), the
settings fields (`SceneSettings.h:74-99`) and **declarations only** in
`RenderSystem.h:600 loadBloom()` / `:604 renderBloom(...)`. Grep for
`RenderSystem::renderBloom|loadBloom` in any `.cpp` returns nothing, no pass calls
them, and `setupShaderArgs` maps no case to those `ShaderType`s. Never claim bloom
ships today; the WIP is in commits `2db29aa6` / `5e7e2a50` - read them first.

## What the design constrains

Bloom needs values above 1.0. Today the colour pass renders into the scene
framebuffer and a fixed-resolution blit (`RenderSystem.cpp:3881`) presents it; there
is no HDR (RGBA16F) scene target and no tonemap pass (`tonemap.frag` is unwired too).
So a threshold on the current target is really *luminance-based glare from LDR
colour*, which looks muddy. Decide explicitly which of these you are doing:

- **A. Prototype in the user chain (fast, no engine edit).** Fork
  `postprocess.frag` into the project and do threshold + separable/gaussian blur +
  additive composite in one pass at quarter/half resolution. Uniforms come from
  `u_fs_postParams` reflection, so tuning needs no rebuild. Use this to prove the
  look and get the user's read before spending engine effort.
- **B. Real HDR bloom (the durable answer).** Requires, in order: an HDR scene
  target, an emissive term in `mesh.frag`/`pbr.glsl` feeding it, threshold pass ->
  5-6 mip downsample ping-pong -> tent upsample -> additive composite **before**
  tone mapping, then a `tonemap.frag` pass, then the existing exposure/blit tail.
  `RENDERING_UPGRADE_PLAN.md` already specifies the mip-chain and histogram-exposure
  half of this; `dof_blur.frag` and `ssao_blur.frag` show the repo's blur style.
- **C. Promote A to a built-in pass** only when it needs depth or multiple RTs. Then
  follow the 6-step recipe in `doriax-postprocess` - note step 4 is the one everyone
  forgets: without `load*`/`render*` **definitions** plus a `draw()` call site, the
  enum entries alone produce exactly the silent no-op that bloom is today.

## Gotchas specific to this pass

- A mip chain means new framebuffers per level; follow the allocation/resize pattern
  in `RenderSystem.cpp:4051-4077` and the `ensureShadowAtlas:1526` "allocate once,
  recreate on size change" style rather than allocating in `draw()`.
- New `ShaderType`s change `ShaderKey` composition -> bump the `.sdat` cache version
  dir (`App.cpp:2768`) or developers keep seeing stale compiles.
- `generate_shaders` must run before any shader-side change is visible
  (`doriax-shaders`).
- Verify headless:
  `doriax-resource-compiler.exe shaders --project <dir> --backend opengl --shader "<type>:<bits>"`
  for each new type. Visual confirmation must come from the user running the editor -
  agents are forbidden from launching `doriax-editor.exe`.
