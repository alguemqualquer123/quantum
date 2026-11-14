---
name: quantum-ui
description: Quantum UI work on both sides - the in-game UI scene system (UILayout anchors, UIContainer layout, widgets, pointer/keyboard event routing) and the ImGui editor with its vendored UILab chrome (windows, dock layout, host panels, Theme tokens, AppSettings persistence). Use for the structure of either side: which component and subsystem function owns anchors, layout rebuild, event routing, or an editor panel. For the widget-level traps and recipes of an in-game HUD, modal, button, text field or font use quantum-game-ui.
---

# UI (game UI + editor UI)

Two separate systems; pick the right one before writing code.

## A. In-game UI (scene type `SceneType::SCENE_UI`)

This section is the *map* - which component and which `UISystem` function owns anchors, the layout
rebuild and the event routing. For the traps you hit while actually building a widget (placement is
the only sizing, array order is the z-order *and* the hit order, atlas cells are not displayable,
the `RED` grayscale format, `FunctionSubscribe` tags as slots, the modal recipe) read `quantum-game-ui`.

Components (`engine/core/component/`):
- `UILayoutComponent.h:35-66` - width/height, `anchorPoint` LTRB, `anchorOffset` LTRB,
  `positionOffset`, `AnchorPreset` (17 presets incl. `FULL_LAYOUT`, `:15-33`), panel
  link, scissor rect/`ignoreScissor`, `ignoreEvents`.
- `UIContainerComponent.h:27-47` - VERTICAL/HORIZONTAL and the WRAP variants (`:14-19`),
  30 `ContainerBox`es, wrap cell size, content min sizes.
- `UIComponent.h:16-70` - buffers/`ObjectRender`/shader/texture/colour plus the event
  surface (`:46-57`): `onGetFocus/onLostFocus/onPointerEnter/Leave/Move/Down/Up/
  onClick/onDoubleClick/onDrag*` and the `focused` flag.
- Widgets: Button, Image, Panel, Progressbar, Text, TextEdit, Polygon.
- Objects: `engine/core/object/ui/{UILayout,Container,Button,Panel,TextEdit,Image,Text,
  Progressbar,Scrollbar,Polygon,Bone...}.h`.

`engine/core/subsystem/UISystem.h:28-167` owns layout (`applyAnchorPreset` `:118`,
anchor reference size `:45-46,:132`), hit-testing/routing (`eventOnPointerDown/Up/Move`,
`eventOnKeyDown`, `eventOnCharInput` `:143-147`, drag/double-click state `:33-43`) and
TextEdit editing including visual-order caret (`:88-91`). Geometry is rebuilt in
`update()` ordered by Transform (`UISystem.cpp:1977-2035`); `UISystem::draw()` is empty
(`UISystem.cpp:1813`) - the GPU upload and draw live in
`RenderSystem::loadUI`/`drawUI` (`RenderSystem.cpp:4296`, `:4399`), and the shaders are
the `ui`/`points`/`lines` entries (`ShaderType` + `setupShaderArgs`).

Lua: `UILayout` `binding/ObjectClassesLua.cpp:728-750`, `Container` `:753-766`,
`Polygon` `:768`, `Button` `:851`, `Panel` `:883`, `TextEdit` `:962`; UIComponent event
properties `ECSClassesLua.cpp:369-376`. Wire a widget callback with the global
`RegisterEvent(self, object, "method")` (`LuaBinding.cpp:300-315`).

Text shaping uses the vendored `engine/libs/sheenbidi` + `harfbuzz` (RTL/bidi). There is
**no IME composition API** (input is raw `wchar_t` char events, `UISystem.h:143`), **no
localisation/i18n framework** (strings are hardcoded), and **no accessibility layer** -
flag these as gaps rather than working around them silently.

## B. The editor (Dear ImGui 1.93-docking, UILab chrome)

`editor/` is `namespace quantum::editor`; `App.cpp` owns every window: it constructs
them, calls `show()` in the per-frame draw list (`App.cpp:1944-1976`), and places them
in the dock. New feature = new window class + a few wiring lines; do not grow `App.cpp`
with feature logic. The window contract (WINDOW_NAME constant, ctor with explicit deps,
`setOpen/isOpen`, `AppMenuCommand` enum value, `menuToggle` registration, dock builder
`DockBuilderDockWindow`, `PanelVisibilitySettings` field, and registration in the
hand-maintained `QUANTUM_EDITOR_SOURCES` list) is in `quantum-editor-dev`.

The **chrome comes from the vendored `libs/uilab` library** (namespace `uilab`, the
UniversalUIComponentsLab component/docking library) through the adapter
`editor/UILabHost.{h,cpp}`: the dockspace (`dockSpaceId()` `App.cpp:1914`, which replaced
`DockSpaceOverViewport`), the main menu bar (`drawMenuBar` `App.cpp:864`, drawing
Quantum's `PlatformMenuModel` through the library's `host.menuBar` hook), the console
panel (`drawConsole` `App.cpp:1960`, fed by the `Out` log mirror) and the three data
panel *windows* (`drawDataPanels` `App.cpp:1966`). A panel the library opens splits its
`show()` (per-frame bookkeeping, still called from `App.cpp`) from `drawContent()`
(drawn inside the library's window), plus `setHostedWindow(true)` and `openFlag()`; the
host registers Quantum's window name and open flag as a `uilab::HostPanel`, so the saved
layout, the View toggles and the X button keep working unchanged. `Structure`,
`Properties` and `ResourcesWindow` are the models. Quantum still applies its own ImGui
style (`Theme::apply`/`applyDpiScale`; the library's `ThemeManager::Apply()` is never
called) and keeps its own bottom strip (`settings.showStatusBar = false`). Anything
under `libs/uilab` is pristine upstream + a patch series: read
`libs/uilab/QUANTUM-PATCHES.md`, then run `scripts/sync-uilab.sh regen` after editing.

ImGui 1.92+ rules that bit this repo: `ImGui::End()` asserts when
`SetCursorPos`/`SetCursorScreenPos` was used to stretch the parent bounds (submit an
item or a `Dummy()` after positioning), `AddLine` no longer adds the 0.5px offset (use
`AddLineH`/`AddLineV`), and `ImGuiTextFilter::Filters`/`ImGuiTextRange` are internal now.

Key sites: `Theme::Colors` static `ImVec4` tokens (`editor/Theme.h:17-43`), Dark/Light
presets (`:12-15`), `apply()/applyDpiScale()/dpi()/dpiScale()` (`:45-59`) - prefer
`ImLerp`/`ImGuiCol_*` over hardcoded RGBA so both presets stay coherent. Fonts:
`App.cpp:1579-1630` merges FontAwesome solid (`fa_solid_900_ttf`), so use `ICON_FA_*`
macros; when sizing a button, **measure the label with the icon included**
(`ImGui::CalcTextSize(ICON_FA_X " Label")`) or the text clips - that was a real bug in
this repo. Docking: rebuild `App.cpp:1929`, default layout builder `:1261`; layout
persists to `editor_layout.ini` in the config dir (`:1570`); settings to `settings.yaml`
(`editor/AppSettings.cpp:109-127`). Shared footer helper lambda `footerActionButton`
(`App.cpp:944`).

## Rules that keep UI work reviewable

- Never mutate scene state from a window without an undo `Command`
  (`editor/command/Command.h`, `editor/command/type/XCmd.{h,cpp}`, pushed on the
  per-scene `CommandHistory`).
- Overlay/fullscreen windows must always be dismissible (Escape, an X, and a click on
  blank space) - a modal that traps the user with no panels behind it has happened here.
- Any change to a `.scene`/`project.yaml` from UI code invalidates generated C++:
  regenerate via `quantum-build-generator generate --project <dir>`.
- Verification is visual, and agents must not launch `quantum-editor.exe`; state
  explicitly when a change was not visually confirmed. When the user does authorize a
  run, `QUANTUM_EDITOR_CAPTURE=<prefix>` makes the frame loop write `<prefix>-f30.png`
  and `<prefix>-f120.png` from the composed backbuffer
  (`GraphicUtils::captureEditorFrame`, `editor/EditorFrame.cpp`) - never capture the
  desktop instead.
