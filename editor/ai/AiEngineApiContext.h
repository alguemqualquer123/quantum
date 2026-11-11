// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AiTypes.h"

#include <string>

namespace quantum::editor::ai {

class AiEngineApiContext {
public:
    static Json search(const std::string& query,
                       const std::string& parentFilter,
                       int maxResults);
};

} // namespace quantum::editor::ai
