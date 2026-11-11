---
name: doriax-physics
description: Realistic physics in doriax - Box2D 3.x and Jolt behind one PhysicsSystem, Body2D/Body3D/Joint components and shapes, stepping and substeps, CCD, sleeping, friction/restitution, buoyancy, raycasts, contacts and the missing pieces (character controller, ragdoll, vehicles, fluid, debug draw). Use when tuning sim quality, jitter/tunnelling, mass or drag, adding joints, or implementing a physics feature.
---

# Physics

## One subsystem, two worlds

`engine/core/subsystem/PhysicsSystem.h` wraps both engines in a single subsystem:
Box2D `b2WorldId world2D` (`:40`) and Jolt `JPH::PhysicsSystem world3D` (`:51`), with
process-wide Jolt `TempAllocatorImpl` + `JobSystemThreadPool` singletons (`:47-49`,
created at `PhysicsSystem.cpp:53-63`) and mask-based broad/object layer filters
(`:53-55`). Helper `engine/core/util/JoltPhysicsAux.h`. Vendored at
`engine/libs/box2d` and `engine/libs/joltphysics`. Jolt runs its **own** job system, not
`ThreadPoolManager` (`doriax-performance`).

## Stepping

Fixed timestep `1/60` (`engine/core/Engine.cpp:81`) -> accumulator
(`Engine.cpp:1074-1085`, cap 100) -> `Scene::fixedUpdate` (`engine/core/Scene.cpp:568`)
-> `PhysicsSystem::fixedUpdate` (`PhysicsSystem.cpp:2525`):
- Box2D `b2World_Step(world2D, fixedStep, subSteps)` with `subSteps = 4` (`:2565`, declared nearby)
- Jolt `world3D.Update(fixedStep, cCollisionSteps, temp_allocator, job_system)` with
  `cCollisionSteps = 1` (`:2637`)
- Transform write-back at `:2579` (2D) and `:2653` (3D). `PhysicsSystem::update()` is
  deliberately empty (`:2519-2523`) - simulation time only advances on the fixed step.

**Substep counts are hardcoded.** "Realistic" tuning starts here: high-speed 3D
tunnelling is fixed by per-body motion quality, not by raising steps, and a request for
stable stacking of many heavy bodies is a request to expose substeps as a scene setting
(follow the settings path in `doriax-api`).

## Components

| | 2D | 3D |
|---|---|---|
| Body | `Body2DComponent.h:52-64` (type, `HybridArray<Shape2D, MAX_SHAPES>`) | `Body3DComponent.h:70-90` (type, `Body3DMotionQuality` DISCRETE/LINEAR_CAST `:36-39`, gravityFactor, `EAllowedDOFs`, sensor, overrideMassProperties `:73`) |
| Shape | `Shape2D` `:22-50` - pointA/B, radius, vertices/loop, density (area-based, default 1.0 `:36-38`), friction 0.6, restitution 0.0, event flags (hit/contact/preSolve/sensor), categoryBits/maskBits/groupIndex | `Shape3D` `:41-68` - SPHERE/BOX/CAPSULE/TAPERED_CAPSULE/CYLINDER/CONVEX_HULL/MESH/HEIGHTFIELD (`:17-26`), volume density kg/m3 default 1000 (`:56-58`), sources RAW_VERTICES/RAW_MESH/ENTITY_MESH/ENTITY_HEIGHTFIELD (`:28-34`) |
| Joint | `Joint2DComponent.h:23-38` DISTANCE/REVOLUTE/PRISMATIC/MOUSE/WHEEL/WELD/MOTOR (PULLEY/GEAR commented out `:15-16`) | `Joint3DComponent.h:27-71` FIXED/DISTANCE/POINT/HINGE/CONE/PRISMATIC/SWINGTWIST/SIXDOF/PATH/GEAR/RACKANDPINON/PULLEY with limits, cone angles, gear teeth, path points |

Wrappers: `engine/core/object/physics/{Body2D,Body3D,Joint2D,Joint3D,Contact2D,
Contact3D,Manifold2D,CollideShapeResult3D}.h`. Caps: `MAX_SHAPES 10`,
`MAX_SHAPE_POINTS_2D 16`, `MAX_SHAPE_VERTICES_3D 256`, `MAX_SHAPE_INDICES_3D 768`,
`MAX_BROADPHASELAYER_3D 6` (`engine/core/Engine.h:106-122`) - and note `HybridArray`
grows silently in editor builds (`doriax-performance`).

## Script surface (Lua)

`Body2D` `binding/ObjectClassesLua.cpp:1034-1148` - create*Shape, position/angle/
linearVelocity/linearDamping/angularDamping, `enableSleep`/`awake`, fixedRotation,
**`bullet` = 2D CCD** (`:1109-1112`), gravityScale, filters, applyForce/Torque/
LinearImpulse. `Body3D` `:1196-1273` - `motionQuality` `:1234`, sensor `:1238`,
`allowSleeping` `:1250`, body-level friction/restitution `:1251-1252`,
collisionGroupID/bits filter `:1246-1249`, `applyBuoyancyImpulse` `:1272`
(impl `engine/core/object/physics/Body3D.cpp:823`). Joints `:1152-1165` (2D),
`:1288-1292` (3D). Contact/event hooks are properties on the subsystem
(`binding/ECSClassesLua.cpp:172-212`): gravity, gravity2D/3D, `pointsToMeterScale2D`,
lock3DBodies, beginContact2D/endContact2D/hitContact2D/preSolve2D/shouldCollide2D,
onBodyActivated3D/onContactAdded3D; get it with `Scene.getPhysicsSystem`
(`CoreClassesLua.cpp:694`). Raycasts: `engine/core/math/Ray.cpp` (`Ray.intersects` over
AABB/OBB/Plane/Sphere/Body2D/Body3D/Scene `:427-546`; Box2D `b2World_CastRay` `:487`,
Jolt `NarrowPhaseQuery().CastRay` `:521,:578`), Lua `Ray` at `MathClassesLua.cpp:593-619`,
`Object.getRay`/`Camera.screenToRay` (`ObjectClassesLua.cpp:226,276`).

## Scale: the trap that ruins realism

`pointsToMeterScale2D` defaults to **64** (`PhysicsSystem.cpp:272`) and is **not
serialized** to scene YAML. Density semantics differ by world too: 2D uses area-based
density default 1.0, 3D uses kg/m3 default 1000. Any "floats like wood / sinks like
stone" request must be answered in those units, and 2D+3D share one subsystem, so a
scale change is global.

## What does not exist (say so before promising)

No character-controller, ragdoll, vehicle, soft-body or fluid component in the engine -
the only character controller is a demo script
(`templates/charactercontrol-main/scripts/CharacterController.h:10`). No overlap/shape-
cast queries (grep `CastShape` finds none). No tunable substeps. No physics debug
rendering: **`PhysicsSystem::draw()` is empty** (`PhysicsSystem.cpp:2680-2682`) and there
is no DebugDrawer anywhere in engine or editor. Authoring is enum rows in
`editor/window/Properties.cpp` (Joint3D types `:294-306`) with `editor/Catalog.cpp`
getters (`:535-578`, `:858`, `:951`, resolver rows `:2891-2894`); add-per-scene in
`editor/window/Structure.cpp:1628-1631`. So "visualise colliders" is a real feature
request, and a debug draw is the natural first deliverable (it also makes every other
physics fix verifiable).

## Editor persistence

Scene defaults `engine/core/SceneSettings.h:101-104` (`gravity2D`/`gravity3D` default
-9.81); YAML write `editor/Stream.cpp:2162-2163`, read `:2269-2270`.
