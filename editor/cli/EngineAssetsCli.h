// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

namespace quantum::editor::cli {

    // Lists, searches and delivers the content the engine ships next to the editor - the
    // same roots the editor's Assets Browser shows, through the same resolver. Read-only
    // against the engine: it writes only into the project `copy` names.
    //
    // `argc`/`argv` come straight from main with the executable name dropped, so argv[0] is
    // the `assets` token and argv[1] the subcommand; `executableName` is only help text.
    int runAssetsCommand(int argc, char** argv, const char* executableName);

}
