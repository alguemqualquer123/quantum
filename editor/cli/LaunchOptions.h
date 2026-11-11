// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include "util/ProjectFile.h"

#include <cstring>
#include <filesystem>
#include <string>

namespace quantum::editor {

    // Launch options parsed from the command line before the backend starts.
    // They serve two flows: the Hub (`--hub`, project/install management with no
    // project loaded) and launchers that open or create a specific project
    // (`--project <dir> [--create] [--template <name>]`). With no options the
    // editor keeps its regular startup behavior.
    struct LaunchOptions {
        bool hub = false;
        bool safeMode = false;
        bool resetLayout = false;
        std::filesystem::path projectPath;
        bool create = false;
        std::string templateName;

#ifdef QUANTUM_HUB_EXECUTABLE
        static constexpr bool isHubExecutable = true;
#else
        static constexpr bool isHubExecutable = false;
#endif

        // True when argv[1] is a recognized launcher option. main.cpp falls
        // through to the help command only for arguments that are not.
        static bool isLaunchCommand(int argc, char** argv) {
            if (isHubExecutable && argc < 2) {
                return true;
            }
            if (argc < 2 || argv[1] == nullptr) {
                return false;
            }
            if (std::strcmp(argv[1], "--hub") == 0 || std::strcmp(argv[1], "--project") == 0
                || std::strcmp(argv[1], "--safe-mode") == 0
                || std::strcmp(argv[1], "--reset-layout") == 0) {
                return true;
            }
            // The shell handler opens a project by naming its marker file, with no
            // flag in front of it, so that path has to count as a launch command.
            return !markerProject(argv[1]).empty();
        }

        static void init(int argc, char** argv) {
            LaunchOptions& options = storage();
            options = LaunchOptions();
            options.hub = isHubExecutable;
            for (int i = 1; i < argc; ++i) {
                if (argv[i] == nullptr) {
                    continue;
                }
                const char* argument = argv[i];
                if (std::strcmp(argument, "--hub") == 0) {
                    options.hub = true;
                } else if (std::strcmp(argument, "--safe-mode") == 0) {
                    options.safeMode = true;
                } else if (std::strcmp(argument, "--reset-layout") == 0) {
                    options.resetLayout = true;
                } else if (std::strcmp(argument, "--project") == 0 && i + 1 < argc) {
                    const std::filesystem::path given = argv[++i];
                    const std::filesystem::path markerProjectPath = ProjectFile::resolve(given);
                    // A marker opens the folder it lives in; anything else is kept
                    // verbatim because --create needs the path of a project that does
                    // not exist yet.
                    options.projectPath = markerProjectPath.empty() ? given : markerProjectPath;
                } else if (std::strcmp(argument, "--create") == 0) {
                    options.create = true;
                } else if (std::strcmp(argument, "--template") == 0 && i + 1 < argc) {
                    options.templateName = argv[++i];
                } else if (i == 1) {
                    const std::filesystem::path resolved = markerProject(argument);
                    if (!resolved.empty()) {
                        options.projectPath = resolved;
                    }
                }
            }
        }

        static const LaunchOptions& get() {
            return storage();
        }

    private:
        // The project a bare command-line argument points at: the marker file
        // double-clicked in Explorer, its project.yaml, or the folder itself.
        static std::filesystem::path markerProject(const char* argument) {
            if (argument == nullptr || argument[0] == '\0' || argument[0] == '-') {
                return {};
            }
            return ProjectFile::resolve(std::filesystem::path(argument));
        }

        static LaunchOptions& storage() {
            static LaunchOptions options;
            return options;
        }
    };

}
