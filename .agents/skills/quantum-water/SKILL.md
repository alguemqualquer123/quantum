---
name: quantum-water
description: Water in Quantum - the Water/WaterExclusion objects, the Gerstner surface with its Sim shader variant, and the CPU shallow-water field (runs downhill, pools, rains, evaporates, ripples, floats and carries bodies). Use when the user asks for water, lakes, puddles, rain, flooding, splashes, buoyancy, currents or underwater effects.
---

# Water

## What exists (do not re-scope it as a new feature)

A `Water` object with a real surface shader and a **heightfield simulation** behind it.
`WaterComponent` (`engine/core/component/WaterComponent.h:36`) is both the draw data and the sim
state; `simulate` (`:121`) is the switch. Off, it is a level plane with sum-of-Gerstner waves
(`size`/`subdivisions` `:81-82`, waves `:88-92`, `MAX_WATER_SUBDIVISIONS 512` `:24`). On, it is a
depth-per-cell field stepped every fixed update.

`WaterExclusion` (`engine/core/component/WaterExclusionComponent.h:23-40`, BOX/SPHERE/HULL) keeps
water out of a volume - the surface is not drawn inside, no underwater fade, no float
(`MAX_WATER_EXCLUSIONS 8` `:15`, `PhysicsSystem.cpp:248-252` `isInsideWaterExclusion`).

## The simulation

`engine/core/subsystem/WaterSystem.cpp`. Registered **before** RenderSystem and PhysicsSystem
(`engine/core/Scene.cpp:36-38`) so the field that steps is the field drawn and floated on in the
same frame.

`fixedUpdate` (`:118`) per simulating water, in this order:
1. `resize` (`:162`) - the field covers the authored `size` in world XZ; a moved or resized water
   keeps its volume (resampled, not restarted).
2. `bakeFloor` (`:220`) - the bed: samples every `TerrainComponent` height plus heightfield meshes
   into `simFloor`. This is why water finds the low spots on its own.
3. `stepFlow` (`:280`) - **Mei-style pipe model with bed drag, mass conserving**. Per wall of 4
   (`WATER_DIRS` `WaterComponent.h:30`): `flow = max(0, flow * damping + k * avgDepth * Δhead)`
   with `k = dt * 9.81 * simFlow` (`:286`) and `damping = 1 / (1 + dt * WATER_BED_DRAG *
   (1 - simFlow * 0.8))` (`:291`, `WATER_BED_DRAG = 2.0f` `:31`). The drag is not optional: the
   pipe model integrates the head difference every step and nothing else takes flux back, so
   without it water on a slope accelerates until the cell is empty - a shelf drains itself bone
   dry in seconds and a stream is never deep enough to see. The reciprocal form is unconditionally
   stable. The pipe is one cell long and the wall one cell wide, so the length cancels - do not
   "fix" a missing pipe-width factor (`:314`, `:322`). Then the divergence update, clamped by
   availability `depth * cellArea / dt`, so a cell can never push out more than it holds.
   `simVelocity` is flux / (cell width * avg depth) = m/s.
4. `stepRipple` (`:361`) - a fast damped layer that moves **no volume**, only disturbs the surface.
   Thin-film guard `max(simDryDepth * 4, WATER_THIN_FILM)` (`:370`).
5. `stepSources` (`:403`) - `simRain` metres/s added over the area, `simEvaporation` removed;
   `rainDrops` (`:422`) punches ripple craters using the subsystem's own xorshift64*
   (`nextRandom` `:147`), never `rand()`, so a step is platform-independent.

Border: `simEdgeDrain` (`WaterComponent.h:128`) off = the border is a wall (puddles pool); on =
the water leaves the field. `simDryDepth` (`:127`) is the **single** threshold: under it a cell has
no surface, no current and no float. `getSimSurface` (`:477`, dry test `:488`) and `getSimFlow`
(`:504`, same test `:515`) must keep that contract identical - if only one answers "dry", a body
floats over water nobody can see.

It is a heightfield, not a fluid solver: vertical only (`isSimulated` `:67` - a tilted water entity
draws the field as if level), no 3D volume, no vortices.

## Caps and cost

`MIN_WATER_SIM 8` / `MAX_WATER_SIM 256` (`WaterComponent.h:27-28`) bound `simResolution` (`:122`,
default 128). `resolutionOf` (`WaterSystem.cpp:71`) is the only place that clamps, and RenderSystem
sizes the texel texture with it **before** the first step allocates. Cost is `cells²` per water per
fixed step on the CPU: 256² = 65k cells. The Resolution row in the editor is the cost knob
(`Properties.cpp:8857`). Field storage: `WaterComponent.h:137-156`
(`simFloor/simWater/simRipple/simRippleVel/simFlux[cells*4]/simVelocity[cells*2]/simTexels`).

## Shader: the Sim variant

`USE_WATER_SIM` = **water bit 5**, code `Sim` (`editor/shader/ShaderBuilder.cpp:529`, whole water
table `:523-530`). Bits are per shader type; never reuse an index across tables (`quantum-shaders`).

`engine/shaderlib/water.vert:42-47` declares `u_vs_waterSimParams` (`simMap`, `simOrigin`,
`simCell`, `simTune`) - **that member order is the GPU byte layout**, reordering corrupts the
surface silently instead of failing to compile. `fieldHeight()` (`:53-56`) is `g + r + b`
(floor + depth + ripple). Fragment: dry fade from `v_simWet` (`water.frag:529-532`) and flow foam
from `v_simFlow` (`:433-437`). `v_simFlow` is **raw metres per second** (`texel[3] = speed`,
`WaterSystem.cpp:479`), so the `smoothstep(0.8, 3.0, ...)` there is a physical calibration, not a
taste knob - and it is multiplied by `crestFoam * 3` before clamping, so a `WaterWaves` crestFoam
over ~0.33 puts every wet cell at full white regardless of speed.

Upload path: `RenderSystem::updateWaterField` creates the RGBA32F dynamic texture
(`RenderSystem.cpp:6509-6524`; texel packing `r=depth g=floor b=ripple a=speed`,
`WaterComponent.h:143`), slot resolved at `:6573`
(`UniformBlockType::WATER_SIM_VS_PARAMS`, `ShaderData.cpp:228`), texels pushed and bound in the
water draw at `:6837-6857`; `advanceWaters` (`:6693`) drives the wave phases.
`WaterSystem::draw()` is empty (`:85`) - **there is no debug draw of the field**.

## Physics: floats on the field, is carried by the current

`PhysicsSystem.cpp:895-919`: picks the water under a body by level and `buoyancyDepth`, asks
`RenderSystem::getWaterSurfaceOffset` (`:6350`, field-aware at `:6381-6386`) for height/normal/
depth, **skips the body when the cell is dry** (`:910`) and passes `WaterSystem::getSimFlow`
(`:915`) as the fluid velocity to `ApplyBuoyancyImpulse` (`:917`). That is 3D/Jolt only - there is
no 2D water.

## Script and editor surface (all wired)

Lua `Water`: sim properties `binding/ObjectClassesLua.cpp:465-476` (`simulate`, `simResolution`,
`simDepth`, `simFlow`, `simRain`, `simEvaporation`, `simDryDepth`, `simEdgeDrain`, `simRipples`,
`simRippleSpeed`, `simRippleDamping`, `simRippleStrength`) and functions `:488-491`
`getSimWaterDepth(x,z)`, `getSimVelocity(x,z)`, `addDisturbance(worldPos,radius,strength)`,
`getSimVolume()` (wrappers `engine/core/object/environment/Water.cpp:652-679`), plus
`addWater(worldPos,radius,cubicMetres)` and `drainWater(...) -> cubicMetres`.
`addDisturbance` is the impact hook - footstep, splash, thrown rock; the crater is what rings out.
**`getVolume` answers in litres** (`WaterSystem.cpp:571` multiplies by 1000) while every other
number in the sim is metres - divide by 1000 before comparing it to a depth times an area.

On top of that surface the engine ships six Lua components (`engine/components/`, installed into
every new project and by `quantum-editor-cmd components install`): `WaterWaves` (wind drift, gusts,
tilt coupled to height), `BeachShore` (one knob that retunes waves, shoreline foam, sky and fog for
a beach), `WaterRipples` (calls `addDisturbance` when a body wakes or crosses the surface, and fires
splash sound/particles), `Waterfall` (the two-field jump above, plus the sprite sheet and the mist),
`FloatOnWater` (samples `getSimSurface` + `getSimFlow` at the centre of mass
and calls `applyBuoyancyImpulse` itself) and `SwimController` (buoyant swimming with hysteresis).
`demo_beach` and `demo_ocean` assemble them and run headless - see `quantum-scripting` for the
ownership rule they depend on: **keep the `Water(scene)` wrapper alive**, not just `wrapper.entity`,
or the GC destroys the entity the whole sim lives on.

Editor: rows `Properties.cpp:8855-8890` with a live litres readout (`:8889`), Catalog
`Catalog.cpp:558-564`, YAML `Stream.cpp:6513-6519` write / `:6574-6580` read, codegen
`Factory.cpp:1657-1663`, create menu `Structure.cpp:716-717`. A new persisted property still needs
the whole checklist in `quantum-api` - the lists are hand-maintained and a miss makes the feature
invisible rather than broken.

Headless proof of conservation: with rain and evaporation off, `getSimVolume` drifts ~1e-5 % over a
40 s settle and the surface reaches flat (hydrostatic) balance. That is what makes the field usable
as a game rule rather than a visual trick.

## Authoring a scene that reads (every one of these was a wrong-looking capture first)

Prove it with `quantum-engine.exe --project templates/waterfall --frames 200 --capture-frame 195
--screenshot x.png` and `WATERFALL_SHOT=wide|pool|fall|lip|top`; `WATERFALL_PROFILE=1` walks the
channel **and** the lake and prints depth + speed per cell.

- **A heightfield cliff is a slope, not a wall.** A ballistic drop only gets steeper as it falls, so
  a rock face that gets gentler going down swallows the sheet. The crest has to *throw* the water
  outward (`Waterfall.lua`'s `plunge`, aimed at the impact point). `scripts/gen_terrain_maps.py`
  prints the clearance per launch speed - negative means "inside the rock". Make the lip profile
  concave (`t ** 0.62`), not a smoothstep: a smoothstep has zero derivative at `t = 0`, so the rock
  barely drops in the first metre and the sheet goes under the surface.
- **With `simEdgeDrain`, the lake settles at the lowest point of the path out**, not where it was
  seeded. A rim that closes over the outlet corridor dams it a metre and a half above the intended
  surface. Compute the divide and put the water plane there.
- **`Water:setShallowColor` / `setDeepColor` run `Color::sRGBToLinear` on the way in**
  (`Water.cpp:64,80`). Numbers that look right in a linear editor render near-black.
- **`depthFade` (`WaterComponent.h:86`, default 3.0) must fit inside the body's real depth** -
  `deepness = 1 - exp(-2.5 * pathDepth / depthFade)`, so a 0.8 m stream with the default is one flat
  deep colour everywhere.
- **Shore foam is the ray distance to the depth buffer, not the water depth**
  (`water.frag:394-428`): `pathDepth = length(behind - v_position)`. A cliff wall rising out of the
  lake counts as zero-thickness water for every pixel beside it and the waterline becomes a snow
  bank. `shoreFoam` (`WaterComponent.h:104`, default 0.4) is the only knob - a lake wants ~0.07 and
  a shallow channel ~0.02.
- **The water's sky reflection is `sky colour x scene.globalIlluminationIntensity`**
  (`RenderSystem.cpp:7835`), and that setting defaults to **0.1**
  (`SceneSettings.h:75`). Under a bright dome a lake renders as ink. Raise it (~0.55) - the same
  knob dims the meshes' ambient, so it is a scene decision, not a water one.
- **`crestFoam` also scales flow foam and is multiplied by three before it clamps**
  (`water.frag:437`); `v_simFlow` is raw m/s against `smoothstep(0.8, 3.0, ...)`.
- **Terrain base colour is the leftover layer** (`getTerrainColor`: `1 - (r+g+b)` goes to the
  entity's own `color`). Independent per-layer ramps leave texels where all three are ~0 and you get
  flat base-colour patches. Make one layer the remainder, `(1-rock)*(1-mud)`, and coverage is
  guaranteed.
- **A particle sprite's alpha *fraction* matters more than the count.** `assets/textures/rain.png`
  is 48x192 with 4.5 % of its pixels above alpha 200 - a two-pixel thread that minifies to nothing.
  `rain_sheet.png` keeps its core ~60 % of the width. And `Precipitation`'s rain preset is
  **0.05 m**, about one pixel at 25 m: set `particleSize` (0.16 with `streak = 7` reads as a streak).
  `density` is particles **per second** and `maxParticles = density x 4` clamped at 8192.
- Both terrain maps must come from **one** height function: `bakeFloor` samples the height map
  CPU-side, so a blend map generated separately makes water flow where the mountain is not.

## What water does NOT have (say so before promising)

- No obstacle coupling back into the field: bodies displace nothing; `addDisturbance` is the only
  entry a game can call.
- The field cannot **fall**. It is a grid at one altitude, so a cliff lip is a wall. The jump is a
  script's job: `WaterSystem::addWater` / `drainWater` (`:576`, statics, per `WaterComponent&`) pour
  and take cubic metres at a world point over a `1 - d²` disc, and Lua sees them as
  `Water:addWater(worldPos, radius, cubicMetres)` / `Water:drainWater(...) -> cubicMetres actually
  taken`. `drainWater` scales to what each cell holds, so it never digs to the bed.
  `engine/components/Waterfall.lua` is the ready-made pair: it drains the upper field at the lip,
  pours the lower one at the impact, and runs the visible sprite sheet. A river is therefore
  sloped `simFloor` + `simFlow` + `simEdgeDrain` + a `Waterfall` on the drop, not a solver feature.
- No autonomous "water AI" with goals. The behaviour is hydrostatic: it runs downhill because the
  flux is head-driven. Chasing, seeking or deciding is a script on top.
- No caustics or refraction driven by the sim (both read the pre-existing scene-depth path), no
  underwater god rays, no 2D water, no fluid solver.
- No noise inside the water shaders: `quantum::Noise` (Perlin/simplex/value, fBm, ridged, domain
  warping) is CPU-only and `engine/shaderlib/includes/` has none, so procedural rain or wave detail
  is either a texture generated at load (`WorldGen`, `quantum-procedural-noise`) or a hand-written
  GLSL hash.

## Scale trap

The sim is written in **metres** (depth, rain m/s, ripple speed m/s) and `Body3D` buoyancy mixes
world units with them. `pointsToMeterScale2D` defaults to 64 (`PhysicsSystem.cpp:471`, inside
`#ifdef QUANTUM_PHYSICS_2D`) and is not serialized, and 2D/3D share one `PhysicsSystem` - keep a
simulated water in metres or the numbers stop meaning anything next to the impulse code.
