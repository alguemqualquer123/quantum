// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "GraphAsset.h"

#include "yaml-cpp/yaml.h"

#include <fstream>
#include <map>
#include <sstream>

namespace quantum::editor::graph {

namespace {
    const char* directionName(PinDirection direction) {
        return direction == PinDirection::Output ? "out" : "in";
    }

    PinDirection parseDirection(const YAML::Node& node) {
        return node.as<std::string>("in") == "out" ? PinDirection::Output : PinDirection::Input;
    }

    YAML::Node emitAsset(const GraphAsset& asset) {
        YAML::Node root;
        root["version"] = asset.version;
        root["name"] = asset.name;
        YAML::Node nodes(YAML::NodeType::Sequence);
        for (const Node& node : asset.nodes) {
            YAML::Node encoded;
            encoded["id"] = node.id;
            encoded["kind"] = node.kind;
            encoded["title"] = node.title;
            YAML::Node position(YAML::NodeType::Sequence);
            position.push_back(node.x);
            position.push_back(node.y);
            encoded["position"] = position;
            YAML::Node pins(YAML::NodeType::Sequence);
            for (const Pin& pin : node.pins) {
                YAML::Node encodedPin;
                encodedPin["name"] = pin.name;
                encodedPin["type"] = pin.type;
                encodedPin["direction"] = directionName(pin.direction);
                pins.push_back(encodedPin);
            }
            encoded["pins"] = pins;
            nodes.push_back(encoded);
        }
        root["nodes"] = nodes;

        YAML::Node edges(YAML::NodeType::Sequence);
        for (const Edge& edge : asset.edges) {
            YAML::Node encoded;
            encoded["from"] = edge.fromNode;
            encoded["from_pin"] = edge.fromPin;
            encoded["to"] = edge.toNode;
            encoded["to_pin"] = edge.toPin;
            edges.push_back(encoded);
        }
        root["edges"] = edges;
        return root;
    }
}

bool GraphAsset::load(const std::string& path, std::string& error) {
    try {
        const YAML::Node root = YAML::LoadFile(path);
        if (!root || !root.IsMap()) {
            error = "graph root must be a YAML map";
            return false;
        }
        version = root["version"].as<int>(1);
        name = root["name"].as<std::string>("");
        nodes.clear();
        edges.clear();

        if (const YAML::Node encodedNodes = root["nodes"]; encodedNodes && encodedNodes.IsSequence()) {
            for (const YAML::Node& encoded : encodedNodes) {
                Node node;
                node.id = encoded["id"].as<int>(0);
                node.kind = encoded["kind"].as<std::string>("");
                node.title = encoded["title"].as<std::string>(node.kind);
                const YAML::Node position = encoded["position"];
                if (position && position.IsSequence() && position.size() >= 2) {
                    node.x = position[0].as<float>(0.0f);
                    node.y = position[1].as<float>(0.0f);
                }
                const YAML::Node pins = encoded["pins"];
                if (pins && pins.IsSequence()) {
                    for (const YAML::Node& encodedPin : pins) {
                        Pin pin;
                        pin.name = encodedPin["name"].as<std::string>("");
                        pin.type = encodedPin["type"].as<std::string>("any");
                        pin.direction = parseDirection(encodedPin["direction"]);
                        node.pins.push_back(pin);
                    }
                }
                nodes.push_back(node);
            }
        }

        if (const YAML::Node encodedEdges = root["edges"]; encodedEdges && encodedEdges.IsSequence()) {
            for (const YAML::Node& encoded : encodedEdges) {
                Edge edge;
                edge.fromNode = encoded["from"].as<int>(0);
                edge.fromPin = encoded["from_pin"].as<std::string>("");
                edge.toNode = encoded["to"].as<int>(0);
                edge.toPin = encoded["to_pin"].as<std::string>("");
                edges.push_back(edge);
            }
        }
    } catch (const YAML::Exception& exception) {
        error = exception.what();
        return false;
    }
    return true;
}

bool GraphAsset::save(const std::string& path, std::string& error) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "could not write graph file: " + path;
        return false;
    }
    file << canonicalYaml();
    if (!file.good()) {
        error = "failed while writing graph file: " + path;
        return false;
    }
    return true;
}

bool GraphAsset::validate(std::vector<std::string>& errors) const {
    errors.clear();
    if (version != 1) errors.push_back("unsupported graph version " + std::to_string(version));
    if (name.empty()) errors.push_back("graph name is empty");

    std::map<int, const Node*> byId;
    for (const Node& node : nodes) {
        if (node.id <= 0) errors.push_back("node id must be positive");
        if (node.kind.empty()) errors.push_back("node " + std::to_string(node.id) + " has no kind");
        if (!byId.emplace(node.id, &node).second) {
            errors.push_back("duplicate node id " + std::to_string(node.id));
        }
        std::map<std::string, bool> pinNames;
        for (const Pin& pin : node.pins) {
            if (pin.name.empty()) errors.push_back("node " + std::to_string(node.id) + " has an unnamed pin");
            if (!pinNames.emplace(pin.name, true).second) {
                errors.push_back("node " + std::to_string(node.id) + " has duplicate pin " + pin.name);
            }
        }
    }

    for (const Edge& edge : edges) {
        const auto from = byId.find(edge.fromNode);
        const auto to = byId.find(edge.toNode);
        if (from == byId.end() || to == byId.end()) {
            errors.push_back("edge references a missing node");
            continue;
        }
        bool fromFound = false;
        bool toFound = false;
        for (const Pin& pin : from->second->pins) {
            if (pin.name == edge.fromPin) {
                fromFound = true;
                if (pin.direction != PinDirection::Output) errors.push_back("edge source pin is not an output: " + edge.fromPin);
                break;
            }
        }
        for (const Pin& pin : to->second->pins) {
            if (pin.name == edge.toPin) {
                toFound = true;
                if (pin.direction != PinDirection::Input) errors.push_back("edge target pin is not an input: " + edge.toPin);
                break;
            }
        }
        if (!fromFound) errors.push_back("edge source pin not found: " + edge.fromPin);
        if (!toFound) errors.push_back("edge target pin not found: " + edge.toPin);
    }
    return errors.empty();
}

std::string GraphAsset::canonicalYaml() const {
    YAML::Emitter emitter;
    emitter << emitAsset(*this);
    return std::string(emitter.c_str()) + "\n";
}

}
