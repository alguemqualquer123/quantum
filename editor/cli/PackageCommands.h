// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace quantum::editor::cli {

    // The marketplace from the command line: registries, catalog, search, install, uninstall,
    // update, and building or verifying a .dplugin package.
    //
    // This is the same code path the Packages window drives, so a package that installs here
    // installs from the editor, and a registry mistake is reproducible without a GUI. Nothing here
    // opens a window, and no plugin binary is mapped unless the verb asks the manager to load one.
    //
    // `argc`/`argv` come from main with the executable name dropped, so argv[0] is the `packages`
    // token; `executableName` is only help text.
    int runPackagesCommand(int argc, char** argv, const char* executableName);

}
