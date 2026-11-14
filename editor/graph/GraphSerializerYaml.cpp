// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "GraphSerializerYaml.h"

#include "graph/GraphTypes.h"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <fstream>
#include <vector>

namespace quantum::editor {

    using graph::GraphAsset;
    using graph::GraphValue;
    using graph::GraphValueType;

    namespace {

        GraphValue zeroValue(GraphValueType type) {
            switch (type) {
                case GraphValueType::Bool: return GraphValue::makeBool(false);
                case GraphValueType::Int: return GraphValue::makeInt(0);
                case GraphValueType::Float: return GraphValue::makeFloat(0.0);
                case GraphValueType::String: return GraphValue::makeString(std::string());
                case GraphValueType::Vector2: return GraphValue::makeVector2(Vector2(0.0f, 0.0f));
                case GraphValueType::Vector3: return GraphValue::makeVector3(Vector3(0.0f, 0.0f, 0.0f));
                case GraphValueType::Vector4: return GraphValue::makeVector4(Vector4(0.0f, 0.0f, 0.0f, 0.0f));
                case GraphValueType::Quaternion: return GraphValue::makeQuaternion(Quaternion());
                case GraphValueType::Rect: return GraphValue::makeRect(Rect(0.0f, 0.0f, 0.0f, 0.0f));
                case GraphValueType::Color3: return GraphValue::makeColor3(Vector3(1.0f, 1.0f, 1.0f));
                case GraphValueType::Color4: return GraphValue::makeColor4(Vector4(1.0f, 1.0f, 1.0f, 1.0f));
                case GraphValueType::Entity: return GraphValue::makeEntity(NULL_ENTITY);
                default: return GraphValue();
            }
        }

        bool decodeTyped(const YAML::Node& node, GraphValueType type, GraphValue& out, std::string& error) {
            try {
                switch (type) {
                    case GraphValueType::Bool:
                        out = GraphValue::makeBool(node.as<bool>());
                        return true;
                    case GraphValueType::Int:
                        out = GraphValue::makeInt(node.as<int64_t>());
                        return true;
                    case GraphValueType::Float:
                        out = GraphValue::makeFloat(node.as<double>());
                        return true;
                    case GraphValueType::String:
                        out = GraphValue::makeString(node.as<std::string>());
                        return true;
                    case GraphValueType::Vector2: {
                        std::vector<float> v = node.as<std::vector<float>>();
                        if (v.size() != 2) {
                            error = "expected 2 components";
                            return false;
                        }
                        out = GraphValue::makeVector2(Vector2(v[0], v[1]));
                        return true;
                    }
                    case GraphValueType::Vector3:
                    case GraphValueType::Color3: {
                        std::vector<float> v = node.as<std::vector<float>>();
                        if (v.size() != 3) {
                            error = "expected 3 components";
                            return false;
                        }
                        out = type == GraphValueType::Color3
                            ? GraphValue::makeColor3(Vector3(v[0], v[1], v[2]))
                            : GraphValue::makeVector3(Vector3(v[0], v[1], v[2]));
                        return true;
                    }
                    case GraphValueType::Vector4:
                    case GraphValueType::Color4: {
                        std::vector<float> v = node.as<std::vector<float>>();
                        if (v.size() != 4) {
                            error = "expected 4 components";
                            return false;
                        }
                        out = type == GraphValueType::Color4
                            ? GraphValue::makeColor4(Vector4(v[0], v[1], v[2], v[3]))
                            : GraphValue::makeVector4(Vector4(v[0], v[1], v[2], v[3]));
                        return true;
                    }
                    case GraphValueType::Quaternion: {
                        std::vector<float> v = node.as<std::vector<float>>();
                        if (v.size() != 4) {
                            error = "expected 4 components (w, x, y, z)";
                            return false;
                        }
                        out = GraphValue::makeQuaternion(Quaternion(v[0], v[1], v[2], v[3]));
                        return true;
                    }
                    case GraphValueType::Rect: {
                        std::vector<float> v = node.as<std::vector<float>>();
                        if (v.size() != 4) {
                            error = "expected 4 components (x, y, width, height)";
                            return false;
                        }
                        out = GraphValue::makeRect(Rect(v[0], v[1], v[2], v[3]));
                        return true;
                    }
                    case GraphValueType::Entity:
                        out = GraphValue::makeEntity(static_cast<Entity>(node.as<uint32_t>()));
                        return true;
                    case GraphValueType::None:
                    case GraphValueType::Any:
                        error = std::string("type '") + graphValueTypeName(type) + "' cannot hold a literal value";
                        return false;
                }
            } catch (const YAML::Exception& e) {
                error = e.what();
                return false;
            }
            error = "unknown value type";
            return false;
        }

        // Node parameters are not typed by the catalog yet, so plain scalars get
        // the usual YAML implicit type (or stay a string, which is what the
        // variable-name parameter needs).
        bool decodeUntyped(const YAML::Node& node, GraphValue& out, std::string& error) {
            if (node.IsScalar()) {
                try {
                    out = GraphValue::makeInt(node.as<int64_t>());
                    return true;
                } catch (const YAML::Exception&) {
                }
                try {
                    out = GraphValue::makeFloat(node.as<double>());
                    return true;
                } catch (const YAML::Exception&) {
                }
                try {
                    out = GraphValue::makeBool(node.as<bool>());
                    return true;
                } catch (const YAML::Exception&) {
                }
                out = GraphValue::makeString(node.as<std::string>());
                return true;
            }

            if (node.IsSequence()) {
                std::vector<float> v;
                try {
                    v = node.as<std::vector<float>>();
                } catch (const YAML::Exception& e) {
                    error = e.what();
                    return false;
                }
                if (v.size() == 2) {
                    out = GraphValue::makeVector2(Vector2(v[0], v[1]));
                    return true;
                }
                if (v.size() == 3) {
                    out = GraphValue::makeVector3(Vector3(v[0], v[1], v[2]));
                    return true;
                }
                if (v.size() == 4) {
                    out = GraphValue::makeVector4(Vector4(v[0], v[1], v[2], v[3]));
                    return true;
                }
                error = "sequence parameters need 2, 3 or 4 numbers";
                return false;
            }

            error = "unsupported parameter value";
            return false;
        }

        bool readNodePosition(const YAML::Node& node, float& x, float& y, std::string& error) {
            if (node["pos"]) {
                try {
                    std::vector<float> v = node["pos"].as<std::vector<float>>();
                    if (v.size() != 2) {
                        error = "pos needs 2 numbers";
                        return false;
                    }
                    x = v[0];
                    y = v[1];
                    return true;
                } catch (const YAML::Exception& e) {
                    error = e.what();
                    return false;
                }
            }
            if (node["x"] || node["y"]) {
                try {
                    if (node["x"]) x = node["x"].as<float>();
                    if (node["y"]) y = node["y"].as<float>();
                    return true;
                } catch (const YAML::Exception& e) {
                    error = e.what();
                    return false;
                }
            }
            return true;
        }

        void emitValue(YAML::Emitter& out, const GraphValue& value) {
            switch (value.type) {
                case GraphValueType::None:
                case GraphValueType::Any:
                    out << YAML::Null;
                    break;
                case GraphValueType::Bool:
                    out << value.asBool();
                    break;
                case GraphValueType::Int:
                    out << static_cast<long long>(value.asInt());
                    break;
                case GraphValueType::Float:
                    out << value.asFloat();
                    break;
                case GraphValueType::String:
                    out << value.asString();
                    break;
                case GraphValueType::Vector2: {
                    Vector2 v = value.asVector2();
                    out << YAML::Flow << YAML::BeginSeq << v.x << v.y << YAML::EndSeq;
                    break;
                }
                case GraphValueType::Vector3:
                case GraphValueType::Color3: {
                    Vector3 v = value.asVector3();
                    out << YAML::Flow << YAML::BeginSeq << v.x << v.y << v.z << YAML::EndSeq;
                    break;
                }
                case GraphValueType::Vector4:
                case GraphValueType::Color4: {
                    Vector4 v = value.asVector4();
                    out << YAML::Flow << YAML::BeginSeq << v.x << v.y << v.z << v.w << YAML::EndSeq;
                    break;
                }
                case GraphValueType::Quaternion: {
                    Quaternion q = value.asQuaternion();
                    out << YAML::Flow << YAML::BeginSeq << q.w << q.x << q.y << q.z << YAML::EndSeq;
                    break;
                }
                case GraphValueType::Rect: {
                    Rect r = value.asRect();
                    out << YAML::Flow << YAML::BeginSeq
                        << r.getX() << r.getY() << r.getWidth() << r.getHeight()
                        << YAML::EndSeq;
                    break;
                }
                case GraphValueType::Entity:
                    out << static_cast<unsigned>(value.asEntity());
                    break;
            }
        }

    }

    bool GraphSerializerYaml::loadString(const std::string& text, GraphAsset& out, std::string& error) {
        out = GraphAsset();
        error.clear();

        YAML::Node root;
        try {
            root = YAML::Load(text);
        } catch (const YAML::Exception& e) {
            error = std::string("YAML: ") + e.what();
            return false;
        }

        if (!root.IsMap()) {
            error = "graph file must be a YAML mapping";
            return false;
        }

        try {
            if (root["version"]) {
                uint32_t version = root["version"].as<uint32_t>();
                if (version != graph::GRAPH_SCHEMA_VERSION) {
                    error = "unsupported graph schema version " + std::to_string(version);
                    return false;
                }
            }
            out.version = graph::GRAPH_SCHEMA_VERSION;
            if (root["name"]) {
                out.name = root["name"].as<std::string>();
            }

            if (const YAML::Node variables = root["variables"]) {
                if (!variables.IsSequence()) {
                    error = "'variables' must be a sequence";
                    return false;
                }
                for (const YAML::Node& entry : variables) {
                    graph::GraphVariable variable;
                    if (!entry["name"]) {
                        error = "variable without a name";
                        return false;
                    }
                    variable.name = entry["name"].as<std::string>();
                    variable.displayName = entry["displayName"]
                        ? entry["displayName"].as<std::string>()
                        : variable.name;
                    if (entry["type"]) {
                        GraphValueType type = GraphValueType::Float;
                        if (!graph::graphValueTypeFromName(entry["type"].as<std::string>(), type)) {
                            error = "variable '" + variable.name + "' has an unknown type";
                            return false;
                        }
                        variable.type = type;
                    }
                    if (entry["exposed"]) {
                        variable.exposed = entry["exposed"].as<bool>();
                    }
                    if (entry["default"]) {
                        std::string valueError;
                        if (!decodeTyped(entry["default"], variable.type, variable.defaultValue, valueError)) {
                            error = "variable '" + variable.name + "' default: " + valueError;
                            return false;
                        }
                    } else {
                        variable.defaultValue = zeroValue(variable.type);
                    }
                    out.variables.push_back(variable);
                }
            }

            if (const YAML::Node nodes = root["nodes"]) {
                if (!nodes.IsSequence()) {
                    error = "'nodes' must be a sequence";
                    return false;
                }
                for (const YAML::Node& entry : nodes) {
                    graph::GraphNode node;
                    if (!entry["id"] || !entry["kind"]) {
                        error = "node without an 'id' or 'kind'";
                        return false;
                    }
                    node.id = entry["id"].as<uint32_t>();
                    node.kind = entry["kind"].as<std::string>();
                    if (entry["title"]) {
                        node.title = entry["title"].as<std::string>();
                    }
                    std::string positionError;
                    if (!readNodePosition(entry, node.x, node.y, positionError)) {
                        error = "node " + std::to_string(node.id) + " position: " + positionError;
                        return false;
                    }
                    if (const YAML::Node params = entry["params"]) {
                        if (!params.IsMap()) {
                            error = "node " + std::to_string(node.id) + " params must be a mapping";
                            return false;
                        }
                        for (const auto& param : params) {
                            graph::GraphParam parsed;
                            parsed.name = param.first.as<std::string>();
                            std::string valueError;
                            if (!decodeUntyped(param.second, parsed.value, valueError)) {
                                error = "node " + std::to_string(node.id) + " param '" + parsed.name + "': " + valueError;
                                return false;
                            }
                            node.params.push_back(parsed);
                        }
                    }
                    out.nodes.push_back(node);
                }
            }

            if (const YAML::Node edges = root["edges"]) {
                if (!edges.IsSequence()) {
                    error = "'edges' must be a sequence";
                    return false;
                }
                for (const YAML::Node& entry : edges) {
                    graph::GraphEdge edge;
                    if (!entry["from"] || !entry["fromPin"] || !entry["to"] || !entry["toPin"]) {
                        error = "edge needs from, fromPin, to and toPin";
                        return false;
                    }
                    edge.fromNode = entry["from"].as<uint32_t>();
                    edge.fromPin = entry["fromPin"].as<std::string>();
                    edge.toNode = entry["to"].as<uint32_t>();
                    edge.toPin = entry["toPin"].as<std::string>();
                    out.edges.push_back(edge);
                }
            }

            if (const YAML::Node events = root["events"]) {
                if (events.IsMap()) {
                    for (const auto& entry : events) {
                        graph::GraphEventBinding binding;
                        binding.id = entry.first.as<std::string>();
                        binding.entryNode = entry.second.as<uint32_t>();
                        out.events.push_back(binding);
                    }
                } else if (events.IsSequence()) {
                    for (const YAML::Node& entry : events) {
                        graph::GraphEventBinding binding;
                        if (!entry["event"] || !entry["node"]) {
                            error = "event needs an 'event' and a 'node'";
                            return false;
                        }
                        binding.id = entry["event"].as<std::string>();
                        binding.entryNode = entry["node"].as<uint32_t>();
                        out.events.push_back(binding);
                    }
                } else {
                    error = "'events' must be a mapping or a sequence";
                    return false;
                }
            }

            if (const YAML::Node comments = root["comments"]) {
                if (!comments.IsSequence()) {
                    error = "'comments' must be a sequence";
                    return false;
                }
                for (const YAML::Node& entry : comments) {
                    graph::GraphComment comment;
                    if (entry["text"]) {
                        comment.text = entry["text"].as<std::string>();
                    }
                    if (entry["x"]) comment.x = entry["x"].as<float>();
                    if (entry["y"]) comment.y = entry["y"].as<float>();
                    if (entry["width"]) comment.width = entry["width"].as<float>();
                    if (entry["height"]) comment.height = entry["height"].as<float>();
                    out.comments.push_back(comment);
                }
            }
        } catch (const YAML::Exception& e) {
            error = std::string("YAML: ") + e.what();
            return false;
        }

        return true;
    }

    bool GraphSerializerYaml::loadFile(const std::string& path, GraphAsset& out, std::string& error) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            error = "cannot open '" + path + "' for reading";
            return false;
        }
        std::string text((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
        return loadString(text, out, error);
    }

    std::string GraphSerializerYaml::saveString(const GraphAsset& asset) {
        YAML::Emitter out;
        out.SetIndent(2);
        out.SetFloatPrecision(9);

        out << YAML::BeginMap;
        out << YAML::Key << "version" << YAML::Value << asset.version;
        out << YAML::Key << "name" << YAML::Value << asset.name;

        if (!asset.variables.empty()) {
            out << YAML::Key << "variables" << YAML::Value << YAML::BeginSeq;
            for (const graph::GraphVariable& variable : asset.variables) {
                out << YAML::BeginMap;
                out << YAML::Key << "name" << YAML::Value << variable.name;
                out << YAML::Key << "displayName" << YAML::Value << variable.displayName;
                out << YAML::Key << "type" << YAML::Value << graph::graphValueTypeName(variable.type);
                out << YAML::Key << "default" << YAML::Value;
                emitValue(out, variable.defaultValue);
                if (!variable.exposed) {
                    out << YAML::Key << "exposed" << YAML::Value << false;
                }
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        if (!asset.nodes.empty()) {
            out << YAML::Key << "nodes" << YAML::Value << YAML::BeginSeq;
            for (const graph::GraphNode& node : asset.nodes) {
                out << YAML::BeginMap;
                out << YAML::Key << "id" << YAML::Value << node.id;
                out << YAML::Key << "kind" << YAML::Value << node.kind;
                if (!node.title.empty()) {
                    out << YAML::Key << "title" << YAML::Value << node.title;
                }
                out << YAML::Key << "pos" << YAML::Value
                    << YAML::Flow << YAML::BeginSeq << node.x << node.y << YAML::EndSeq;
                if (!node.params.empty()) {
                    out << YAML::Key << "params" << YAML::Value << YAML::BeginMap;
                    for (const graph::GraphParam& param : node.params) {
                        out << YAML::Key << param.name << YAML::Value;
                        emitValue(out, param.value);
                    }
                    out << YAML::EndMap;
                }
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        if (!asset.edges.empty()) {
            out << YAML::Key << "edges" << YAML::Value << YAML::BeginSeq;
            for (const graph::GraphEdge& edge : asset.edges) {
                out << YAML::BeginMap;
                out << YAML::Key << "from" << YAML::Value << edge.fromNode;
                out << YAML::Key << "fromPin" << YAML::Value << edge.fromPin;
                out << YAML::Key << "to" << YAML::Value << edge.toNode;
                out << YAML::Key << "toPin" << YAML::Value << edge.toPin;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        if (!asset.events.empty()) {
            out << YAML::Key << "events" << YAML::Value << YAML::BeginSeq;
            for (const graph::GraphEventBinding& binding : asset.events) {
                out << YAML::BeginMap;
                out << YAML::Key << "event" << YAML::Value << binding.id;
                out << YAML::Key << "node" << YAML::Value << binding.entryNode;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        if (!asset.comments.empty()) {
            out << YAML::Key << "comments" << YAML::Value << YAML::BeginSeq;
            for (const graph::GraphComment& comment : asset.comments) {
                out << YAML::BeginMap;
                out << YAML::Key << "text" << YAML::Value << comment.text;
                out << YAML::Key << "x" << YAML::Value << comment.x;
                out << YAML::Key << "y" << YAML::Value << comment.y;
                out << YAML::Key << "width" << YAML::Value << comment.width;
                out << YAML::Key << "height" << YAML::Value << comment.height;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        }

        out << YAML::EndMap;

        return out.good() ? std::string(out.c_str()) : std::string();
    }

    bool GraphSerializerYaml::saveFile(const std::string& path, const GraphAsset& asset, std::string& error) {
        error.clear();
        std::string text = saveString(asset);
        if (text.empty()) {
            error = "failed to serialize graph";
            return false;
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            error = "cannot open '" + path + "' for writing";
            return false;
        }
        file << text;
        if (!file.good()) {
            error = "failed to write '" + path + "'";
            return false;
        }
        return true;
    }

}
