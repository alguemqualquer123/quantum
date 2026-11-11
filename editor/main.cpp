// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "Backend.h"
#include "cli/CommandLine.h"
#include "cli/LaunchOptions.h"
#include "cli/BuildGeneratorCli.h"
#include "cli/ComponentLibrary.h"
#include "cli/EngineAssetsCli.h"
#include "cli/MaterialCommands.h"
#include "cli/PluginCommands.h"
#include "cli/PackageCommands.h"
#include "cli/ProjectDoctor.h"
#include "cli/ResourceCompilerCli.h"
#include "cli/ScriptCommands.h"
#include "cli/ShortcutCommands.h"
#include "cli/TerrainCommands.h"
#include "cli/WorldCreatorCommands.h"
#include "cli/ApiCommands.h"
#include "util/ShellEnv.h"

#include <cstring>

using namespace quantum;

int main(int argc, char* argv[]) {
    // A desktop launcher starts the editor without the user's shell environment,
    // so the build tools go back on PATH before anything can spawn a command.
    editor::ShellEnv::bootstrapPath();

    // CLI subcommands run before the backend starts, so they never create a window.
    if (argc >= 2 && std::strcmp(argv[1], "export") == 0) {
        return editor::CommandLine::runExportCommand(argc - 1, argv + 1, argv[0]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "script") == 0) {
        return editor::CommandLine::runScriptCommand(argc - 1, argv + 1, argv[0]);
    }
    // Every executable built from this main() can install the shell association,
    // because the handler it writes is always the editor GUI next to it.
    if (argc >= 2 && std::strcmp(argv[1], "project-file") == 0) {
        return editor::CommandLine::runProjectFileCommand(argc - 1, argv + 1, argv[0]);
    }
    // The component library sits in the engine folder, so listing and installing it
    // is plain file work and never needs a window or a project loaded.
    if (argc >= 2 && std::strcmp(argv[1], "components") == 0) {
        return editor::cli::runComponentsCommand(argc - 1, argv + 1, argv[0]);
    }
    // The rest of the engine folder is content too, and browsing it is the same plain
    // file work: only `assets copy` writes, and only into the project it names.
    if (argc >= 2 && std::strcmp(argv[1], "assets") == 0) {
        return editor::cli::runAssetsCommand(argc - 1, argv + 1, argv[0]);
    }
    // The default primitive material is a folder of maps, so reporting which project
    // owns a copy needs no window and no loaded project.
    if (argc >= 2 && std::strcmp(argv[1], "material") == 0) {
        return editor::cli::runMaterialCommand(argc - 1, argv + 1, argv[0]);
    }
    // The doctor reads project.yaml and the scene files straight off disk without
    // ever loading a Project or a Scene, so it also runs on projects that would
    // crash the editor - which is exactly when they need auditing.
    if (argc >= 2 && std::strcmp(argv[1], "doctor") == 0) {
        return editor::cli::runProjectDoctorCommand(argc - 1, argv + 1, argv[0]);
    }
    // Checking, outlining and searching scripts is text work the editor's own services
    // already do, so the CLI reaches them without loading a project or opening a window.
    if (argc >= 2 && std::strcmp(argv[1], "scripts") == 0) {
        return editor::cli::runScriptsCommand(argc - 1, argv + 1, argv[0]);
    }
    // The shortcut registry is a static table, so listing and checking it needs no
    // window and no project - and it never writes settings.yaml.
    if (argc >= 2 && std::strcmp(argv[1], "shortcuts") == 0) {
        return editor::cli::runShortcutsCommand(argc - 1, argv + 1, argv[0]);
    }
    // The engine API list is compiled in from the generated header, so browsing it needs
    // no window, no project and no writes.
    if (argc >= 2 && std::strcmp(argv[1], "api") == 0) {
        return editor::cli::runApiCommand(argc - 1, argv + 1, argv[0]);
    }
    // Plugins are binaries, so this one does map DLLs - but it maps them in a process that never
    // opens a window, which is how a packaged plugin gets checked without touching a scene.
    if (argc >= 2 && std::strcmp(argv[1], "plugins") == 0) {
        return editor::cli::runPluginsCommand(argc - 1, argv + 1, argv[0]);
    }
    // A package is a file on disk and a hash: building, verifying and installing one is file work,
    // so the marketplace runs headless and the same code path the Packages window uses gets checked.
    if (argc >= 2 && std::strcmp(argv[1], "packages") == 0) {
        return editor::cli::runPackagesCommand(argc - 1, argv + 1, argv[0]);
    }
    // A heightmap is math and a file: the generator runs in the engine, so writing one and reading
    // one back need neither a project nor a window.
    if (argc >= 2 && std::strcmp(argv[1], "terrain") == 0) {
        return editor::cli::runTerrainCommand(argc - 1, argv + 1, argv[0]);
    }
    // FASE 3 put the plugin panel and the World Creator behind ImGui. ImGui is compiled into this
    // executable and needs no renderer, so a context with no backend drives both headless - which is
    // how the schema contract is checked while the editor itself stays closed.
    if (argc >= 2 && std::strcmp(argv[1], "worldcreator") == 0) {
        return editor::cli::runWorldCreatorCommand(argc - 1, argv + 1, argv[0]);
    }
#ifdef QUANTUM_CLI_FRONTEND
    // quantum-resource-compiler: headless asset tool. Only the recognized
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
    if (argc >= 2 && std::strcmp(argv[1], "graph") == 0) {
        return editor::cli::runGraphCommand(argc - 1, argv + 1, argv[0]);
    }
    // quantum-build-generator: headless project generation and build orchestration.
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
