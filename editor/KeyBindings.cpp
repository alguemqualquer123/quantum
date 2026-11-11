// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "KeyBindings.h"

#include "AppMenuCommand.h"
#include "AppSettings.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace quantum::editor {

namespace {

// The shortcuts the editor answers. `command` is the AppMenuCommand value carried by the
// menu item and the palette entry, so one row drives all three (Run and Resume Scene
// deliberately share one binding, both answered by F5; Rename has no menu item).
const KeyBinding kBindings[] = {
    { "file.save", uint32_t(AppMenuCommand::Save), "File", "Save",
        "Ctrl+S", KeyBinding_None },
    { "file.saveAll", uint32_t(AppMenuCommand::SaveAll), "File", "Save All",
        "Ctrl+Shift+S", KeyBinding_None },
    { "file.openProject", uint32_t(AppMenuCommand::OpenProject), "File", "Open Project...",
        "Ctrl+O", KeyBinding_None },

    { "edit.undo", uint32_t(AppMenuCommand::Undo), "Edit", "Undo",
        "Ctrl+Z", KeyBinding_StopInText },
    { "edit.redo", uint32_t(AppMenuCommand::Redo), "Edit", "Redo",
        "Ctrl+Shift+Z, Ctrl+Y", KeyBinding_StopInText },
    { "edit.duplicate", uint32_t(AppMenuCommand::DuplicateSelection), "Edit", "Duplicate",
        "Ctrl+D", KeyBinding_StopInText },
    { "edit.delete", uint32_t(AppMenuCommand::DeleteSelection), "Edit", "Delete",
        "Delete, Backspace", KeyBinding_IgnoreMods },
    { "code.comment", uint32_t(AppMenuCommand::ToggleCodeComment), "Edit", "Toggle Comment",
        "Ctrl+/, Ctrl+KeypadDivide", KeyBinding_None },
    { "code.blockComment", uint32_t(AppMenuCommand::ToggleCodeBlockComment), "Edit",
        "Toggle Block Comment", "Ctrl+Shift+A", KeyBinding_None },
    { "code.format", uint32_t(AppMenuCommand::FormatCodeDocument), "Edit", "Format Document",
        "Ctrl+Shift+F", KeyBinding_None },

    { "scene.run", uint32_t(AppMenuCommand::RunScene), "Scene", "Run or Resume Scene",
        "F5", KeyBinding_None },
    { "scene.pause", uint32_t(AppMenuCommand::PauseScene), "Scene", "Pause",
        "F6", KeyBinding_None },
    { "scene.stop", uint32_t(AppMenuCommand::StopScene), "Scene", "Stop",
        "F7", KeyBinding_None },
    { "scene.detachCamera", uint32_t(AppMenuCommand::DetachCamera), "Scene", "Detach Camera",
        "F8", KeyBinding_NoRepeat },

    { "view.commandPalette", uint32_t(AppMenuCommand::CommandPalette), "View",
        "Command Palette...", "Ctrl+K", KeyBinding_None },
    { "view.terminal", uint32_t(AppMenuCommand::ToggleTerminal), "View", "Toggle Terminal",
        "Ctrl+`", KeyBinding_StopInText },
    { "inspector.rename", 0u, "View", "Rename in Inspector",
        "F2", KeyBinding_StopInText | KeyBinding_NoRepeat },

    { "help.documentation", uint32_t(AppMenuCommand::Documentation), "Help", "Documentation",
        "F1", KeyBinding_None },

    // Ship unbound: these are conveniences, and a default chord for a settings tab would
    // cost a key the user may want for a command that matters.
    { "settings.shortcuts", uint32_t(AppMenuCommand::SettingsShortcutsTab), "Editor Settings",
        "Keyboard Shortcuts Tab", "", KeyBinding_None },
    { "settings.graphics", uint32_t(AppMenuCommand::SettingsGraphicsTab), "Editor Settings",
        "Graphics Tab", "", KeyBinding_None },
    { "settings.api", uint32_t(AppMenuCommand::SettingsEngineApiTab), "Editor Settings",
        "Engine API Tab", "", KeyBinding_None },
};

// Glyphs for the keys whose ImGui name is a mouthful. Parsing also accepts the
// ImGui name, so a chord copied out of io.KeysData still reads back.
struct KeyGlyph {
    ImGuiKey key;
    const char* text;
};

const KeyGlyph kGlyphs[] = {
    { ImGuiKey_GraveAccent, "`" },
    { ImGuiKey_Minus, "-" },
    { ImGuiKey_Equal, "=" },
    { ImGuiKey_LeftBracket, "[" },
    { ImGuiKey_RightBracket, "]" },
    { ImGuiKey_Backslash, "\\" },
    { ImGuiKey_Semicolon, ";" },
    { ImGuiKey_Apostrophe, "'" },
    { ImGuiKey_Comma, "," },
    { ImGuiKey_Period, "." },
    { ImGuiKey_Slash, "/" },
    { ImGuiKey_KeypadDivide, "Numpad /" },
    { ImGuiKey_Space, "Space" },
    { ImGuiKey_Escape, "Esc" },
    { ImGuiKey_Backspace, "Backspace" },
    { ImGuiKey_Enter, "Enter" },
    { ImGuiKey_Insert, "Ins" },
    { ImGuiKey_Delete, "Del" },
    { ImGuiKey_PageUp, "PageUp" },
    { ImGuiKey_PageDown, "PageDown" },
    { ImGuiKey_Home, "Home" },
    { ImGuiKey_End, "End" },
    { ImGuiKey_LeftArrow, "Left" },
    { ImGuiKey_RightArrow, "Right" },
    { ImGuiKey_UpArrow, "Up" },
    { ImGuiKey_DownArrow, "Down" },
};

// Words accepted for the modifiers, because "Cmd" and "Win" are what the key on the
// user's keyboard actually says.
struct ModAlias {
    const char* name;
    ImGuiKeyChord mod;
};

const ModAlias kModAliases[] = {
    { "ctrl", ImGuiMod_Ctrl },
    { "control", ImGuiMod_Ctrl },
    { "shift", ImGuiMod_Shift },
    { "alt", ImGuiMod_Alt },
    { "option", ImGuiMod_Alt },
    { "super", ImGuiMod_Super },
    { "cmd", ImGuiMod_Super },
    { "command", ImGuiMod_Super },
    { "win", ImGuiMod_Super },
    { "meta", ImGuiMod_Super },
};

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(unsigned(a[i])) != std::tolower(unsigned(b[i]))) return false;
    }
    return true;
}

std::string trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(unsigned(value[begin]))) ++begin;
    while (end > begin && std::isspace(unsigned(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::vector<std::string> splitList(const std::string& value, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t next = value.find(separator, start);
        parts.push_back(trim(value.substr(start, next == std::string::npos
                                                ? std::string::npos : next - start)));
        if (next == std::string::npos) break;
        start = next + 1;
    }
    return parts;
}

ImGuiKey keyFromName(const std::string& token) {
    for (const KeyGlyph& glyph : kGlyphs) {
        if (iequals(token, glyph.text)) return glyph.key;
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const ImGuiKey key = static_cast<ImGuiKey>(k);
        const char* name = ImGui::GetKeyName(key);
        if (name && iequals(token, name)) return key;
    }
    return ImGuiKey_None;
}

const char* keyGlyph(ImGuiKey key) {
    for (const KeyGlyph& glyph : kGlyphs) {
        if (glyph.key == key) return glyph.text;
    }
    return nullptr;
}

std::map<std::string, std::vector<ImGuiKeyChord>> gOverrides;
std::map<std::string, std::vector<ImGuiKeyChord>> gDefaultCache;
bool gLoaded = false;

std::vector<ImGuiKeyChord> parseList(const std::string& value) {
    std::vector<ImGuiKeyChord> chords;
    for (const std::string& entry : splitList(value, ',')) {
        ImGuiKeyChord chord = 0;
        if (KeyBindings::parseChord(entry, chord)) chords.push_back(chord);
    }
    return chords;
}

std::string joinChords(const std::vector<ImGuiKeyChord>& chords) {
    std::string text;
    for (ImGuiKeyChord chord : chords) {
        if (!text.empty()) text += ", ";
        text += KeyBindings::chordText(chord);
    }
    return text;
}

} // namespace

const std::vector<KeyBinding>& KeyBindings::all() {
    static const std::vector<KeyBinding> bindings(kBindings, kBindings + IM_COUNTOF(kBindings));
    return bindings;
}

const KeyBinding* KeyBindings::find(const std::string& id) {
    for (const KeyBinding& binding : all()) {
        if (id == binding.id) return &binding;
    }
    return nullptr;
}

const KeyBinding* KeyBindings::findByCommand(uint32_t command) {
    if (command == 0u) return nullptr;
    for (const KeyBinding& binding : all()) {
        if (binding.command == command) return &binding;
    }
    return nullptr;
}

const char* KeyBindings::defaultChords(const std::string& id) {
    const KeyBinding* binding = find(id);
    return binding ? binding->chords : "";
}

bool KeyBindings::parseChord(const std::string& token, ImGuiKeyChord& out) {
    ImGuiKey key = ImGuiKey_None;
    ImGuiKeyChord mods = 0;
    for (const std::string& part : splitList(token, '+')) {
        if (part.empty()) return false;
        bool matchedMod = false;
        for (const ModAlias& alias : kModAliases) {
            if (iequals(part, alias.name)) {
                mods |= alias.mod;
                matchedMod = true;
                break;
            }
        }
        if (matchedMod) continue;
        if (key != ImGuiKey_None) return false;  // two keys in one chord
        key = keyFromName(part);
        if (key == ImGuiKey_None || key < ImGuiKey_NamedKey_BEGIN) return false;
    }
    // A bare modifier is a key the OS already owns; accepting it would silently
    // swallow every plain Ctrl press.
    if (key == ImGuiKey_None) return false;
    out = static_cast<ImGuiKeyChord>(key) | mods;
    return true;
}

std::string KeyBindings::chordText(ImGuiKeyChord chord) {
    const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    if (key == ImGuiKey_None) return {};

    const char* glyph = keyGlyph(key);
    const char* name = glyph ? glyph : ImGui::GetKeyName(key);
    std::string text = name ? name : "?";
    if (chord & ImGuiMod_Ctrl) text = "Ctrl+" + text;
    if (chord & ImGuiMod_Shift) text = "Shift+" + text;
    if (chord & ImGuiMod_Alt) text = "Alt+" + text;
    if (chord & ImGuiMod_Super) text = "Super+" + text;
    return text;
}

const std::vector<ImGuiKeyChord>& KeyBindings::chordsFor(const KeyBinding& binding) {
    if (!gLoaded) reload();
    const auto overridden = gOverrides.find(binding.id);
    if (overridden != gOverrides.end()) return overridden->second;

    const auto cached = gDefaultCache.find(binding.id);
    if (cached != gDefaultCache.end()) return cached->second;
    return gDefaultCache.emplace(binding.id, parseList(binding.chords)).first->second;
}

std::vector<ImGuiKeyChord> KeyBindings::effective(const std::string& id) {
    const KeyBinding* binding = find(id);
    return binding ? chordsFor(*binding) : std::vector<ImGuiKeyChord>();
}

std::string KeyBindings::text(const std::string& id) {
    const KeyBinding* binding = find(id);
    return binding ? joinChords(chordsFor(*binding)) : std::string();
}

std::string KeyBindings::primaryText(const std::string& id) {
    const KeyBinding* binding = find(id);
    if (!binding) return {};
    const std::vector<ImGuiKeyChord>& chords = chordsFor(*binding);
    return chords.empty() ? std::string() : chordText(chords.front());
}

std::string KeyBindings::textForCommand(uint32_t command) {
    const KeyBinding* binding = findByCommand(command);
    return binding ? primaryText(binding->id) : std::string();
}

bool KeyBindings::isOverridden(const std::string& id) {
    if (!gLoaded) reload();
    return gOverrides.count(id) != 0u;
}

bool KeyBindings::bind(const std::string& id, const std::string& chordText, std::string& error) {
    const KeyBinding* binding = find(id);
    if (!binding) {
        error = "unknown shortcut '" + id + "'";
        return false;
    }
    const std::string wanted = trim(chordText);
    if (wanted.empty()) {
        AppSettings::setShortcutOverride(id, "");
        reload();
        return true;  // unbound, which is a legitimate choice
    }

    std::vector<ImGuiKeyChord> chords;
    for (const std::string& entry : splitList(wanted, ',')) {
        ImGuiKeyChord chord = 0;
        if (entry.empty() || !parseChord(entry, chord)) {
            error = entry.empty() ? "empty chord" : "unreadable key '" + entry + "'";
            return false;
        }
        chords.push_back(chord);
    }
    // Store the canonical spelling, so settings.yaml diffed across machines matches.
    AppSettings::setShortcutOverride(id, joinChords(chords));
    reload();
    return true;
}

bool KeyBindings::reset(const std::string& id) {
    if (!find(id)) return false;
    AppSettings::removeShortcutOverride(id);
    reload();
    return true;
}

void KeyBindings::resetAll() {
    AppSettings::clearShortcutOverrides();
    reload();
}

void KeyBindings::reload() {
    gOverrides.clear();
    gDefaultCache.clear();
    gLoaded = true;
    for (const auto& overrideEntry : AppSettings::getShortcutOverrides()) {
        if (!find(overrideEntry.first)) continue;  // a binding this editor no longer has
        gOverrides[overrideEntry.first] = parseList(overrideEntry.second);
    }
}

bool KeyBindings::pressed(const std::string& id) {
    const KeyBinding* binding = find(id);
    if (!binding) return false;
    const ImGuiIO& io = ImGui::GetIO();
    if ((binding->flags & KeyBinding_StopInText) && io.WantTextInput) return false;

    const bool repeat = (binding->flags & KeyBinding_NoRepeat) == 0u;
    for (ImGuiKeyChord chord : chordsFor(*binding)) {
        const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
        if (binding->flags & KeyBinding_IgnoreMods) {
            if (ImGui::IsKeyPressed(key, repeat)) return true;
            continue;
        }
        // Exact modifier match: Ctrl+Shift+S is Save All, and Ctrl+S stays Save.
        if (io.KeyMods != (chord & ImGuiMod_Mask_)) continue;
        if (ImGui::IsKeyPressed(key, repeat)) return true;
    }
    return false;
}

bool KeyBindings::pressedCommand(uint32_t command) {
    const KeyBinding* binding = findByCommand(command);
    return binding ? pressed(binding->id) : false;
}

std::vector<std::pair<std::string, std::string>> KeyBindings::conflicts() {
    std::vector<std::pair<std::string, std::string>> found;
    const std::vector<KeyBinding>& bindings = all();
    for (size_t a = 0; a < bindings.size(); ++a) {
        for (size_t b = a + 1; b < bindings.size(); ++b) {
            const std::vector<ImGuiKeyChord>& first = chordsFor(bindings[a]);
            const std::vector<ImGuiKeyChord>& second = chordsFor(bindings[b]);
            bool clash = false;
            for (ImGuiKeyChord chord : first) {
                for (ImGuiKeyChord other : second) {
                    if (chord == other) clash = true;
                }
                // A binding that ignores modifiers answers its key with any mods held,
                // so it also steals the plain-key chord of a modifier-less binding.
                if ((bindings[a].flags & KeyBinding_IgnoreMods) ||
                    (bindings[b].flags & KeyBinding_IgnoreMods)) {
                    const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
                    for (ImGuiKeyChord other : second) {
                        if (static_cast<ImGuiKey>(other & ~ImGuiMod_Mask_) == key) clash = true;
                    }
                }
            }
            if (clash) found.emplace_back(bindings[a].id, bindings[b].id);
        }
    }
    return found;
}

} // namespace quantum::editor
