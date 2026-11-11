// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

namespace quantum::editor::cli {

    // `scripts check|outline|search|replace` for quantum-editor-cmd: the same text
    // services the Script Editor page runs, from a shell and without a window. CI can
    // gate on it (`check` exits 1 on a syntax error) and an agent can read the --json.
    int runScriptsCommand(int argc, char** argv, const char* executableName);

}
