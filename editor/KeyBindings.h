// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

namespace quantum::editor {

enum KeyBindingFlags : uint32_t {
    KeyBinding_None = 0,

    // Skipped while a text field owns the caret: "z" and Delete are typing there, not
    // an undo or a destroyed selection.
    KeyBinding_StopInText = 1u << 0,

    // The chord matches whatever modifiers are held. Used by Delete, which the editor
    // has always accepted with or without Ctrl.
    KeyBinding_IgnoreMods = 1u << 1,

    // Holding the key does not fire it again (ImGui repeats by default).
    KeyBinding_NoRepeat = 1u << 2,
};

// One remappable keyboard shortcut. `id` is the stable name written to settings.yaml,
// so it survives a renamed label and a reordered list.
struct KeyBinding {
    const char* id;
    uint32_t command;      // AppMenuCommand value, 0 when no menu item exposes it
    const char* category;  // groups the rows in the Shortcuts list
    const char* label;
    const char* chords;    // default chord text, comma separated ("Ctrl+Shift+Z, Ctrl+Y")
    uint32_t flags;
};

// The editor's keyboard shortcuts, in one place. Before this existed the same chord was
// written twice - once as a literal in the menu label, once in an ImGuiKey test in the
// input pass - and the two disagreed (Redo was offered as Ctrl+Shift+Z and as Ctrl+Y in
// the palette, and dispatched by both). The registry is the only source now: menus,
// palette and dispatch all read it, and the user's overrides live in AppSettings.
class KeyBindings {
public:
    static const std::vector<KeyBinding>& all();

    static const KeyBinding* find(const std::string& id);
    // First binding whose command matches; several menu commands share one binding
    // (Run and Resume Scene both answer F5).
    static const KeyBinding* findByCommand(uint32_t command);
    static const char* defaultChords(const std::string& id);

    // Display text of the effective chords, "" when the command is unbound.
    static std::string text(const std::string& id);
    // Only the first chord: menu rows are too narrow for a binding with alternates,
    // and the Shortcuts list shows the full text().
    static std::string primaryText(const std::string& id);
    static std::string textForCommand(uint32_t command);
    static std::vector<ImGuiKeyChord> effective(const std::string& id);

    static bool isOverridden(const std::string& id);
    // chordText "" unbinds the command; anything that does not parse is rejected with
    // the offending token in `error`, so the UI can say why nothing happened.
    static bool bind(const std::string& id, const std::string& chordText, std::string& error);
    static bool reset(const std::string& id);
    static void resetAll();

    // True on the frame one of the binding's chords was pressed. App code calls this
    // instead of testing ImGuiKey values.
    static bool pressed(const std::string& id);
    static bool pressedCommand(uint32_t command);

    // Pairs of binding ids that answer the same chord, for the Shortcuts list to warn
    // about before a user silently loses a key.
    static std::vector<std::pair<std::string, std::string>> conflicts();

    // Re-reads the overrides from AppSettings. Called on the first use and after every
    // edit, so a settings.yaml edited by hand lands without a restart.
    static void reload();

    static std::string chordText(ImGuiKeyChord chord);
    static bool parseChord(const std::string& token, ImGuiKeyChord& out);

private:
    static const std::vector<ImGuiKeyChord>& chordsFor(const KeyBinding& binding);
};

} // namespace quantum::editor
