---
name: doriax-slate-ui
description: Build the retained-mode UI stack inspired by Unreal Slate/UMG and Unity UI Toolkit (engine/slate, namespace doriax::slate) - widgets with visual states, StyleSet/brushes, USS + UXML markup, capture/target/bubble event routing, focus, CommonUI-style layer stacks and gamepad navigation, virtualized lists and ViewModel binding. Use when working in engine/slate or engine/slate_demo, choosing among doriax's three UI stacks, or porting an ImGui editor panel to retained mode.
---

# Slate/UMG-style retained UI (`doriax::slate`)

Retained, data-driven UI for **game** HUDs/menus/inventory and future editor panels.
Dear ImGui stays exactly as it is for editor tooling; the legacy scene UI stays for
existing projects. Nothing here replaces them.

## 1. Pick the stack before writing code

| stack | lives in | use it for | status |
|---|---|---|---|
| Legacy scene UI | `engine/core/object/ui/*`, `engine/core/subsystem/UISystem.*`, `ShaderType::UI` | existing `SceneType::SCENE_UI` projects, uGUI-style anchors in YAML | shipped, do not extend |
| `doriax::ui` | `engine/core/ui/*` (UIRuntime, VisualElement, YogaLayout, PaintList, VertexBuilder, FontAtlas, Label, Panels) | element tree + flexbox + paint list + sokol submit | in flight, **owned by another agent** - read-only for you |
| `doriax::slate` | `engine/slate/*`, demo `engine/slate_demo` | widgets, states, style sets, USS/UXML, events, focus, layers, binding | this skill |

Why two retained stacks exist: `engine/core/ui` was being written when this work was
scoped, and the user chose an isolated directory over stopping it. Consequence:
**never edit** `engine/core/ui`, `engine/renders/sokol/SokolUIRenderer.*`,
`engine/ui_demo`, `tests/ui` or the `core/ui` entries in `engine/CMakeLists.txt`.
Reusing the *vendored* `engine/libs/yoga` and `libs/doctest` is fine and expected.
The merge decision (which stack ships) is scheduled for the editor-integration phase.

## 2. Concept map (Unreal/Unity -> doriax)

| source | concept | doriax::slate counterpart |
|---|---|---|
| Slate | declarative widget build | `Slate::New<Button>().Text("Fire").StyleName("Hud.Button")` fluent builders |
| Slate | `SBox/SOverlay/SGridPanel/SHorizontalBox` | `Box`, `Overlay`, `GridPanel`, `HBox`, `VBox`, `WrapBox`, `ScrollBox`, `SizeBox`, `ScaleBox`, `Border` |
| Slate | `Slot` with `HAlign/VAlign/Padding/SizeBox/Fill` | `FSlateSlot` returned by `addSlot()`, container lays children out from slots |
| Slate | `FSlateStyleSet` + named styles + `FSlateBrush` | `StyleSet` + `NamedStyle<WidgetStyle>` + `SlateBrush` (texture + nine-slice margins + draw scaling) |
| Slate | visual states (Normal/Hovered/Pressed/Disabled/Focused) | `WidgetVisualState` enum resolved through the state machine on each `paint` |
| UMG | `CanvasPanel` anchors/pivot/offsets | `AnchorRect`: min/max anchors (0..1), pivot, offsets, resolved against the canvas - mirrors `UILayout.h:10-50` |
| UMG | `HorizontalBox/VerticalBox/GridPanel/WrapBox/ScrollBox/SizeBox/ScaleBox/Border` | same names, backed by Yoga flexbox except `AnchorRect`/`ScaleBox` |
| UMG | `CanvasScaler` / doriax DPI | reuse `Engine::setCanvasSize` + `Engine::setScalingMode` (`Scaling::FITWIDTH/FITHEIGHT/NATIVE`) and `Engine::getViewRect()` |
| UMG | Widget Blueprint | UXML document + `<Template>` + slots |
| CommonUI | `UCommonUIExtensions::PushInputModeToStack` | `LayerStack::push/pop` (Game, Menu, Modal, Tooltip, Notification) |
| CommonUI | activatable widgets | `IActivatable::onActivated/onDeactivated` (bound to button/pad keys) |
| CommonUI | `FCommonInputActionData` | `InputAction`: abstract named action + platform bindings, so widgets ask for "Confirm" not "Enter" |
| CommonUI | per-device icons | `GamepadIconMapper`: action -> icon name (`SlateBrush` picked from a theme atlas) |
| UI Toolkit | capture/target/bubble | `EventRouter` with the same three phases and `stopPropagation()` |
| UI Toolkit | USS | `USSParser` (type/`#id`/`.class`/descendant/child, `:hover :active :focus :disabled`, `--vars`, cascade by specificity) |
| UI Toolkit | `Change<T>` binding | `ViewModel` + `Binding<T>` one-way/two-way + `IValueConverter` |

## 3. Frame pipeline (fixed order, one owner each)

1. `Slate::tick(dt)` - animations, transitions, drag state.
2. `StyleResolver::resolve(root)` - only subtrees with `DirtyFlags::Style`.
3. `LayoutEngine::layout(root, canvasSize)` - Yoga for flow, `AnchorRect` for canvas slots; writes absolute `layoutRect`s.
4. `PaintList::clear()` + paint walk - accumulates clip (intersection) and opacity (multiply); emits `Rect/Border/Image/Text/Gradient/Shadow` commands.
5. `ISlateRenderer::render(mesh, canvasSize, viewRect, target)` - batches by texture+clip, tessellates rounded corners, `sg_apply_scissor_rect` per range.

Layout and paint are the only per-frame allocations allowed to grow, and they grow once
and are reused: no `new` inside the paint walk, no per-frame `std::string`, no container
that is destroyed and rebuilt each frame.

## 4. Decisions already taken (do not relitigate without the user)

- **Yoga, not a hand-written flexbox.** It is vendored at `engine/libs/yoga`, MIT, and
  flexbox matches UI Toolkit. Own code only for `AnchorRect`/`ScaleBox`, which Yoga
  cannot express.
- **No FreeType.** Text reuses `engine/core/util/STBText` (HarfBuzz shaping, kerning,
  fallback chain, SheenBidi bidi, `shapeLines` wrapping). SDF is a later, optional
  upgrade behind `IFontBackend`; adding FreeType needs the user's explicit OK.
- **No new shader in phase 1.** Geometry is tessellated and submitted through the three
  existing `ShaderType::UI` variants (solid `Vc4`, textured `Tex+Vc4`, font `Ftx`) the way
  `engine/renders/sokol/SokolUIRenderer.cpp:22-26,108` does. A dedicated `slate.vert/frag`
  is deliberately deferred because touching it means `generate_shaders`, `.sdat` cache,
  `ShaderPool` bits and `ShaderBuilder` - the hand-maintained list that breaks silently.
- **Binding without reflection.** There is no reflection registry; engine, editor, YAML
  and the code generator each keep parallel hand-written lists, so bindings are lambdas
  over named fields, not discovered properties.
- **`_HAS_EXCEPTIONS=0` propagates into this module** (Jolt's CMakeLists exports it
  PUBLIC): nothing may throw across a task/future boundary. USS/UXML parsers report
  errors through a diagnostics list, not exceptions.

## 5. Input ownership (the part that bites)

Order of claims per event: ImGui (editor only) -> legacy `UISystem` -> `doriax::ui` ->
`doriax::slate` -> game scripts. Consumption is announced with
`Engine::isUIEventReceived()` / `Engine::setIgnoreEventsHandledByUI()` (`Engine.h:324-327`),
which is how scripts already get blocked. Routing enters through
`Engine::systemMouseDown/Move/Up/Touch*/KeyDown/CharInput` (`Engine.cpp:1214-1440`), after
`Engine::transformCoordPos` maps screen -> canvas (`Engine.cpp:1200-1212`).
Input is polling (`Input::isMousePressed`, `getMousePosition`, `gamepad*`) *plus* the
`Engine::on*` `FunctionSubscribe` events (`Engine.h:421-437`); a retained system needs the
event form, so hook the `on*` subscriptions rather than sampling in `update`.
Cursor: `Engine::setMouseCursor(CursorType)` (`Engine.cpp:491`).
In the editor, `SceneWindow.cpp:958-1019` only forwards input to the engine while
`playState == PLAYING`; a Slate editor panel must not fight ImGui for focus.

## 6. Widget catalog

Each widget's minimal example, states and style keys: `references/widget-catalog.md`.
Rules for every widget: derive from `Widget`, override `paint`, expose its look through
`StyleSet` names (never hard-coded colors), respond to pointer *and* keyboard *and*
`InputAction`, and mark only the flags it actually needs dirty.

## 7. Build and verify (headless only)

```bash
cmake -S . -B "$DORIAX_BUILD_DIR"
cmake --build "$DORIAX_BUILD_DIR" --target doriax-slate-demo --config Debug
cmake --build "$DORIAX_BUILD_DIR" --target doriax-slate-tests
"$DORIAX_BUILD_DIR/Debug/doriax-slate-demo.exe" --headless      # tree+layout+paint, no GPU
"$DORIAX_BUILD_DIR/Debug/doriax-slate-demo.exe" --frames 120    # auto-close gallery
"$DORIAX_BUILD_DIR/Debug/doriax-slate-tests.exe"                # doctest, GPU-free
```

`--headless` must succeed with no window and no GL context; it is the proof that layout
and paint are testable. Never launch `doriax-editor.exe` to check UI work. A GPU-verified
screenshot has to come from the user, so say "visually unverified" until then.
`ctest` is empty in this repo: passing tests means running the test exe, not ctest.

Two blank-window traps found the hard way, both in `render/sokol/SokolSlateRenderer.cpp`:

- **Never call `sg_*` from this module.** sokol is a *static* lib linked into both
  `doriax.dll` and the consumer, so outside the dll `sg_setup` ran on a different copy of
  `_sg` and any direct call asserts `_sg.valid`. All GPU calls go through the exported
  `CameraRender`/`ObjectRender`/`TextureRender` types. That also means the scissor must be
  flipped to sokol's bottom-origin convention against the height of the pass being drawn
  (`startRenderPass` is exclusive, so the window size is the right source).
- **A text range with no texture draws nothing, silently.** `PaintList::addText` carries the
  font atlas `Texture` and `Geometry::build` copies it into the range; `drawRange` returns
  early on an empty/uncreated atlas. A gallery that shows panels but no glyphs is that bug.

## 8. Phase status (update this table when you land or remove code)

| phase | deliverable | state |
|---|---|---|
| F0 | module skeleton + cmake targets + this skill | done |
| F1 | core tree, style keys, paint list, sokol submit, text | done - gallery renders in a window |
| F2 | Yoga flexbox + `AnchorRect` + DPI/reference-res scaling | done - reflow verified on resize |
| F3 | events, focus, tab order, gamepad navigation | not started |
| F4 | USS, UXML, templates, hot reload | not started |
| F5 | advanced widgets (virtualized `ListView`, `TextBox`, `Dropdown`, `TabView`, `ToolTip`, `Modal`) | not started |
| F6 | transitions/animations + `ViewModel` binding | not started |
| F7 | `LayerStack`/activatable/`InputAction`/device icons + world-space render target | not started |
| F8 | editor integration + performance + docs | not started |

Acceptance targets: window resize reflows the layout; a menu defined 100% in UXML/USS;
10.000 `ListView` items scroll smoothly; a HUD bound to a ViewModel with no manual update
code; < 1 ms UI CPU with 5.000 visible elements.
