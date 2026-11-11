---
name: quantum-physics
description: Realistic physics in Quantum - Box2D 3.x and Jolt behind one PhysicsSystem, Body2D/Body3D/Joint components and shapes, stepping and substeps, CCD, sleeping, friction/restitution, buoyancy, raycasts, contacts and the missing pieces (character controller, ragdoll, vehicles, fluid, debug draw). Use when tuning sim quality, jitter/tunnelling, mass or drag, adding joints, or implementing a physics feature.
---

# Physics

## One subsystem, two worlds

`engine/core/subsystem/PhysicsSystem.h` wraps both engines in a single subsystem:
Box2D `b2WorldId world2D` (`:40`) and Jolt `JPH::PhysicsSystem world3D` (`:51`), with
process-wide Jolt `TempAllocatorImpl` + `JobSystemThreadPool` singletons (`:47-49`,
created at `PhysicsSystem.cpp:110-121`) and mask-based broad/object layer filters
(`:53-55`). Helper `engine/core/util/JoltPhysicsAux.h`. Vendored at
`engine/libs/box2d` and `engine/libs/joltphysics`. Jolt runs its **own** job system, not
`ThreadPoolManager` (`quantum-performance`).

## Stepping

Fixed timestep `1/60` (`engine/core/Engine.cpp:81`) -> accumulator
(`Engine.cpp:1074-1085`, cap 100) -> `Scene::fixedUpdate` (`engine/core/Scene.cpp:568`)
-> `PhysicsSystem::fixedUpdate` (`PhysicsSystem.cpp:3030`):
- Box2D `b2World_Step(world2D, fixedStep, subSteps)` with `subSteps = 4` at `:3070-3071`
- Jolt `world3D.Update(fixedStep, cCollisionSteps, temp_allocator, job_system)` with
  `cCollisionSteps = 1` declared `:3132`, call `:3143`
- Transform write-back at `:3091` (2D) and `:3159` (3D), plus the same call in
  `interpolate()` (`:3164-3190`, body helpers `:719`/`:745`). `PhysicsSystem::update()` is
  deliberately empty (`:3024-3029`) - simulation time only advances on the fixed step.
  Both paths skip only bodies flagged `followingTransform`, so **a script that writes an entity
  transform must not own a dynamic body**: the write-back overwrites it on the next step.

**Substep counts are hardcoded.** "Realistic" tuning starts here: high-speed 3D
tunnelling is fixed by per-body motion quality, not by raising steps, and a request for
stable stacking of many heavy bodies is a request to expose substeps as a scene setting
(follow the settings path in `quantum-api`).

## Components

| | 2D | 3D |
|---|---|---|
| Body | `Body2DComponent.h:52-64` (type, `HybridArray<Shape2D, MAX_SHAPES>`) | `Body3DComponent.h:70-90` (type, `Body3DMotionQuality` DISCRETE/LINEAR_CAST `:36-39`, gravityFactor, `EAllowedDOFs`, sensor, overrideMassProperties `:73`) |
| Shape | `Shape2D` `:22-50` - pointA/B, radius, vertices/loop, density (area-based, default 1.0 `:36-38`), friction 0.6, restitution 0.0, event flags (hit/contact/preSolve/sensor), categoryBits/maskBits/groupIndex | `Shape3D` `:41-68` - SPHERE/BOX/CAPSULE/TAPERED_CAPSULE/CYLINDER/CONVEX_HULL/MESH/HEIGHTFIELD (`:17-26`), volume density kg/m3 default 1000 (`:56-58`), sources RAW_VERTICES/RAW_MESH/ENTITY_MESH/ENTITY_HEIGHTFIELD (`:28-34`) |
| Joint | `Joint2DComponent.h:23-38` DISTANCE/REVOLUTE/PRISMATIC/MOUSE/WHEEL/WELD/MOTOR (PULLEY/GEAR commented out `:15-16`) | `Joint3DComponent.h:27-71` FIXED/DISTANCE/POINT/HINGE/CONE/PRISMATIC/SWINGTWIST/SIXDOF/PATH/GEAR/RACKANDPINON/PULLEY with limits, cone angles, gear teeth, path points |

Wrappers: `engine/core/object/physics/{Body2D,Body3D,Joint2D,Joint3D,Contact2D,
Contact3D,Manifold2D,CollideShapeResult3D}.h`. `Ray.cast` and `Ray.overlaps` now provide
circle/sphere shape casts and overlap queries for both worlds. Caps: `MAX_SHAPES 10`,
`MAX_SHAPE_POINTS_2D 16`, `MAX_SHAPE_VERTICES_3D 256`, `MAX_SHAPE_INDICES_3D 768`,
`MAX_BROADPHASELAYER_3D 6` (`engine/core/Engine.h:106-122`) - and note `HybridArray`
grows silently in editor builds (`quantum-performance`).

## Script surface (Lua)

`Body2D` `binding/ObjectClassesLua.cpp:1236-1360` (`deriveClass<Body2D, EntityHandle>` `:1236`) -
create*Shape `:1241`, position/angle/linearVelocity/**linearDamping+angularDamping `:1316-1317`**,
`enableSleep`/`awake`, fixedRotation, **`bullet` = 2D CCD** (`:1321`), `type` `:1322`, gravityScale,
filters, applyForce/Torque/LinearImpulse. `Body3D` `:1405-1490`
(`deriveClass<Body3D, EntityHandle>` `:1405`) - `createBoxShape`/`createSphereShape` each with
**two overloads** `:1409-1414`, `type` `:1444`, `motionQuality` `:1445`, `sensor` `:1449`,
`gravityFactor` `:1456`, `collisionGroupID` `:1460`, `allowSleeping` `:1464`, body-level
friction/restitution `:1465-1466`, `applyBuoyancyImpulse` `:1486`
(impl `engine/core/object/physics/Body3D.cpp:823`). Joints are registered with
`deriveClass<Joint2D, EntityHandle>` and `deriveClass<Joint3D, EntityHandle>`. Contact/event hooks are properties on the
subsystem (`binding/ECSClassesLua.cpp:172-212`, hooks at `:194-209`): gravity, gravity2D/3D,
`pointsToMeterScale2D`, `lock3DBodies`, beginContact2D/endContact2D/hitContact2D/preSolve2D/
shouldCollide2D, onBodyActivated3D/onContactAdded3D; get it with `Scene.getPhysicsSystem`
(`CoreClassesLua.cpp:1028`). Raycasts: `engine/core/math/Ray.cpp` (`Ray.intersects` over
AABB/OBB/Plane/Sphere/Body2D/Body3D/Scene `:427-546`; Box2D `b2World_CastRay` `:487`,
Jolt `NarrowPhaseQuery().CastRay` `:521,:578`), Lua `Ray` at `MathClassesLua.cpp:593-619`,
`Object.getRay`/`Camera.screenToRay` (`ObjectClassesLua.cpp:226,276`).

## Six physics traps proven from Lua (each one killed a script)

- **`Body3D` damping is persistent and live.** `linearDamping` / `angularDamping` are stored on
  `Body3DComponent`, serialized in scenes, applied to Jolt creation settings and exposed as Lua
  properties/setters. Values are clamped to non-negative coefficients; static bodies retain the
  authored values but have no motion properties to update live.
- **`Joint2D`/`Joint3D` must retain their `EntityHandle` base in Lua.** They are now registered
  with `deriveClass<Joint2D, EntityHandle>` and `deriveClass<Joint3D, EntityHandle>` in
  `ObjectClassesLua.cpp`, so scripts can access inherited `scene`, `entity`, `name` and
  `entityOwned` members and use the scene/entity constructors. The generated API docs confirm
  `Base: EntityHandle`; a dedicated runtime Lua scene harness is still pending, so do not claim
  end-to-end joint creation is fully verified yet. Authoring remains available in the Inspector
  (`editor/window/Properties.cpp:294-306`).
- **Choose the body type before the body is loaded.** `Body3D::setType` on an already-added body
  calls `JPH::BodyInterface::SetMotionType(..., EActivation::Activate)`
  (`engine/core/object/physics/Body3D.cpp:382-395`), which asserts in a Debug engine build and
  kills the process (exit 3, buffered `stdout` lost). Pass the type when creating: static via the
  `false` argument, kinematic via `BodyType.KINEMATIC`, omit it for dynamic.
- **2D and 3D are bound by the same `#ifdef QUANTUM_PHYSICS_2D` gate** - the default
  `pointsToMeterScale2D = 64.0` is written at `PhysicsSystem.cpp:471` *inside* that guard, next to
  the lazy `ensureWorld2D` comment. A 3D-only scene never initialises it.
- **A static Jolt body has no motion properties, and two engine setters dereferenced them
  blindly.** `Body::SetAllowSleeping` writes `mMotionProperties->mAllowSleeping` with no check
  (`engine/libs/joltphysics/Jolt/Physics/Body/Body.cpp:74`) and `mMotionProperties` is only
  allocated for a dynamic/kinematic body, so `body3d.allowSleeping = x` on a STATIC body stored to
  address 0 and killed the process (measured: `quantum-engine.exe` exit 139; the faulting frame's
  next instruction is `ResetSleepTimer`, which is how it was identified without symbols).
  `Body3D::setAllowSleeping`/`isAllowSleeping`/`setMass` now go through
  `GetMotionPropertiesUnchecked()` and refuse instead (`Body3D.cpp:652-673`, `:524-533`), the same
  guard `setGravityFactor` already used. Two rules follow: a collider component must not promise
  `mass`, sleep state or CCD to a static body, and `JPH::BodyInterface` setters are safe there -
  the whole file has one raw `GetMotionProperties()` and it is guarded by `!IsStatic()`, so
  friction, restitution, `sensor`, the collision filters and `motionQuality` all work.
- **A capsule silently loses its half-height when the body is reloaded.** The shape is cooked from
  the entity's accumulated world scale - `halfHeight * absEntityScale.y` (`PhysicsSystem.cpp:1297`)
  and `shapeData.position * entityWorldScale(...)` (`:1630`) - while the radius comes from
  `maxScaleXZ(...)`. The reload at `:3145` fires whenever `entityWorldScale(scene, entity) !=
  body.loadedScale`, and `entityWorldScale` (`:36-39`) reads `transform->worldScale` **without
  resolving it first**, even though `resolveWorldPose` (`:57`) exists for exactly that. A reload
  that lands while the render pass has not updated the transform therefore cooks the capsule with
  Y = 0: offset and half-height gone, radius intact, and a 1.9 m character collider becomes a ball
  at the ankle. Measured from Lua on a walking rig: centre of mass +0.920 m and crown +1.890 m over
  the body position while idle at the spawn, +0.008 m and +0.098 m once the route moves. The shape
  is otherwise built exactly as asked - an airborne width ladder reads 0.119 m of girth at every
  height from +0.2 to +1.5 m over the origin, nothing at 0.0 or 2.3 - so do not chase a "capsule
  convention"; the instrument is `Projects/MountainRoad/lua/hero_probe.lua` (`MR_SHAPE=1`,
  `MR_LIFT=1` lifts the body into clear air first). Until the reload resolves the pose, author a rig
  whose *ball* and whose *capsule* both put their bottom where the feet are.

## Scale: the trap that ruins realism

`pointsToMeterScale2D` defaults to **64** (`PhysicsSystem.cpp:471`, inside `#ifdef
QUANTUM_PHYSICS_2D`) and is **not serialized** to scene YAML. Density semantics differ by world
too: 2D uses area-based density default 1.0, 3D uses kg/m3 default 1000. Any "floats like wood /
sinks like stone" request must be answered in those units, and 2D+3D share one subsystem, so a
scale change is global.

## What does not exist (say so before promising)

No character-controller, ragdoll, vehicle, soft-body or fluid **component** in the engine; the
only C++ character controller is a template script
(`templates/charactercontrol-main/scripts/CharacterController.h:10`), and the Lua controllers below
are the shipped answer for gameplay. `Ray.cast(scene, filter, radius)` and
`Ray.overlaps(scene, filter, radius)` now cover circle/sphere queries in C++ and Lua; they return
the closest hit or unique entity list and are covered by the MSVC physics smoke. No tunable substeps. Runtime physics
debug rendering now exists through `PhysicsSystem.debugDrawEnabled`: it emits a Lines overlay for Box2D
and Jolt shapes, with C++/Lua access and a headless geometry smoke. There is still no editor toggle or
contact-manifold visualizer. Authoring is enum rows in
`editor/window/Properties.cpp` (Joint3D types `:294-306`) with `editor/Catalog.cpp`
getters (`:535-578`, `:858`, `:951`, resolver rows `:2891-2894`); add-per-scene in
`editor/window/Structure.cpp:1628-1631`. So "visualise colliders" is a real feature
request, and a debug draw is the natural first deliverable (it also makes every other
physics fix verifiable).

What *does* exist is Lua-side and ships with the engine: `engine/components/` has `Rope`,
`Slingshot`, `FallingObject`, `GravityZone`, `PhysicsGrabber`, `FloatOnWater`, `SwimController`,
`CharacterController3D` and `PhysicsMover3D` built on the bindings above (`quantum-scripting` for
how a dragged slot arrives; `engine/components/README.md` for the recipes). Read them before
building a physics gameplay script from scratch - they already encode the four traps.

## Terrain and ground height from Lua

A terrain surface only collides once it has a STATIC `Body3D` whose shape is
`type: heightfield`, `source: entity_heightfield` (`samplesSize: 0` lets the builder pick
`getMinNearestPowerOfTwo()` of the heightmap). The builder is `PhysicsSystem::createShape3DForIndex`
(`:1259`), heightfield branch `:1430-1501`, and it **needs `TerrainComponent::heightMapLoaded`**:
while the heightmap is still landing on a worker thread `isShapeSourceLoading` (`:1522`) makes
`syncBody3DShapes` (`:1550`) retry per fixed step, and a shape pointed at an entity with no terrain
logs `Cannot create heightfield shape without loaded terrain heightmap` and adds nothing.

The grid spans `terrainSize` edge to edge with vertical scale `maxHeight` (`:1456-1458`), so the
collider matches the rendered CDLOD surface - but the **entity origin is the bottom of that range**,
not the ground. A controller that reads `Object(scene, terrain).position.y` as ground height sinks
the character by up to `maxHeight` metres. Sample the surface with a downward ray instead; the
`Ray` direction length *is* the cast distance in 3D (`engine/core/math/Ray.cpp:473-575`, `distance`
is a fraction of it) and there is no `pointsToMeterScale` in the 3D path.

## Editor persistence

Scene defaults `engine/core/SceneSettings.h:101-104` (`gravity2D`/`gravity3D` default
-9.81); YAML write `editor/Stream.cpp:2162-2163`, read `:2269-2270`.
