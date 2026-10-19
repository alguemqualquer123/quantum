---
name: doriax-postprocess
description: Add or change a doriax post-process effect - the working user postProcess chain (forked postprocess.frag + u_fs_postParams uniforms), the SSAO/SSR passes, and the built-in pass recipe. Use when the user asks for a screen effect, color grading, anti-aliasing, SSR/SSAO changes, or wonders why bloom/DOF/tonemap files do nothing.
---

# Post-processing

## Reality check first (read this before promising an effect)

**Running today**: SSAO (`ssao.frag`, `renderSSAO:3329`), SSR + glossy blur +
composite (`ssr.frag`/`ssr_blur.frag`/`composite.frag`, `renderSSR:3709`,
`renderGBufferPass:3550`), depth pre-pass (`:3396`), fixed-res blit (`:3881`),
per-pixel fog, and the **generic user chain** (`renderPostProcess:4087`).

**Not wired**: bloom, tonemap, exposure, colour grading, DOF, TAA, FXAA, motion blur,
velocity, volumetric fog, height fog, light shafts, VSM, SSGI. For those, the
`shaderlib/*.frag` files, the `ShaderType` entries (`engine/core/render/Render.h:57-78`),
the `UniformBlockType` entries (`:170-184`), the texture slots (`:232+`), the
`SceneSettings.h:74-99` fields, and the `RenderSystem.h:444-614` **declarations** all
exist - but there is **no definition and no call site anywhere**, and
`setupShaderArgs` maps no case to them. Verified: `grep renderBloom engine/` hits only
`RenderSystem.h:604`. The half-written versions are preserved in commits `2db29aa6`
and `5e7e2a50` - read those before writing a pass from scratch. Do not tell the user
an effect "exists"; say it is declared-but-unwired and quote this file.

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
   re-run `generate_shaders` (`doriax-shaders`).

Then verify the variants compile headless:
`doriax-resource-compiler.exe shaders --out <dir> --backend opengl --shader "..."`.
