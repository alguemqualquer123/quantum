// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

namespace quantum::editor::cli {

    // The command-line port of the plugin system: list what is installed, describe one, run its
    // commands, enable or disable it, install a folder.
    //
    // It is the only way to drive a plugin without the editor window, which makes it the check a
    // packaged plugin is verified with: a manifest that parses, a binary that maps and a command
    // that answers are three different failures and this prints which one happened.
    //
    // `argc`/`argv` come straight from main with the executable name dropped, so argv[0] is the
    // `plugins` token; `executableName` is only help text.
    int runPluginsCommand(int argc, char** argv, const char* executableName);

}
