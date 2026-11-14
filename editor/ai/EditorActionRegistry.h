// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AiTypes.h"

#include <string>
#include <vector>

namespace quantum::editor::ai {

struct ValidationResult {
    bool ok = false;
    std::string error;
};

class EditorActionRegistry {
public:
    static std::vector<ToolDefinition> tools();
    static bool hasTool(const std::string& name);
    static bool isReadOnly(const std::string& name);
    static ToolDefinition getTool(const std::string& name);
    static ValidationResult validate(const std::string& name, const Json& arguments);
    static std::string describe(const std::string& name, const Json& arguments);
    // Engine rules for any model using the tools, shared by the chat and MCP
    static std::string guidance();
};

} // namespace quantum::editor::ai
