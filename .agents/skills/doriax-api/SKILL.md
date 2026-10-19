---
name: doriax-api
description: Extend doriax's own API surface - there is no generic reflection registry, so adding a component or a persisted/editor-editable/Lua-visible property means editing hand-maintained parallel lists across engine, editor catalog, YAML stream, factory and CMake. Also covers doriax-reflector and engine_api_suggestions.h. Use when adding a component, a setting, or an enum, or when something you added is invisible in the editor.
---

# API, reflection and persistence

## The shape of the codebase

There is **no reflection registry**. Engine, editor, serializer and code generator each
keep their own parallel list of components. Adding one type means touching all of them;
miss one and the feature compiles but is invisible (never crashes). Two caps to respect:
`Signature = std::bitset<64>` (`engine/core/ecs/Signature.h:12`) is a **hard limit of 64
component types**, and `editor/Catalog.h:113-115` states that new `ComponentType` values
**must be appended** because the ordinal is the bit position in the EntityBundle
component-override mask. Count the registrations with
`grep -c registerComponent engine/core/registry/EntityRegistry.cpp` before adding one.

## Checklist: one new component, one persisted + editor-editable + Lua-visible property

1. `engine/core/component/MyComponent.h` - a plain struct (shape:
   `engine/core/component/ScriptComponent.h:28-30`), `DORIAX_API` export macro.
2. `engine/core/registry/EntityRegistry.h` include (`:15-66`) and
   `registerComponent<MyComponent>()` in the ctor list (`:10-62`, e.g. ScriptComponent
   `:50`).
3. `engine/CMakeLists.txt` hand-maintained source list (`:835-839` style) **and** the
   Xcode project `engine/workspaces/xcode/Doriax.xcodeproj/project.pbxproj`. Same
   trap on the editor side: `DORIAX_EDITOR_SOURCES` in the root `CMakeLists.txt`.
4. `editor/Catalog.h:61-116` append the `ComponentType`; add an `UpdateFlags` bit if a
   change must trigger a rebuild (`:30-58`).
5. `editor/Catalog.cpp` - `getComponentName` (`:2979`, this function *is* the YAML key
   producer), `getComponentType` (`:3210`), the `getProperties` switch (`:3357`; Sound
   case `:3115`), and a `kFastComponentResolvers` row `{ComponentType,
   findComponentPtr<T>, resolveXPropertyFast, enumerateXProperties}` (`:2863-2916`,
   lookup `tryGetFastProperty` `:2918`).
6. `editor/Stream.h` declare `encodeMyComponent`/`decodeMyComponent` (pattern `:291-298`);
   `editor/Stream.cpp` implement and hook `encodeComponents` (`:3009`,
   `signature.test` + `compNode[Catalog::getComponentName(...)] = encode...`
   `:3012-3015`) and `decodeComponents` (`:3285`).
7. `editor/Factory.cpp` - a `case ComponentType::MyComponent:` so exported/generated
   scene factories emit it (`:2130` pattern); make it addable in
   `editor/window/dialog/ComponentAddDialog.cpp`; `editor/util/ProjectUtils.cpp`
   enumerates components too.
8. Lua: `engine/core/script/binding/ECSClassesLua.cpp`
   `.beginClass<MyComponent>("MyComponent")...addProperty("myProp", &MyComponent::myProp)`
   and regenerate the API header (step 9).
9. API header: the build regenerates `engine_api_suggestions.h` through the **old
   Python** `generate_api_suggestions.py` (`CMakeLists.txt:397-410`);
   `doriax-reflector reflect --bindings engine/core/script/binding --out <hpp>
   [--json api.json]` is the standalone tool (`tools/reflector/`, target
   `CMakeLists.txt:467`, `reflector_main.cpp:25-46`), and
   `check --against engine_api_suggestions.h` exits 1 on drift. Consumers:
   `editor/ai/AiEngineApiContext.cpp:6`, autocomplete in
   `editor/window/widget/CustomTextEditor.cpp:7`. Note: no `.github` workflow runs
   `check`, so staleness is caught only if someone runs it (`.qoder/skills/
   doriax-build-verify/SKILL.md:56`).

Settings-only variant (a scene setting rather than a component) has a shorter path:
`SceneSettings.h` field with its default -> `Scene` delegating setter if a reload is
needed -> `Stream.cpp` key -> `Properties.cpp` row -> codegen in `Factory.cpp`.

## Scene settings and schema evolution

`SceneSettings` (`engine/core/SceneSettings.h:47`) is the **single source of defaults** -
member initializers are the factory values, and the editor reads a default-constructed
`SceneSettings` instead of building a `Scene` (which owns a physics runtime).

There is **no numeric schema version key** for scene YAML. Compatibility is handled by
keeping legacy wire names readable, as `Stream.cpp:975-994` does with an explicit
"remove these branches once support for scenes written by older editor versions is
intentionally dropped". Follow that convention: read old names, write new ones, and
mark the branch so a future cleanup can find it. Separately, changing anything that
affects compiled shaders requires bumping the `.sdat` cache version dir
(`App.cpp:2768`, history `:2732-2767`).

## Constraints that bite

- `_HAS_EXCEPTIONS=0` propagates from `engine/libs/joltphysics/CMakeLists.txt:545-548`
  through `doriax`'s PUBLIC link (`engine/CMakeLists.txt:899-905`) into the editor. It
  is keyed on `CPP_EXCEPTIONS_ENABLED`, which **the repo never defines**. Nothing may
  throw across a task/future boundary; report failure through state
  (`editor/shader/ShaderBuilder.cpp:611-618`) and prefer error codes over throws.
- `HybridArray` is a fixed stack array in engine builds (`engine/core/util/HybridArray.h:48`)
  but a resizing vector under `#ifdef DORIAX_EDITOR` (`:17-41`) - an index cap can pass
  in the editor and overflow in a game build.
- Caps live in `engine/core/Engine.h`: `MAX_SHAPES 10` `:110`,
  `MAX_SHAPE_POINTS_2D 114`, `MAX_SHAPE_VERTICES_3D 118`, `MAX_SUBMESHES 67`,
  `MAX_BONES 88`, `MAX_SPRITE_FRAMES 79`, `MAX_TILEMAP_TILES 75`,
  `MAX_EXTERNAL_BUFFERS 102`, `SSAO_KERNEL_SIZE 32` `:98`.
- Logging: engine `Log::verbose/warn/error` always format and route
  (`engine/core/util/Log.cpp:10-45`); `Log::debug` is compiled out under `NDEBUG`
  (`:24-31`). Editor-side user output is `Out::*` (`editor/Out.h:58-73`).
- Verification: no unit tests. `ctest` exists but there are none. Build with MSVC, then
  run a headless executable - see `doriax-build-verify`.
