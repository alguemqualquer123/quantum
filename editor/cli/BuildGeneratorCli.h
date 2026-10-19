// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace doriax::editor::cli {

    namespace fs = std::filesystem;

    // doriax-build-generator subcommands. None of them open a window or touch
    // the graphics backend; they drive the same Generator / Exporter / CommandRunner
    // code the editor's Play and Export paths use, headlessly, for CI and automation.

    // Machine-parseable progress sink: fraction is 0..1, stage and detail are
    // free-form (e.g. "compile", "configure", "link"). May be absent.
    using BuildProgressFn = std::function<void(const std::string& stage, float fraction,
                                               const std::string& detail)>;

    // Reports one filesystem write during `generate` (path + whether it was
    // newly created or changed). May be absent.
    using GenerateFileFn = std::function<void(const fs::path& file, bool created)>;

    // `generate --project <dir|project.yaml> [--out <dir>] [--kit ...] [--verbose]`
    int runGenerateCommand(int argc, char** argv, const char* executableName);

    // `configure --project <dir> [--build-dir <dir>] [--config <type>] [--kit ...] [--verbose]`
    int runConfigureCommand(int argc, char** argv, const char* executableName);

    // `build --project <dir> [--build-dir <dir>] [--config Debug|Release] [--target X] [--jobs N] [--verbose]`
    int runBuildCommand(int argc, char** argv, const char* executableName);

    // `status --project <dir> [--build-dir <dir>] [--verbose]`
    // Exit 0 when the generated sources are up to date, non-zero when stale.
    int runStatusCommand(int argc, char** argv, const char* executableName);

    // Top level usage for the build generator. With no arguments it still only
    // prints usage: this executable never starts the backend.
    int runBuildGeneratorHelpCommand(int argc, char** argv, const char* executableName);

}
