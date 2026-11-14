// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include "component/ScriptComponent.h"

#include <filesystem>
#include <string>
#include <vector>

namespace quantum::editor {

    class Project;

    // Mirrors the exposed variables of a compiled graph into ScriptEntry properties,
    // so the Inspector can render the same rows it already renders for Lua/C++ scripts.
    namespace GraphScriptProperties {

        // Reads exposed variables from a .gdat blob. Values and defaults are initialized
        // from the graph asset defaults.
        bool loadExposed(const std::filesystem::path& gdatPath, std::vector<ScriptProperty>& out,
                         std::string& error);

        // Refreshes entry.properties from the compiled graph entry.path points to.
        // Existing values are preserved when the variable name and type still match.
        bool refresh(ScriptEntry& entry, const Project& project);

        // Refreshes every ScriptEntry whose graph resolves to gdatPath.
        // Returns the number of updated entries.
        int refreshAll(Project& project, const std::filesystem::path& gdatPath);

    }

}
