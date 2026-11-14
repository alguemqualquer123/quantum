// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "Graph.h"

#include <algorithm>

namespace quantum {
namespace graph {

    const GraphParam* GraphNode::findParam(const std::string& name) const {
        for (const GraphParam& param : params) {
            if (param.name == name) {
                return &param;
            }
        }
        return nullptr;
    }

    void GraphNode::setParam(const std::string& name, const GraphValue& value) {
        for (GraphParam& param : params) {
            if (param.name == name) {
                param.value = value;
                return;
            }
        }
        GraphParam param;
        param.name = name;
        param.value = value;
        params.push_back(param);
    }

    std::string GraphNode::getStringParam(const std::string& name, const std::string& fallback) const {
        const GraphParam* param = findParam(name);
        if (!param || param->value.type != GraphValueType::String) {
            return fallback;
        }
        return param->value.asString();
    }

    double GraphNode::getFloatParam(const std::string& name, double fallback) const {
        const GraphParam* param = findParam(name);
        if (!param) {
            return fallback;
        }
        return param->value.asFloat();
    }

    bool GraphNode::getBoolParam(const std::string& name, bool fallback) const {
        const GraphParam* param = findParam(name);
        if (!param) {
            return fallback;
        }
        return param->value.asBool();
    }

    const GraphVariable* GraphAsset::findVariable(const std::string& name) const {
        for (const GraphVariable& variable : variables) {
            if (variable.name == name) {
                return &variable;
            }
        }
        return nullptr;
    }

    const GraphNode* GraphAsset::findNode(uint32_t id) const {
        for (const GraphNode& node : nodes) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    }

    GraphNode* GraphAsset::findNode(uint32_t id) {
        for (GraphNode& node : nodes) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    }

    uint32_t GraphAsset::nextNodeId() const {
        uint32_t next = 1;
        for (const GraphNode& node : nodes) {
            next = std::max(next, node.id + 1);
        }
        return next;
    }

}
}
