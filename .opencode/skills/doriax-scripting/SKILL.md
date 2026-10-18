---
name: doriax-scripting
description: Write and debug doriax game scripts - Lua (LuaBinding, the lua:// VFS, require, RegisterEngineEvent/RegisterEvent, the init hook, hot reload) and C++ scripts (ScriptBase, DPROPERTY, REGISTER_ENGINE_EVENT, generated initScripts glue), plus input access from scripts. Use when writing gameplay logic, or when a script's update/collision callback "never fires".
---

# Scripting (Lua and C++)

## The single most common bug

**Defining `update()` in a Lua script does nothing.** There is no per-name dispatch:
`LuaBinding::initializeLuaScripts` (`engine/core/script/LuaBinding.cpp:630-829`) loads
each enabled LUA entry from `lua://<path>`, requires a **returned table**, clones it as
the instance (`__index` metatable `:678-685`), injects `self.scene` / `self.entity`
(`:687-695`), pushes properties (`:698-722`), resolves `EntityReference` properties in
PASS 2 (`:734-805`) and then calls exactly **one** automatic hook: `init` (PASS 3,
`:807-828`). Everything else is opt-in, from inside `init`:

```lua
RegisterEngineEvent(self, "onUpdate")           -- global, LuaBinding.cpp:318-346
Engine.onKeyDown = function(s, key) ... end     -- direct assignment, platformer_player.lua:7
local p = self.scene:getPhysicsSystem()
p.beginContact2D = function(a, b) ... end       -- ECSClassesLua.cpp:189
RegisterEvent(self, buttonObject, "onClick")    -- UI/widget events, LuaBinding.cpp:300-315
```

Engine events are static properties on the `Engine` global
(`binding/CoreClassesLua.cpp:265-267`), dispatched from
`Engine::onFixedUpdate/onUpdate/onPostUpdate.call()` at `Engine.cpp:1079,1096,1100`;
the full list (onDraw, onTouch*, onMouse*, onKeyDown/Up, onGamepad*) is
`engine/core/Engine.h:421-448`. `Scene.getPhysicsSystem` is
`CoreClassesLua.cpp:694`.

## Modules and reload

`require` uses a custom searcher and **replaces** the default searchers, so
`package.path` is never consulted (`LuaBinding.cpp:406-409`); `moduleLoader` resolves
`lua://<name>.lua` then `lua://lua/<name>.lua` (`:372-404`). The entry point is
`lua://main.lua` (or `lua/main.lua`) (`:429-459`). Hot reload: each Play clears the
project module cache (`clearLoadedProjectModules` `:610-628`, called from
`editor/Project.cpp:7661`) and per-instance teardown unregisters subscriptions by the
`ClassName_addr_method` tag (`:251-252,511-519,831-851`). A `ScriptComponent` holds
`ScriptEntry{type, path, className, enabled, properties, instance}`
(`engine/core/component/ScriptComponent.h:17-26`).

## C++ scripts

No registration macro exists (D_OBJECT/GDCLASS/GENERATED_BODY were explicitly
rejected - see `editor/ai/EditorActionExecutor.cpp:1119-1122`). The shape:

```cpp
class MyScript : public doriax::ScriptBase {
    DPROPERTY("Speed") float speed = 5.0f;
public:
    MyScript(doriax::Scene* s, doriax::Entity e) : ScriptBase(s, e) {
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
  destructor** - tags are what make cleanup possible. `DORIAX_CRASH_GUARD`
  auto-removes a subscriber that throws (`:277-296`).
- Generated glue: `editor/Generator.cpp:549` emits `extern "C" initScripts(scene)`,
  instantiating per matched className (`:568-571`) and binding
  `prop.memberPtr` + `syncToMember()` (`:587-652`, `ScriptProperty.h:119-122`);
  cleanup deletes the wrappers (`:671-689`). The editor loads the project as a plugin
  and calls it via dlopen (`editor/Conector.cpp:183-192`); the standalone exe calls it
  directly (`Generator.cpp:1266,1307`). Sample output:
  `templates/fps-fps/.doriax/scene_scripts.cpp:18-22`.
- Regenerate generated output with `doriax-build-generator generate --project <dir>`;
  never hand-edit `.doriax/` or the project's `CMakeLists.txt`.

## Input from a script

Keycodes are `#define D_KEY_*` (`engine/core/Input.h:17+`), exposed as `Input.KEY_*`,
`Input.GAMEPAD_BUTTON_*` (`CoreClassesLua.cpp:716,852`), `GAMEPAD_AXIS_*` (`:875-879`),
`MODIFIER_*` (`:710-715`), with `Input.isKeyPressed/isMousePressed/isGamepadConnected/
isGamepadButtonPressed/getGamepadAxis/getMousePosition/getModifiers/numGamepads/
findGamepadIndex` (`:881-898`).

## Adding a Lua-callable method

Bindings are plain LuaBridge chains, split by owner:
`binding/CoreClassesLua.cpp` (Engine/Scene/Input/Log/SceneManager),
`ObjectClassesLua.cpp` (entity handles; `deriveClass<Terrain, Mesh>("Terrain")` `:525`),
`ECSClassesLua.cpp` (components/systems), `MathClassesLua.cpp`, plus Action/IO/Util/
Thread - all registered by `registerClasses` (`LuaBinding.cpp:461-476`).
`.addFunction("update", &Scene::update)` `:629`, `.addProperty(...)`
(`ECSClassesLua.cpp:705`), getter/setter pair taking `lua_State*` for events
(`:189,401`), `.addStaticProperty` for constants; enums marshal through `EnumWrapper`
specializations (`engine/core/script/LuaBridgeAddon.h:19-60`). Keep the call shape
canonical - `doriax-reflector` string-parses these files (see `doriax-api`).

## Verification without the editor

Run the project headless: `"$DORIAX_BUILD_DIR/Debug/doriax-engine.exe" --project <dir>
--window 1280x720`. Lua entry scripts execute there; `.scene` YAML deserialization is
editor-side, so scene-driven runs go through the generated C++ instead. Agents must
never launch `doriax-editor.exe`.
