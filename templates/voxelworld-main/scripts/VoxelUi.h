// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

// The game's UI layer: the primitives a menu is made of (modal, button, request, labelled text)
// drawn with the original sprite art from the local pack.
//
// Two measurements shaped this whole file, and both are engine facts rather than choices made here:
//
//   * `UISystem::createImagePatches` maps UVs across the **whole** texture - there is no sub-rect,
//     so an atlas of sprites is not something a widget can sample. Every sprite is its own PNG,
//     which is exactly how the pack ships them, so the pack is read as it is and nothing derived
//     from it is ever written to disk.
//   * `TextureData::loadTexture` keeps a one-channel PNG as `ColorFormat::RED` (sokol `R8`), and a
//     shader sampling R8 reads (v,0,0,1). Half the GUI sprites in the pack - every button, the slot
//     frame, the popup panel - are one-channel, so they would paint **red** instead of gray. The
//     loader below expands those to RGBA in memory and hands the widget a `TextureData`, never a
//     path.

#include "VoxelConfig.h"

#include <functional>
#include <string>
#include <vector>

namespace quantum {
class Scene;
class Image;
class Text;
class Button;
class TextEdit;
}

namespace voxel {

struct Probe;

// The sprites this layer knows, in the order of the table in the .cpp. Append-only: the numbers
// appear in the probe output, and a run's log has to stay readable against an older build's.
enum UiSprite {
    UI_SPRITE_HEART_CONTAINER = 0,
    UI_SPRITE_HEART_FULL,
    UI_SPRITE_HEART_HALF,
    UI_SPRITE_FOOD_FULL,
    UI_SPRITE_FOOD_HALF,
    UI_SPRITE_FOOD_EMPTY,
    UI_SPRITE_AIR_FULL,
    UI_SPRITE_AIR_EMPTY,
    UI_SPRITE_AIR_BURSTING,
    UI_SPRITE_HOTBAR,
    UI_SPRITE_HOTBAR_SELECTION,
    UI_SPRITE_CROSSHAIR,
    UI_SPRITE_SLOT,
    UI_SPRITE_BUTTON,
    UI_SPRITE_BUTTON_HOVER,
    UI_SPRITE_BUTTON_DISABLED,
    UI_SPRITE_TEXT_FIELD,
    UI_SPRITE_PANEL,
    UI_SPRITE_TOOLTIP_FRAME,
    UI_SPRITE_COUNT
};

// ---- the pack ----

// True when the pack's GUI sprite root is on disk. Everything here degrades to the procedural
// plates the HUD already used when it is not, and a run without the pack is the normal case.
bool uiPackOnDisk();

const char* uiSpriteName(int sprite);
// Project-relative path into the pack, "" for a name the table does not carry.
const char* uiSpritePath(int sprite);
// Measured on the file rather than trusted from the table: the border is a pixel count and a sprite
// whose real size disagrees with the table would sample the wrong cells with no complaint at all.
bool uiSpritePresent(int sprite);
int uiSpriteWidth(int sprite);
int uiSpriteHeight(int sprite);
// The nine-patch border in pixels (`.mcmeta`'s gui.scaling.border), 0 when the sprite is not a patch.
int uiSpriteBorder(int sprite);

// Binds the sprite to the image: RGBA-normalised pixels under a stable id, the nine-patch margins,
// and the sprite's own size as the layout box. False - and no change - when the pack is missing, so
// the caller keeps whatever plate it authored.
bool uiBind(quantum::Image* image, int sprite);

// ---- the widget layer ----

// Called once by the host after the plates exist. The layer builds its widgets in this scene, which
// is the HUD's own scene layer, and remembers it to flip UI events on for a modal.
void uiAttach(quantum::Scene* hudScene);
quantum::Scene* uiScene();

// The row of vitals under the world view: ten hearts, ten food shanks, ten air drops, each drawn
// from the pack at the size the pack ships them. `showAir` is the underwater case - out of the water
// the air row is hidden rather than full, which is what the original HUD does.
//
// Thirst is not one of the three rows because the pack has no sprite for it: the only bubble art
// under `hud/` is the air column, and painting a thirst bar with it would say "you are running out of
// breath" to a player who is merely dry. The text plate keeps carrying that number.
void uiBuildVitals();
void uiSetVitals(float health, float food, float breath, bool showAir);
// How many icons a row has, and whether the row exists at all. Ten is the shape of the original
// bar: twenty units of vitals, half a bar per icon.
int uiVitalsRowCount();
bool uiVitalsBuilt();

// The hotbar frames, re-bound from the procedural plate to the pack's slot sprite. A bound cell is
// repainted white, because the plate's own colour is a multiply and a black 42 % plate over a sprite
// is a black square. Returns how many cells were re-bound, which is 0 without the pack.
int uiBindHotbar(quantum::Image* const* cells, int count, quantum::Image* selection, int selected);
// True once the cells carry the pack's frame: the caller then shades the selected slot with the
// sprite's own brightness instead of the plate tint it used before.
bool uiHotbarBound();

// A text with a font of its own. An empty path means the engine's embedded default; a path that is
// not on disk is refused rather than handed to the loader, which would log an error every frame.
bool uiSetFontPath(const std::string& path);
const char* uiFontPath();
quantum::Text* uiMakeLabel(const char* text, unsigned int fontSize, int xmid, int top, int bottom,
                          float r, float g, float b, float a);

// ---- modal ----
//
// There is no z-order in this UI: the hit test walks the layout array and the **last** widget whose
// rect contains the pointer owns the click. A modal is therefore not a layer on top of the HUD, it
// is (a) built after every widget it has to sit above, so it wins the hit test, and (b) invisible
// when closed, because `eventOnPointerDown` skips anything whose Transform is not visible. The
// backdrop covers the canvas so no click reaches a widget behind it at all.

bool uiOpenModal(const char* id, const char* title, int width, int height);
bool uiCloseModal();
bool uiModalOpen();
const char* uiModalId();

// One row of the modal, added in call order from the top down.
quantum::Text* uiModalLabel(const char* text);
// A button on the modal. `action` runs when the widget is clicked, and `uiClickButton` runs the same
// function - which is how a headless run presses a button it has no pointer for.
quantum::Button* uiModalButton(const char* name, const char* label, std::function<void()> action);
// A field the user can type into. Asking for a value is a modal plus a field plus two buttons.
quantum::TextEdit* uiModalField(const char* value, const char* placeholder);
// The two halves of a request, wired to the callback the caller keeps.
void uiAskText(const char* id, const char* title, const char* placeholder,
               const std::function<void(bool accepted, const std::string& value)>& done);
void uiAskConfirm(const char* id, const char* question,
                  const std::function<void(bool accepted)>& done);

bool uiClickButton(const char* name);
bool uiHasButton(const char* name);
int uiButtonCount();
const char* uiButtonName(int index);
std::string uiFieldText();
void uiSetFieldText(const std::string& text);

// True while a modal owns the pointer and the keyboard, so the controller can stand down.
bool uiInputBlocked();

// ---- reporting ----
const std::string& uiStats();
void probeVoxelUi(Probe& probe);

}  // namespace voxel
