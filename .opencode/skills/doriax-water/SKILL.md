---
name: doriax-water
description: Build water in doriax - there is no water system today, so this is the assembly plan from existing parts (Gerstner surface shader, SSR/probe reflection, depth-based refraction, foam splatting like the terrain blend map, Body3D buoyancy). Use when the user asks for water, lakes/ocean, floating bodies, buoyancy, or underwater effects.
---

# Water

## Starting point: nothing exists

`grep -rni water engine/ editor/` hits only vendored third-party sources
(Jolt, tinyxml2, the iOS ads framework) plus unrelated AI-service strings. No
WaterComponent, no water shader, no `ShaderType`, no settings field, no scene key.
So "create water" is a **new feature**, not a config task - say so and scope it.

## Parts you can reuse (all verified)

| Need | Existing piece | Anchor |
|---|---|---|
| Large tessellated surface | CDLOD terrain: instance-node morphing, crack-free LOD | `engine/shaderlib/includes/terrain_vs.glsl` `morphVertex`/`getTerrainPosition`, `TerrainComponent.h:59` |
| Surface displacement | forward `mesh.vert` + variant bits | `editor/shader/ShaderBuilder.cpp:446-473`, bits computed `RenderSystem.cpp:2422-2455` |
| Reflection | SSR (+`ssr_blur`, `composite`) and capture probes with box projection | `renderSSR:3709`, `renderReflectionProbeCapture:1347`, box projection `:1270` |
| Refraction / depth | depth pre-pass + G-buffer | `renderDepthPrePass:3396`, `renderGBufferPass:3550` |
| Foam / texture blending | terrain 3-channel splat | `includes/terrain_fs.glsl` `getTerrainColor` (`u_blendMap` r/g/b + backTextureAmount) |
| Floating bodies | per-body fluid impulse, already bound to Lua | `engine/core/object/physics/Body3D.cpp:823`, `binding/ObjectClassesLua.cpp:1272` |
| Transparency sorting | transparents drawn sorted after opaque | `RenderSystem.cpp:7366` |
| Waves need noise | **no Perlin/simplex anywhere** | see `doriax-procedural-noise` |

## Suggested build order (each step independently useful)

1. **Mesh + material.** A subdivided plane entity with a project-forked shader is the
   fastest visible result: author `water.frag`/`water.vert` in the project and use the
   user post-process/variant machinery. Do not add a `ShaderType` until the look is
   agreed - a new type forces `setupShaderArgs`, new variant bits and a cache-version
   bump (`doriax-shaders`).
2. **Surface motion.** Sum-of-Gerstner in a new `includes/water.glsl`, normals
   computed analytically from the same sum (not finite differences), plus a distance
   edge fade so the plane has no hard boundary. Reuse the terrain morphing trick if the
   plane needs to be huge.
3. **Reflection.** Screen-space first (SSR is already wired and handles the common
   case), then probe-based for off-screen content. Planar reflection would be a new
   capture pass - the pattern to copy is `renderReflectionProbeCapture`.
4. **Refraction.** Screen-space UV offset from the depth texture using the surface
   normal; this is why the depth pre-pass matters, and it is already available.
5. **Foam.** Copy the terrain splat idea: a blend/foam texture sampled in the water
   shader, driven by a `blendMap`-style uniform set on the component.
6. **Gameplay.** A `WaterVolumeComponent` (axis-aligned region) whose script/Lua
   surface queries depth at an entity position and calls the existing
   `Body3D.applyBuoyancyImpulse(surfacePosition, surfaceNormal, buoyancy, linearDrag,
   angularDrag, fluidVelocity, gravity, deltaTime)`. Do **not** invent a fluid solver:
   Jolt/Box2D are wired as one `PhysicsSystem` (`PhysicsSystem.h:40` `b2WorldId world2D`,
   `:51` `JPH::PhysicsSystem world3D`) and there is no fluid simulation in the repo.
7. **Underwater look.** Post-process only: fork `postprocess.frag` for tint + blur +
   caustic scroll; trigger it from a script on camera-entry.

A new component with a persisted, editor-editable, Lua-visible property requires the
8-file checklist in `doriax-api` - the touchpoints are hand-maintained and missing one
makes the feature invisible rather than broken.

## Physics scale trap

2D/3D share one subsystem and `pointsToMeterScale2D` defaults to **64**
(`PhysicsSystem.cpp:272`, event hook exposed in `ECSClassesLua.cpp:172-212`) and is
**not serialized** to scene YAML. Buoyancy math mixes world units and metres, so
decide the scale convention before writing the impulse code.
