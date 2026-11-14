// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace quantum::editor::cli {

    // Runs the `terrain` subcommand: write a procedural heightmap and report what one contains.
    // Both verbs are engine-side math and file IO, so neither loads a project nor opens a window.
    int runTerrainCommand(int argc, char** argv, const char* executableName);
}
