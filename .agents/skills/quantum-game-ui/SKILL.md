---
name: quantum-game-ui
description: Build in-game UI in Quantum with the runtime UI system - modals, buttons, text fields, labels, nine-slice panels and sprite atlases. Covers anchor placement (the only thing that sizes a widget), the missing z-order and what replaces it, the pointer hit test, ButtonComponent's four state textures, FunctionSubscribe tags as handler slots, Text/TextEdit fonts, and the two loader traps that silently paint a grayscale sprite red or stretch an atlas cell across the whole texture. Use when adding or debugging a HUD, menu, inventory, modal or any in-game widget in C++ or Lua. For the editor's ImGui panels use quantum-ui or quantum-editor-dev; for the retained slate system use quantum-slate-ui.
---

# In-game UI

Everything here is `engine/core/object/ui/*` over `UIComponent` / `UILayoutComponent` /
`ButtonComponent`, drawn and hit-tested by `engine/core/subsystem/UISystem.cpp`. It is **not** ImGui
(`quantum-editor-dev`) and **not** `engine/slate` (`quantum-slate-ui`).

## A UI lives in its own scene layer

A `Text`/`Image` in the world scene is a quad in the perspective camera, positioned in **world units**.
For a HUD, make a scene, add it with `Engine::addSceneLayer`, and set its event gate explicitly:
`Scene::setEnableUIEvents(UIEventState::ENABLED|DISABLED)` (`engine/core/Scene.h:367`, enum `:24-28`).
`DISABLED` is what lets the world controller keep mouse look while the HUD still draws; a modal
flips it to `ENABLED` for as long as it is open and restores what it found.

There is also `Scene::setEnableUIEvents(bool)` (`:371`) - the same name, different meaning. Pass the
enum on purpose.

## Anchors are the only thing that sizes a widget

`UILayout::setAnchorPoints(left, top, right, bottom)` (`engine/core/object/ui/UILayout.h:24`) and
`setAnchorOffsets(left, top, right, bottom)` (`:35`) - four normalized parent edges plus four pixel
offsets. The layout pass then **derives** the box from them and writes the result back into
`layout.width/height` (`UISystem.cpp:2316-2331`: `posX = abAnchorLeft + anchorOffsetLeft`,
`width = ceil(anchorRight - posX + anchorOffsetRight)`, floor 1 px). So `setSize()` is overwritten on
the next pass. Place a box by its four edges:

```cpp
placeBox(image, 0.0f, 1.0f, 0.0f, 1.0f, 8, -58, 26, -40);   // bottom-left, 18x18
```

Anchor **presets and manual offsets do not mix**: `applyAnchorPreset` (`UISystem.cpp:1663`, called
from the layout pass at `:2283` whenever `usingAnchors`) rewrites all four anchor points *and* all
four offsets - it rebuilds the offsets from the current `layout.width` - so a preset plus an offset
edit is a fight the preset wins. Use `AnchorPreset::NONE` and place by hand.

`Container` (`engine/core/object/ui/Container.h`) does real placement for children -
`VERTICAL`, `HORIZONTAL`, `VERTICAL_WRAP`, `HORIZONTAL_WRAP` (`UIContainerComponent.h:14-19`), laid
out in `UISystem.cpp:2067+`. It positions children; each child still needs its own size.

## There is no z-order - draw order *is* hit order, and both are array order

The draw walks the `Transform` component array (`RenderSystem.cpp`, the `UIComponent` branch of the
same walk) and the hit test walks `getComponentArray<UILayoutComponent>()` **overwriting**
`lastUIFromPointer` for every rect that contains the pointer (`UISystem.cpp:2892-2925`). So:

- the **last** widget in array order wins the click and paints on top;
- a plate must be created **before** the text that sits on it;
- `Object::moveToTop()` / `moveUp()` / `moveDown()` / `moveToBottom()` (`engine/core/object/Object.h:32`)
  are the whole layering API. They reach `EntityRegistry::moveChildToTop` (`engine/core/registry/EntityRegistry.cpp:492`)
  → `moveChildAux` (`:145`) / `moveChildToIndex` (`:455`), which move the entity's branch in the
  Transform array and then call `sortComponentsByTransform` (`:487`) to reorder the sibling component
  arrays - which is why one call moves the draw **and** the click together, and why a modal is
  `moveToTop()` + `setEnableUIEvents(ENABLED)` and nothing else.
- a decorative image with no `ignoreEvents` **eats the click** of whatever is under it
  (`UILayoutComponent::ignoreEvents` `:64`; the test is `uirect.contains(...) && !layout->ignoreEvents`).
  Set it on every plate, socket and overlay that is not a control.

## Images: nine-slice, and the two traps

`Image::setPatchMargin(margin)` (`:19`) or the per-edge overload (`:20`)
(`engine/core/object/ui/Image.h`) produces the 9-patch in `createImagePatches`. Read the border from
the art, do not guess it.

- **UVs span the whole texture.** `createImagePatches` computes `texCutRatioW = 1.0 / texWidth`
  (`UISystem.cpp:112-120`) - there is no sub-rect, no `textureRect` on an `Image`. **An atlas cell is
  not displayable by an `Image`.** One file per sprite. (The 3D mesh path is the opposite:
  `Mesh`/`Points` do take a rect - do not carry that expectation here.)
- **`setColor` multiplies the texture.** A widget authored as a translucent black plate
  (`0,0,0,0.42`) stays black over any sprite you bind to it. Re-binding a sprite means repainting
  white and expressing selection as sprite brightness instead of tint.

### Loading pixels from memory (a pack, a generated atlas, a download)

`TextureData(int w, int h, unsigned int size, ColorFormat, int channels, void* data)`
(`engine/core/render/Render.h`) takes a **non-const** `void*` - a `const unsigned char*` fails with
"conversion loses qualifiers". Its copy is a **shallow pointer copy** and the destructor frees only
when `dataOwned`, so an owning copy handed to a pool that also copies is a double free. The safe
shape is a **non-owning view** of a buffer whose owner lives as long as the process (a static array
indexed by sprite id).

- **A 1-channel PNG arrives as `ColorFormat::RED`** and an R8 sample reads `(v, 0, 0, 1)`: every
  grayscale sprite paints **red**. Expand to RGBA yourself (fourth byte opaque - the file has no
  alpha) and keep the owner. `ColorFormat` is `{RED, RGBA, RED16, RGBA16F, R16F}`
  (`engine/core/render/Render.h:284`); refuse anything else with a log line, not a crash.

## Buttons

`Button : public Image` (`engine/core/object/ui/Button.h`, Lua `ObjectClassesLua.cpp:1053`), backed by
`ButtonComponent` (`engine/core/component/ButtonComponent.h`): four state textures
`textureNormal` / `textureHovered` / `texturePressed` / `textureDisabled` (`:15-18`), `hovered`
(`:34`), `disabled` (`:35`) and `needUpdateButton` (`:37`).

The setters take a **path**. To skin a button from in-memory pixels, write the four `Texture` fields
on the component directly and set `needUpdateButton = true`; the image pass then calls
`updateButton` and clears the flag (`UISystem.cpp:1925-1928`). The state swap itself is
`applyButtonVisual` (`:615-634`): it picks normal/disabled/pressed/hovered into `ui.texture` and sets
`needUpdateTexture`, and runs on every hover/press (`:2664`). **One trap**: `updateButton` calls
`load()` on all four textures only inside its `if (!ui.loaded)` branch (`:673-695`), i.e. on the
first pass of the widget - skin a button *after* it has already loaded and the new state textures are
swapped in but never loaded, so they render as the old one. Label: `setLabel` / `setLabelFontSize` /
`setLabelColor` / `setLabelFont`.

The press is `ButtonComponent::onPress` - `FunctionSubscribe<void()>` (`:30`), called at
`UISystem.cpp:2736`; `onRelease` is `:31`. At the rect level, `UIComponent::onPointerDown` and
`onClick` are `FunctionSubscribe<void(float,float)>` with the position **local to the widget**
(`engine/core/component/UIComponent.h:57,59`, called `UISystem.cpp:3054`).

## `FunctionSubscribe`: the tag is a slot

`add(tag, fn)` calls `addImpl`, and `addImpl` **`remove(tag)` first** (`engine/core/util/
FunctionSubscribe.h:181-187`). Consequences:

- re-arming a pooled widget with the same tag cannot stack handlers - it replaces;
- two **different** tags on one event both fire, in subscription order, so a leak is a tag you never
  reuse, not a duplicate;
- `remove(tag)` removes the **first** match only (`:294-300`).

This is what makes a UI testable headless: store the action in a named slot and expose
`click(name)` that calls it directly. A modal then needs no pointer, no window and no finger.

## Text and fonts

`Text` (`engine/core/object/ui/Text.h`): `createText()` (`:19`) must run before measuring,
`setFontSize` (`:42`), `setFont(path)` / `setFont(index, path)` (`:37-38`), `setMultiline` (`:45`),
`setMaxTextSize` (`:30` - **default 100**; a debug block passes it in the second frame and every
growth logs a warning and reallocs), `getCharWidth(codepoint)` (`:61`).

- **The engine never sizes a box to the run.** Centring means summing `getCharWidth` per character
  and re-anchoring around the centre. Sum in `float` and round **once** at the end: truncating per
  glyph loses ~1/3 px per character (23 px short on a 62-char line). `getAABB()` is not a measurement
  - it comes from the already-built vertex buffer.
- **Fonts are vector files.** `setFont` wants a TTF/OTF the engine's font loader can open. A bitmap
  font atlas (Minecraft's `font/ascii.png` plus a metrics `.json`) **cannot** be pointed at - there is
  no glyph-rect path in this system. Validate a font path against the disk before storing it: the
  loader only logs, once per frame, and a widget aimed at a missing file is a running game with no
  text and no error.
- A `Text` created with `new Text(scene)` where `scene` is the inherited `ScriptBase` member lands in
  the **world** scene, not the HUD. Name the HUD scene's pointer differently; the C4458 shadowing
  warning is the symptom of exactly this bug.

## Text fields (the "ask for a value" widget)

`TextEdit : public Image` (`engine/core/object/ui/TextEdit.h:12`, Lua `:1167`) - it *is* a nine-slice
image with a `Text`, a cursor `Polygon` and a selection `Polygon` children
(`getText` / `getCursorObject` / `getSelectionObject`). `setText` / `getText`, `setPlaceholder` +
`setPlaceholderColor`, `setPassword` + `setPasswordChar`, `setCursorIndex`, `setSelection(anchor,
focus)`, `setCursorBlink` / `setCursorWidth` / `setCursorColor`, `setTextFont`, `setFontSize`,
`setDisabled`. Bind the frame sprite to the `TextEdit` itself - that is what `setTexture` on an Image
means, and the text children inherit the box.

Typing requires the scene to receive the keyboard: `setEnableUIEvents(ENABLED)` and the widget
focused (`UIComponent::focused`, `onGetFocus` / `onLostFocus`). If you build your own field, the raw
character stream is `Engine::onCharInput` (`engine/core/Engine.h:538`, `void(wchar_t)`).

## The modal recipe that survives a real frame

1. Build the skeleton **once** at HUD creation: panel + N label slots + M button slots + one field,
   all hidden. Pooling avoids the one thing that bites - each `new Widget(scene)` appends to the
   component arrays, so rebuilding a menu per open grows the arrays and reorders the hit test.
2. Open = fill labels, `setVisible(true)`, `moveToTop()` on the container (or on each widget, in the
   order you want them layered), save the scene's event state and set `ENABLED`.
3. Every action is a `std::function` in a named slot; `onPress.add(tag, ...)` fires it. The tag is a
   slot, so re-claiming a button is safe.
4. Close = hide, restore the saved event state. Never destroy the widgets.

## What this system does not have (say so before promising)

No z-order property, no atlas-cell `Image`, no box auto-sized to its text, no rich/inline markup, no
layout that sizes children (only places them), no scroll view without a `Scrollbar` + `scissor`
(`UILayoutComponent::scissor` / `scissorActive` / `ignoreScissor` `:61-63`), no theme/stylesheet and no
USS/UXML - those are
`engine/slate` (`quantum-slate-ui`). Widget creation is per-entity: a 10-icon HUD row is 20 entities
(socket + overlay), which is fine at HUD scale and not fine at inventory-grid scale - pool it.

## Verify

There is no render test harness and launching `quantum-editor.exe` is forbidden
(`quantum-safety-rules`). Prove a UI slice two ways:

- **headless probe**: assert the numbers a widget must answer (how many icons lit, how many bound,
  the anchor read **back off the component** - `scene->findComponent<UILayoutComponent>(entity)` -
  rather than trusting the call that placed it) and fire the button callbacks directly;
- **a captured frame**: `VOXEL_SHOT=... VOXEL_FRAMES=...` in a template project, or
  `quantum-engine.exe --project <dir> --screenshot out.png --capture-frame N`. Then crop and upscale
  the HUD region before looking at it - a 9 px icon in a 1280x720 PNG is not legible at 1:1.

The probe cannot see placement, and the frame cannot see a callback. A UI slice that ships only one of
the two has not proven the other half. In practice the overlap class of bug - two rows built on the
same line, one drawn on top of the other, every count still correct - is **only** visible in the PNG.
