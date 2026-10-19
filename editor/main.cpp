// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Backend.h"
#include "cli/CommandLine.h"
#include "cli/LaunchOptions.h"
#include "cli/BuildGeneratorCli.h"
#include "cli/ResourceCompilerCli.h"
#include "util/ShellEnv.h"

#include <cstring>

using namespace doriax;

int main(int argc, char* argv[]) {
    // A desktop launcher starts the editor without the user's shell environment,
    // so the build tools go back on PATH before anything can spawn a command.
    editor::ShellEnv::bootstrapPath();

    // CLI subcommands run before the backend starts, so they never create a window.
    if (argc >= 2 && std::strcmp(argv[1], "export") == 0) {
        return editor::CommandLine::runExportCommand(argc - 1, argv + 1, argv[0]);
    }
#ifdef DORIAX_CLI_FRONTEND
    // doriax-resource-compiler: headless asset tool. Only the recognized
    // subcommands run, and the backend never starts, so no window opens even
    // when invoked without arguments or with an unknown one.
    if (argc >= 2 && std::strcmp(argv[1], "shaders") == 0) {
        return editor::cli::runShadersCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "pak") == 0) {
        return editor::cli::runPakCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "inspect") == 0) {
        return editor::cli::runInspectCommand(argc - 1, argv + 1, argv[0]);
    }
    // doriax-build-generator: headless project generation and build orchestration.
    if (argc >= 2 && std::strcmp(argv[1], "generate") == 0) {
        return editor::cli::runGenerateCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "configure") == 0) {
        return editor::cli::runConfigureCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "build") == 0) {
        return editor::cli::runBuildCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "status") == 0) {
        return editor::cli::runStatusCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argv[0] && std::strstr(argv[0], "build-generator")) {
        return editor::cli::runBuildGeneratorHelpCommand(argc - 1, argv + 1, argv[0]);
    }
    return editor::cli::runResourceCompilerHelpCommand(argc - 1, argv + 1, argv[0]);
#else
    if (argc >= 2 && std::strcmp(argv[1], "shaders") == 0) {
        return editor::CommandLine::runShadersCommand(argc - 1, argv + 1, argv[0]);
    }
    // Rendering benchmark mode still needs a window, so it only starts the
    // backend after printing the usage when asked for help.
    if (argc >= 2 && std::strcmp(argv[1], "benchmark") == 0) {
        if (argc >= 3 && (std::strcmp(argv[2], "-h") == 0 || std::strcmp(argv[2], "--help") == 0 || std::strcmp(argv[2], "help") == 0)) {
            return editor::CommandLine::runBenchmarkHelp(argc - 1, argv + 1, argv[0]);
        }
        return editor::Backend::init(argc, argv);
    }
    // Launcher options start the editor itself: the Hub (`--hub`) and project
    // shortcuts (`--project <dir> [--create] [--template <name>]`). Anything
    // else keeps the help behavior below.
    if (editor::LaunchOptions::isLaunchCommand(argc, argv)) {
        editor::LaunchOptions::init(argc, argv);
        return editor::Backend::init(argc, argv);
    }
    if (argc >= 2) {
        return editor::CommandLine::runHelpCommand(argc - 1, argv + 1, argv[0]);
    }

    return editor::Backend::init(argc, argv);
#endif
}
