// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace quantum::editor::cli {

    // Runs the `worldcreator` subcommand. FASE 3 put two editor windows on top of the generator and
    // the plugin schema, and both are pure layout plus the engine's own math, so this verb drives
    // them with a real ImGui context that has no renderer: the panel contract, the heightmap write
    // and the two windows' frames are checked without launching the editor.
    int runWorldCreatorCommand(int argc, char** argv, const char* executableName);
}
