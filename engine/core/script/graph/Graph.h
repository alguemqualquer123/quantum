// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef GRAPH_H
#define GRAPH_H

#include "GraphValue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace quantum {
namespace graph {

    // Pins are derived from the node kind (NodeCatalog) instead of being stored
    // in the .graph / .gdat, so a catalog update does not require rewriting
    // every authored asset.
    struct QUANTUM_API GraphPinDef {
        std::string name;
        GraphValueType type = GraphValueType::None;
        GraphPinKind kind = GraphPinKind::Data;
        GraphPinDir dir = GraphPinDir::Input;
        GraphValue defaultValue;
    };

    struct QUANTUM_API GraphVariable {
        std::string name;
        std::string displayName;
        GraphValueType type = GraphValueType::Float;
        GraphValue defaultValue;
        bool exposed = true;
    };

    struct QUANTUM_API GraphParam {
        std::string name;
        GraphValue value;
    };

    struct QUANTUM_API GraphNode {
        uint32_t id = 0;
        std::string kind;
        std::string title;
        float x = 0.0f;
        float y = 0.0f;
        std::vector<GraphParam> params;

        const GraphParam* findParam(const std::string& name) const;
        void setParam(const std::string& name, const GraphValue& value);
        std::string getStringParam(const std::string& name, const std::string& fallback = std::string()) const;
        double getFloatParam(const std::string& name, double fallback) const;
        bool getBoolParam(const std::string& name, bool fallback) const;
    };

    struct QUANTUM_API GraphEdge {
        uint32_t fromNode = 0;
        std::string fromPin;
        uint32_t toNode = 0;
        std::string toPin;
    };

    struct QUANTUM_API GraphEventBinding {
        std::string id;
        uint32_t entryNode = 0;
    };

    struct QUANTUM_API GraphComment {
        std::string text;
        float x = 0.0f;
        float y = 0.0f;
        float width = 220.0f;
        float height = 120.0f;
    };

    struct QUANTUM_API GraphAsset {
        uint32_t version = GRAPH_SCHEMA_VERSION;
        std::string name;
        std::vector<GraphVariable> variables;
        std::vector<GraphNode> nodes;
        std::vector<GraphEdge> edges;
        std::vector<GraphEventBinding> events;
        std::vector<GraphComment> comments;

        const GraphVariable* findVariable(const std::string& name) const;
        const GraphNode* findNode(uint32_t id) const;
        GraphNode* findNode(uint32_t id);
        uint32_t nextNodeId() const;
    };

}
}

#endif //GRAPH_H
