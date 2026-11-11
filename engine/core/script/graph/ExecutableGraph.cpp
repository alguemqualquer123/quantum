// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "ExecutableGraph.h"

#include "NodeCatalog.h"
#include "NodeDefs.h"

namespace quantum {
namespace graph {

    const BoundNode* ExecutableGraph::findNode(uint32_t id) const {
        std::map<uint32_t, uint32_t>::const_iterator it = nodeIndexById.find(id);
        return it == nodeIndexById.end() ? nullptr : &nodes[it->second];
    }

    const std::vector<uint32_t>* ExecutableGraph::findEventEntries(const std::string& eventId) const {
        std::map<std::string, std::vector<uint32_t>>::const_iterator it = eventEntries.find(eventId);
        return it == eventEntries.end() ? nullptr : &it->second;
    }

    int ExecutableGraph::getVariableIndex(const std::string& name) const {
        std::map<std::string, int>::const_iterator it = variableIndexByName.find(name);
        return it == variableIndexByName.end() ? -1 : it->second;
    }

    const GraphVariable* ExecutableGraph::findVariable(const std::string& name) const {
        int index = getVariableIndex(name);
        return index < 0 ? nullptr : &asset.variables[static_cast<size_t>(index)];
    }

    int BoundNode::findNamed(const std::vector<std::string>& names, const std::string& name) {
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int ExecutableGraph::addRegister() {
        registerDefaults.emplace_back();
        return registerCount++;
    }

    bool ExecutableGraph::prepare(const GraphAsset& sourceAsset, std::string& error) {
        asset = sourceAsset;
        nodes.clear();
        nodeIndexById.clear();
        eventEntries.clear();
        variableIndexByName.clear();
        registerDefaults.clear();
        registerCount = 0;
        error.clear();

        for (size_t i = 0; i < asset.variables.size(); ++i) {
            variableIndexByName[asset.variables[i].name] = static_cast<int>(i);
        }

        const NodeCatalog& catalog = NodeCatalog::core();

        std::vector<std::vector<GraphPinDef>> resolvedPins(asset.nodes.size());
        for (size_t i = 0; i < asset.nodes.size(); ++i) {
            const GraphNode& source = asset.nodes[i];

            if (source.id == 0) {
                error = "node id 0 is reserved";
                return false;
            }
            if (!nodeIndexById.insert(std::make_pair(source.id, static_cast<uint32_t>(i))).second) {
                error = "duplicate node id " + std::to_string(source.id);
                return false;
            }

            resolvedPins[i] = catalog.resolvePins(source, &asset);

            const GraphOpDef* op = findGraphOp(source.kind);
            if (!op) {
                error = "no runtime operation for node kind '" + source.kind + "'";
                return false;
            }

            const GraphNodeDef* def = catalog.find(source.kind);
            BoundNode bound;
            bound.index = static_cast<uint32_t>(i);
            bound.id = source.id;
            bound.source = &asset.nodes[i];
            bound.kind = op->opKind;
            bound.exec = op->exec;
            bound.pure = op->pure;
            bound.sequence = def && def->sequence;

            // Every variable node (get, set, add, increment, ...) resolves the
            // variable it owns by name once, here, so handlers never parse params.
            if (source.kind.compare(0, 4, "var.") == 0) {
                bound.dataIndex = getVariableIndex(source.getStringParam("variable"));
                if (bound.dataIndex < 0) {
                    error = "node " + std::to_string(source.id) + " references unknown variable '" +
                            source.getStringParam("variable") + "'";
                    return false;
                }
            }

            nodes.push_back(std::move(bound));
        }

        for (size_t i = 0; i < nodes.size(); ++i) {
            BoundNode& bound = nodes[i];
            size_t dataInputs = 0;
            size_t dataOutputs = 0;
            size_t execOutputs = 0;

            for (const GraphPinDef& pin : resolvedPins[i]) {
                if (pin.kind == GraphPinKind::Data) {
                    if (pin.dir == GraphPinDir::Input) {
                        bound.inputNames.push_back(pin.name);
                        dataInputs++;
                    } else {
                        bound.outputs.push_back(addRegister());
                        bound.outputNames.push_back(pin.name);
                        bound.outputTypes.push_back(pin.type);
                        dataOutputs++;
                    }
                } else if (pin.dir == GraphPinDir::Output) {
                    bound.execNames.push_back(pin.name);
                    execOutputs++;
                }
            }

            (void)dataOutputs;
            bound.inputs.resize(dataInputs);
            bound.execTargets.resize(execOutputs);
        }

        // Data inputs: an incoming edge binds the source output register, an
        // unconnected pin gets its own register seeded with the catalog default.
        for (size_t i = 0; i < nodes.size(); ++i) {
            BoundNode& bound = nodes[i];
            size_t inputOrdinal = 0;

            for (const GraphPinDef& pin : resolvedPins[i]) {
                if (pin.kind != GraphPinKind::Data || pin.dir != GraphPinDir::Input) {
                    continue;
                }

                BoundInput input;
                const GraphEdge* incoming = nullptr;
                for (const GraphEdge& edge : asset.edges) {
                    if (edge.toNode == bound.id && edge.toPin == pin.name) {
                        incoming = &edge;
                        break;
                    }
                }

                if (incoming) {
                    std::map<uint32_t, uint32_t>::const_iterator sourceIt = nodeIndexById.find(incoming->fromNode);
                    if (sourceIt == nodeIndexById.end()) {
                        error = "edge source node " + std::to_string(incoming->fromNode) + " does not exist";
                        return false;
                    }

                    size_t sourceDataOutput = 0;
                    int sourceOrdinal = -1;
                    for (const GraphPinDef& sourcePin : resolvedPins[sourceIt->second]) {
                        if (sourcePin.kind == GraphPinKind::Data && sourcePin.dir == GraphPinDir::Output) {
                            if (sourcePin.name == incoming->fromPin) {
                                sourceOrdinal = static_cast<int>(sourceDataOutput);
                            }
                            sourceDataOutput++;
                        }
                    }

                    if (sourceOrdinal < 0) {
                        error = "edge pin '" + incoming->fromPin + "' is not a data output of node " +
                                std::to_string(incoming->fromNode);
                        return false;
                    }

                    BoundNode& sourceNode = nodes[sourceIt->second];
                    input.reg = sourceNode.outputs[static_cast<size_t>(sourceOrdinal)];
                    input.pureNode = sourceNode.kind == BoundOpKind::Pure
                        ? static_cast<int>(sourceIt->second)
                        : -1;
                } else {
                    // No edge: a matching node param is the literal typed on
                    // the pin, otherwise the catalog default applies.
                    GraphValue defaultValue = pin.defaultValue;
                    const GraphParam* literal = bound.source->findParam(pin.name);
                    if (literal && !literal->value.isNone()) {
                        GraphValue converted;
                        if (literal->value.convertTo(pin.type, converted)) {
                            defaultValue = converted;
                        }
                    }
                    input.reg = addRegister();
                    registerDefaults[static_cast<size_t>(input.reg)] = defaultValue;
                }

                bound.inputs[inputOrdinal++] = input;
            }
        }

        // Exec targets: one list per exec output pin.
        for (const GraphEdge& edge : asset.edges) {
            std::map<uint32_t, uint32_t>::const_iterator sourceIt = nodeIndexById.find(edge.fromNode);
            if (sourceIt == nodeIndexById.end()) {
                error = "edge source node " + std::to_string(edge.fromNode) + " does not exist";
                return false;
            }

            size_t execOrdinal = 0;
            int pinOrdinal = -1;
            for (const GraphPinDef& pin : resolvedPins[sourceIt->second]) {
                if (pin.kind == GraphPinKind::Exec && pin.dir == GraphPinDir::Output) {
                    if (pin.name == edge.fromPin) {
                        pinOrdinal = static_cast<int>(execOrdinal);
                    }
                    execOrdinal++;
                }
            }
            if (pinOrdinal < 0) {
                continue;
            }

            std::map<uint32_t, uint32_t>::const_iterator targetIt = nodeIndexById.find(edge.toNode);
            if (targetIt == nodeIndexById.end()) {
                error = "edge target node " + std::to_string(edge.toNode) + " does not exist";
                return false;
            }

            nodes[sourceIt->second].execTargets[static_cast<size_t>(pinOrdinal)].push_back(targetIt->second);
        }

        for (const GraphEventBinding& binding : asset.events) {
            std::map<uint32_t, uint32_t>::const_iterator it = nodeIndexById.find(binding.entryNode);
            if (it == nodeIndexById.end()) {
                error = "event '" + binding.id + "' entry node does not exist";
                return false;
            }
            eventEntries[binding.id].push_back(it->second);
        }

        return true;
    }

}
}
