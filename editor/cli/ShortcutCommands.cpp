// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "ShortcutCommands.h"

#include "AppSettings.h"
#include "KeyBindings.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace quantum::editor::cli {

namespace {

    void usage(const char* executableName) {
        std::cout
            << "usage: " << (executableName ? executableName : "quantum-editor-cmd")
            << " shortcuts <list | parse <chord> | verify | export | apply <file>> [--list-names]\n"
            << "\n"
            << "  list            every configurable shortcut, its chords and its defaults\n"
            << "  parse <chord>   show the canonical spelling of one chord (e.g. \"ctrl+shift+s\")\n"
            << "  verify          round-trip every default and report conflicts; exits 1 on trouble\n"
            << "  export          print the current overrides as a settings.yaml block\n"
            << "  apply <file>    bind every \"id: chord\" line of a file and report what failed\n"
            << "\n"
            << "Read-only: nothing here writes settings.yaml, so it runs beside an open editor.\n"
            << "apply binds in this process only, which makes it a check of a bindings file.\n"
            << "With --list-names it also prints the key names a chord may use.\n";
    }

    std::string flagText(const KeyBinding& binding) {
        std::string text;
        if (binding.flags & KeyBinding_StopInText) text += "stop-in-text ";
        if (binding.flags & KeyBinding_IgnoreMods) text += "any-mods ";
        if (binding.flags & KeyBinding_NoRepeat) text += "no-repeat ";
        return text.empty() ? "-" : text;
    }

    std::string trimmed(const std::string& value) {
        const size_t begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return std::string();
        return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
    }

    std::vector<std::string> splitList(const std::string& value) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= value.size()) {
            const size_t comma = value.find(',', start);
            parts.push_back(trimmed(value.substr(start, comma == std::string::npos
                                                      ? std::string::npos : comma - start)));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return parts;
    }

    // A default that does not survive parse -> format -> parse is a chord the editor would
    // never answer, and the Shortcuts tab would call it unreadable.
    void checkRoundTrip(const KeyBinding& binding, std::vector<std::string>& problems) {
        const std::string formatted = KeyBindings::text(binding.id);
        if (formatted.empty()) {
            if (binding.chords[0] != '\0') {
                problems.push_back(std::string(binding.id) + ": default '" + binding.chords +
                                   "' does not parse");
            }
            return;
        }
        for (const std::string& part : splitList(formatted)) {
            ImGuiKeyChord reparsed = 0;
            if (!KeyBindings::parseChord(part, reparsed)) {
                problems.push_back(std::string(binding.id) + ": '" + formatted +
                                   "' does not read back");
                return;
            }
            if (KeyBindings::chordText(reparsed) != part) {
                problems.push_back(std::string(binding.id) + ": '" + part + "' reformats as '" +
                                   KeyBindings::chordText(reparsed) + "'");
                return;
            }
        }
    }

    int listCommand(bool namesRequested) {
        AppSettings::initialize();
        std::cout << "settings file: " << AppSettings::getConfigDirectory().string() << "\n\n";

        const char* category = nullptr;
        for (const KeyBinding& binding : KeyBindings::all()) {
            if (!category || std::strcmp(category, binding.category) != 0) {
                category = binding.category;
                std::cout << "\n" << category << "\n";
            }
            const bool overridden = KeyBindings::isOverridden(binding.id);
            std::cout << "  " << binding.id;
            for (size_t pad = std::strlen(binding.id); pad < 22; ++pad) std::cout << ' ';
            std::cout << KeyBindings::text(binding.id);
            if (overridden) {
                std::cout << "   (default " << binding.chords << ")";
            }
            std::cout << "\n      " << binding.label << "  [" << flagText(binding) << "]\n";
        }

        const auto conflicts = KeyBindings::conflicts();
        std::cout << "\n" << KeyBindings::all().size() << " bindings, "
                  << conflicts.size() << " conflict(s)\n";
        for (const auto& conflict : conflicts) {
            std::cout << "  CONFLICT " << conflict.first << " and " << conflict.second << "\n";
        }

        if (namesRequested) {
            std::cout << "\nkey names accepted in a chord:\n  ";
            for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
                const char* name = ImGui::GetKeyName(static_cast<ImGuiKey>(key));
                if (!name) continue;
                std::cout << name << " ";
            }
            std::cout << "\n";
        }
        return 0;
    }

} // namespace

std::string bindingOverridesYaml() {
    std::string text = "  shortcuts:\n";
    for (const KeyBinding& binding : KeyBindings::all()) {
        if (!KeyBindings::isOverridden(binding.id)) continue;
        text += "    " + std::string(binding.id) + ": \"" + KeyBindings::text(binding.id) + "\"\n";
    }
    return text;
}

// Quotes are stripped because the block this reads is the one bindingOverridesYaml()
// prints, and YAML wants the chord quoted. A header line ("shortcuts:") names no command
// and carries no chord, so it is prose and is skipped instead of reported.
int applyBindingLines(const std::string& text, std::vector<std::string>& problems) {
    int applied = 0;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        const bool last = end == std::string::npos;
        if (last) end = text.size();
        std::string line = trimmed(text.substr(start, end - start));
        start = last ? text.size() : end + 1;
        if (line.empty() || line[0] == '#') continue;

        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            problems.push_back(line + ": no \"id: chord\" colon");
            continue;
        }
        const std::string id = trimmed(line.substr(0, colon));
        std::string chord = trimmed(line.substr(colon + 1));
        chord.erase(std::remove_if(chord.begin(), chord.end(),
                                   [](char c) { return c == '"' || c == '\''; }),
                    chord.end());

        if (!KeyBindings::find(id)) {
            if (chord.empty()) continue;
            problems.push_back(id + ": unknown command");
            continue;
        }
        std::string error;
        if (!KeyBindings::bind(id, chord, error)) {
            problems.push_back(id + ": " + error);
            continue;
        }
        ++applied;
    }
    return applied;
}

int runShortcutsCommand(int argc, char** argv, const char* executableName) {
    const bool wantsNames = argc >= 2 && std::strcmp(argv[argc - 1], "--list-names") == 0;
    if (argc < 2) {
        usage(executableName);
        return 1;
    }

    const std::string action = argv[1];
    if (action == "list") return listCommand(wantsNames);

    if (action == "export") {
        AppSettings::initialize();
        std::cout << bindingOverridesYaml();
        return 0;
    }

    if (action == "apply") {
        if (argc < 3) {
            std::cout << "shortcuts apply: a file of \"id: chord\" lines is required\n";
            return 1;
        }
        std::ifstream in(argv[2]);
        if (!in) {
            std::cout << "shortcuts apply: cannot read " << argv[2] << "\n";
            return 1;
        }
        std::string text;
        std::string line;
        while (std::getline(in, line)) {
            text += line;
            text += "\n";
        }

        // Bound in this process only: settings.yaml is left alone, so the same file the
        // editor would write can be checked here without racing an open session.
        AppSettings::initialize();
        std::vector<std::string> problems;
        const int applied = applyBindingLines(text, problems);
        for (const std::string& problem : problems) std::cout << "SKIP " << problem << "\n";
        std::cout << (problems.empty() ? "OK " : "FAILED ") << applied << " binding(s) applied, "
                  << problems.size() << " problem(s)\n";
        return problems.empty() ? 0 : 1;
    }

    if (action == "parse") {
        if (argc < 3) {
            std::cout << "shortcuts parse: a chord is required, e.g. \"ctrl+shift+s\"\n";
            return 1;
        }
        // Accept the chord as separate words too, so `shortcuts parse Ctrl Shift S` works
        // in a shell where quoting + is awkward.
        std::string chord;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--list-names") == 0) continue;
            if (!chord.empty()) chord += "+";
            chord += argv[i];
        }
        ImGuiKeyChord parsed = 0;
        if (!KeyBindings::parseChord(chord, parsed)) {
            std::cout << "cannot read '" << chord << "': use <Mod>+...+<Key> with a known key name\n";
            return 1;
        }
        std::cout << chord << "  ->  " << KeyBindings::chordText(parsed) << "\n";
        return 0;
    }

    if (action == "verify") {
        AppSettings::initialize();
        std::vector<std::string> problems;
        for (const KeyBinding& binding : KeyBindings::all()) {
            checkRoundTrip(binding, problems);
        }
        for (const auto& conflict : KeyBindings::conflicts()) {
            problems.push_back("conflict: " + conflict.first + " and " + conflict.second);
        }
        for (const std::string& problem : problems) std::cout << "FAIL " << problem << "\n";
        std::cout << (problems.empty() ? "OK " : "") << KeyBindings::all().size()
                  << " bindings checked, " << problems.size() << " problem(s)\n";
        return problems.empty() ? 0 : 1;
    }

    std::cout << "shortcuts: unknown action '" << action << "'\n";
    usage(executableName);
    return 1;
}

} // namespace quantum::editor::cli
