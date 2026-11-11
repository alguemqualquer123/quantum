// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace quantum::editor::cli {

    // Browses the engine API the editor has compiled in: the same rows the script editor
    // completes from and the Engine API settings tab lists. Read-only, and it needs no
    // window, so it is also the check that the generated metadata is present and shaped.
    //
    // `argc`/`argv` come straight from main with the executable name dropped, so argv[0] is
    // the `api` token and argv[1] the subcommand; `executableName` is only used for help.
    int runApiCommand(int argc, char** argv, const char* executableName);

}
