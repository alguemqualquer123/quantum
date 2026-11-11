// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>

namespace quantum::editor::ProjectFile {

    // A project is still a folder holding project.yaml. The marker is one small file
    // inside it whose only job is to give the operating system something to bind an
    // icon and "open with" to; nothing reads settings out of it, so project.yaml
    // stays the single source of truth and the marker can be deleted at any time.

    // ".dproj" is also the Delphi project extension, so the real value comes from
    // AppSettings and this is only the default.
    inline constexpr const char* DEFAULT_EXTENSION = ".dproj";

    // ".dproj", ".DProj" and "dproj" all normalize to the stored form. Anything
    // that could not become a file name falls back to DEFAULT_EXTENSION.
    std::string normalizeExtension(const std::string& extension);

    // The marker of a project: an existing file under the wanted extension is reused,
    // so renaming a project does not leave a stale second marker behind. With none,
    // the name is "<project name><extension>", sanitized. An empty path means the
    // folder is not a project or its name cannot become a file name.
    std::filesystem::path markerPath(const std::filesystem::path& projectDir,
                                     const std::string& projectName,
                                     const std::string& extension);

    // Creates the marker when the project does not have one yet. An existing marker is
    // never rewritten, so hand edits and comments survive reopening the project, and a
    // foreign file that already owns the name is reported instead of clobbered.
    bool ensureMarker(const std::filesystem::path& projectDir,
                     const std::string& projectName,
                     const std::string& extension,
                     std::string& error);

    // Turns anything a launcher or the desktop shell hands over - the marker itself,
    // project.yaml, or the project directory - into the directory holding
    // project.yaml, or an empty path when it is not a project. Deliberately
    // extension-agnostic: a build configured with another extension must still open
    // markers written by the old one.
    std::filesystem::path resolve(const std::filesystem::path& path);

    // True when the directory holds a project.
    bool isProjectDir(const std::filesystem::path& dir);
}
