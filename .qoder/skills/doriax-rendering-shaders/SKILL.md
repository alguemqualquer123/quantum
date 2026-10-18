---
name: doriax-rendering-shaders
description: Superseded entry point for doriax rendering work. Use when a task previously routed here; the content now lives in doriax-rendering plus the specialist skills doriax-shaders, doriax-lighting, doriax-postprocess, doriax-bloom, doriax-volumetrics, doriax-water, doriax-terrain and doriax-procedural-noise.
---

# Superseded: use the rendering skills

This skill was merged and corrected. Doriax lighting is **forward only** - the earlier
claim of "deferred/forward passes" here was wrong: `SokolGBuffer`/
`SokolDeferredLighting` are never instantiated and
`engine/renders/sokol/SokolDeferredLighting.cpp:24` loads
`deferred_lighting.{vert,frag}`, which do not exist.

Read instead:

- `doriax-rendering` - layering, `Render.h` vocabulary, the real pass order in
  `RenderSystem::draw()`, and the declared-but-unwired list.
- `doriax-shaders` - shaderlib, variant bits, `generate_shaders` embedding, the `.sdat`
  cache, async compile aborts.
- `doriax-lighting`, `doriax-postprocess`, `doriax-bloom`, `doriax-volumetrics`,
  `doriax-water`, `doriax-terrain`, `doriax-procedural-noise` - one per specialty.
- `doriax-build-verify` for the build/verify commands.
