---
name: quantum-scripting
description: Write and debug Quantum game scripts - Lua (LuaBinding, the lua:// VFS, require, RegisterEngineEvent/RegisterEvent, the init hook, hot reload, the 59 ready-made components in engine/components and what a dragged Inspector slot really delivers) and C++ scripts (ScriptBase, DPROPERTY, REGISTER_ENGINE_EVENT, generated initScripts glue), plus input access from scripts. Use when writing gameplay logic, or when a script's update/collision callback "never fires".
---

# Scripting (Lua and C++)

## The single most common bug

**Defining `update()` in a Lua script does nothing.** There is no per-name dispatch:
`LuaBinding::initializeLuaScripts` (`engine/core/script/LuaBinding.cpp:686-909`) loads
each enabled LUA entry from `lua://<path>`, requires a **returned table**, clones it as
the instance (`__index` metatable `:762`), injects `self.scene` / `self.entity`, pushes
properties (`:778`, EntityReference skipped), resolves those pointers in PASS 2 (`:822`)
and then calls exactly **one** automatic hook: `init` (PASS 3, `:890`). Everything else
is opt-in, from inside `init`:

```lua
RegisterEngineEvent(self, "onUpdate")           -- global, LuaBinding.cpp:337-364
Engine.onKeyDown = function(s, key) ... end     -- direct assignment, platformer_player.lua:7
local p = self.scene:getPhysicsSystem()
p.beginContact2D = function(a, b) ... end       -- ECSClassesLua.cpp:195
RegisterEvent(self, buttonObject, "onClick")    -- UI/widget events, LuaBinding.cpp:319-333
```

Engine events are static properties on the `Engine` global
(`binding/CoreClassesLua.cpp:430`), dispatched from
`Engine::onFixedUpdate/onUpdate/onPostUpdate.call()` at `Engine.cpp:1169,1193,1197`;
the full list (onDraw, onTouch*, onMouse*, onKeyDown/Up, onGamepad*) is
`engine/core/Engine.h:459-489`. `Scene.getPhysicsSystem` is
`CoreClassesLua.cpp:1028`.

`Engine.onX = f` and the component hooks (`physics.beginContact2D`, `ui.onClick`) are
**single assignments**: the second writer replaces the first handler silently, and the
component hooks are per-scene, so filter by `self.entity` inside them. `RegisterEngineEvent`
adds a subscriber instead - prefer it whenever something else may also want the event. There is
no way to ask whether a subscriber already exists, so a component that subscribes from its own
`init()` and is then attached by `Kit.attach` runs its handler **twice per step**. The convention
that fixes it: the component records `self._registeredEvents = { onFixedUpdate = true }` next to
its `RegisterEngineEvent` call, and `Kit.attach` (`engine/components/demos/kit_demo.lua`) skips any
event already in that table. `CapsuleCollider` and `MeshCollider` follow it; the other 52 do not, so
a time-gated retry or a `maxRetries` counter in a kit demo burns double.

## Types.lua: the whole Lua surface as a file

Every project open gets `.quantum/engine-api/Types.lua` - the same bindings the C++
snapshot next to it came from, rendered as EmmyLua annotations (one `---@class` per
class, `---@field` for properties/events/constants, `function Class:method(...) end`
declarations, `---@enum` blocks) followed by a hand-written usage chapter
(`tools/reflector/lua_usage.lua`). `Project::writeLuaTypesFileToProject()` writes it from
a string embedded in the editor binary, so it is always the API that build ships.

It is a `---@meta` definition file: **never `require` it, never attach it to a scene**,
and never hand-edit it (it is regenerated on project open; `quantum-lua-api` covers
regenerating the renderer). It lands inside `.quantum` on purpose - the code editor's
project-symbol scan skips dot-directories (`editor/window/CodeEditor.cpp:466-471`) and
the packer never sees it, so a shipped game carries no 200 KB of comments.

## Sentinels, not nil

Scene queries do not return `nil` when they miss, so `if ray:intersects(...)` is always
true: `Ray:intersects` hands back a `RayReturn` userdata (`engine/core/math/Ray.h:25-48`)
whose fields are `hit`, `distance`, `point`, `normal`, `body`, `shapeIndex` - test
`res.hit`. `Scene.findEntity` returns `NULL_ENTITY` (`0`, bound at
`binding/CoreClassesLua.cpp:358-372`), and `FileData:open` returns a `FileErrors` whose
success value is `FILEDATA_OK = 0` (`engine/core/io/FileData.h:16-21`). Compare against
the documented sentinel, then read the fields.

## Modules and reload

`require` uses a custom searcher and **replaces** the default searchers, so
`package.path` is never consulted (`configureProjectLuaLoader`, `LuaBinding.cpp:425-428`);
`moduleLoader` (`:391-423`) resolves `lua://<name>.lua` then `lua://lua/<name>.lua`, with
dots turned into path separators, and reports "no file in assets directory" otherwise.
The entry point is `lua://main.lua` (or `lua/main.lua`) (`:454-455`). Hot reload: each
Play clears the project module cache (`clearLoadedProjectModules` `:638`, called from
`editor/Project.cpp:9415`) and per-instance teardown unregisters subscriptions by the
`ClassName_addr_method` tag (`luaRegisterEventImpl` `:239`, tag `:270`,
`removeSubscriptionsByTag` `:523-538`). A `ScriptComponent` holds
`ScriptEntry{type, path, className, enabled, properties, instance}`
(`engine/core/component/ScriptComponent.h:18-27`).

## C++ scripts

No registration macro exists (D_OBJECT/GDCLASS/GENERATED_BODY were explicitly
rejected - see `editor/ai/EditorActionExecutor.cpp:1119-1122`). The shape:

```cpp
class MyScript : public quantum::ScriptBase {
    DPROPERTY("Speed") float speed = 5.0f;
public:
    MyScript(quantum::Scene* s, quantum::Entity e) : ScriptBase(s, e) {
        REGISTER_ENGINE_EVENT(onUpdate);
    }
    ~MyScript() { UNREGISTER_ENGINE_EVENT(onUpdate); }
    void onUpdate() { ... }
};
```

- `DPROPERTY` expands to **nothing but `public:`** (optionally a
  `/* @DPROPERTY_TYPE: T */` marker) - `engine/core/script/ScriptProperty.h:14-22`.
  It is *parse-only*: `editor/util/ScriptParser.cpp:360-383` regex-reads your header
  (`inheritsScriptBase` `:289-305`). A typo costs you an inspector row, not a
  compile error, and nothing type-checks until generation.
- The exact template the editor emits is `editor/window/dialog/ScriptCreateDialog.cpp:185-231`;
  a real example is `templates/charactercontrol-main/scripts/CharacterController.h:8-48`.
- Macros `REGISTER/UNREGISTER_ENGINE_EVENT`, `REGISTER_BUTTON_EVENT` in
  `engine/core/util/FunctionSubscribe.h:31-96`. **You must unregister in the
  destructor** - tags are what make cleanup possible. `QUANTUM_CRASH_GUARD`
  auto-removes a subscriber that throws (`:277-296`).
- Generated glue: `editor/Generator.cpp:549` emits `extern "C" initScripts(scene)`,
  instantiating per matched className (`:568-571`) and binding
  `prop.memberPtr` + `syncToMember()` (`:587-652`, `ScriptProperty.h:119-122`);
  cleanup deletes the wrappers (`:671-689`). The editor loads the project as a plugin
  and calls it via dlopen (`editor/Conector.cpp:183-192`); the standalone exe calls it
  directly (`Generator.cpp:1266,1307`). Sample output:
  `templates/fps-fps/.quantum/scene_scripts.cpp:18-22`.
- Regenerate generated output with `quantum-build-generator generate --project <dir>`;
  never hand-edit `.quantum/` or the project's `CMakeLists.txt`.

## Input from a script

Keycodes are `#define D_KEY_*` (`engine/core/Input.h:17+`), exposed as `Input.KEY_*`,
`Input.GAMEPAD_BUTTON_*` (`CoreClassesLua.cpp:716,852`), `GAMEPAD_AXIS_*` (`:875-879`),
`MODIFIER_*` (`:710-715`), with `Input.isKeyPressed/isMousePressed/isGamepadConnected/
isGamepadButtonPressed/getGamepadAxis/getMousePosition/getModifiers/numGamepads/
findGamepadIndex` (`:881-898`).

All of them are **state** tests: `Input.isKeyPressed(K)` is true every frame the key is
down, not on the frame it went down. For one-shot reactions subscribe `Engine.onKeyDown`
/ `onKeyUp` - the handler receives a `repeat` flag that distinguishes the auto-repeat
from the first press.

## Adding a Lua-callable method

Bindings are plain LuaBridge chains, split by owner:
`binding/CoreClassesLua.cpp` (Engine/Scene/Input/Log/SceneManager),
`ObjectClassesLua.cpp` (entity handles; `deriveClass<Terrain, Mesh>("Terrain")` `:525`),
`ECSClassesLua.cpp` (components/systems), `MathClassesLua.cpp`, plus Action/IO/Util/
Thread - all registered by `registerClasses` (`LuaBinding.cpp:480`).
`.addFunction("update", &Scene::update)` `:629`, `.addProperty(...)`
(`ECSClassesLua.cpp:705`), getter/setter pair taking `lua_State*` for events
(`:189,401`), `.addStaticProperty` for constants; enums marshal through `EnumWrapper`
specializations (`engine/core/script/LuaBridgeAddon.h:19-60`). Keep the call shape
canonical - `quantum-reflector` string-parses these files (see `quantum-api`).
After a binding change regenerate both consumers: `generate_lua_docs` for the Markdown
reference and `generate_lua_types` for the `Types.lua` embedded in the editor (the
latter is an editor build dependency, so the next editor build does it for you). See
`quantum-lua-api`.

## Calling a plugin from Lua (`Plugins`)

`engine/core/script/binding/PluginClassesLua.cpp` registers a global `Plugins` namespace over
`plugin::PluginManager`: `list()`, `commands()`, `state(id)`, `describe(id)`, `isEnabled(id)`,
`setEnabled(id, on)` and `call(name, argsTable)` -> `{ok, value, error}`. It answers instead of raising
(`_HAS_EXCEPTIONS=0`), so check `ok`. Those functions are raw `int (lua_State*)` C functions, the same
form `Vector3.__mul` uses, because LuaBridge satisfies a declared `lua_State*` parameter by consuming
stack slot 1. A project's boot chunk runs *before* `PluginManager::loadAll()`, so `Plugins.list()` is
empty at the top level of `lua/main.lua`; plugin work belongs in `Engine.onUpdate` or in a component's
`init`. `PLUGIN_SYSTEM.md` is the boundary spec, and
`tests/plugin/lua-project/lua/main.lua` is the runnable proof.

## The ready-made component library (`engine/components/`)

59 gameplay scripts the engine ships as data, plus `demos/` (10 runnable demos - beach, ocean,
daynight, sky, waterfall, storm, rope, fire, asset-pack, character-creator - and `kit_demo.lua`, the
shared scene-building driver they all `require`). Each
component declares its own Inspector rows in a `properties` table and takes entities through dragged
slots, so nothing is registered in any list. `engine/components/README.md` has the table, the recipes,
the limits and the list of proven traps below.

The sky is the one place a script reaches into rendering: `DayNightCycle` (clock: sun, **moon**,
named phases `morning`/`day`/`afternoon`/`dusk`/`night`, fog range, ambient) and `Clouds` (a separate
coverage/shape/wind layer, deliberately not a function of the hour) both write
`SkyComponent::shaderUniforms` by name, and `engine/components/assets/shaders/sky_daynight.{vert,frag}`
reads them. The fork ships **prebuilt** (`sky_assets_shaders_sky_daynight_glsl410.sdat` beside it)
because `quantum-engine` never compiles - see `quantum-shaders` for the `customParams` seam, the
deterministic name and the compile recipe. Without the fork the two scripts still work: the stock
`SkyBox` just shows the colour the clock writes.

The same dome can be turned into a file: `demos/demo_cubemap.lua` renders one cube face per headless
run and `scripts/assemble_sky_cubemap.py` packs the six into the 4x3 cross
`TextureData::loadCubeMapFromSingleFile` reads back through `SkyBox:setTexture` - a night cube built
that way ships at `templates/daynight/assets/textures/sky/night_cross.png`. The measured row
convention is **image top edge = world +Y**: the up vectors the engine uses for a point light's shadow
cube (`RenderSystem.cpp:9557`, `-Y` on the side faces) round-trip pixel-perfect and still put the moon
*under* the horizon, so the check that decides correctness is where the light sits in a horizon view,
not whether the faces came back in the right slots (`CUBE_DIR="dx dy dz ux uy uz"` gives the diagonal
views that test the seams). A saved cube is worth more than the picture: a sky texture with all six
faces is what `RenderSystem::updateSkyEnvironment` needs to build the `IBLEnvironment` (irradiance +
prefiltered), so a cube feeds real night reflections that a shader-drawn dome does not.

Each file also declares its group in a `-- category: <Group>` comment under the summary line
(`categoryOf` in `editor/cli/ComponentLibrary.cpp`). That marker is the only grouping metadata: it
drives the order of `components list` and the categories the editor's *New component* dialog shows
under "Ready-made scripts". A component without it still installs and attaches - it just lists under
"Scripts"/"Ungrouped".

They reach a project as *files*, never as a reference: `ScriptEntry.path`
(`engine/core/component/ScriptComponent.h:20`) is stored relative to the project's Lua root,
the picker rejects anything outside it (`editor/window/Properties.cpp:9483`), and `lua://`
resolves only against `System::getLuaPath()` (`engine/core/io/FileData.cpp:157-160`). So
`copy_quantum_editor_engine` carries the folder to `<exeDir>/engine/components`,
`Project::installReadyMadeComponents` seeds every new project (called from
`createNewProjectAt`, right after `copyEngineApiToProject`), and
`quantum-editor-cmd components list|install --project <dir>`
(`editor/cli/ComponentLibrary.cpp`) does it for existing projects. An install never
overwrites a file the project already has - `--overwrite` is the only way to replace it, and
`--only A,B` (which also drops the demos, since a demo needs components the project would not
have), `--no-demos` and `--lua <dir>` narrow what and where. Demos land at `<luaRoot>/demos`,
outside the `Components` folder, so `--scene demos/demo_beach` runs them.

## A crowd built by script is `addInstance`, not N entities

No component in the library batches meshes — that is raw `Mesh` API (so `Model` and `Sprite`
inherit it) and it is the only way a scripted city holds its frame: `createInstancedMesh`,
`addInstance` (6 overloads), `getInstance`, `updateInstance`, `maxInstances`, `cullDistance`,
`distanceFade`, `instancedBillboard` (`ObjectClassesLua.cpp:583-605`). One entity holds N
`InstanceData` rows and the batch costs one draw call per LOD bucket instead of one per copy.
Two traps: an instance has **no body and no entity**, so rays and physics see nothing
(`engine/core/object/Mesh.cpp:780`); and the tuning rows only stick once the batch exists, so
`createInstancedMesh()` comes before `maxInstances`/`cullDistance`/`distanceFade` — an empty
batch draws nothing (`selectMeshDraw` returns false at `instanceCount == 0`). Measured in
Debug, 1000 cubes at 1280x720: plain 1000 draw calls / 51 fps, one batch 1 draw call /
145 fps, same 12000 triangles (`probe_instances.lua`, `KIT_INST_MODE=plain|batched`).

## What a dragged slot delivers (and what it does not)

PASS 2 of `initializeLuaScripts` resolves every `EntityReference` property
(`LuaBinding.cpp:822-889`) in this order, and the *declared type string* is what picks the branch:

| `type =` in the `properties` table | What `self.<name>` becomes |
|---|---|
| `"entity"` | the **first enabled Lua script table** on the target (`className` match wins first, `:851-863`) |
| your component's own name (`"Health"`, `"WaterWaves"`, ...) | that exact instance, by the same `className` lookup |
| a handle name (`"camera"`, `"text"`, `"water"`, `"terrain"`, `"body3d"`, ... ) | the engine handle, via `pushEntityHandleByType` (`:568-636`) |
| `"EntityReference"` | a plain `EntityHandle` **plus** `Log::warn("pushEntityHandleByType: unknown type ...")` at `:633` |

The last row is the trap: the C++ type name is not one of the accepted keys, so nothing
recognisable arrives. A slot must be `"entity"`, the component name, or a handle name.

## Ownership and userdata traps (all reproduced, all now defended against)

- **The Lua wrapper owns the entity it created.** `EntityHandle(Scene*)` sets
  `entityOwned = true` and `~EntityHandle` calls `scene->destroyEntity(entity)`
  (`engine/core/object/EntityHandle.cpp:37-56`; only the `(Scene*, Entity)` ctor is
  `entityOwned = false`). Keeping `wrapper.entity` - the bare number - and letting the wrapper be
  collected deletes the entity under you, and the next access logs
  `(ERROR): Retrieving non-existent component` with `invalid unordered_map<K, T> key`
  (`engine/core/ecs/ComponentArray.h:231`). Hold the **wrapper**.
- **Vectors, colors, entity handles and `Scene` arrive as LuaBridge userdata, not tables.**
  `type(v) == "table"` is never a value test, and indexing userdata for a member the class does
  not have *raises* instead of returning `nil`. Read `v.x`, then `v:getX()` / `v:X()` /
  `v:get(x,y,z)` / `v:toTable()`, then `v[1]` - in that order, with `type(m) == "function"`
  guards on every cross-component call.
- **An engine `float` property never equals a Lua literal.** LuaBridge widens the float on the way
  out, so `scene.bloomDirtIntensity == 0.4` is false for the value just stored (it arrives as
  0.4000000059604645). Compare with a tolerance, `math.abs(v - 0.4) < 1e-5` - and note the probe
  line will print `0.40` right next to the failed equality.
- **`Scene:setPostProcessUniform` returns `void`** and logs `(ERROR): Post-process pass N does
  not exist` on every call against a pass the scene lacks: gate on `#scene.postProcessPasses`
  first or you get one error line per frame (`quantum-postprocess`).
- `Body3D` has no `linearDamping`/`angularDamping` and script-side `Joint3D` construction does not
  exist - see `quantum-physics` before writing physics from Lua.
- **`Camera:setTarget` binds only `(float, float, float)`**
  (`engine/core/script/binding/ObjectClassesLua.cpp:275`) - the `Vector3` setter is the
  `camera.target` property (`:274`). Passing one `Vector3` fails every frame with
  `Error decoding argument #3: The lua object can't be cast to desired type`, even though the
  generated `Types.lua` shows both forms.
- **A `Material` read from Lua is a read-only copy.** `Mesh:getMaterial(index)` returns a
  `Material` by value (`engine/core/object/Mesh.h`), and every bound field is a read-only
  property (`binding/CoreClassesLua.cpp:855-857`), so `mat.baseColorTexture = Texture(path)`
  raises `'baseColorTexture' is read-only` and the write is lost. Reading a texture *out* of it
  is worse than useless: `mat.heightTexture:getPath()` fails with
  `Error decoding argument #1: The lua object can't be cast to desired type` because the wrapped
  `TextureData` is not a Lua-owned object. Scalars do read back (`mat.heightScale` prints fine),
  which is what makes a probe look half-working - write with the `Mesh` setters
  (`setTexture`, `setHeightTexture`, `setHeightScale`, bound at `ObjectClassesLua.cpp` in the
  `deriveClass<Mesh, Object>` chain) and prove a map by the shader variant it forces on disk
  (`quantum-shaders`), not by reading the field back.
- **The engine's Lua is 5.4 without `LUA_COMPAT_MATHLIB`**, so `math.atan2`, `math.pow`,
  `math.cosh/sinh/tanh`, `math.frexp/ldexp/log10`, `table.getn/foreach/foreachi`, `loadstring`,
  `setfenv/getfenv` are `nil` there - while a distribution `lua54` (5.4.8) still answers to them,
  which is why `engine/components/checks/kit_harness.lua` greps for them instead of trusting the
  interpreter. Use `math.atan(y, x)` and `x ^ y`.
- **A component handle is not a component.** `Camera(scene, id)` and `Body3D(scene, id)` only stamp
  (scene, entity) - they never add the component. Reading a component-backed property on an entity
  that lacks it goes through `ComponentArray::getComponent`, which logs
  `(ERROR): Retrieving non-existent component: %s` **and rethrows** (`engine/core/ecs/ComponentArray.h:225-233`),
  so an `onUpdate` writing `camera.position` on a camera-less entity produces one engine error plus
  one Lua error *per frame*. Lua has no `hasComponent` (`Signature` is not a bound class), so the
  only probe is a `pcall` around a property exclusive to the component - run it **once in `init`**,
  as `engine/components/FollowCamera.lua`'s `cameraOf()` does.
- **A terrain's surface is not its entity origin.** The heightmap displaces the grid by
  `height * maxHeight` in local space, so an entity at `y = -5` with `maxHeight 5` has ground
  anywhere in `[-5, 0]`. `Object(scene, terrain).position.y` is the *bottom*. Ground height has to
  come from `Ray(origin, direction):intersects(scene, RayFilter.BODY_3D)` against a STATIC
  `heightfield` `Body3D` on the terrain (`quantum-terrain`, `quantum-physics`);
  `CharacterController3D` does exactly that and caches the last valid hit.
- **A glTF bone is a named entity, and it does receive the animation.** Every node of a skinned
  model - joints included - becomes a child entity with a `BoneComponent`, so
  `scene:findEntityInParent(name, modelEntity)` *is* the socket lookup and reparenting a part onto
  that entity makes it follow the clip. Measured with `assets/characters/human_male.glb` under
  `Dance_Loop`: the Head bone's world position went from `(0, +1.600, -0.017)` to
  `(-0.033, +1.501, +0.005)`. Use the name index, never `Model:getBone(name)` - that *raises* on an
  unknown name and an exception crossing the Lua boundary takes the process down.
- **Transform writes propagate during draw, so a script read is one frame stale.**
  `RenderSystem::updateTransform` recomputes the hierarchy after scripts run, and
  `Object:updateTransform()` refreshes only that entity, not its children. Consequence: an attachment
  that *copies* a bone transform (instead of reparenting) must be rewritten every frame -
  `engine/components/AvatarPart.lua`'s `mode = "follow"` sat 1.55 m below the head until the re-apply
  was taken out from behind the `syncEveryFrame` gate.
- **A quaternion read off a skinned model is not unit length.** Animation blending leaves `|q| != 1`,
  and `AABB::setExtents` asserts `min <= max` once such a rotation reaches a transform. Normalise
  every read before using it, and never build a rotation out of a `position` row (that was the
  degenerate-quat path behind the assert - `engine/components/README.md` traps 25-27).
- **`Camera:screenToRay(x, y)` is canvas pixels and its direction is not a unit vector.** The
  un-projection divides by `Engine::getCanvasWidth/Height` (`engine/core/object/Camera.cpp:599-606`),
  so a 640x360 window over a 1280x720 canvas must be addressed in the 1280x720 space - feeding it the
  window centre aims a quarter of the way into the frame and the shot hits nothing. `Ray.direction`
  is `far_point - near_point` (`:617-621`), i.e. one frustum long (200 m at `far = 200`), so scaling
  it by a range without normalising first makes the range meaningless; divide by its own length, then
  multiply. `intersects(scene, filter, ignoreEntity)` is the third overload and it works - it drops
  the *entity*, so a shot that starts inside the shooter's own body reports the far body behind it,
  not nothing (verified: pane ignored, wall hit at 5.90 m).
- **A `Particles` or `Action` entity has no Transform.** Only `Object`-derived wrappers add one
  (`Points`, `Lines`, `Light`, `Mesh`, `Terrain`); `Action(scene)` and `Particles(scene)` stamp just
  their own component, and `Sound(scene)` adds none - the `(Scene, true)` form is the one that
  carries a Transform. So an emitter follows the camera by moving the **`Points`** entity
  (`Precipitation.lua`), and writing `position` on the action entity logs one engine error per frame.
- **The particle pool only exists once the action has started.**
  `ActionSystem::particleActionStart` (`engine/core/subsystem/ActionSystem.cpp:1121`) is the sole
  allocator: it sizes `particles.particles` and `points.points` from `particles.maxParticles`
  (default **100**, `engine/core/component/ParticlesComponent.h:237`) and mirrors that onto
  `points.maxPoints` (`:1171-1172`). An emitter built without a `restart()` prints `0 drops`, and a
  huge `rate` on the default pool lands on exactly 100 live particles. `maxParticles` is bound in
  Lua beside `rate` (`ActionClassesLua.cpp`) for that reason.
- **A script-made `Camera` is a UI camera with a 10-unit far clip**
  (`CameraType::CAMERA_UI` and `DEFAULT_ORTHO_FAR`, `engine/core/component/CameraComponent.h:37,63`)
  and nothing corrects it for a scene assembled in Lua, so every 3D demo was silently clipping its
  own world. `Camera:setPerspective(yfov, aspect, near, far)` fixes the projection but turns
  `autoResize` off (`engine/core/object/Camera.cpp:73-86`), so put it back afterwards - the demos do
  it in `Kit.camera3D`.
- **A script-built `Terrain` needs the scene camera *and* a heightmap.**
  `MeshSystem::createOrUpdateTerrain` refuses without a camera (`engine/core/subsystem/MeshSystem.cpp:6300`)
  and with an empty `heightMap` (`:6340`), and each refusal runs `resetTerrainToUnbuilt`, which
  clears `needUpdateTerrain` **permanently** (`:3136`). The terrain then never comes back and the
  renderer logs `Mesh of 'Terrain' has failed to load 8 times` forever. Order: `scene:setCamera(...)`,
  `terrain:setHeightMap(path)`, and let the per-frame loop (`:6510-6521`) generate it - a manual
  `createTerrain()` before the camera is exactly what burns the flag. The relief itself is GPU
  vertex displacement; the CPU mesh is the flat subdivided grid.
- **A Lua-built camera is not the scene's camera.** `Scene::getRenderCamera()`
  (`engine/core/Scene.cpp:161-163`) answers `viewCamera` or else the `createDefaultCamera()` UI
  camera (`:165-179`, `CAMERA_UI`, far clip 10) until somebody calls `scene:setCamera(camera)`.
  A 3D scene assembled in Lua therefore renders through that 10-metre UI box: the perspective
  camera exists, answers `.farClip`, moves every frame, and is never used. It also decides what a
  **render distance** measures - `Scene.renderDistance` caps perspective projections only
  (`RenderSystem::getRenderedNearFar`, `engine/core/subsystem/RenderSystem.cpp:8770`) - so a probe
  that forgot `setCamera` prints identical draw counts for every distance and reads as "culling is
  broken". Every demo and both `engine/components/checks/probe_bench.lua` and
  `probe_render_distance.lua` call it.
- **A bare number is read as an entity id.** The slot-shaped helpers resolve
  `entityId(reference)` by "any number ≠ 0 is an id", so `Lightning:strike(120)` looked up entity 120
  and logged `(ERROR): Retrieving non-existent component` per strike. Distinguish number / point
  table / handle explicitly, in that order, before falling back to a lookup.
- **Mouse capture is real on Windows — and a per-frame warp must re-base its own baseline.**
  `Engine.mouseMode = MouseMode.CAPTURED` (static property `CoreClassesLua.cpp:406`) ->
  `Engine::setMouseMode` (`Engine.cpp:524`) -> `WindowWin::setMouseMode` (`WindowWin.cpp:430`)
  hides and clips the cursor and switches to relative motion; the `WM_INPUT` branch of
  `WinInputRouter` accumulates raw deltas into `Engine::systemMouseMove` (`WinInputRouter.cpp:47-59`)
  while the `WM_MOUSEMOVE` branch is swallowed (`:61-69`), so `Input.getMousePosition()` keeps
  reporting movement while captured. `transformCoordPos` never drops a captured event
  (`Engine.cpp:1430`). The mode is stored and re-applied in `systemViewLoaded`
  (`Engine.cpp:1080-1082`), not once per frame, and `setMouseMode` itself defers until the view
  exists (`:526`) — so a script that set it once is not fought by the loop, but it also only
  lands after the view loads.
  The trap that cost a real player their horizontal look: a full spin needs the pointer pinned to
  the centre, and `Engine::setMousePosition` (`Engine.cpp:535-549`) *overwrites* the reported
  position — `Input::setMousePosition` in canvas coords plus `SystemWin::setMousePosition` in
  window coords, which rebases the router (`SystemWin.cpp:160-165`). So the frame delta must be
  measured against **the warp point**, not against the previous frame's own reading. With a
  steady travel `T`, `d = mp_N - warp_{N-1}` is `T`; `d = mp_N - mp_{N-1}` is `0`, because the warp
  is subtracted from the travel as well. Constant velocity is the case that cancels — a jittery
  hand still turns, which is exactly why "I cannot look sideways" survives casual testing and dies
  in a sweep. Print `mp` and the delta from the script before touching the engine.

## Verification without the editor

Run the project headless: `"$QUANTUM_BUILD_DIR/Debug/quantum-engine.exe" --project <dir>
--window 1280x720`. Lua entry scripts execute there; `.scene` YAML deserialization is
editor-side, so scene-driven runs go through the generated C++ instead. Agents must
never launch `quantum-editor.exe`.

Three things about that runner are load-bearing when you verify a script:

- **It boots from `<project>/lua/`, not from `luaDir:`.** `resolveAndEnterProject` chdirs into the
  project (`tools/engine/main.cpp:173-204`) and `runLuaEntry` then tries `lua://<name>.lua` and
  `lua://lua/<name>.lua` (`:212-242`), which `FileData.cpp:157-160` resolves against
  `System::getLuaPath()` -> the cwd's `lua/`. A template project with `luaDir: .` therefore needs
  `--lua lua` (or an editor session) before `--scene demos/demo_beach` will find anything.
- **No `assets/shaders/*.sdat` -> exit 1 immediately**, with the `Quantum is missing precompiled
  shaders` block. A scratch project needs the shader variants copied in, not just `project.yaml`.
- **Give the run a way to end.** `timeout` does not reliably kill the child here, a killed process
  loses buffered `stdout` (the log comes back empty even though the run errored), and an orphaned
  `quantum-engine.exe` holds `quantum.dll` so the next link fails with `LNK1168`. Make the script
  call `System.quit()` from a frame counter - `engine/components/demos` uses `KIT_DEMO_FRAMES`.
