---
name: quantum-volumetrics
description: Fog, height fog, haze, mist, smoke and light shafts in Quantum - what actually runs (per-pixel exponential fog) versus the unwired volumetric shaders, plus the depth/HIZ sources a raymarched pass needs and the particle path for smoke. Use when the user asks for fog, fumaca, atmosphere, god rays, or atmospheric scattering.
---

# Fog, smoke and volumetrics

## What runs

Per-pixel **forward** fog only: `FogComponent` LINEAR/EXP/EXP2 evaluated at
`RenderSystem.cpp:982-1003`, applied in `engine/shaderlib/includes/fog.glsl:7`
`getFogColor()` (uniform block `u_fs_fog`: `color_type` xyz+type,
`density_start_end`), gated by mesh variant bit 12 `HAS_FOG`/'Fog'
(`editor/shader/ShaderBuilder.cpp:459`), authored in YAML at
`editor/Stream.cpp:5092-5117`. It is a colour blend per fragment - it has no light
scattering, no height falloff, and it does not affect the sky unless you handle it in
`sky.frag`.

## What is written but dead

`height_fog.frag`, `volumetric_fog_inject.frag`, `volumetric_fog_scattering.frag`,
`volumetric_fog_composite.frag`, `lightshafts.frag`, `ssgi.frag` exist in
`engine/shaderlib/`, and their `ShaderType`/`UniformBlockType` entries exist in
`engine/core/render/Render.h`, but **no definitions and no call sites** exist and
`setupShaderArgs` (`ShaderBuilder.cpp:661-740`) maps no case to them. Read the
orphan `.frag` files and commits `2db29aa6` / `5e7e2a50` before authoring new GLSL -
the intended interface is probably already drafted. Never report these as features.

## Ingredients a volumetric pass needs (all of them already exist)

| Need | Existing source |
|---|---|
| Scene depth | depth pre-pass `renderDepthPrePass` (`RenderSystem.cpp:3396`), `depth.frag`, `velocity.frag` |
| G-buffer (normal/depth for SSR) | `renderGBufferPass:3550`, gated `isSSREnabled():7081-7084` |
| Hierarchical depth for cheap ray steps | `hiz_gen.frag`, `ssr_hiz.frag` |
| Shadow-map depth for shafts | shadow atlas `ensureShadowAtlas:1526`, spot/point atlases; PCSS in `includes/pcss.glsl` |
| Multi-RT ping-pong + resize pattern | `RenderSystem.cpp:4051-4077` (post-process framebuffers), `ensureShadowAtlas` style allocation |
| Fullscreen-quad entry | `fullscreen.vert` + `blit.frag`/`composite.frag` |

The `volumetric_fog_{inject,scattering,composite}` trio implies the intended design is
a 3-pass density-injection -> one-slice scattering -> accumulation-composite chain.
That needs a new 3D texture (or a 2D slab with epipolar lines) allocated in
`RenderSystem`, which is real engine work: follow the 6-step built-in recipe in
`quantum-postprocess` and do not skip step 4 (definitions + a `draw()` call).

## The cheap answers, worth offering first

1. **Fog colour/density/exponent animation from Lua** - free, already supported;
   reaches the PhysicsSystem/Scene settings, not the renderer.
2. **Smoke as sprites**: `ParticlesComponent` (`engine/core/component/ParticlesComponent.h:234`,
   `emitter` flag `:242`) is a full CPU particle system with initializers, modifiers
   and gradients, rendered through the points shaders. Soft alpha-blended billboards
   with a noise-scrolling UV give convincing smoke today with zero render changes.
3. **Height fog as a forward term**: extend `fog.glsl`'s `getFogColor` to fold
   world-Y into the density (fragment already has access to view position) - one
   include file, one variant, no new pass. Cheaper than the orphan `height_fog.frag`
   full-screen pass, and it composes correctly with transparent sorting.
4. **Screen-space shafts**: `lightshafts.frag` radial-blur style can be prototyped as
   a user post-process pass (`u_fs_postParams` reflection means no C++ rebuild) -
   see `quantum-postprocess`. Prove the look there, then decide whether to promote it.

## Order of work when the user wants "real" volumetric fog

Agree the resolution/step budget first (a half-res 64-step raymarch is the usual
floor), then: settings fields -> framebuffer allocation -> the 3 passes -> light
contribution from the shadow atlas -> editor rows -> cache version bump -> headless
variant compile. Ask the user to do the visual pass; agents must not launch
`quantum-editor.exe`.
