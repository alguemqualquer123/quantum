// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace doriax::editor {

    // Small process helpers the Hub uses to start editors and to reveal folders.
    class ProcessLaunch {
    public:
        // Fire-and-forget: starts the executable in workingDir with the given
        // arguments, without waiting for it and without inheriting this
        // process's handles. Returns false when the process could not start.
        static bool detached(const std::filesystem::path& executable,
                             const std::vector<std::string>& arguments,
                             const std::filesystem::path& workingDir);

        // Opens the folder in the OS file manager (Explorer / Finder / xdg-open).
        static bool revealFolder(const std::filesystem::path& folder);
    };

}
