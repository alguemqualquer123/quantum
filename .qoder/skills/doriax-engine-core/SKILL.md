---
name: doriax-engine-core
description: Work on the doriax C++ engine core (engine/) - ECS entities/components/subsystems, the engine loop, DORIAX_INIT bootstrap, scene settings, Lua and C++ scripting bindings. Use when adding or changing engine components, subsystems, scene/post-process settings, script bindings, or anything in engine/core.
---

# Doriax engine core

## Overview

`engine/` is the runtime library (target `doriax`, shared dll by default). The ECS is
entity -> components -> subsystems, all owned by a `Scene`. Follow the checklists in
`references/` before touching a new area: the registration touchpoints are spread
across several files and are easy to miss.

Specialised skills that go deeper than this overview: `doriax-api` (the full
add-a-component/persist-a-property checklist and the caps), `doriax-scripting` (Lua and
C++ script hooks), `doriax-performance` (threading, pools, `MAX_*` limits),
`doriax-rendering`, `doriax-physics`, `doriax-ui`, `doriax-safety-rules`.

## Architecture in one screen

- `Engine` is all-static (`engine/core/Engine.h:199`). The platform `System` pumps
  `Engine::systemDraw()` (`Engine.cpp:1035`): fixed-step loop capped at 100 steps
  (:1074-1092) -> variable update (`onUpdate`, `scene->update(dt)`, `onPostUpdate`,
  :1096-1100) -> `onDraw` + per-scene `draw()` + composite + `SystemRender::commit()`
  (:1103-1114). Scene stack is guarded by `sceneListMutex` + `drawSemaphore`.
- `DORIAX_INIT` (`Engine.h:135-144`) declares `void init()` and registers it into
  `Engine::getOnInit()`, called from `Engine::systemInit()` (`Engine.cpp:931`). The
  editor build defines `NO_CPP_INIT`/`NO_LUA_INIT`; project exes use it.
- `Scene : EntityRegistry` (`Scene.h:30`). `EntityRegistry` composes `EntityManager`
  (system ids 1-1000, user ids 1001+, free-list + generation counters) and
  `ComponentManager` (typeid-string -> dense index, sequential `ComponentId`).
- `Signature = std::bitset<64>` (`ecs/Signature.h:12`): **hard cap of 64 component
  types**; 53 are registered today (`registry/EntityRegistry.cpp:8+`, count with
  `grep -c registerComponent`). Check the count before adding one.

## Adding a component

Copy the closest existing one. `ReflectionProbeComponent` is the reference path;
`references/ecs-add-component.md` lists every required touchpoint in order
(engine registration + the editor-side files without which it is invisible in UI).

## Adding a subsystem

1. Class in `engine/core/subsystem/X.{h,cpp}` deriving `SubSystem`
   (`ecs/SubSystem.h`: `load/draw/destroy/update/fixedUpdate` +
   `onComponentAdded/onComponentRemoved` hooks).
2. `registerSystem<XSystem>();` inside `Scene::init()` (`Scene.cpp:30-37`).
3. Look it up with `scene->getSystem<XSystem>()`; forward new lifecycle calls from
   `Scene` if needed.

## Settings and scripting

- Scene settings are pure data in `SceneSettings` (`engine/core/SceneSettings.h:47`,
  single source of defaults). Adding one: field in `SceneSettings` -> delegating
  getter/setter in `Scene` when a side effect is needed (e.g. shader reload) ->
  YAML key in `editor/Stream.cpp` -> UI row in `editor/window/Properties.cpp`.
- Lua bindings: `engine/core/script/binding/*.cpp` use LuaBridge
  `getGlobalNamespace(L).beginNamespace(...).addFunction/addProperty/addStaticFunction`.
  Every string there is parsed by `doriax-reflector` (see
  `doriax-build-verify`), so keep the call shape canonical.
- C++ script mode: user classes derive `doriax::ScriptBase` and expose fields with
  `DPROPERTY("Name", Type)` (`core/script/ScriptProperty.h:16-24`). The editor
  discovers them statically via `editor/util/ScriptParser.cpp`; `Factory.cpp` emits
  `extern "C" initScripts/cleanupScripts` and hot-reloads through `Conector`.

## Assets the engine carries itself

`BuiltinModels` (`engine/core/io/BuiltinModel.{h,cpp}`) serves GLB bytes compiled
into the dll behind a `builtin://Name.glb` path, so a project without the asset
still renders it. Anything that resolves a model path must let that scheme
through untouched - `FileData::getSystemPath()` (`core/io/FileData.cpp:158+`)
returns it as-is, which is what keeps cache keys from claiming to be files - and
`MeshSystem` picks bytes over disk in one place, `parseGLTFFromPath()`, used by
both the sync `loadGLTF` and the async worker.
Regenerate the header with `engine/tools/generate_builtin_model.py`; it emits a
byte array like `editor/resources/fonts/*_ttf.h`, because MSVC folds a 17k-part
string-literal concatenation into one token and dies with C1060.

## Conventions

`// (c) Eduardo Doria` + `// SPDX-License-Identifier: MIT` header on every file;
`namespace doriax` with classic include guards in the engine (`#pragma once` in
editor); English comments explaining *why*; logging via `Log::error/warn` in the
engine (`Out::*` in the editor); ECS hot paths throw `std::out_of_range` on
misuse. There are no unit tests - verification is compile + run (see
`doriax-build-verify`).

## Resources

- `references/ecs-add-component.md` - ordered checklist with file:line anchors for a new component, engine and editor side.
- `references/scene-project-format.md` - `project.yaml` / `*.scene` YAML schema notes and which editor files encode/decode them.
