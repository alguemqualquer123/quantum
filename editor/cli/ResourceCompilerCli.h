// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace quantum::editor::cli {

    namespace fs = std::filesystem;

    // Names the engine's ResourcePack reader and the exporter both rely on.
    inline constexpr const char* RESOURCE_PACK_FILENAME = "resources.pak";
    inline constexpr const char* RESOURCE_PACK_TEMP_FILENAME = "resources.pak.tmp";

    // Reports a named step and a 0..1 fraction; may be absent.
    using PakProgressFn = std::function<void(const std::string& step, float value)>;

    // Polled between entries; when it returns true the pack is abandoned and
    // the half-written temporary is cleaned up. May be absent (never cancelled).
    using PakCancelFn = std::function<bool()>;

    struct PakOptions {
        // Directory holding the assets/ and lua/ roots to pack (the export root
        // for the editor, the project directory for the CLI).
        fs::path projectDir;
        // Where resources.pak is written.
        fs::path outputPack;
        PakProgressFn onProgress;
        PakCancelFn isCancelled;
    };

    // Packs projectDir/assets and projectDir/lua into the DXPK1 resource pack.
    // `created` reports whether a pack file was written (no entries removes any
    // stale pack instead). On failure `error` carries the message.
    bool buildResourcePack(const PakOptions& options, bool& created, std::string& error);

    // Clears a pack and the temporary an interrupted write may have left beside it.
    bool removeResourcePack(const fs::path& packPath, std::string& error);

    // Gathers a packed tree's contents before anything is removed, so the walk never
    // mutates the directory it is reading. The directory itself is left in place.
    bool collectPackedTreeEntries(const fs::path& dir, std::vector<fs::path>& out,
                                  std::string& error, const fs::path& keep = fs::path());

    // The engine loads a "resources.pak" at the asset root, so the name is reserved.
    bool isReservedPackName(std::string name);

    // quantum-resource-compiler subcommands. None of them open a window.

    // `pak --project <dir|project.yaml> [--out <file.pak>] [--verbose]`
    int runPakCommand(int argc, char** argv, const char* executableName);

    // `shaders --project <dir|project.yaml> [--force] [--verbose]`
    // Falls back to the legacy `shaders --out <dir> --shader <spec>` path when
    // no --project is given.
    int runShadersCommand(int argc, char** argv, const char* executableName);

    // `inspect --pack <file.pak> [--verbose]`
    int runInspectCommand(int argc, char** argv, const char* executableName);

    // `graph --check <file.graph> | --dump | --compile [--out <file.gdat>] | --catalog | --validate-runtime`
    int runGraphCommand(int argc, char** argv, const char* executableName);

    // Top level usage for the resource compiler. With argc < 1 (no arguments at
    // all) it still only prints usage: this executable never starts the backend.
    int runResourceCompilerHelpCommand(int argc, char** argv, const char* executableName);

}
