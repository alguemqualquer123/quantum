---
name: doriax-game-projects
description: Work on doriax game projects and templates (YAML scenes, Lua scripts, C++ DPROPERTY scripts, ProjectBuild.cmake, exported builds) rather than engine or editor code. Use when creating or editing a project under templates/, writing gameplay logic in Lua/C++, running a project headless, or debugging generated build files.
---

# Doriax game projects

A doriax *project* is data + scripts: `project.yaml`, `scenes/*.scene`, `scripts/`,
`assets/`. The C++ the game actually compiles is generated from it - never hand-edit
generated output.

## Anatomy

```
project.yaml        # name, canvas, scalingMode, textureStrategy, vsync/window*,
                    # luaDir, startSceneId, terrainEditor{}, tabs[], scenes[], standaloneBundles
scenes/main.scene   # id/name/scene{...}/sceneType/mainCamera/maxValues/shaderKeys/entities[]
scripts/*.lua       # attached through ScriptComponent; self.scene / self.entity available
scripts/*.h|.cpp    # C++ scripts: classes deriving doriax::ScriptBase with DPROPERTY fields
CMakeLists.txt      # GENERATED every build - do not edit
ProjectBuild.cmake  # the hook for your own build settings; never overwritten
.doriax/            # generated/: scene + bundle factories, main.cpp, scene_scripts.cpp,
                    # engine-api/ snapshot, thumbs/
AGENTS.md           # auto-generated project brief (Generator.cpp ~:1109)
```

Read the project's own `AGENTS.md` first - it is generated and states the exact local
engine API snapshot path for that project.

## Editing rules

- Any change to `*.scene`, `*.bundle` or `project.yaml` invalidates the generated C++:
  regenerate by playing/running in the editor, or headless with
  `doriax-build-generator generate --project <dir>` (then `configure` + `build`).
- Put custom compiler flags, includes or link libraries in `ProjectBuild.cmake`, using
  `${DORIAX_TARGET}` and `${DORIAX_SCRIPTS_DIR}` instead of literal names - the target
  name differs between the in-editor build and the exported one.
- Entity ids: 1-1000 are reserved for system entities, user entities start at 1001.
- Enum values in YAML are written lowercase (`shadowQuality: low`); colors/vectors are
  flow sequences (`[0.25, 0.45, 0.65, 1]`, sRGB).

## Running and building without the editor

`$DORIAX_BUILD_DIR` (default `build`) is the one build tree; see `doriax-build-verify`.

```bash
"$DORIAX_BUILD_DIR/Debug/doriax-engine.exe" --project templates/fps-fps --window 1280x720
doriax-build-generator.exe status   --project <dir>     # stale generated files? exit!=0
doriax-build-generator.exe generate --project <dir>
doriax-build-generator.exe build    --project <dir> --config Release
doriax-resource-compiler.exe pak    --project <dir> --out game.pak
```

Limits worth knowing: `resources.pak` is compiled out of the editor-config dll build,
and `.scene` YAML deserialization is editor-side (projects run through generated C++
or Lua entries), so `doriax-engine --project` currently executes Lua entries only - the
runner logs this explicitly rather than failing silently.

## Lua vs C++ scripts

- Full hook/VFS/hot-reload detail is in `doriax-scripting`; gameplay-relevant limits
  (physics components, water, procedural noise) are in `doriax-physics`,
  `doriax-water` and `doriax-procedural-noise`.
- Lua is the fast path for gameplay; API surface is what `engine/core/script/binding/*.cpp`
  exposes, and `engine_api_suggestions.h` (regenerate with `doriax-reflector`, see
  `doriax-build-verify`) is the completion list - use it to confirm a method exists
  before writing code against it.
- C++ scripts need `DPROPERTY("Name", Type)` on each exposed field so the editor's
  `ScriptParser` can offer it in the inspector, and are hot-reloaded through the
  project shared lib (`Conector` looking up `initScripts`).
- No engine internals are available to project code: if a feature needs new engine API,
  that is a `doriax-engine-core` task (binding + reflector regeneration).

## Templates as seeds

`templates/` holds complete sample projects: `platformer-2d`, `fps-fps`, `moba-click`,
`charactercontrol-main`, `third-person`, `voxelworld-main`, `mobile-game`. Copy one and
rename in `project.yaml` rather than starting from scratch. They also carry stray
`editor.log` / `settings.yaml` / `editor_layout.ini` from local runs - do not commit
changes to those.
