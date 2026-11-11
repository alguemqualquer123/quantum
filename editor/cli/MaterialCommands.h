// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace quantum::editor::cli {

    namespace fs = std::filesystem;

    // `material status [--project <dir>]` and `material install --project <dir>`:
    // what the engine ships as the default primitive material, and which copy of it a
    // project owns. Running it is also the headless proof of the asset-root-relative
    // paths a scene gets written with.
    int runMaterialCommand(int argc, char** argv, const char* executableName);

}
