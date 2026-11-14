// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace quantum::editor::graph {

    enum class PinDirection {
        Input,
        Output
    };

    struct Pin {
        std::string name;
        std::string type = "any";
        PinDirection direction = PinDirection::Input;
    };

    struct Node {
        int id = 0;
        std::string kind;
        std::string title;
        float x = 0.0f;
        float y = 0.0f;
        std::vector<Pin> pins;
    };

    struct Edge {
        int fromNode = 0;
        std::string fromPin;
        int toNode = 0;
        std::string toPin;
    };

    struct GraphAsset {
        int version = 1;
        std::string name;
        std::vector<Node> nodes;
        std::vector<Edge> edges;

        bool load(const std::string& path, std::string& error);
        bool save(const std::string& path, std::string& error) const;
        bool validate(std::vector<std::string>& errors) const;
        std::string canonicalYaml() const;
    };

}
