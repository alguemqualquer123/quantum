# Window pattern - finer points

## Skeleton

```cpp
// editor/window/X.h
#pragma once
#include <...>

namespace quantum::editor {
    class XWindow {
    private:
        bool windowOpen = true;
        Project* project;                    // deps by pointer, never owned
        void drawBody();
    public:
        static constexpr const char* WINDOW_NAME = "X";
        XWindow(Project* project);
        void show();                         // called every frame by App
        void setOpen(bool open);
        bool isOpen() const;
    };
}
```

`show()` opens the window with `ImGui::Begin(WINDOW_NAME, &windowOpen, flags)`,
draws, then `ImGui::End()`. Keep it frame-cheap: no disk I/O or process spawning
inside `show()` without a time gate.

## Data from other threads

The pattern used by `NotificationsWindow` (`editor/window/NotificationsWindow.h`):

- producer threads call a mutex-guarded `push()` that only appends to an `incoming`
  vector;
- `show()` (UI thread) calls `drainIncoming()` and moves entries into the history /
  toast buffers, which stay UI-thread-only;
- nothing is ever drawn from the producer thread, and the mutex is never held across
  ImGui calls.
- After pushing from a background thread, request a repaint the way
  `ExportWindow` does (`Backend::getApp().requestRedraw()`), otherwise the toast sits
  unseen until the next input event.

## Toast / overlay windows

For windows that must float over the dockspace (toasts, command palette):
`ImGuiWindowFlags_NoDocking | NoSavedSettings | NoFocusOnAppear`(palette除外, it
needs the keyboard) and position against
`ImGui::GetMainViewport()->WorkPos/WorkSize` rather than a window position, so
multi-viewport mode does not move them to the wrong monitor. Draw them **after** the
docked windows in `App`'s frame so they render on top.

## Docking pitfalls

- A window only docks if `WINDOW_NAME` is handled in the dock builder
  (`App.cpp:959-968`); a new window with no `DockBuilderDockWindow` call lands
  floating and looks like a regression.
- `editor_layout.ini` (repo root and every template) stores `Window/DockId` stanzas
  keyed by window title. Renaming a `WINDOW_NAME` orphans saved layouts - bump or
  clear the stale entry instead of silently ignoring it.
- Sizes are physical pixels and are recorded with the UI scale they were captured at
  (`AppSettings::layoutUiScale`); never scale them twice.

## Wiring checklist for a new window

1. `editor/window/X.{h,cpp}` created and compiles standalone.
2. Added to `QUANTUM_EDITOR_SOURCES` in root `CMakeLists.txt` (window block ~:551-563).
3. Member + construction in `App`, `show()` in the per-frame list (~:1547).
4. `AppMenuCommand` value + `menuToggle` entry in the View menu (~:341) + dispatch case.
5. `DockBuilderDockWindow` placement; `PanelVisibilitySettings` field for persistence.
6. `ResetLayout` still restores it (`setOpen(true)` where the other panels are).

## Related new windows (2026-10)

`NotificationsWindow` (toasts + history), `CommandPaletteWindow` (fuzzy action
launcher, needs `setExecuteCallback` + `setCustomCommands` from `App` because
`ai::EditorActionRegistry::tools()` exposes only AI tool names, no ids/shortcuts),
`WelcomeScreenWindow` (recents via `AppSettings::getRecentProjects()`, template cards
from `templates/`, all actions injected through `WelcomeCallbacks`),
`ResourceMonitorWindow` (system monitor: machine-wide CPU/RAM/GPU/VRAM/network from
the shared `util/SystemStats` sampler - also used by the footer HUD in `App::showFooter` -
plus editor/child process stats; children registered by pid with `addChildProcess`).
