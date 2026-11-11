# Quantum plugin system

The engine loads optional modules at run time. A plugin is a shared library that the engine maps with
`LoadLibrary`/`dlopen`, checks against three contracts (ABI, engine version, platform) and then calls
through a small C++ vtable. Nothing in `engine/plugin/` is World Creator specific; World Creator is
just the first plugin that will use it.

Source: [engine/plugin/](engine/plugin) · example: [plugins/hello/](plugins/hello) ·
acceptance probe: [tests/plugin/](tests/plugin) · design rationale: [WORLD_CREATOR_PLAN.md](WORLD_CREATOR_PLAN.md)

## 1. Where plugins live

| Directory | Contents |
|---|---|
| `<cwd>/plugins/<folder>/` | one folder per plugin: `plugin.json`, the library, `Resources/`, `Shaders/` |
| `<cwd>/plugins/settings/` | one JSON file per plugin id, written by the engine |

Both default to the paths above, resolved on first use, and are overridden by the environment
`QUANTUM_PLUGINS_DIR` and `QUANTUM_PLUGIN_SETTINGS_DIR`. The editor, `quantum-engine.exe` and a shipped
game all use the same layout, because the manager lives in the engine
([engine/core/Engine.cpp](engine/core/Engine.cpp), end of `systemInit`) and the editor only observes it.
`QUANTUM_PLUGINS_DIR` is also what a CI build points at the binaries CMake produced.

## 2. The ABI contract

Three `extern "C"` symbols, and nothing else, cross the boundary
([engine/plugin/PluginAbi.h](engine/plugin/PluginAbi.h)):

| Symbol | Returns | Purpose |
|---|---|---|
| `QuantumPluginAbi()` | `unsigned` | must equal `QUANTUM_PLUGIN_ABI_VERSION` (currently 2) |
| `QuantumPluginCreate()` | `void*` | a `new` plugin, cast to `void*` |
| `QuantumPluginDestroy(void*)` | - | deletes what `Create` returned |

Write them with the macro instead of by hand:

```cpp
QUANTUM_PLUGIN_ENTRY(HelloPlugin)
```

The ABI number is checked **before** the vtable is touched, so a plugin built against an older
contract is refused rather than called. `plugins/broken` exists to prove that: its manifest claims the
matching ABI and its binary exports `999`, so the refusal can only have come from the symbol.

| Number | Contract |
|---|---|
| 1 | lifecycle, the schema-described settings panel, commands, `tick`, `scene` |
| 2 | the above plus hosted windows (`onDrawWindow`/`onWindowInput`), menu items and the root-bounded `files` API |

`IPlugin` gained two virtuals at 2, which moves the vtable, so a plugin built against 1 must not be
called through this one. `QUANTUM_PLUGIN_ABI_VERSION` in
[engine/plugin/PluginAbi.h](engine/plugin/PluginAbi.h) is the number; a plugin writes the same number in
its `plugin.json`, and the acceptance probe asserts the two agree.

## 3. `IPlugin`

[engine/plugin/PluginInterface.h](engine/plugin/PluginInterface.h) is the only interface a plugin
implements. Every method has a default, so a plugin overrides what it uses:

| Method | Called | Contract |
|---|---|---|
| `pluginId()` / `pluginVersion()` / `pluginAbiVersion()` | after `initialize` | compared with the manifest; a mismatch is a logged warning, not a refusal |
| `initialize(PluginContext&)` | once, after the library maps | return false and the plugin is `Failed` and unloaded |
| `activate()` / `deactivate()` | on enable / before unload | registration happens here |
| `shutdown()` | before the instance is deleted | release what commands still reference |
| `buildUiSchema(UiSchemaBuilder&)` | when the panel is first drawn | see §5 |
| `onDrawWindow(windowId, UiSchemaBuilder&)` | every frame that window is open | see §5b |
| `onWindowInput(windowId, values, pressedButtons)` | only when something in that window changed | see §5b |
| `executeCommand(name, args, result)` | every manager-routed call | return false for a name you do not own |
| `onUpdate(deltaSeconds)` | once per frame, after the scenes | only with the `tick` permission; see §6a |
| `onSettingChanged(key, value)` | when the user edits a control | persist it if you want it to survive |

`quantum::plugin::PluginBase` implements `initialize` for you and stores the context, so
`context().settings()` works in every later call. Derive from it. The context is owned by the record
and stays alive exactly as long as the instance does.

Nothing in this interface may throw. `_HAS_EXCEPTIONS=0` propagates PUBLIC from
[engine/libs/joltphysics/CMakeLists.txt](engine/libs/joltphysics/CMakeLists.txt) into the engine and
the editor, so an exception crossing the boundary aborts the process instead of unwinding.

## 4. `plugin.json`

JSON, not YAML, because yaml-cpp is not linked into `quantum.dll` while nlohmann's json is
(decision D2 in the plan). A parse failure is reported as data, never as an exception.

```json
{
  "id": "quantum.hello",
  "name": "Hello Plugin",
  "description": "What this adds.",
  "author": "Acme",
  "version": "1.0.0",
  "minEngineVersion": "0.8.0",
  "abiVersion": 2,
  "type": "editor",
  "platforms": ["windows", "linux", "macos"],
  "libraries": ["quantum-hello"],
  "entryPoint": "QuantumPluginCreate",
  "permissions": ["log", "settings", "tick", "ui", "files"],
  "dependencies": ["acme.base"],
  "commands": ["hello.ping", "hello.echo"],
  "panel": { "title": "Hello" },
  "settings": { "greeting": "Hello from a plugin", "loud": false, "repeat": 1 }
}
```

`id`, `name`, `version`, `abiVersion` and a non-empty `libraries` are required, and `id` may not
contain a space. `libraries` names the file without its suffix - `.dll` / `.so` / `.dylib` is added for
the running OS - and the engine looks for it in the plugin folder, in `<folder>/lib/`, and next to the
other binaries. `dependencies` accepts a bare id string or `{"id": ..., "minVersion": ...}`. A
`settings` value has to be a scalar: a nested object is reported as an error rather than dropped
silently. `panel` is optional, and a plugin without one gets no settings panel in the Plugins window -
a window the plugin owns itself is registered from code instead, see §5b.

`minEngineVersion` is compared with the version the build was configured with
(`QUANTUM_ENGINE_VERSION`, a git-describe string in a normal checkout). When the engine side cannot be
parsed as a number, the check is **skipped with a written reason** - a local `-dirty` build must never
refuse every plugin.

## 5. The panel is described, not drawn

ImGui is compiled into each editor executable ([CMakeLists.txt](CMakeLists.txt), the imgui block) and
`quantum_imgui` is INTERFACE-only, so the engine never links it. A plugin that drew its own widgets
would create a second ImGui context inside the editor's process. Instead a plugin returns a schema
([engine/plugin/PluginUiSchema.h](engine/plugin/PluginUiSchema.h)) and the editor draws it with its own
context:

```cpp
void buildUiSchema(UiSchemaBuilder& builder) override {
    PluginSettings& settings = context().settings();
    builder.title("Hello");
    builder.group("Greeting");
    builder.text("greeting", "Greeting", settings.getString("greeting"));
    builder.boolean("loud", "Shout it", settings.getBool("loud"));
    builder.group("Ping");
    builder.intSlider("repeat", "Repeat", settings.getInt("repeat", 1), 1, 8, 1);
    builder.lastField().tooltip = "How many times hello.ping says it.";
    builder.button("hello.ping", "Ping now");
}
```

Controls: `label`, `separator`, `boolean`, `intSlider`, `intInput`, `floatSlider`, `floatInput`,
`enumeration`, `color`, `button`, `text`. They write into the `UiSchema` the builder was constructed
with, and `lastField()` reaches the one just added so `tooltip` and `enabled` can be set on it. A
plugin that forgets `group()` still gets a usable panel - the builder opens an untitled one.
`collectSchemaValues(schema)` reads the edited values back into a `UiValueMap`, skipping the controls
that carry no data.

The consequence is honest and worth stating: a Gaea-style node graph cannot be shipped by a plugin
through this seam. Groups and sections are what a plugin can express today; the graph is future work.

## 5b. Windows, menu items and files

A plugin owns as many editor windows as it registers, and the same rule applies to all of them: it
describes, the editor draws. From `activate()`:

```cpp
context().registerWindow("hello.stats", "Hello Stats", "Tools", "", 380, 340);
context().registerMenuItem("Hello Stats", "hello.open.stats", "Tools");
```

| Call | Needs | What the editor does |
|---|---|---|
| `registerWindow(id, title, category, icon, width, height)` | `ui` | the id is unique across plugins (re-registering your own replaces it, another plugin's claim is refused); `category` groups it in the Plugins menu; `width`/`height` are the size it opens at, and the user's resize wins after that |
| `openWindow(id)` | `ui` | queues a request the host drains at the frame boundary - a plugin never calls into editor code, and a request for a window another plugin owns is refused |
| `registerMenuItem(label, command, category)` | `ui` | one menu row that runs `command` through `executeCommand`; a command you do not own is refused at registration, so no row can be a lie |
| `registerTool(name, label, action)` | - | a row under "Plugin Tools" that runs `action`. A tool whose action is no registered command is shown disabled rather than hidden |
| `readFile` / `writeTextFile` / `appendTextFile` / `removeFile` / `fileExists` / `listFiles` | `files` | relative paths under `<settings>/data/<plugin id>` only |

`onDrawWindow(windowId, builder)` then runs for each open window every frame, and
`onWindowInput(windowId, values, pressed)` runs only when the user actually touched something in it:
every value in the frame, plus the ids of the buttons pressed. A window with no plugin behind it -
unloaded, disabled, or a plugin that stopped describing it - closes on the next frame rather than
drawing an error forever, and one that describes no groups says so on the panel.

The file API is bounded before it touches the disk: a path may not be absolute and may not carry a
`..` step, a read is capped at 64 MB, and reading a file that does not exist is a plain `false` with no
log line (a window that reads on each describe would otherwise write sixty a second on a plugin's first
run). `dataDirectory()` is the only absolute path a plugin is given, and the writable state of a plugin
lives outside the folder discovery reads, so an installed plugin stays read-only.

On the editor side the host is [editor/window/PluginWindowHost.cpp](editor/window/PluginWindowHost.cpp):
it drains the requests, draws each open window with the schema panel of §5, and writes the open list to
`editor/plugin_windows` in settings.yaml. The Plugins menu is rebuilt with the rest of the menu bar every
frame, so an install or an unload changes it without a restart. Plugin windows are not in the dock
builder - their names are not known when the layout is built - so they open floating and dock when the
user drags one in.

## 6. Commands, settings, logs

`PluginRegistry` holds the commands, panels and tools every plugin registered. Command and tool names
are unique across all plugins - a collision is refused, not resolved by load order. The editor and Lua
route through `PluginManager::executeCommand(name, args, result, error)`, which only reaches a plugin
that is `Active`, and returns a written reason otherwise.

Settings are per plugin id in one JSON file each, and `UiValue` coerces between bool/int/float/string
on read, so a manifest default written as `"1"` still answers `getInt`. Values the plugin ships are
defaults; values the user saved win; `reset()` goes back. Saving happens on unload and on `setEnabled`.

`PluginContext::log(level, format, ...)` appends to a ring buffer capped at 1024 lines and mirrors into
the engine log, so a headless run shows plugin output. It is gated on the `log` permission (§6a); the
manager's own lines and the engine's lines about a plugin (a refused registration, a denied capability)
always go through, because a plugin that is not allowed to log must still be describable.

## 6a. Permissions, the tick and the scene

`permissions` in `plugin.json` is not decoration. Five names are enforced by the engine, and the rest
are declarations the plugin is expected to honour:

| Permission | What the engine does without it |
|---|---|
| `log` | `PluginContext::log` drops the message. Exactly one warning line is written - by the engine, through the ungated path - so a silenced plugin still shows why it is silent |
| `scene` | `PluginContext::scene()` returns `nullptr` and writes one warning |
| `tick` | `IPlugin::onUpdate` is never called, and `startInstance` writes one verbose line saying so |
| `ui` | `registerWindow`, `registerMenuItem`, `openWindow` and the two halves of a hosted window (`buildWindowSchema` / `sendWindowInput` in the manager) are refused, each with one warning |
| `files` | every `readFile`/`writeTextFile`/`appendTextFile`/`removeFile`/`fileExists`/`listFiles` call returns false and the plugin's data directory is never created |
| `settings` | nothing: the engine cannot gate a plugin reading its own settings. It is shown to the user before a load |

Any other name is a typo and is reported at discovery (`unknown permission "scense" in plugin.json`),
because a misspelling would otherwise deny the capability in silence.
`isEnforcedPermission` / `isKnownPermission` in
[engine/plugin/PluginManifest.h](engine/plugin/PluginManifest.h) are the two lists, and the probe
asserts them.

Refusals are reported **once per plugin per capability**, not per call: the gates sit in a per-frame
path and a repeating warning would write sixty lines a second.

`onUpdate(deltaTime)` runs inside `Engine::runFrame`'s variable-timestep phase, after
`Engine::onPostUpdate` and before the draw, so a plugin reads the scene state the same frame produced
it. `deltaTime` is the capped frame delta (`Engine::getDeltatime`, never a raw one) and the whole phase
is inside `if (!paused)`, so a paused game does not tick its plugins. Each plugin's slice is measured
and reported with `Engine::addSystemStats("plugin:<id>", ms, 1)`, so it appears in the frame stats next
to the subsystems - one of 32 slots, with the overflow folded into `+overflow` like any other system.

A plugin that unloads another from its own `onUpdate` would free the record the tick loop is standing
on, so `unload` (and with it `reload`, `setEnabled`) is refused during the tick with a written reason.

## 7. Lifecycle

```
discovered → (compat check) → loaded → initialized → active → disabled
                     ↘ incompatible            ↘ failed
```

| State | Meaning |
|---|---|
| `Discovered` | `plugin.json` parsed, nothing mapped |
| `Incompatible` | refused before load: platform, declared ABI, engine version, dependency, cycle |
| `Failed` | mapped but `initialize`/`activate` returned false, or the symbols were missing |
| `Loaded` / `Initialized` / `Active` | progressively more alive |
| `Disabled` | unloaded on request; the record stays so the editor can show why |

`loadAll()` discovers, skips ids saved disabled, and then repeatedly takes whatever has its
dependencies satisfied, so a manifest author does not have to sort them. Dependencies that can never
be satisfied fail immediately with a reason; a cycle ends as `Incompatible` with `dependency cycle`
rather than hanging. `unload` deactivates, shuts down, deletes the instance **through the plugin's own
`QuantumPluginDestroy`**, then closes the library - which is why an instance must never outlive its
DLL. On POSIX `dlclose` is best-effort, the same caveat `editor/Conector.cpp` documents.

`installFromDirectory()` copies a folder (and everything beside it) into the plugins directory and
re-discovers; it refuses to overwrite unless asked.

## 8. Building a plugin

`plugins/quantum_plugins.cmake` defines `quantum_add_plugin(<target> <folder> <sources...>)`, which
links `quantum`, puts the binary and a copy of `plugin.json` in `build/plugins/<folder>/`, and sets the
include paths the engine's own subtree list would otherwise hide. A plugin's `CMakeLists.txt` is then
one call:

```cmake
quantum_add_plugin(quantum-hello hello HelloPlugin.cpp)
```

Plugins are not built for Emscripten, Android or iOS, where there is nothing to map at run time.

## 9. Driving it without a window

`quantum-editor-cmd plugins` discovers, loads, describes and calls plugins from a process that opens
nothing. (`editor/main.cpp` dispatches the subcommand before any backend starts, so `quantum-editor.exe
plugins ...` reaches the same code - that path has never been run, because the editor binary is not
launched to verify anything.)

```bash
build/Debug/quantum-editor-cmd.exe plugins list
build/Debug/quantum-editor-cmd.exe plugins describe quantum.hello
build/Debug/quantum-editor-cmd.exe plugins run hello.ping repeat=3
build/Debug/quantum-editor-cmd.exe plugins windows
build/Debug/quantum-editor-cmd.exe plugins window hello.stats --set lines=3 --press note
build/Debug/quantum-editor-cmd.exe plugins check path/to/my-plugin
build/Debug/quantum-editor-cmd.exe plugins install path/to/my-plugin --overwrite
build/Debug/quantum-editor-cmd.exe plugins disable quantum.hello
```

`--plugins-dir` / `--settings-dir` name the folders, otherwise `$QUANTUM_PLUGINS_DIR` /
`$QUANTUM_PLUGIN_SETTINGS_DIR` and then `./plugins` decide. `list` prints every record with its state and
reason, which is the difference between "the manifest does not parse", "the binary is not there" and
"the ABI does not match" - three failures that look identical from a build log. `check` validates a
folder without installing it and exits 1, so it is the CI gate for a packaged plugin. `describe` prints
the schema the editor would draw, which is the only way to see a plugin's UI without the editor.
`windows` lists every registered window, menu item and tool - and the frame each window describes, with
the command each row would run. `window <id>` goes one step further: `--set field=value` and
`--press button` go through the same `sendWindowInput()` the editor host calls, and the frame printed
after them is the plugin's answer, so a window that reads but never writes back is visible here.

From Lua, in a shipped game, the same registry is a global table:

```lua
print(Plugins.state("quantum.hello"))            -- "active", "failed", "missing", ...
for i, id in ipairs(Plugins.list()) do print(id) end

-- "repeat" is a reserved word, so a command argument with that name needs brackets here; the CLI
-- spells the same argument `repeat=3`.
local answer = Plugins.call("hello.ping", { ["repeat"] = 3, loud = true })
if answer.ok then
    print(answer.value)                          -- "Hello from a plugin Hello from a plugin ..."
else
    print(answer.error)                          -- why: unknown command, inactive plugin, refused
end
```

`Plugins.call` returns a table `{ok, value, error}` rather than raising: `_HAS_EXCEPTIONS=0` means
nothing may throw across the boundary, and a script that forgets to check still gets a readable answer.
Arguments are a Lua table keyed by name; numbers arrive as `Int` when whole and `Float` otherwise,
strings coerce on the plugin side, and a table or function argument arrives unset rather than
stringified. `Plugins.describe(id)` gives the manifest as a table (an unknown id gives an empty one, so
`describe("x").state` reads nil without indexing into nil), and `Plugins.isEnabled` /
`Plugins.setEnabled` read and write the same flag the editor writes.

One ordering to know: the project's Lua entry chunk runs during `QUANTUM_INIT`, before
`PluginManager::loadAll()`. `Plugins.list()` is therefore empty at the top level of `main.lua`, and
plugin work belongs in a frame callback (`Engine.onUpdate = function(dt) ... end`) or in a component's
`init`, both of which happen after the plugins are up.

## 10. Verifying it

```bash
cmake --build "$QUANTUM_BUILD_DIR" --target quantum-plugin-tests --config Debug
"$QUANTUM_BUILD_DIR"/quantum-plugin-tests.exe        # 47 cases, no GPU
```

The probe loads the binaries CMake really built, so it proves `LoadLibrary`, the ABI check and a vtable
call across the module boundary - the three things a clean compile cannot show. It also drives
`PluginManager::update` against the real `quantum-hello.dll` and reads the frame counter back through
`hello.ticks`, and it asserts the permission gates on an in-process context, so "the plugin was never
ticked" and "the plugin was not allowed to be ticked" are two different test failures.
`PluginManager::describe()` prints the same facts as text (engine version, ABI, platform, toolchain,
bitness, every record with its reason, every command) and is what the editor's browser will show.

The Lua surface is proven by a real engine run against a real project, not by a compile:

```bash
QUANTUM_PLUGINS_DIR="$PWD/build/plugins" \
  "$QUANTUM_BUILD_DIR"/Debug/quantum-engine.exe --project tests/plugin/lua-project \
  --frames 20 --window 640x360            # 20 checks, exits 0
```

`tests/plugin/lua-project/lua/main.lua` is that project's entry chunk; it asserts every `Plugins.*`
call, the int/string coercion of arguments, that an unknown command answers instead of raising, and
that the tick count moved - which only happens if `Engine::runFrame` reached the plugin.

## 11. What is not here yet

- A plugin cannot subscribe to an engine event of its own: it gets the frame tick and whatever a
  command is called with. Reaching a scene signal (component added, collision, load finished) means
  the plugin is called from a script or from the editor that owns that signal.
- No per-plugin resource VFS; `PluginContext::resourcePath()` builds a path, and a plugin reads it with
  plain file APIs.
- A plugin cannot add a render pass, a component type or a shader variant. Those still need the
  hand-maintained engine touchpoints (`quantum-api`, `quantum-shaders`).
