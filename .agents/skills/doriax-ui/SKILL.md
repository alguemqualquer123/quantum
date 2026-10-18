---
name: doriax-ui
description: Doriax UI work on both sides - the in-game UI scene system (UILayout anchors, UIContainer layout, widgets, pointer/keyboard event routing) and the ImGui editor (windows, dock layout, Theme tokens, AppSettings persistence). Use when building a menu/HUD/in-game widget, changing anchor or container behaviour, adding an editor panel, or restyling with theme tokens.
---

# UI (game UI + editor UI)

Two separate systems; pick the right one before writing code.

## A. In-game UI (scene type `SceneType::SCENE_UI`)

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

## B. The editor (Dear ImGui, docking)

`editor/` is `namespace doriax::editor`; `App.cpp` owns every window: it constructs
them, calls `show()` in the per-frame draw list (`App.cpp:1934-1969`), and places them
in the dock. New feature = new window class + a few wiring lines; do not grow `App.cpp`
with feature logic. The window contract (WINDOW_NAME constant, ctor with explicit deps,
`setOpen/isOpen`, `AppMenuCommand` enum value, `menuToggle` registration, dock builder
`DockBuilderDockWindow`, `PanelVisibilitySettings` field, and registration in the
hand-maintained `DORIAX_EDITOR_SOURCES` list) is in `doriax-editor-dev`.

Key sites: `Theme::Colors` static `ImVec4` tokens (`editor/Theme.h:17-43`), Dark/Light
presets (`:12-15`), `apply()/applyDpiScale()/dpi()/dpiScale()` (`:45-59`) - prefer
`ImLerp`/`ImGuiCol_*` over hardcoded RGBA so both presets stay coherent. Fonts:
`App.cpp:1564-1589` merges FontAwesome solid (`fa_solid_900_ttf`), so use `ICON_FA_*`
macros (`App.cpp:392-394,1005,1030`); when sizing a button, **measure the label with the
icon included** (`ImGui::CalcTextSize(ICON_FA_X " Label")`) or the text clips - that was
a real bug in this repo. Docking: `DockSpaceOverViewport` `:1915`, rebuild `:1910`,
default layout builder `:1259-1307`; layout persists to `editor_layout.ini` in the
config dir (`:1557`); settings to `settings.yaml` (`editor/AppSettings.cpp:109-127`).
Shared footer helper lambda `footerActionButton` (`App.cpp:935`).

## Rules that keep UI work reviewable

- Never mutate scene state from a window without an undo `Command`
  (`editor/command/Command.h`, `editor/command/type/XCmd.{h,cpp}`, pushed on the
  per-scene `CommandHistory`).
- Overlay/fullscreen windows must always be dismissible (Escape, an X, and a click on
  blank space) - a modal that traps the user with no panels behind it has happened here.
- Any change to a `.scene`/`project.yaml` from UI code invalidates generated C++:
  regenerate via `doriax-build-generator generate --project <dir>`.
- Verification is visual, and agents must not launch `doriax-editor.exe`; state
  explicitly when a change was not visually confirmed.
