// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "GraphSchema.h"

#include <cstdio>
#include <map>
#include <set>
#include <utility>

namespace quantum {
namespace graph {

    static void report(std::vector<GraphDiagnostic>& diagnostics, GraphDiagnosticSeverity severity,
                       uint32_t nodeId, const std::string& message) {
        GraphDiagnostic diagnostic;
        diagnostic.severity = severity;
        diagnostic.nodeId = nodeId;
        diagnostic.message = message;
        diagnostics.push_back(diagnostic);
    }

    static const GraphPinDef* findPin(const std::vector<GraphPinDef>& pins, const std::string& name) {
        for (const GraphPinDef& pin : pins) {
            if (pin.name == name) {
                return &pin;
            }
        }
        return nullptr;
    }

    bool GraphSchema::typesCompatible(GraphValueType from, GraphValueType to) {
        return graphValueTypesCompatible(from, to);
    }

    bool GraphSchema::hasErrors(const std::vector<GraphDiagnostic>& diagnostics) {
        for (const GraphDiagnostic& diagnostic : diagnostics) {
            if (diagnostic.severity == GraphDiagnosticSeverity::Error) {
                return true;
            }
        }
        return false;
    }

    std::string GraphSchema::format(const std::vector<GraphDiagnostic>& diagnostics) {
        std::string out;
        char buffer[512];
        for (const GraphDiagnostic& diagnostic : diagnostics) {
            const char* level = diagnostic.severity == GraphDiagnosticSeverity::Error ? "error" : "warning";
            if (diagnostic.nodeId != 0) {
                std::snprintf(buffer, sizeof(buffer), "[%s] node %u: %s\n", level,
                              diagnostic.nodeId, diagnostic.message.c_str());
            } else {
                std::snprintf(buffer, sizeof(buffer), "[%s] %s\n", level,
                              diagnostic.message.c_str());
            }
            out += buffer;
        }
        return out;
    }

    void GraphSchema::validate(const GraphAsset& asset, std::vector<GraphDiagnostic>& diagnostics) {
        diagnostics.clear();

        if (asset.name.empty()) {
            report(diagnostics, GraphDiagnosticSeverity::Warning, 0, "graph has no name");
        }

        std::set<std::string> variableNames;
        for (const GraphVariable& variable : asset.variables) {
            if (variable.name.empty()) {
                report(diagnostics, GraphDiagnosticSeverity::Error, 0, "variable with empty name");
                continue;
            }
            if (!variableNames.insert(variable.name).second) {
                report(diagnostics, GraphDiagnosticSeverity::Error, 0,
                       "duplicate variable '" + variable.name + "'");
            }
            if (!variable.defaultValue.isNone() &&
                !variable.defaultValue.canConvertTo(variable.type)) {
                report(diagnostics, GraphDiagnosticSeverity::Error, 0,
                       "default value of variable '" + variable.name + "' is not compatible with type '" +
                       graphValueTypeName(variable.type) + "'");
            }
        }

        const NodeCatalog& catalog = NodeCatalog::core();
        std::map<uint32_t, const GraphNode*> nodesById;
        std::map<uint32_t, std::vector<GraphPinDef>> pinsById;

        for (const GraphNode& node : asset.nodes) {
            if (node.id == 0) {
                report(diagnostics, GraphDiagnosticSeverity::Error, node.id,
                       "node id 0 is reserved");
            } else if (!nodesById.insert(std::make_pair(node.id, &node)).second) {
                report(diagnostics, GraphDiagnosticSeverity::Error, node.id,
                       "duplicate node id");
            }

            if (node.kind.empty()) {
                report(diagnostics, GraphDiagnosticSeverity::Error, node.id,
                       "node has no kind");
            } else if (!catalog.find(node.kind)) {
                report(diagnostics, GraphDiagnosticSeverity::Error, node.id,
                       "unknown node kind '" + node.kind + "'");
            }

            if (node.kind.compare(0, 4, "var.") == 0) {
                std::string variableName = node.getStringParam("variable");
                if (variableName.empty()) {
                    report(diagnostics, GraphDiagnosticSeverity::Error, node.id,
                           "variable node has no 'variable' parameter");
                } else if (!variableNames.count(variableName)) {
                    report(diagnostics, GraphDiagnosticSeverity::Error, node.id,
                           "variable '" + variableName + "' is not declared");
                }
            }

            std::set<std::string> paramNames;
            for (const GraphParam& param : node.params) {
                if (!paramNames.insert(param.name).second) {
                    report(diagnostics, GraphDiagnosticSeverity::Error, node.id,
                           "duplicate parameter '" + param.name + "'");
                }
            }

            pinsById[node.id] = catalog.resolvePins(node, &asset);
        }

        struct EdgeKey {
            uint32_t fromNode;
            std::string fromPin;
            uint32_t toNode;
            std::string toPin;

            bool operator<(const EdgeKey& other) const {
                if (fromNode != other.fromNode) return fromNode < other.fromNode;
                if (fromPin != other.fromPin) return fromPin < other.fromPin;
                if (toNode != other.toNode) return toNode < other.toNode;
                return toPin < other.toPin;
            }
        };

        std::set<EdgeKey> edgesSeen;
        std::set<std::pair<uint32_t, std::string>> dataInputSources;

        for (const GraphEdge& edge : asset.edges) {
            std::map<uint32_t, const GraphNode*>::const_iterator fromIt = nodesById.find(edge.fromNode);
            std::map<uint32_t, const GraphNode*>::const_iterator toIt = nodesById.find(edge.toNode);

            if (fromIt == nodesById.end()) {
                report(diagnostics, GraphDiagnosticSeverity::Error, edge.fromNode,
                       "edge source node does not exist");
                continue;
            }
            if (toIt == nodesById.end()) {
                report(diagnostics, GraphDiagnosticSeverity::Error, edge.toNode,
                       "edge target node does not exist");
                continue;
            }

            EdgeKey key{edge.fromNode, edge.fromPin, edge.toNode, edge.toPin};
            if (!edgesSeen.insert(key).second) {
                report(diagnostics, GraphDiagnosticSeverity::Error, edge.fromNode,
                       "duplicate edge");
                continue;
            }

            const std::vector<GraphPinDef>& fromPins = pinsById[edge.fromNode];
            const std::vector<GraphPinDef>& toPins = pinsById[edge.toNode];

            const GraphPinDef* fromPin = findPin(fromPins, edge.fromPin);
            const GraphPinDef* toPin = findPin(toPins, edge.toPin);

            if (!fromPin) {
                report(diagnostics, GraphDiagnosticSeverity::Error, edge.fromNode,
                       "node has no pin '" + edge.fromPin + "'");
                continue;
            }
            if (fromPin->dir != GraphPinDir::Output) {
                report(diagnostics, GraphDiagnosticSeverity::Error, edge.fromNode,
                       "pin '" + edge.fromPin + "' is not an output pin");
                continue;
            }
            if (!toPin) {
                report(diagnostics, GraphDiagnosticSeverity::Error, edge.toNode,
                       "node has no pin '" + edge.toPin + "'");
                continue;
            }
            if (toPin->dir != GraphPinDir::Input) {
                report(diagnostics, GraphDiagnosticSeverity::Error, edge.toNode,
                       "pin '" + edge.toPin + "' is not an input pin");
                continue;
            }

            if (fromPin->kind != toPin->kind) {
                report(diagnostics, GraphDiagnosticSeverity::Error, edge.toNode,
                       "cannot connect an exec pin to a data pin (or the reverse)");
                continue;
            }

            if (fromPin->kind == GraphPinKind::Data) {
                if (!typesCompatible(fromPin->type, toPin->type)) {
                    report(diagnostics, GraphDiagnosticSeverity::Error, edge.toNode,
                           std::string("cannot connect value of type '") +
                           graphValueTypeName(fromPin->type) + "' to pin of type '" +
                           graphValueTypeName(toPin->type) + "'");
                }

                std::pair<uint32_t, std::string> inputKey(edge.toNode, edge.toPin);
                if (!dataInputSources.insert(inputKey).second) {
                    report(diagnostics, GraphDiagnosticSeverity::Error, edge.toNode,
                           "data input pin '" + edge.toPin + "' receives more than one connection");
                }
            }
        }

        std::set<std::string> eventIds;
        for (const GraphEventBinding& binding : asset.events) {
            if (binding.id.empty()) {
                report(diagnostics, GraphDiagnosticSeverity::Error, 0, "event binding with empty id");
                continue;
            }
            if (!eventIds.insert(binding.id).second) {
                report(diagnostics, GraphDiagnosticSeverity::Error, 0,
                       "duplicate event binding '" + binding.id + "'");
            }
            const GraphNode* entry = asset.findNode(binding.entryNode);
            if (!entry) {
                report(diagnostics, GraphDiagnosticSeverity::Error, binding.entryNode,
                       "event '" + binding.id + "' entry node does not exist");
            } else if (entry->kind.compare(0, 6, "event.") != 0) {
                report(diagnostics, GraphDiagnosticSeverity::Warning, entry->id,
                       "event '" + binding.id + "' entry node is not an event node");
            }
        }
    }

}
}
