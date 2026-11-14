// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include "graph/Graph.h"

#include <string>

namespace quantum::editor {

    // Reads and writes the authoring format of a visual-script graph (.graph).
    // YAML is the source of truth; the runtime consumes only the compiled DGR1
    // blob produced from it. Unknown keys are ignored so newer editor builds can
    // add authoring-only fields without breaking older tooling.
    class GraphSerializerYaml {
    public:
        static bool loadFile(const std::string& path, graph::GraphAsset& out, std::string& error);
        static bool loadString(const std::string& text, graph::GraphAsset& out, std::string& error);

        static bool saveFile(const std::string& path, const graph::GraphAsset& asset, std::string& error);
        static std::string saveString(const graph::GraphAsset& asset);
    };

}
