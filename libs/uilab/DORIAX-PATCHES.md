# doriax patches on UILab

`libs/uilab` is a vendored snapshot of
[UniversalUIComponentsLab](https://github.com/alguemqualquer123/UniversalUIComponentsLab),
used as the doriax editor's UI component library. The snapshot is **pristine upstream
sources plus the patch files in `doriax-patches/`** — nothing else may be edited in
place, so a new upstream revision can be re-applied mechanically.

`third_party/` (Dear ImGui, GLFW) is deliberately **not** vendored: doriax builds ImGui
1.93.0-docking in `libs/imgui` and hands it to the library through the `doriax_imgui`
INTERFACE target (`CMakeLists.txt`, next to `IMGUI_SOURCES`).

## Workflow

```sh
# after changing anything under libs/uilab: rewrite the patch series
scripts/sync-uilab.sh regen

# take a new upstream revision and re-apply doriax's changes on top
scripts/sync-uilab.sh sync <sha|branch>
```

`regen` diffs `libs/uilab` against the scratch clone in `build/uilab-upstream` (git
ignored) and writes one patch per changed file; `sync` copies the pristine tree back
over `src/`, `cmake/`, `examples/`, `CMakeLists.txt` and `README.md`, then `git apply`s
the patches in order. Review either with `git diff -- libs/uilab`.

## How doriax consumes the library

`add_subdirectory(libs/uilab)` runs with:

| Switch | Value | Why |
| --- | --- | --- |
| `UILAB_BUILD_APP` | `OFF` | doriax owns the window, the loop and the backends |
| `UILAB_BUILD_DEMOS` | `OFF` | the seven paradigm pages are not part of the editor |
| `UILAB_BUILD_EDITOR` | `ON` | the dockable panels are what the editor uses |
| `UILAB_PROVIDE_THIRD_PARTY` | `OFF` | ImGui + GLFW come from the host |
| `UILAB_IMGUI_TARGET` | `doriax_imgui` | include paths only; sources stay in the editor exe |
| `UILAB_FETCH_DEPS` | `OFF` | no network at configure time |
| `UILAB_EXPORT_TARGETS` | `OFF` | nothing here installs |

`uilab_core` and `uilab_editor` are STATIC and link into `doriax-editor` /
`doriax-editor-cmd`; ImGui symbols resolve against the objects each executable already
compiles.

## Patch index

One patch per changed file, rewritten by `regen`; `NNNN-...-new.patch` creates a file
doriax added.

| Patch | What it changes |
| --- | --- |
| `0001-CMakeLists-txt.patch` | Builds as a static library inside a host: GLFW/OpenGL are linked only for `UILAB_BUILD_APP` (the libraries never touch a window), `UILAB_EXPORT_TARGETS` defaults to the top-level state instead of `ON`, the per-target defines become `PRIVATE` so `NOMINMAX`/`WIN32_LEAN_AND_MEAN` cannot leak into the host, and `IMGUI_DEFINE_MATH_OPERATORS` is set per target because the host's ImGui may be built without the free vector operators. |
| `0002-src-core-AppContext-h.patch` + `0013-src-host-HostHooks-h-new.patch` | The `uilab::HostHooks` seam: seven `std::function` slots on `AppContext` through which an embedding application supplies the content of a panel, plus `installDefaultDockTree` for a host that keeps its own dock arrangement. |
| `0003-src-core-EventLog-cpp.patch` | Bug fix, not a doriax feature: `Push()` never filled `wallTime`/`frame`, so every console line printed `00:00.000`. Stamps them from the active ImGui context, and leaves them at 0 when there is none so the headless self-test is unaffected. Worth sending upstream on its own. |
| `0004-src-layout-DockingLayout-cpp.patch` + `0005-src-layout-DockingLayout-h.patch` | `layout::DockSpaceId()`, and `BeginFrame()` honours `installDefaultDockTree`: it still creates the dockspace host window, but stops installing the laboratory's node tree on a host that builds its own. |
| `0006..0012-src-panels-*.patch` | `AssetBrowserPanel`, `ComponentTreePanel`, `HierarchyPanel`, `InspectorPanel`, `MenuBarPanel`, `StatusBarPanel`, `ToolbarPanel`: each checks its hook right after opening its window (or main menu bar) and, when set, lets the host draw the content before the library closes it again. Empty hook = the laboratory's own demo content, so the standalone app is byte-for-byte unchanged in behaviour. |

Nothing else is patched: the library's theme, registry, canvas, undo stack and log are
used as they are - doriax feeds data into `EventLog`, `AssetLibrary` and `CanvasScene`
rather than replacing them, and the host keeps applying its own ImGui style because
`ThemeManager::Apply()` is only called by the standalone application.
