// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>

namespace quantum::editor::ShellAssociation {

    // Binds the project marker file to this editor build, so double-clicking it in
    // Explorer opens the project. Everything is written under
    // HKCU\Software\Classes: per-user, no administrator rights, and it never
    // touches the machine-wide HKLM view.

    struct Target {
        std::filesystem::path executable;  // what double-clicking the marker runs
        std::filesystem::path icon;        // empty = the executable's own icon
        std::string extension;             // ".dproj"
    };

    // Registration only exists on Windows; the other platforms get honest stubs so
    // the menu and the CLI subcommand can ask instead of guessing.
    bool isSupported();

    // What the extension is bound to right now: the ProgID already stored, or an
    // empty string when nothing owns it. Used to refuse stealing an extension that
    // belongs to another program, and to report the status.
    std::string currentHandler(const std::string& extension);

    // True when the extension opens files with this editor's ProgID.
    bool isRegistered(const std::string& extension);

    bool registerTarget(const Target& target, std::string& error);

    // Removes only what registerTarget wrote: the extension key is dropped when it
    // still points at our ProgID, and the ProgID subtree is removed wholesale.
    bool unregisterTarget(const std::string& extension, std::string& error);
}
