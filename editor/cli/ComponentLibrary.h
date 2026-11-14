// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace quantum::editor::cli {

    namespace fs = std::filesystem;

    // The ready-made Lua components the engine ships in <engineDir>/components. One
    // component is one script file, and its Inspector rows come from the `properties`
    // table the file declares, so delivering one to a project is a file copy: no
    // component type, no serializer key, no registry entry.

    struct ReadyMadeComponent {
        std::string name;    // file stem, which is also the Lua module name
        fs::path source;
        std::string summary; // first comment line below the licence header
        std::string category; // "-- category:" header line, empty when the file has none
    };

    // <engineDir>/components, or the same folder in the source tree when the build has
    // not copied the engine next to the executable. Empty when neither exists.
    fs::path findComponentsDir();

    std::vector<ReadyMadeComponent> listReadyMadeComponents(const fs::path& componentsDir);

    struct ReadyMadeInstallResult {
        fs::path destination;
        int copied = 0;
        int skipped = 0;
        int failed = 0;
        // Counted separately because both callers print how many *components* arrived.
        int demos = 0;
        int assets = 0;
    };

    // Installs `names` (empty means every component in the library) as
    // <luaRoot>/<subdir>/<name>.lua. A file already in the project is left alone unless
    // `overwrite`: once installed, that copy belongs to the project. `withDemos` adds the
    // example scenes from <componentsDir>/demos and the kit's sprites and thunder sample from
    // <componentsDir>/assets, but only for a whole-library install - a --only install cannot run
    // a demo that needs components the project does not have. Assets land in <projectRoot>/assets
    // because that is where the runtime resolves a bare "textures/rain.png" from, whatever
    // project.yaml says about luaDir.
    ReadyMadeInstallResult installReadyMadeComponents(const fs::path& componentsDir,
                                                      const fs::path& luaRoot,
                                                      const fs::path& projectRoot,
                                                      const std::string& subdir,
                                                      const std::vector<std::string>& names,
                                                      bool overwrite,
                                                      const std::function<void(const std::string&)>& log,
                                                      bool withDemos = true);

    // `components list [--project <dir>]` and
    // `components install --project <dir> [--lua <dir>] [--into <subdir>] [--only A,B] [--overwrite]
    //  [--no-demos]`
    int runComponentsCommand(int argc, char** argv, const char* executableName);

}
