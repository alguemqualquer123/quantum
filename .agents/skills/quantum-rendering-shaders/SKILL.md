---
name: quantum-rendering-shaders
description: Superseded entry point for Quantum rendering work. Use when a task previously routed here; the content now lives in quantum-rendering plus the specialist skills quantum-shaders, quantum-lighting, quantum-postprocess, quantum-bloom, quantum-volumetrics, quantum-water, quantum-terrain and quantum-procedural-noise.
---

# Superseded: use the rendering skills

This skill was merged and corrected. Quantum lighting is **forward only** - the earlier
claim of "deferred/forward passes" here was wrong: `SokolGBuffer`/
`SokolDeferredLighting` are never instantiated and
`engine/renders/sokol/SokolDeferredLighting.cpp:24` loads
`deferred_lighting.{vert,frag}`, which do not exist.

Read instead:

- `quantum-rendering` - layering, `Render.h` vocabulary, the real pass order in
  `RenderSystem::draw()`, and the declared-but-unwired list.
- `quantum-shaders` - shaderlib, variant bits, `generate_shaders` embedding, the `.sdat`
  cache, async compile aborts.
- `quantum-lighting`, `quantum-postprocess`, `quantum-bloom`, `quantum-volumetrics`,
  `quantum-water`, `quantum-terrain`, `quantum-procedural-noise` - one per specialty.
- `quantum-build-verify` for the build/verify commands.
