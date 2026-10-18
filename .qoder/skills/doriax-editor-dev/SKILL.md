---
name: doriax-editor-dev
description: Work on the doriax editor (editor/) - ImGui panels and windows, dock layout, AppSettings persistence, undo/redo commands, theming, output logging, and the headless CLI executables. Use when adding or changing editor windows, menus, dock behaviour, settings, commands, or editor/cli code.
---

# Doriax editor development

## Overview

`editor/` is a Dear ImGui (docking) application in `namespace doriax::editor`.
`App.cpp` owns every window: it constructs them, calls `show()` each frame, and
places them in the dock. New feature = new window class + a handful of wiring lines
in `App.cpp`; do not grow `App.cpp` with feature logic.

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
6. Register the `.cpp` in the fixed `DORIAX_EDITOR_SOURCES` list in the root
   `CMakeLists.txt` (window files are around :551-563). That list is hand-maintained;
   a missing entry means the window silently never appears.

`references/window-pattern.md` has the finer points (thread-safe pushes, redraw
requests, modal/toast windows, theme usage).

## Supporting APIs

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

## Headless executables

The suite is splitting out of `App.cpp`. `editor/main.cpp` dispatches CLI
subcommands before `Backend::init()` when built with `DORIAX_CLI_FRONTEND`; targets
live in `tools/*/*.cmake` and are `include()`d from the root `CMakeLists.txt`:
`doriax-reflector` (API metadata, replaces `generate_api_suggestions.py`),
`doriax-resource-compiler` (`pak` / `shaders` / `inspect`),
`doriax-build-generator` (generate/configure/build/status), `doriax-engine`
(standalone game runtime). Editor code that a CLI needs must exist as a free
function in `editor/cli/` taking plain structs + `std::function` callbacks - not as
an `App`/`Project` member.

## Conventions

License header (`// (c) Eduardo Doria` + SPDX MIT) on every file; `#pragma once`;
4-space indent matching the surrounding file; English "why" comments; no new
third-party deps without a strong reason. Verify with `doriax-build-verify`.

## Resources

- `references/window-pattern.md` - window lifecycle details, thread-safe notifications, redraws, docking pitfalls, and the App.cpp wiring checklist.
