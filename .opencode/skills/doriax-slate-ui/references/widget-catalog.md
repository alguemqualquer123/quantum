# Widget catalog (`doriax::slate`)

The API contract for the Slate-style widgets. Each entry: what it is for, the minimal
example, the visual states it resolves through `StyleSet`, and the USS keys a theme must
provide. Everything here is reachable by mouse, keyboard and `InputAction`.

Conventions used in the examples:

```cpp
using namespace doriax::slate;

// containers add children and return the child, so calls chain left-to-right
Box* menu = Box::vertical(root, "menu").setGap(8.0f).setPadding(16.0f);
Button* play = menu->add<Button>("play")->setText("Play").setStyleName("Menu.Button");

// detached construction (Slate's SNew equivalent), attach later
std::unique_ptr<Label> title = Slate::newWidget<Label>("title")->setText("Doriax");
root->addChild(std::move(title));
```

`Widget` itself provides: `setId`, `addStyleClass`, `setStyleName`, `setVisible`,
`setEnabled`, `setTooltip`, `setZOrder`, `markDirty(DirtyFlags::...)`. Setters return the
concrete type so chains keep the derived type.

---

## Text and pictures

### Label
Non-interactive text. Wrapping and alignment come from the style, not the call site.
```cpp
page->add<Label>("score")->setText("Score: {0}").setWrap(true).setAlign(TextAlign::Center);
```
States: Normal, Disabled. USS: `color`, `font-size`, `font-family`, `letter-spacing`,
`line-height`, `-slate-text-align`, `-slate-text-overflow: ellipsis`.

### Image
A `SlateBrush` on screen: texture, nine-slice margins, draw scaling, tint.
```cpp
page->add<Image>("icon")->setBrush(Theme::brush("Icons.Save")).setTint(Color::white());
```
States: Normal, Hovered. USS: `image`, `-slate-image-size`, `-slate-draw-mode`
(`simple|tile|slice|box`), `opacity`.

### RichText
`Label` with inline markup (`<b> <i> <color=#RRGGBB> <outline>`), UTF-8 through the same
`STBText` shaping path. No nested widgets.
```cpp
page->add<RichText>("log")->setText("<b>Player</b> picked up <color=66ccff>3</color> coins");
```

## Buttons and choices

### Button
Click/press with content that may be text, an icon brush, or both.
```cpp
menu->add<Button>("resume")->setText("Resume").setIcon(Theme::brush("Icons.Play"))
     .onClicked([](const ClickEvent& e) { Slate::closeTopModal(); });
```
States: Normal, Hovered, Pressed, Focused, Disabled, and the style is per-state.
USS: `background`, `border`, `border-radius`, `padding`, `color`, plus the
`:hover :active :focus :disabled` blocks.

### CheckBox (Toggle)
Two-state; tri-state is the `ToggleMode::Indeterminate` variant used by tree headers.
```cpp
opts->add<CheckBox>("vsync")->setLabel("VSync").setChecked(true)
     .onChanged([](bool on) { AppSettings::setVSync(on); });
```
States: unchecked/checked x normal/hover/pressed/focus/disabled.

### RadioButton + RadioGroup
Mutually exclusive group with one focus stop, arrows navigate inside it.
```cpp
RadioGroup* res = opts->add<RadioGroup>("resolution").setValue(Resolution::HD);
res->addRadioButton("hd", "1280x720");
res->addRadioButton("fhd", "1920x1080").onSelectedChanged(...);
```

### Slider
Continuous value with optional step and grabbed-thickness growth.
```cpp
mixer->add<Slider>("volume").setRange(0.0f, 1.0f).setStep(0.05f).setValue(0.7f)
      .onValueChanged([](float v) { AudioSystem::setMasterVolume(v); });
```
USS: `slider-track`, `slider-grabber`, `-slate-slider-tick-interval`.

### ProgressBar
Read-only fill; animate the value, never the widget.
```cpp
hud->add<ProgressBar>("hp").setRange(0.0f, 100.0f).setValue(72.0f).setTextFormat("{0:0}%");
```

### Dropdown (ComboBox)
Closed selector + popup list; the popup is a `Menu` layer so it can exceed the parent clip.
```cpp
opts->add<Dropdown>("language").setOptions({"Portugues", "English", "Espanol"})
     .setCurrentIndex(0).onSelectionChanged([](int i) { Locale::set(i); });
```

## Containers

### Box (vertical/horizontal), Overlay, GridPanel, WrapBox
Flex containers; `Overlay` stacks children in the same rect (HUD anchors), `GridPanel`
takes explicit row/column spans like Slate's `SGridPanel`.
```cpp
Box* row = page->add<Box>("actions", Orientation::Horizontal).setGap(8.0f).setAlignItems(AlignItems::Center);
Grid* kv = page->add<Grid>("keys"); kv->setColumns(2);
kv->addAt<Label>(0, 0, "fps")->setText("FPS"); kv->addAt<Label>(1, 0, "ms")->setText("ms");
```

### CanvasPanel
Absolute placement with UMG anchors: `anchorMin/anchorMax` in 0..1, pivot, offsets in px.
```cpp
canvas->add<Image>("crosshair")->setAnchors(0.5f, 0.5f, 0.5f, 0.5f).setPivot(0.5f, 0.5f)
      .setOffsets(-8.0f, -8.0f, -8.0f, -8.0f);   //left, top, right, bottom
canvas->add<Box>("hudBottomLeft")->setAnchors(0.0f, 1.0f, 0.0f, 1.0f).setOffsets(16.0f, -64.0f, 240.0f, -16.0f);
```

### SizeBox / ScaleBox
`SizeBox` pins width/height/min-max for a child that sizes to content; `ScaleBox` renders
a child at a design size then scales it (UMG's DPI trick) - it must not blur text.

### ScrollBox
Clips and scrolls; owns scrollbars, wheel, drag, kinetic page scroll and caret follow.
```cpp
ScrollBox* log = page->add<ScrollBox>("log").setOrientation(Orientation::Vertical).setGap(2.0f);
log->setOnWheelEnabled(true); log->scrollToEnd();
```

### Foldout (ExpandCollapse)
Disclosure triangle with animated height; content is lazily built on first open.
```cpp
settings->add<Foldout>("audio").setHeader("Audio").setExpanded(false).add<Slider>("master");
```

### TabView
Tab strip + one page per tab; pages are built lazily and kept alive (`retainPages`).
```cpp
TabView* tabs = window->add<TabView>("tabs");
tabs->addTab("general", "General", [] (Widget* page) { page->add<CheckBox>("vsync"); });
tabs->addTab("input", "Input", buildInputPage);
```

## Lists and trees

### ListView (virtualized)
Only visible rows exist. `ListView` asks the source for count, recycles row widgets, and
keeps selection/scroll position stable when the source mutates. This is the widget the
10.000-item acceptance test is about.
```cpp
auto items = std::make_shared<InventorySource>(10000);
ListView* list = page->add<ListView>("items")
    .setSource(items)
    .setRowHeight(48.0f)
    .setFactory([](Widget* row, const ModelIndex& index) {
        InventoryEntry* e = items->at(index);
        row->add<Image>("icon")->setBrush(e->icon);
        row->add<Label>("name")->setText(e->name);
        row->add<Label>("count")->setText(std::to_string(e->count));
    })
    .setSelectionMode(SelectionMode::Multi)
    .onSelectionChanged([](const ModelSelection& s) { details->setEntries(s); });
```
Rules: the factory may only reuse the passed row (never allocate), row height must be
uniform unless `setUniformRowHeight(false)` + measured cache, and `setSource` invalidates
cache but not selection.

### TreeView
`ListView` over a hierarchical source with expand state per node.
```cpp
tree->add<TreeView>("outliner").setRoot(outlinerRoot).setIndent(16.0f)
    .bindLabel([](const TreeNode& n) { return n.name; });
```

## Text input

### TextBox
Single-line editable text: caret, selection, clipboard, IME/`onCharInput`, validation.
```cpp
page->add<TextBox>("playerName").setPlaceholder("Your name").setMaxLength(16)
     .setValidator(Validator::alnum()).onTextChanged([](const std::string& s) { profile = s; });
```
States: Normal, Focused, Disabled. USS: `background`, `caret`, `selection-color`, `padding`.

### TextEdit
Multi-line variant (wrapping, scroll, read-only mode) used by the debug console.

## Floating and chrome

### ToolTip
Hover/hold-with-dwell popup on the `Tooltip` layer; never takes focus.
```cpp
button->setTooltip("Save the scene (Ctrl+S)");
```

### Menu / ContextMenu
Keyboard-navigable popup with items, separators and sub-menus; `InputAction` "Cancel"
closes it.
```cpp
ContextMenu::openAt(world, pointerCanvasPos, {{"Duplicate", duplicateFn}, {"Delete", removeFn}});
```

### Modal / Dialog
Pushes a `Layer` that captures all input; result via `onClosed(DialogResult)`.
```cpp
Modal::open(quitConfirm, [](DialogResult r) { if (r == DialogResult::Yes) Engine::quit(); });
```

### WidgetWindow (drag + focus)
Floats inside the canvas with title bar, drag, focus-on-click and layer promotion.
`LayerStack::promoteToTop(window)` on any pointer-down.

## CommonUI layer

### Activatable wiring
Every focusable widget implements `IActivatable`; `InputAction::Confirm/Cancel/Navigate*`
route through `LayerStack` to the topmost activatable widget that `canReceiveInput()`.

### Device icons
`GamepadIconMapper::brushFor(action)` returns the `SlateBrush` for the current device
(Xbox / PlayStation / Switch / keyboard), so `Button` and `TabView` tab labels swap icons
without any widget knowing the platform.
