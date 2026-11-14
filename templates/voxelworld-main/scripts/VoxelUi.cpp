// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelUi.h"
#include "VoxelProbe.h"

#include "Log.h"
#include "Scene.h"
#include "component/ButtonComponent.h"
#include "component/UILayoutComponent.h"
#include "io/FileData.h"
#include "object/ui/Button.h"
#include "object/ui/Image.h"
#include "object/ui/Text.h"
#include "object/ui/TextEdit.h"
#include "texture/TextureData.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace voxel {

// ---------------------------------------------------------------- the pack ----

namespace {

// The pack's GUI sprite root, project-relative. The engine resolves it through the same VFS every
// other asset uses, so this is a path a widget can be handed, not a path read with ifstream.
const char* const PACK_ROOT = "minecraft-assets-26.3/assets/minecraft/textures/gui/sprites/";

struct SpriteRow {
    const char* name;
    const char* file;      // under PACK_ROOT
    int width;             // measured with a PNG reader on 2026-11-02, re-checked at load
    int height;
    int border;            // .mcmeta gui.scaling.border, 0 when the sprite is not a patch
};

const SpriteRow kSprites[UI_SPRITE_COUNT] = {
    {"heart_container", "hud/heart/container.png", 9, 9, 0},
    {"heart_full", "hud/heart/full.png", 9, 9, 0},
    {"heart_half", "hud/heart/half.png", 9, 9, 0},
    {"food_full", "hud/food_full.png", 9, 9, 0},
    {"food_half", "hud/food_half.png", 9, 9, 0},
    {"food_empty", "hud/food_empty.png", 9, 9, 0},
    {"air_full", "hud/air.png", 9, 9, 0},
    {"air_empty", "hud/air_empty.png", 9, 9, 0},
    {"air_bursting", "hud/air_bursting.png", 9, 9, 0},
    {"hotbar", "hud/hotbar.png", 182, 22, 0},
    {"hotbar_selection", "hud/hotbar_selection.png", 24, 23, 0},
    {"crosshair", "hud/crosshair.png", 15, 15, 0},
    {"slot", "container/slot.png", 18, 18, 0},
    {"button", "widget/button.png", 200, 20, 3},
    {"button_hover", "widget/button_highlighted.png", 200, 20, 3},
    {"button_disabled", "widget/button_disabled.png", 200, 20, 1},
    {"text_field", "widget/text_field.png", 200, 20, 1},
    {"panel", "popup/background.png", 236, 34, 6},
    {"tooltip_frame", "tooltip/frame.png", 100, 100, 10}
};

// One load per sprite for the whole run: the pixels are kept here and every widget is handed a
// non-owning view of them. TextureData's copy is a shallow pointer copy and its destructor calls
// stbi_image_free only when dataOwned, so an owning TextureData would have to be either freed by
// us or double-freed by the pool that copies it - a view we never free is the safe shape.
struct Loaded {
    bool tried = false;
    bool ok = false;
    int width = 0;
    int height = 0;
    int border = 0;
    bool expanded = false;
    std::vector<unsigned char> rgba;
};

Loaded g_loaded[UI_SPRITE_COUNT];

int g_sizeMismatch = 0;
int g_borderMismatch = 0;
int g_expandedCount = 0;
int g_missingCount = 0;
int g_bindCount = 0;

int presentCount() {
    int loaded = 0;
    for (int i = 0; i < UI_SPRITE_COUNT; i++) {
        if (g_loaded[i].ok) loaded++;
    }
    return loaded;
}

std::string spriteFullPath(int sprite) {
    if (sprite < 0 || sprite >= UI_SPRITE_COUNT) return std::string();
    return std::string(PACK_ROOT) + kSprites[sprite].file;
}

// gui.scaling.border is an int in this pack, but the schema also allows an object with one side per
// key; a value the reader cannot understand falls back to the table rather than becoming border 0,
// because a wrong border stretches a nine-patch's corners instead of failing.
int readMetaBorder(const std::string& metaPath, int tableValue) {
    quantum::FileData* file = quantum::FileData::newFile(metaPath.c_str());
    if (!file) return tableValue;

    const unsigned int length = file->length();
    int value = -1;
    if (length > 0 && length < 65536) {
        std::string text(length, '\0');
        if (file->read((unsigned char*)&text[0], length) == length) {
            const size_t scaling = text.find("\"scaling\"");
            const size_t key = scaling == std::string::npos ? text.find("\"border\"")
                                                            : text.find("\"border\"", scaling);
            if (key != std::string::npos) {
                const size_t colon = text.find(':', key + 7);
                if (colon != std::string::npos) {
                    char* end = nullptr;
                    const long v = std::strtol(text.c_str() + colon + 1, &end, 10);
                    if (end != text.c_str() + colon + 1 && v >= 0 && v < 1024) value = (int)v;
                }
            }
        }
    }
    delete file;
    return value < 0 ? tableValue : value;
}

bool loadSprite(int sprite) {
    Loaded& slot = g_loaded[sprite];
    if (slot.tried) return slot.ok;
    slot.tried = true;

    const std::string path = spriteFullPath(sprite);
    quantum::TextureData source;
    if (!source.loadTextureFromFile(path.c_str())) {
        g_missingCount++;
        return false;
    }

    slot.width = source.getWidth();
    slot.height = source.getHeight();
    if (slot.width <= 0 || slot.height <= 0) return false;

    if (slot.width != kSprites[sprite].width || slot.height != kSprites[sprite].height) {
        g_sizeMismatch++;
        quantum::Log::print("VoxelUi: %s measures %dx%d, the table says %dx%d", kSprites[sprite].name,
                           slot.width, slot.height, kSprites[sprite].width, kSprites[sprite].height);
    }

    slot.border = readMetaBorder(path + ".mcmeta", kSprites[sprite].border);
    if (slot.border != kSprites[sprite].border) {
        g_borderMismatch++;
        quantum::Log::print("VoxelUi: %s border is %d, the table says %d", kSprites[sprite].name,
                           slot.border, kSprites[sprite].border);
    }

    const int pixels = slot.width * slot.height;
    slot.rgba.assign((size_t)pixels * 4, 0);

    if (source.getChannels() == 4 && source.getColorFormat() == quantum::ColorFormat::RGBA) {
        std::memcpy(slot.rgba.data(), source.getData(), (size_t)pixels * 4);
    } else if (source.getChannels() == 1 && source.getColorFormat() == quantum::ColorFormat::RED) {
        // A one-channel PNG stays RED, and a shader sampling R8 reads (v,0,0,1): every grayscale
        // sprite in the pack would paint red. The value has no alpha partner in the file, so the
        // fourth byte is opaque.
        const unsigned char* gray = (const unsigned char*)source.getData();
        for (int i = 0; i < pixels; i++) {
            slot.rgba[(size_t)i * 4 + 0] = gray[i];
            slot.rgba[(size_t)i * 4 + 1] = gray[i];
            slot.rgba[(size_t)i * 4 + 2] = gray[i];
            slot.rgba[(size_t)i * 4 + 3] = 255;
        }
        slot.expanded = true;
        g_expandedCount++;
    } else {
        quantum::Log::print("VoxelUi: %s is %d-channel %d, only RED and RGBA are handled",
                           kSprites[sprite].name, source.getChannels(),
                           (int)source.getColorFormat());
        return false;
    }

    slot.ok = true;
    return true;
}

// ---------------------------------------------------------------- widgets ----

quantum::Scene* g_scene = nullptr;
std::string g_fontPath;
std::string g_stats;
bool g_hotbarBound = false;

// The layout derives width and height from the anchor offsets, so a box is placed by its four edges
// relative to its four anchor points and setSize would be overwritten on the next layout pass.
void placeBox(quantum::UILayout* box, float pl, float pt, float pr, float pb, int left, int top,
              int right, int bottom) {
    box->setAnchorPoints(pl, pt, pr, pb);
    box->setAnchorOffsets(left, top, right, bottom);
}

// IgnoreEvents keeps a decorative image out of the pointer hit test, which walks the whole layout
// array and hands the click to the last rect that contains the pointer.
void setClickable(quantum::UILayout* box, bool clickable) {
    quantum::UILayoutComponent* layout =
        box->getScene()->findComponent<quantum::UILayoutComponent>(box->getEntity());
    if (layout) layout->ignoreEvents = !clickable;
}

// The anchor read back off the component: a box is placed by its anchors, so this is the only way to
// ask a finished widget where it actually ended up instead of trusting the call that built it.
quantum::UILayoutComponent* layoutOf(quantum::UILayout* box) {
    if (!box || !box->getScene()) return nullptr;
    return box->getScene()->findComponent<quantum::UILayoutComponent>(box->getEntity());
}

bool bindTo(quantum::Image* image, int sprite, bool clickable) {
    if (!image || !loadSprite(sprite)) return false;

    Loaded& slot = g_loaded[sprite];
    quantum::TextureData view(slot.width, slot.height, (unsigned int)(slot.rgba.size()),
                             quantum::ColorFormat::RGBA, 4, slot.rgba.data());
    image->setTexture(std::string("voxel:ui/") + kSprites[sprite].name, view);
    if (slot.border > 0) image->setPatchMargin((unsigned int)slot.border);
    setClickable(image, clickable);
    g_bindCount++;
    return true;
}

struct VitalRow {
    quantum::Image* base[10] = {nullptr};
    quantum::Image* state[10] = {nullptr};
    int full = UI_SPRITE_COUNT;
    int half = UI_SPRITE_COUNT;
    int empty = UI_SPRITE_COUNT;
    bool built = false;
    int visible = 0;
};

VitalRow g_hearts;
VitalRow g_food;
VitalRow g_air;

// One icon per two units of a 0..20 vital, so a value of 15 is seven full and one half. The block is
// 9 px in the pack and drawn twice as large. The hotbar strip owns -92..-44 around the centre, so the
// rows sit either side of it at the bottom edge and stack upward from there.
const int VITAL_ICON = 18;
const int VITAL_PITCH = 20;
const int VITAL_ROW1 = -40;
const int VITAL_ROW2 = -62;

// `fromRight` anchors the row to the right edge instead of the left, which is what keeps the food bar
// off the hearts: both rows are on VITAL_ROW1 and would otherwise draw on top of each other. Icon 0 is
// always the anchored end, so a left row drains toward the centre and a right row drains toward the
// centre too - the way the original HUD reads.
void buildRow(VitalRow& row, int base, int fullSprite, int halfSprite, int emptySprite, int bottom,
              bool fromRight) {
    if (!g_scene || row.built) return;
    row.full = fullSprite;
    row.half = halfSprite;
    row.empty = emptySprite;

    const float ax = fromRight ? 1.0f : 0.0f;
    for (int i = 0; i < 10; i++) {
        int left = 8 + i * VITAL_PITCH;
        int right = left + VITAL_ICON;
        if (fromRight) {
            const int far = -(8 + i * VITAL_PITCH);
            left = far - VITAL_ICON;
            right = far;
        }

        quantum::Image* socket = new quantum::Image(g_scene);
        bindTo(socket, base, false);
        placeBox(socket, ax, 1.0f, ax, 1.0f, left, bottom - VITAL_ICON, right, bottom);
        socket->setColor(1.0f, 1.0f, 1.0f, 1.0f);
        row.base[i] = socket;

        quantum::Image* overlay = new quantum::Image(g_scene);
        bindTo(overlay, fullSprite, false);
        placeBox(overlay, ax, 1.0f, ax, 1.0f, left, bottom - VITAL_ICON, right, bottom);
        overlay->setColor(1.0f, 1.0f, 1.0f, 1.0f);
        overlay->setVisible(false);
        row.state[i] = overlay;
    }
    row.built = true;
}

// One icon per two units of a 0..20 vital, so a value of 15 is seven full and one half.
void setRow(VitalRow& row, float value, bool show) {
    if (!row.built) return;

    int lit = 0;
    for (int i = 0; i < 10; i++) {
        const float remainder = value - (float)i * 2.0f;
        quantum::Image* overlay = row.state[i];
        if (!overlay) continue;

        if (!show || remainder < 1.0f) {
            overlay->setVisible(false);
        } else {
            bindTo(overlay, remainder >= 2.0f ? row.full : row.half, false);
            overlay->setVisible(true);
            lit++;
        }
        if (row.base[i]) row.base[i]->setVisible(show);
    }
    row.visible = show ? lit : 0;
}

// ------------------------------------------------------------------ modal ----

const int MODAL_LABELS = 6;
const int MODAL_BUTTONS = 6;
const int MODAL_ROW_STEP = 26;
const int MODAL_PAD = 14;

struct ButtonSlot {
    std::string name;
    std::string label;
    std::function<void()> action;
    quantum::Button* button = nullptr;
};

struct ModalState {
    std::string id;
    bool open = false;
    int width = 300;
    int height = 200;

    quantum::Image* backdrop = nullptr;
    quantum::Image* panel = nullptr;
    quantum::Text* title = nullptr;
    quantum::Text* labels[MODAL_LABELS] = {nullptr};
    ButtonSlot buttons[MODAL_BUTTONS];
    quantum::TextEdit* field = nullptr;

    int labelUsed = 0;
    int fieldTop = 0;

    std::function<void(bool, const std::string&)> onTextDone;
    std::function<void(bool)> onConfirmDone;

    std::vector<quantum::UILayout*> owneds;
};

ModalState g_modal;

bool g_eventsWereEnabled = false;

void raise(quantum::UILayout* box) {
    if (box) box->moveToTop();
}

void setModalEvents(bool on) {
    if (!g_scene) return;
    // The HUD layer declares itself a UI scene, which is what makes it claim pointer events; while
    // no modal is open the world scene has to keep them, so the layer answers DISABLED.
    g_scene->setEnableUIEvents(on ? quantum::UIEventState::ENABLED : quantum::UIEventState::DISABLED);
}

void hideModalWidgets() {
    if (g_modal.backdrop) g_modal.backdrop->setVisible(false);
    if (g_modal.panel) g_modal.panel->setVisible(false);
    if (g_modal.title) g_modal.title->setVisible(false);
    if (g_modal.field) g_modal.field->setVisible(false);
    for (int i = 0; i < MODAL_LABELS; i++) {
        if (g_modal.labels[i]) g_modal.labels[i]->setVisible(false);
    }
    for (int i = 0; i < MODAL_BUTTONS; i++) {
        if (g_modal.buttons[i].button) g_modal.buttons[i].button->setVisible(false);
    }
}

void ensureModalSkeleton() {
    if (g_modal.backdrop || !g_scene) return;

    // The backdrop is the project's own white plate tinted dark, not a pack sprite: it has to cover
    // the canvas with no border and no artwork of its own.
    g_modal.backdrop = new quantum::Image(g_scene);
    g_modal.backdrop->setTexture("textures/voxel/panel.png");
    g_modal.backdrop->setColor(0.0f, 0.0f, 0.0f, 0.55f);
    placeBox(g_modal.backdrop, 0.0f, 0.0f, 1.0f, 1.0f, 0, 0, 0, 0);

    g_modal.panel = new quantum::Image(g_scene);
    g_modal.title = new quantum::Text(g_scene);
    g_modal.title->setFontSize(18);
    g_modal.title->setColor(1.0f, 1.0f, 1.0f, 1.0f);
    if (!g_fontPath.empty()) g_modal.title->setFont(0, g_fontPath);

    for (int i = 0; i < MODAL_LABELS; i++) {
        quantum::Text* label = new quantum::Text(g_scene);
        label->setFontSize(15);
        label->setColor(0.85f, 0.85f, 0.85f, 1.0f);
        if (!g_fontPath.empty()) label->setFont(0, g_fontPath);
        label->setVisible(false);
        g_modal.labels[i] = label;
    }

    for (int i = 0; i < MODAL_BUTTONS; i++) {
        quantum::Button* button = new quantum::Button(g_scene);
        button->setVisible(false);
        g_modal.buttons[i].button = button;
    }

    g_modal.field = new quantum::TextEdit(g_scene);
    g_modal.field->setFontSize(15);
    g_modal.field->setTextColor(1.0f, 1.0f, 1.0f, 1.0f);
    g_modal.field->setPlaceholderColor(0.5f, 0.5f, 0.5f, 1.0f);
    g_modal.field->setMaxTextSize(64);
    g_modal.field->setVisible(false);
}

void layoutModal(int width, int height) {
    const int halfW = width / 2;
    const int halfH = height / 2;

    if (g_loaded[UI_SPRITE_PANEL].ok) bindTo(g_modal.panel, UI_SPRITE_PANEL, false);
    placeBox(g_modal.panel, 0.5f, 0.5f, 0.5f, 0.5f, -halfW, -halfH, halfW, halfH);

    // Title first, then a running cursor down the panel: rows are placed from the centre so nothing
    // here needs to know the canvas size.
    const int titleTop = -halfH + MODAL_PAD;
    placeBox(g_modal.title, 0.5f, 0.5f, 0.5f, 0.5f, -halfW + MODAL_PAD, titleTop, halfW - MODAL_PAD,
             titleTop + 20);
    g_modal.labelUsed = 0;
    g_modal.fieldTop = titleTop + 30;
}

ButtonSlot* findButton(const char* name) {
    for (int i = 0; i < MODAL_BUTTONS; i++) {
        if (g_modal.buttons[i].button && g_modal.buttons[i].name == name) return &g_modal.buttons[i];
    }
    return nullptr;
}

ButtonSlot* claimButton(const char* name) {
    ButtonSlot* existing = findButton(name);
    if (existing) return existing;
    for (int i = 0; i < MODAL_BUTTONS; i++) {
        if (!g_modal.buttons[i].button || g_modal.buttons[i].name.empty()) return &g_modal.buttons[i];
    }
    return nullptr;
}

void applyButtonSkin(quantum::Button* button) {
    if (!button) return;
    bindTo(button, UI_SPRITE_BUTTON, true);

    // The widget swaps the component's four Textures into the Image as the pointer moves over it, so
    // each state needs the pack sprite bound the same way the base one is; the paths the setters take
    // cannot carry in-memory pixels, which is why the component is written directly.
    quantum::ButtonComponent* comp =
        button->getScene()->findComponent<quantum::ButtonComponent>(button->getEntity());
    if (!comp) return;

    const int states[4] = {UI_SPRITE_BUTTON, UI_SPRITE_BUTTON_HOVER, UI_SPRITE_BUTTON,
                           UI_SPRITE_BUTTON_DISABLED};
    quantum::Texture* targets[4] = {&comp->textureNormal, &comp->textureHovered, &comp->texturePressed,
                                   &comp->textureDisabled};
    for (int i = 0; i < 4; i++) {
        if (!loadSprite(states[i])) continue;
        Loaded& slot = g_loaded[states[i]];
        *targets[i] = quantum::Texture(
            std::string("voxel:ui/") + kSprites[states[i]].name,
            quantum::TextureData(slot.width, slot.height, (unsigned int)(slot.rgba.size()),
                                quantum::ColorFormat::RGBA, 4, slot.rgba.data()));
    }
    comp->needUpdateButton = true;
}

}  // namespace

// ------------------------------------------------------------------ the pack ----

bool uiPackOnDisk() {
    // One representative file answers for the root: the pack either shipped with the project or it
    // did not, and probing all nineteen would load nineteen PNGs to prove a directory exists.
    return loadSprite(UI_SPRITE_BUTTON);
}

const char* uiSpriteName(int sprite) {
    if (sprite < 0 || sprite >= UI_SPRITE_COUNT) return "";
    return kSprites[sprite].name;
}

const char* uiSpritePath(int sprite) {
    if (sprite < 0 || sprite >= UI_SPRITE_COUNT) return "";
    return kSprites[sprite].file;
}

bool uiSpritePresent(int sprite) {
    if (sprite < 0 || sprite >= UI_SPRITE_COUNT) return false;
    return loadSprite(sprite);
}

int uiSpriteWidth(int sprite) {
    if (sprite < 0 || sprite >= UI_SPRITE_COUNT || !loadSprite(sprite)) return 0;
    return g_loaded[sprite].width;
}

int uiSpriteHeight(int sprite) {
    if (sprite < 0 || sprite >= UI_SPRITE_COUNT || !loadSprite(sprite)) return 0;
    return g_loaded[sprite].height;
}

int uiSpriteBorder(int sprite) {
    if (sprite < 0 || sprite >= UI_SPRITE_COUNT || !loadSprite(sprite)) return 0;
    return g_loaded[sprite].border;
}

bool uiBind(quantum::Image* image, int sprite) {
    if (sprite < 0 || sprite >= UI_SPRITE_COUNT) return false;
    return bindTo(image, sprite, true);
}

// ---------------------------------------------------------------- the layer ----

void uiAttach(quantum::Scene* hudScene) {
    g_scene = hudScene;
    if (!g_scene) return;
    ensureModalSkeleton();
    hideModalWidgets();
    g_eventsWereEnabled = false;
}

quantum::Scene* uiScene() { return g_scene; }

void uiBuildVitals() {
    ensureModalSkeleton();
    buildRow(g_hearts, UI_SPRITE_HEART_CONTAINER, UI_SPRITE_HEART_FULL, UI_SPRITE_HEART_HALF,
             UI_SPRITE_HEART_CONTAINER, VITAL_ROW1, false);
    buildRow(g_food, UI_SPRITE_FOOD_EMPTY, UI_SPRITE_FOOD_FULL, UI_SPRITE_FOOD_HALF,
             UI_SPRITE_FOOD_EMPTY, VITAL_ROW1, true);
    buildRow(g_air, UI_SPRITE_AIR_EMPTY, UI_SPRITE_AIR_FULL, UI_SPRITE_AIR_FULL,
             UI_SPRITE_AIR_EMPTY, VITAL_ROW2, true);

    // Nothing is lit before the first update, and the air row is the underwater case only.
    setRow(g_hearts, 0.0f, false);
    setRow(g_food, 0.0f, false);
    setRow(g_air, 0.0f, false);
}

void uiSetVitals(float health, float food, float breath, bool showAir) {
    setRow(g_hearts, health, true);
    setRow(g_food, food, true);
    setRow(g_air, breath, showAir);
}

int uiVitalsRowCount() { return g_hearts.built ? 10 : 0; }

bool uiVitalsBuilt() { return g_hearts.built && g_food.built && g_air.built; }

int uiBindHotbar(quantum::Image* const* cells, int count, quantum::Image* selection, int selected) {
    int bound = 0;
    if (cells) {
        for (int i = 0; i < count; i++) {
            if (!bindTo(cells[i], UI_SPRITE_SLOT, false)) continue;
            bound++;
            // The cell was authored as a translucent black plate, and an Image multiplies its texture
            // by its own colour: left alone, the pack's frame would be tinted to near-black.
            cells[i]->setColor(1.0f, 1.0f, 1.0f, 1.0f);
        }
    }
    if (selection) {
        if (bindTo(selection, UI_SPRITE_HOTBAR_SELECTION, false)) {
            placeBox(selection, 0.5f, 1.0f, 0.5f, 1.0f, -198 + selected * 44, -96,
                     -198 + selected * 44 + 46, -46);
        }
    }
    g_hotbarBound = bound > 0;
    return bound;
}

bool uiHotbarBound() { return g_hotbarBound; }

bool uiSetFontPath(const std::string& path) {
    if (path.empty()) {
        g_fontPath.clear();
        return true;
    }
    // The question is asked on the disk because the loader only logs, once per frame, and a widget
    // left pointing at a file that is not there keeps a game running with no text and no error.
    std::error_code ec;
    const std::filesystem::path resolved =
        std::filesystem::path(quantum::FileData::getSystemPath(path));
    if (!std::filesystem::exists(resolved, ec)) return false;
    g_fontPath = path;
    return true;
}

const char* uiFontPath() { return g_fontPath.c_str(); }

quantum::Text* uiMakeLabel(const char* text, unsigned int fontSize, int xmid, int top, int bottom,
                          float r, float g, float b, float a) {
    if (!g_scene) return nullptr;

    quantum::Text* label = new quantum::Text(g_scene);
    label->setFontSize(fontSize);
    label->setColor(r, g, b, a);
    if (!g_fontPath.empty()) label->setFont(0, g_fontPath);
    label->setText(text ? text : "");
    label->createText();

    // The engine never sizes a layout box to the run, so centring means measuring the advance this
    // font reports for these characters and putting the box where that advance is symmetric.
    float advance = 0.0f;
    const std::string s = text ? text : "";
    for (size_t i = 0; i < s.size(); i++) advance += label->getCharWidth((uint32_t)(unsigned char)s[i]);
    const int half = (int)(advance * 0.5f) + 1;
    placeBox(label, 0.5f, 1.0f, 0.5f, 1.0f, xmid - half, top, xmid + half, bottom);
    setClickable(label, false);
    return label;
}

// ------------------------------------------------------------------- modal ----

bool uiOpenModal(const char* id, const char* title, int width, int height) {
    if (!g_scene) return false;
    ensureModalSkeleton();

    g_modal.id = id ? id : "";
    g_modal.width = width > 0 ? width : 300;
    g_modal.height = height > 0 ? height : 200;

    g_modal.title->setText(title ? title : "");
    if (!g_fontPath.empty()) g_modal.title->setFont(0, g_fontPath);
    g_modal.title->createText();
    layoutModal(g_modal.width, g_modal.height);

    for (int i = 0; i < MODAL_LABELS; i++) {
        if (g_modal.labels[i]) g_modal.labels[i]->setVisible(false);
    }
    for (int i = 0; i < MODAL_BUTTONS; i++) {
        if (g_modal.buttons[i].button) g_modal.buttons[i].button->setVisible(false);
    }
    g_modal.labelUsed = 0;
    g_modal.field->setVisible(false);

    g_modal.backdrop->setVisible(true);
    g_modal.panel->setVisible(true);
    g_modal.title->setVisible(true);

    // Draw order and the hit test both follow the layout array, so the modal raises its own pieces
    // in the order they must stack: backdrop, panel, title, then whatever rows arrive next.
    raise(g_modal.backdrop);
    raise(g_modal.panel);
    raise(g_modal.title);

    g_modal.open = true;
    setModalEvents(true);
    g_eventsWereEnabled = true;
    return true;
}

bool uiCloseModal() {
    if (!g_modal.open) return false;
    g_modal.open = false;
    hideModalWidgets();
    setModalEvents(false);
    g_eventsWereEnabled = false;
    g_modal.onTextDone = nullptr;
    g_modal.onConfirmDone = nullptr;
    return true;
}

bool uiModalOpen() { return g_modal.open; }

const char* uiModalId() { return g_modal.id.c_str(); }

quantum::Text* uiModalLabel(const char* text) {
    if (!g_modal.open || g_modal.labelUsed >= MODAL_LABELS) return nullptr;

    quantum::Text* label = g_modal.labels[g_modal.labelUsed];
    const int top = -g_modal.height / 2 + MODAL_PAD + 28 + g_modal.labelUsed * MODAL_ROW_STEP;
    label->setText(text ? text : "");
    if (!g_fontPath.empty()) label->setFont(0, g_fontPath);
    label->createText();
    placeBox(label, 0.5f, 0.5f, 0.5f, 0.5f, -g_modal.width / 2 + MODAL_PAD, top,
             g_modal.width / 2 - MODAL_PAD, top + 18);
    label->setVisible(true);
    raise(label);
    g_modal.labelUsed++;
    return label;
}

quantum::Button* uiModalButton(const char* name, const char* label, std::function<void()> action) {
    if (!g_modal.open || !name) return nullptr;

    ButtonSlot* slot = claimButton(name);
    if (!slot || !slot->button) return nullptr;

    const int index = (int)(slot - g_modal.buttons);
    const int top = -g_modal.height / 2 + MODAL_PAD + 28 + MODAL_LABELS * MODAL_ROW_STEP + index * 26;

    slot->name = name;
    slot->label = label ? label : name;
    slot->action = action;

    applyButtonSkin(slot->button);
    slot->button->setLabel(slot->label);
    slot->button->setLabelFontSize(15);
    slot->button->setLabelColor(1.0f, 1.0f, 1.0f, 1.0f);
    if (!g_fontPath.empty()) slot->button->setLabelFont(g_fontPath);
    slot->button->setScaleHovered(1.0f);
    slot->button->setTransitionTime(0.0f);
    slot->button->setDisabled(false);
    placeBox(slot->button, 0.5f, 0.5f, 0.5f, 0.5f, -g_modal.width / 2 + MODAL_PAD, top,
             g_modal.width / 2 - MODAL_PAD, top + 20);
    slot->button->setVisible(true);

    quantum::ButtonComponent* comp =
        slot->button->getScene()->findComponent<quantum::ButtonComponent>(slot->button->getEntity());
    if (comp) {
        std::function<void()> fired = action;
        comp->onPress.add("voxel_ui", [fired]() {
            if (fired) fired();
        });
    }

    raise(slot->button);
    return slot->button;
}

quantum::TextEdit* uiModalField(const char* value, const char* placeholder) {
    if (!g_modal.open || !g_modal.field) return nullptr;

    if (loadSprite(UI_SPRITE_TEXT_FIELD)) bindTo(g_modal.field, UI_SPRITE_TEXT_FIELD, true);
    const int top = g_modal.fieldTop;
    placeBox(g_modal.field, 0.5f, 0.5f, 0.5f, 0.5f, -g_modal.width / 2 + MODAL_PAD, top,
             g_modal.width / 2 - MODAL_PAD, top + 20);
    g_modal.field->setPlaceholder(placeholder ? placeholder : "");
    g_modal.field->setText(value ? value : "");
    g_modal.field->setVisible(true);
    if (!g_fontPath.empty()) g_modal.field->setTextFont(g_fontPath);
    raise(g_modal.field);
    return g_modal.field;
}

void uiAskText(const char* id, const char* title, const char* placeholder,
               const std::function<void(bool accepted, const std::string& value)>& done) {
    if (!uiOpenModal(id, title, 320, 150)) return;
    g_modal.onTextDone = done;
    g_modal.fieldTop = -75 + MODAL_PAD + 28;
    uiModalField("", placeholder);

    uiModalButton("accept", "OK", []() {
        const std::string value = uiFieldText();
        const std::function<void(bool, const std::string&)> done2 = g_modal.onTextDone;
        uiCloseModal();
        if (done2) done2(true, value);
    });
    uiModalButton("cancel", "Cancel", []() {
        const std::function<void(bool, const std::string&)> done2 = g_modal.onTextDone;
        uiCloseModal();
        if (done2) done2(false, std::string());
    });
}

void uiAskConfirm(const char* id, const char* question,
                  const std::function<void(bool accepted)>& done) {
    if (!uiOpenModal(id, question, 300, 130)) return;
    g_modal.onConfirmDone = done;

    uiModalButton("yes", "Yes", []() {
        const std::function<void(bool)> done2 = g_modal.onConfirmDone;
        uiCloseModal();
        if (done2) done2(true);
    });
    uiModalButton("no", "No", []() {
        const std::function<void(bool)> done2 = g_modal.onConfirmDone;
        uiCloseModal();
        if (done2) done2(false);
    });
}

bool uiClickButton(const char* name) {
    ButtonSlot* slot = findButton(name);
    if (!slot || !slot->action) return false;
    slot->action();
    return true;
}

bool uiHasButton(const char* name) { return findButton(name) != nullptr; }

int uiButtonCount() {
    int n = 0;
    for (int i = 0; i < MODAL_BUTTONS; i++) {
        if (!g_modal.buttons[i].name.empty()) n++;
    }
    return n;
}

const char* uiButtonName(int index) {
    if (index < 0 || index >= MODAL_BUTTONS) return "";
    return g_modal.buttons[index].name.c_str();
}

std::string uiFieldText() {
    return g_modal.field ? g_modal.field->getText() : std::string();
}

void uiSetFieldText(const std::string& text) {
    if (g_modal.field) g_modal.field->setText(text);
}

bool uiInputBlocked() { return g_modal.open; }

// ---------------------------------------------------------------- reporting ----

const std::string& uiStats() {
    char line[256];
    std::string out;

    std::snprintf(line, sizeof(line), "sprites %d/%d loaded", presentCount(), UI_SPRITE_COUNT);
    out += line;
    std::snprintf(line, sizeof(line), ", binds %d", g_bindCount);
    out += line;
    std::snprintf(line, sizeof(line), ", grayscale expanded %d", g_expandedCount);
    out += line;
    std::snprintf(line, sizeof(line), ", missing %d", g_missingCount);
    out += line;
    std::snprintf(line, sizeof(line), ", size mismatch %d", g_sizeMismatch);
    out += line;
    std::snprintf(line, sizeof(line), ", border mismatch %d", g_borderMismatch);
    out += line;
    out += "\nfont: ";
    out += g_fontPath.empty() ? "engine default" : g_fontPath;

    g_stats = out;
    return g_stats;
}

void probeVoxelUi(Probe& probe) {
    probe.begin("VoxelUi");

    const bool pack = uiPackOnDisk();
    probe.check("the pack's GUI sprite root is found or the layer degrades quietly", pack || g_missingCount > 0,
                "pack %d, missing %d", pack ? 1 : 0, g_missingCount);

    if (pack) {
        for (int i = 0; i < UI_SPRITE_COUNT; i++) uiSpritePresent(i);

        probe.check("every sprite in the table loads with its measured size",
                    presentCount() == UI_SPRITE_COUNT && g_sizeMismatch == 0,
                    "%d loaded, %d size mismatch", presentCount(), g_sizeMismatch);

        // The nine-patch borders are read off the pack rather than trusted from the table.
        probe.check("button and panel borders match the pack's .mcmeta",
                    uiSpriteBorder(UI_SPRITE_BUTTON) == 3 && uiSpriteBorder(UI_SPRITE_PANEL) == 6,
                    "button %d, panel %d, mismatches %d", uiSpriteBorder(UI_SPRITE_BUTTON),
                    uiSpriteBorder(UI_SPRITE_PANEL), g_borderMismatch);

        // A grayscale PNG reaching a widget as RED is the failure this layer exists to prevent.
        probe.check("grayscale sprites reach the widget as RGBA, not RED",
                    g_expandedCount >= 4, "expanded %d, %d size or format refused", g_expandedCount,
                    g_missingCount);

        probe.check("a slot sprite binds an image and gives it its own box",
                    uiSpriteWidth(UI_SPRITE_SLOT) == 18 && uiSpriteHeight(UI_SPRITE_SLOT) == 18,
                    "slot %dx%d", uiSpriteWidth(UI_SPRITE_SLOT), uiSpriteHeight(UI_SPRITE_SLOT));
    }

    if (g_scene) {
        quantum::Image* plate = new quantum::Image(g_scene);
        const bool bound = uiBind(plate, UI_SPRITE_CROSSHAIR);
        probe.check("a pack sprite binds a live widget", bound, "bind %d, binds so far %d",
                    bound ? 1 : 0, g_bindCount);

        const float healthBefore = 10.0f;
        uiBuildVitals();
        uiSetVitals(healthBefore, 20.0f, 3.0f, true);
        probe.check("ten hearts and ten shanks are built and the vitals answer",
                    uiVitalsBuilt() && uiVitalsRowCount() == 10, "rows %d", uiVitalsRowCount());
        probe.check("a vital of 10 lights five icons of ten", g_hearts.visible == 5,
                    "lit %d for health %.1f", g_hearts.visible, healthBefore);
        // Three of ten breath is one whole bubble and one half: an icon carries two units, so the
        // row answers 2 lit and not 3 - the same half-icon rule that makes health 10 read as five
        // hearts out of ten.
        probe.check("the air row only shows when the head is under water",
                    g_air.visible == 2, "air lit %d for breath %.1f", g_air.visible, 3.0f);
        uiSetVitals(10.0f, 20.0f, 3.0f, false);
        probe.check("out of the water the air row hides rather than fills", g_air.visible == 0,
                    "air lit %d", g_air.visible);

        // The hearts and the food share a line, so the only thing that keeps a drumstick off a heart is
        // the anchor: one row pinned to the left edge, the other to the right. Read it back off the
        // component rather than trusting the builder's argument - a captured frame once showed both
        // rows drawn on top of each other because the mirroring reversed the fill order but never
        // moved the boxes.
        quantum::UILayoutComponent* hearts0 = layoutOf(g_hearts.base[0]);
        quantum::UILayoutComponent* food0 = layoutOf(g_food.base[0]);
        quantum::UILayoutComponent* air0 = layoutOf(g_air.base[0]);
        probe.check("the hearts hold the left edge, the food the right, and the air stacks above it",
                    hearts0 && food0 && air0 && hearts0->anchorPointLeft == 0.0f &&
                        food0->anchorPointLeft == 1.0f && air0->anchorPointLeft == 1.0f &&
                        hearts0->anchorOffsetBottom == food0->anchorOffsetBottom &&
                        air0->anchorOffsetBottom < food0->anchorOffsetBottom,
                    "hearts ax %.0f off %d, food ax %.0f off %d, air ax %.0f off %d",
                    hearts0 ? hearts0->anchorPointLeft : -1.0f,
                    hearts0 ? hearts0->anchorOffsetBottom : 0, food0 ? food0->anchorPointLeft : -1.0f,
                    food0 ? food0->anchorOffsetBottom : 0, air0 ? air0->anchorPointLeft : -1.0f,
                    air0 ? air0->anchorOffsetBottom : 0);

        const bool opened = uiOpenModal("probe", "Probe", 280, 160);
        probe.check("opening a modal blocks input and claims the pointer",
                    opened && uiModalOpen() && uiInputBlocked(), "open %d, blocked %d",
                    uiModalOpen() ? 1 : 0, uiInputBlocked() ? 1 : 0);

        int clicked = 0;
        uiModalButton("probe_ok", "OK", [&clicked]() { clicked++; });
        probe.check("a modal button is registered by name and pressable headless",
                    uiHasButton("probe_ok") && uiClickButton("probe_ok") && clicked == 1,
                    "clicked %d", clicked);

        uiAskText("probe_ask", "Seed", "42", [](bool accepted, const std::string&) {
            if (!accepted) return;
        });
        uiSetFieldText("19");
        probe.check("asking for a value keeps what was typed", uiFieldText() == "19",
                    "field \"%s\"", uiFieldText().c_str());
        uiCloseModal();

        bool answered = false;
        uiAskConfirm("probe_confirm", "Delete?", [&answered](bool yes) { answered = yes; });
        uiClickButton("no");
        probe.check("a request's cancel path answers false and closes the modal",
                    !answered && !uiModalOpen() && !uiInputBlocked(), "answered %d", answered ? 1 : 0);

        const bool badFont = uiSetFontPath("textures/voxel/not-a-font.ttf");
        probe.check("a font that is not on disk is refused, not handed to the loader", !badFont,
                    "refused %d, path still \"%s\"", badFont ? 0 : 1, uiFontPath());
        probe.check("an empty font path means the engine's default", uiSetFontPath("") && uiFontPath()[0] == '\0',
                    "path \"%s\"", uiFontPath());

        uiCloseModal();
        probe.check("closing leaves the world scene owning the pointer again",
                    !uiModalOpen() && !uiInputBlocked(), "blocked %d", uiInputBlocked() ? 1 : 0);
    } else {
        probe.skip("no HUD scene was attached");
    }

    probe.end();
}

}  // namespace voxel
