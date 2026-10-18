// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include <cstring>
#include <filesystem>
#include <string>

namespace doriax::editor {

    // Launch options parsed from the command line before the backend starts.
    // They serve two flows: the Hub (`--hub`, project/install management with no
    // project loaded) and launchers that open or create a specific project
    // (`--project <dir> [--create] [--template <name>]`). With no options the
    // editor keeps its regular startup behavior.
    struct LaunchOptions {
        bool hub = false;
        std::filesystem::path projectPath;
        bool create = false;
        std::string templateName;

        // True when argv[1] is a recognized launcher option. main.cpp falls
        // through to the help command only for arguments that are not.
        static bool isLaunchCommand(int argc, char** argv) {
            if (argc < 2 || argv[1] == nullptr) {
                return false;
            }
            return std::strcmp(argv[1], "--hub") == 0 || std::strcmp(argv[1], "--project") == 0;
        }

        static void init(int argc, char** argv) {
            LaunchOptions& options = storage();
            options = LaunchOptions();
            for (int i = 1; i < argc; ++i) {
                if (argv[i] == nullptr) {
                    continue;
                }
                const char* argument = argv[i];
                if (std::strcmp(argument, "--hub") == 0) {
                    options.hub = true;
                } else if (std::strcmp(argument, "--project") == 0 && i + 1 < argc) {
                    options.projectPath = argv[++i];
                } else if (std::strcmp(argument, "--create") == 0) {
                    options.create = true;
                } else if (std::strcmp(argument, "--template") == 0 && i + 1 < argc) {
                    options.templateName = argv[++i];
                }
            }
        }

        static const LaunchOptions& get() {
            return storage();
        }

    private:
        static LaunchOptions& storage() {
            static LaunchOptions options;
            return options;
        }
    };

}
