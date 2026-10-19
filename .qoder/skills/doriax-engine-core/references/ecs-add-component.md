# Adding a component - ordered checklist

A component is invisible in the product until **both** sides are done. Work top to
bottom; each step is a real file that already lists its siblings - copy the nearest
pattern (`ReflectionProbeComponent`, `LightComponent`).

## Engine side (`engine/`)

1. `engine/core/component/XComponent.h` - plain data struct/class. Components are
   header-only data; behavior lives in the subsystem that reads them.
2. `engine/core/registry/EntityRegistry.h` - add the `#include` to the component
   list (~:15-66).
3. `engine/core/registry/EntityRegistry.cpp` - add `registerComponent<XComponent>();`
   inside the `EntityRegistry` constructor (from :8). Watch the 64-bit signature cap:
   `grep -c registerComponent engine/core/registry/EntityRegistry.cpp`.
4. `engine/core/Doriax.h` - umbrella include (~:60) so project code sees it.
5. Consume it in the owning subsystem (`engine/core/subsystem/*.cpp`) - that is where
   `onComponentAdded`/`onComponentRemoved` and the signature filter decide which
   entities the component affects.
6. Optional Lua: add to `engine/core/script/binding/ECSClassesLua.cpp` with the
   canonical `.beginClass<XComponent>("Name") .addFunction/.addProperty` shape, then
   regenerate the API header with `doriax-reflector` (see `doriax-build-verify`).

## Editor side (`editor/`) - required for discoverability

7. `editor/Catalog.h` - `enum class ComponentType` entry (:61) plus the name/property
   maps in `Catalog.cpp` (`getComponentName` :154, `getComponentId` :155).
8. `editor/Stream.cpp` - encode/decode the component's YAML keys (`encodeSceneProject`
   :210 / `decodeSceneProjectEntities` :212 in `Stream.h`).
9. `editor/Factory.cpp` - C++ codegen so generated scene factories construct it.
10. `editor/window/Properties.cpp` - editable rows in the inspector.
11. `editor/window/dialog/ComponentAddDialog.cpp` - "add component" menu entry.
12. `editor/window/Structure.cpp` - icon/label in the hierarchy.
13. `editor/render/SceneRender3D.*` (or `SceneRender2D`) - viewport preview if it has
    a visual representation.

## Gotchas

- After editing `*.scene`/`project.yaml` by hand the generated C++ is stale: the
  project must be re-run in the editor (or through `doriax-build-generator`) so
  `.doriax/generated/` is rewritten.
- Entity id ranges are reserved: system 1-1000, user 1001+ (`ecs/EntityManager.h`).
  Do not allocate user entities below 1001.
- Adding a field to a component changes its serialized form; keep old keys loadable
  (`Stream.cpp` must tolerate a missing key with the `SceneSettings` default).
