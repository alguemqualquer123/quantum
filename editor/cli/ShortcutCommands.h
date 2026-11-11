// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace quantum::editor::cli {

    // Lists and checks the editor's configurable keyboard shortcuts. Read-only: it never
    // writes settings.yaml, so it can run beside an open editor session.
    //
    // `argc`/`argv` come straight from main with the executable name dropped, so argv[0] is
    // the `shortcuts` token and argv[1] the subcommand; `executableName` is only help text.
    int runShortcutsCommand(int argc, char** argv, const char* executableName);

    // The current overrides as a settings.yaml block, quoting each chord.
    std::string bindingOverridesYaml();

    // Binds every "id: chord" line of `text` through the same registry the Shortcuts tab
    // writes, so a saved binding list can be checked before it is pasted anywhere. Returns
    // how many landed; `problems` gets one line per line that did not. Nothing is written
    // to disk, which keeps this safe to run while the editor is open.
    int applyBindingLines(const std::string& text, std::vector<std::string>& problems);

}
