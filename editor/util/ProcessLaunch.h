// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace quantum::editor {

    // Small process helpers the Hub uses to start editors and to reveal folders.
    class ProcessLaunch {
    public:
        // Fire-and-forget: starts the executable in workingDir with the given
        // arguments, without waiting for it and without inheriting this
        // process's handles. Returns false when the process could not start.
        static bool detached(const std::filesystem::path& executable,
                             const std::vector<std::string>& arguments,
                             const std::filesystem::path& workingDir);

        // Same as detached(), but keeps a GUI child visible on Windows. This is
        // used for game runtimes launched by the editor; shell helpers keep the
        // original hidden-console behaviour above.
        static bool detachedVisible(const std::filesystem::path& executable,
                                    const std::vector<std::string>& arguments,
                                    const std::filesystem::path& workingDir);

        // Opens the folder in the OS file manager (Explorer / Finder / xdg-open).
        static bool revealFolder(const std::filesystem::path& folder);
    };

}
