// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_COMPONENT_H
#define SCRIPT_COMPONENT_H

#include "ScriptProperty.h"
#include <vector>

namespace quantum{

    enum class ScriptType {
        CPP,
        LUA,
        GRAPH,
    };

    struct QUANTUM_API ScriptEntry {
        ScriptType type = ScriptType::CPP;
        std::string path;        // .cpp or .lua (for Lua: script file path)
        std::string headerPath;  // for C++; empty for Lua
        std::string className;   // C++ class or Lua module name (file base name)
        bool enabled = false;
        std::vector<ScriptProperty> properties;

        void* instance = nullptr; // C++ instance OR Lua handle
    };

    struct QUANTUM_API ScriptComponent {
        std::vector<ScriptEntry> scripts;
    };

}

#endif //SCRIPT_COMPONENT_H
