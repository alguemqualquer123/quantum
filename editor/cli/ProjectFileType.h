// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>

namespace quantum::editor::cli::ProjectFileType {

    // Double-clicking a project's marker file has to open it in the editor, which
    // needs two things the operating system owns: the file extension and the program
    // bound to it. The Project menu and the "project-file" subcommand both come
    // through here, so the two entry points cannot drift apart.

    // Every call below takes the extension to act on, or an empty string to mean the
    // one configured in Editor Settings.

    // The extension a request acts on, normalized to its stored form.
    std::string resolveExtension(const std::string& extension = {});

    // What a marker launches when double-clicked. Always the editor: a suite tool
    // asked to register must not make itself the program that opens projects.
    std::filesystem::path editorExecutablePath();

    // One sentence saying what the extension currently does, for the output panel and
    // for "project-file status".
    std::string describe(const std::string& extension = {});

    bool isRegistered(const std::string& extension = {});

    // Install or remove this build's association. Both leave the reason in error when
    // they return false.
    bool registerFileType(const std::string& extension, std::string& error);
    bool unregisterFileType(const std::string& extension, std::string& error);
}
