---
name: quantum-editor-dev
description: Work on the Quantum editor (editor/) - ImGui panels and windows, dock layout, AppSettings persistence, undo/redo commands, theming, output logging, and the headless CLI executables. Use when adding or changing editor windows, menus, dock behaviour, settings, commands, or editor/cli code.
---

# Quantum editor development

## Overview

`editor/` is a Dear ImGui 1.93 (docking) application in `namespace quantum::editor`.
`App.cpp` owns every window: it constructs them, calls `show()` each frame, and
places them in the dock. New feature = new window class + a handful of wiring lines
in `App.cpp`; do not grow `App.cpp` with feature logic.

The chrome - dockspace, main menu bar, console panel, and the window frames of the
outliner / inspector / content browser - comes from the vendored `libs/uilab` library
through `editor/UILabHost.{h,cpp}`. `libs/uilab` is pristine upstream plus the patch
series in `libs/uilab/quantum-patches`: read `libs/uilab/QUANTUM-PATCHES.md` and run
`scripts/sync-uilab.sh regen` after editing anything there, and prefer adding a host
seam (a `HostHooks` slot or a `HostPanel` name) over changing library behaviour.

## The window contract

Every panel follows the same shape (`OutputWindow`, `Structure` are the models):

1. `editor/window/X.h`: `static constexpr const char* WINDOW_NAME`, ctor takes
   explicit deps (e.g. `Structure(Project*, SceneWindow*)`), public
   `show()` / `setOpen(bool)` / `isOpen()`, private draw helpers.
2. `App.cpp`: member pointer, constructed near the others, `xWindow->show()` inside
   the per-frame draw list (see `outputWindow->show()` at `App.cpp:1547`).
3. Visibility: add an `AppMenuCommand` enum value, register it with
   `menuToggle(AppMenuCommand::ToggleX, X::WINDOW_NAME, x->isOpen())` in the View
   menu (`App.cpp:341+`), and handle the command with `setOpen(!isOpen())` in the
   dispatch switch.
4. Dock placement: `ImGui::DockBuilderDockWindow(X::WINDOW_NAME, node)` in the dock
   layout builder (`App.cpp:959-968`).
5. Persisted visibility: add a field to `PanelVisibilitySettings`
   (`editor/AppSettings.h:14`) so the View menu state survives restarts.
6. Register the `.cpp` in the fixed `QUANTUM_EDITOR_SOURCES` list in the root
   `CMakeLists.txt` (window files are around :551-563). That list is hand-maintained;
   a missing entry means the window silently never appears.

A window whose frame the library owns adds a seventh step: split `show()` (bookkeeping,
still called from `App.cpp`) from `drawContent()` (everything between `Begin` and `End`),
call `setHostedWindow(true)`, and hand the host `openFlag()` - it registers the window
name and that flag as a `uilab::HostPanel`, so the layout, the View toggles and the X
button are unchanged from the user's side. `Structure`, `Properties` and
`ResourcesWindow` are the models; `UILabHost::drawDataPanels()` (`App.cpp:1966`) opens
them. Keep `show()` working for the case where no host exists.

`references/window-pattern.md` has the finer points (thread-safe pushes, redraw
requests, modal/toast windows, theme usage). `quantum-ui` covers the in-game UI system
and the editor theming/docking details; `quantum-safety-rules` states the rules that
override any convenience here (never launch the editor, one build tree, no `git add -A`).

## Supporting APIs

- Script editor: `editor/window/widget/CustomTextEditor` is a hand-written widget (one
  instance per open file, drawn from `CodeEditor.cpp`), not an `ImGui::InputText`. Its
  completions come from `SemanticSuggestions` fed by the generated
  `engine_api_suggestions.h` plus hand-added `Engine.onX` events; `Ctrl+Space` and
  `Shift+Space` open the list with `manualInvoke` (which bypasses the min-prefix rule, so
  Shift+Space also has to drop the space Windows queued with it), `Ctrl+/` toggles the line
  comment, `Alt+Shift+A` the block comment and `Shift+Alt+F` reformats. These three are
  whole-buffer steps built as `addUndoRecord()` / mutate / `tokenizeAll()` /
  `finalizeUndoRecord()`, and they must not call `scrollToCursor()`: when dispatched from
  the menu or the command palette there is no editor window current, and that function
  reads `ImGui::GetWindowHeight()`.
- Logging: `Out::info/success/warning/error/build` (`editor/Out.h:58-73`), printf-style,
  routed into `OutputWindow`'s ring buffer. User-facing build output goes through it.
- Settings: `AppSettings` is all static fields mirrored to `settings.yaml`
  (`AppSettings.cpp:107-117`). Add a getter/setter pair plus load/save lines; keep
  machine-specific paths editor-wide rather than per-project.
- Undo/redo: implement `editor/command/Command.h` (`execute/undo/mergeWith/
  affectsStructure`) in `editor/command/type/XCmd.{h,cpp}` and push it onto the
  per-scene `CommandHistory`. Never mutate scene state from a window without a command.
- Theme: `Theme::Colors` tokens + `Theme::dpi(...)` for sizes (`editor/Theme.h`).
  Colors derive from the active preset (dark/light) and accent, so prefer
  `ImLerp`/`ImGuiCol_*` over hardcoded RGBA.
- Child processes: `editor/util/CommandRunner.cpp` (`run` :29, `runCaptureNoWindow`
  :50, `msvcEnvPrefix` :124) is how the editor shells out; `MsBuildProgress` turns
  MSVC output into progress.
- Project marker and shell association: a project stays a folder holding `project.yaml`,
  and `editor/util/ProjectFile.{h,cpp}` drops one `<name>.dproj` marker beside it — written
  on open and on create, never read back (`resolve()` is extension-agnostic, so a marker
  written under another extension still opens). `editor/util/ShellAssociation.{h,cpp}` binds
  that extension to this build under `HKCU\Software\Classes` only (ProgID `Quantum.Project`,
  `shell\open\command` = `"exe" "%1"`, `DefaultIcon` = the exe's own first icon group, refuses
  an extension another program owns, `SHChangeNotify` after every change).
  `editor/cli/ProjectFileType.{h,cpp}` is the single entry point used by both the Project-menu
  toggle and `project-file status|register|unregister`; registration is never automatic. The
  extension is the editor-wide `AppSettings::getProjectFileExtension` (default `.dproj`, which
  Delphi also claims), and `LaunchOptions` turns a bare marker path in `argv[1]` into the
  project directory.
- Offscreen previews: `editor/render/preview/{MaterialRender,ModelRender,MeshPreviewRender}`
  each own a `Scene` with a render-to-texture camera. `Engine::executeSceneOnce(scene)` only
  ticks and stages it - the pixels arrive on the next `Engine::systemDraw()`, and that runs
  in `App::engineRender()` (`App.cpp:2804`) *before* `App::show()` (`EditorFrame.cpp:183-190`),
  so a preview texture read from inside the ImGui pass is always one frame behind. A
  readback (`GraphicUtils::saveFramebufferImage`, which is `glReadPixels`) must therefore
  happen in the `engineRender()` block after `systemDraw()`, next to
  `resourcesWindow->processMaterialThumbnails()` (`App.cpp:2987`) - that is why
  `MaterialBrowserWindow` splits `stageNextThumb()` (UI pass) from
  `processPendingPreviews()` (called from `engineRender`). Ask for the follow-up frame with
  `Backend::getApp().requestRedraw()`, or the editor idles and the staged preview never
  lands.
- The editor turns on `Engine::setAsyncLoading(true)` (`App.cpp:2660`), so a `Texture` bound
  to a preview scene draws with the empty-white `TextureRender` for several frames. Read the
  framebuffer back only once every bound map is in `TexturePool::get(id)` - re-registering
  the one-time scene each of those frames - or the cache stores a textureless white sphere
  and shows it forever (`Properties::getMaterialPreview` waits the same way). Preview PNGs
  live in `%LOCALAPPDATA%/quantum/material-previews` keyed by a hash of the file set; that
  hash mixes `PREVIEW_REVISION` (`MaterialBrowserWindow.cpp`), so **bump it whenever the
  preview renderer changes what a finished sphere looks like**, otherwise the stale images
  stay valid and the fix is invisible.
- Map-set discovery: `editor/util/MaterialSets.{h,cpp}` groups loose texture files into a
  `MaterialSet` by filename token (`diffuse`/`nor_gl`/`rough`/`arm`/...) and builds a
  `Material` from it. Resolution tokens are stripped **wherever they sit** (`4096`,
  `2k`, `0_5k`, a bare number 32..16384 - below that it is a variant index, so
  `rock_02` keeps its suffix), which is what makes `brick_diff_1k` and `brick_4096`
  one entry instead of one per size. A one-file set whose name carries no map token
  at all is dropped: a lone screenshot or app icon is not a material. `Material` has no separate
  roughness slot, only the glTF `metallicRoughnessTexture` (G = rough, B = metal), so a lone
  roughness map is listed in the set and deliberately left unbound - binding it would put
  roughness in the metal channel.

## Headless executables

The suite is splitting out of `App.cpp`. `editor/main.cpp` dispatches CLI
subcommands before `Backend::init()` when built with `QUANTUM_CLI_FRONTEND`; targets
live in `tools/*/*.cmake` and are `include()`d from the root `CMakeLists.txt`:
`quantum-reflector` (API metadata, replaces `generate_api_suggestions.py`),
`quantum-resource-compiler` (`pak` / `shaders` / `inspect`),
`quantum-build-generator` (generate/configure/build/status), `quantum-engine`
(standalone game runtime). Editor code that a CLI needs must exist as a free
function in `editor/cli/` taking plain structs + `std::function` callbacks - not as an
`App`/`Project` member.

The verbs currently dispatched there are `export`, `project-file`, `components`, `assets`,
`material`, `doctor`, `scripts`, `shortcuts`, `api`, `plugins`, `terrain`, `worldcreator`,
`shaders`, `pak`, `inspect`, `graph`, `generate`, `configure`, `build`, `status`. None of them
may construct a `Project`: `Project()` runs `resetConfigs()`, which calls `getEditorHost()`,
`ShaderBuilder::invalidateCustomShaders()` and `Engine::clearUnusedPools()`. Panels called from
a verb therefore keep their logic in plain statics (`WorldCreatorWindow::writeHeightmap` /
`measureField` are public statics precisely so `worldcreator selftest` exercises the *Generate
button's* code, not a copy of it), window registration uses TU-local globals instead of `App.h`,
and a `nullptr` project is a supported constructor argument. A probe verb writes to `std::cout`
rather than `Out::*`, because `Out` routes into `OutputWindow`; the verbs that do use `Out` call
`Out::setOutputWindow(nullptr)` first (pattern `editor/cli/CommandLine.cpp:551`).

`editor/cli/ComponentLibrary.cpp` is that pattern done right and is also a subcommand of
the console twin: `quantum-editor-cmd components list|install --project <dir> [--only A,B]
[--lua <dir>] [--into <subdir>] [--no-demos] [--overwrite]`. It installs
`engine/components/` (59 ready-made Lua gameplay scripts) into `<luaRoot>/<subdir>` and the
six demos + their shared driver into `<luaRoot>/demos`, counting them separately
(`ReadyMadeInstallResult::demos`) because both callers print how many *components* arrived.
One `place` lambda serves both loops: never overwrite a file the project already owns
unless `--overwrite`. `Project::installReadyMadeComponents` (`editor/Project.cpp:4771`, called at
`:5120`) is the same free function with a null logger, reached from `createNewProjectAt` - so a new
project is seeded without the user running anything.

`editor/util/EngineAssets.{h,cpp}` is the single resolver behind both the Resources window's
Engine origin and its CLI twin, `quantum-editor-cmd assets list|find|copy`
(`editor/cli/EngineAssetsCli.cpp`). `engineDir()` caches its answer for the process (the
`copy_quantum_editor_engine` copy beside the exe, else a checkout's `engine/`;
`overrideEngineDir` - what `--engine-dir` calls - is ignored after the first query), `roots()`
keeps only the folders that exist in display order (`assets`, `components`, `shaderlib`), and
`projectCopyDestination` is the one rule for where a delivered file lands - `<luaDir>/Components`
for a `.lua`, `assets/engine/<root>` for anything else - so the panel's *Copy into Project*
(`copyEngineSelectionIntoProject`, `editor/window/ResourcesWindow.cpp:2553`, undoable through
`CopyFileCmd`) and `assets copy` cannot drift apart. Engine content is read-only everywhere
except that copy:
`isEnginePath` is what refuses a non-engine source in the CLI, and the same test is what draws
the row read-only in the window.

`editor/util/ReadyMadeComponents.{h,cpp}` turns that library into rows of the *New component*
dialog: `libraryComponentCategories()` groups the files by their `-- category:` marker,
`runsLibraryComponent()` hides one an entity already runs, and `attachLibraryComponent()` installs
the file when the project lacks it and adds the `ScriptEntry` as a single `MultiPropertyCmd`
(`AddComponentCmd(ScriptComponent)` when the entity has none, then
`project->updateScriptProperties(...)` plus `PropertyCmd<std::vector<ScriptEntry>>` - the same
recipe as "New Script"). Undo removes the row but keeps the installed file. `ComponentAddDialog`
draws them after the engine component types under a "Ready-made scripts" heading, so it must get
`setReadyMadeComponents()` before `open()`; both lists share its `drawCategoryHeader`/`drawEntryRow`
helpers. This lives in its own TU on purpose: `Properties.cpp` is already at MSVC's
object-section limit (see `quantum-build-verify`).

## Proving an ImGui panel without launching the editor

`quantum-editor-cmd` compiles the whole `QUANTUM_EDITOR_SOURCES` list, so a verb dispatched in
`editor/main.cpp` before `Backend::init()` has real ImGui and no renderer. That is how a panel
gets proven while rule 1 keeps `quantum-editor.exe` closed (`editor/cli/WorldCreatorCommands.cpp`,
verb `worldcreator selftest`, is the reference harness). The shape:
`ImGui::CreateContext()` + `io.DisplaySize` + `io.Fonts->GetTexDataAsRGBA32`, then
`NewFrame` / draw / `Render`, with input injected through `io.AddMousePosEvent` +
`AddMouseButtonEvent` **before** `NewFrame`. Two things will waste an hour if you do not know them:

- **ImGui hands hover only to the front/hovered window.** A control drawn into a window created
  later in the same run eats every click, so each per-control check must share ONE host window -
  `SetNextWindowFocus()` does not fix it. A test that aims at a disabled control then passes
  vacuously; pair it with "the same aim edits an enabled control".
- **A slider mutates on the *held* frame; a checkbox and a button fire on the *release* frame.**
  A click test has to merge the two frames' reports (`mergeTouch`) or every slider reads "no edit".
  A checkbox only answers inside its square (`rect.from.x + frameHeight*0.5`), not on its label.

Anything that needs a live scene (target pickers, viewport overlays, the undo of an applied map)
stays unverifiable here - say so instead of claiming it works.

## The tracked-entity list is what gets saved



`SceneProject::entities` is the authority for both the outliner and
`Stream::encodeSceneProject` (`editor/Stream.cpp:2551`), so an id left in it after its entity is
destroyed is drawn as a blank row (`Structure::getObjectIcon` falls through to
`ICON_FA_CIRCLE_DOT` at `editor/window/Structure.cpp:873` and the name is "") *and* written into
the `.scene` file as a nameless entity that the next load really creates — the corruption then
survives save/load and multiplies. Any code that destroys entities outside `DeleteEntityCmd` must
follow it with `ProjectUtils::reconcileTrackedEntities` (`editor/util/ProjectUtils.cpp:1726`),
which prunes destroyed ids and drops repeated ones. `ProjectUtils::destroyModelNodes`
(`:392`) is the usual offender: a model's bone/mesh nodes are tracked whenever they were made in
the editor, which is what `ProjectUtils::hasTrackedModelEntities` (`:407`) exists to answer.

Those same recycled ids make a model claim an unrelated row: `ModelComponent`'s
`bonesIdMapping`, `meshNodesMapping` and `nodesIdMapping` store raw entity ids, and after a node is
destroyed the pool hands the id to the next entity it creates. `getLockedEntityParent`
(`editor/util/ProjectUtils.cpp:560`) only trusts a mapping hit when the entity still carries that
model's `BoneComponent` or its `Transform` parent chain reaches the model; without the evidence,
deleting an authored entity fails with "It is a locked child of another component"
(`editor/command/type/DeleteEntityCmd.cpp:411`).

## Conventions

License header (`// (c) SR VINIX` + SPDX MIT) on every file; `#pragma once`;
4-space indent matching the surrounding file; English "why" comments; no new
third-party deps without a strong reason. Verify with `quantum-build-verify`.

## Resources

- `references/window-pattern.md` - window lifecycle details, thread-safe notifications, redraws, docking pitfalls, and the App.cpp wiring checklist.
