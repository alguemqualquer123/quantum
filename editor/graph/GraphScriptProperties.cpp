// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "GraphScriptProperties.h"

#include "Project.h"
#include "Log.h"
#include "graph/Graph.h"
#include "graph/GraphReader.h"
#include "graph/GraphValue.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace quantum::editor {
namespace GraphScriptProperties {

    namespace {

        bool graphTypeToScript(graph::GraphValueType type, ScriptPropertyType& out) {
            switch (type) {
                case graph::GraphValueType::Bool: out = ScriptPropertyType::Bool; return true;
                case graph::GraphValueType::Int: out = ScriptPropertyType::Int; return true;
                case graph::GraphValueType::Float: out = ScriptPropertyType::Float; return true;
                case graph::GraphValueType::String: out = ScriptPropertyType::String; return true;
                case graph::GraphValueType::Vector2: out = ScriptPropertyType::Vector2; return true;
                case graph::GraphValueType::Vector3: out = ScriptPropertyType::Vector3; return true;
                case graph::GraphValueType::Vector4: out = ScriptPropertyType::Vector4; return true;
                case graph::GraphValueType::Color3: out = ScriptPropertyType::Color3; return true;
                case graph::GraphValueType::Color4: out = ScriptPropertyType::Color4; return true;
                case graph::GraphValueType::Entity: out = ScriptPropertyType::EntityReference; return true;
                default: return false;
            }
        }

        ScriptPropertyValue graphValueToScript(const graph::GraphValue& value, ScriptPropertyType type) {
            switch (type) {
                case ScriptPropertyType::Bool:
                    return value.asBool();
                case ScriptPropertyType::Int:
                    return static_cast<int>(value.asInt());
                case ScriptPropertyType::Float:
                    return static_cast<float>(value.asFloat());
                case ScriptPropertyType::String:
                    return value.asString();
                case ScriptPropertyType::Vector2:
                    return value.asVector2();
                case ScriptPropertyType::Vector3:
                case ScriptPropertyType::Color3:
                    return value.asVector3();
                case ScriptPropertyType::Vector4:
                case ScriptPropertyType::Color4:
                    return value.asVector4();
                case ScriptPropertyType::EntityReference: {
                    EntityReference reference;
                    reference.entity = value.asEntity();
                    return reference;
                }
            }
            return std::monostate{};
        }

        bool readGraph(const std::filesystem::path& path, graph::GraphAsset& asset, std::string& error) {
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                error = "cannot open '" + path.string() + "'";
                return false;
            }

            const std::string bytes((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
            const std::vector<uint8_t> data(bytes.begin(), bytes.end());
            graph::GraphBlobInfo info;
            return graph::GraphReader::read(data, asset, info, error);
        }

        std::filesystem::path normalizePath(const std::filesystem::path& path) {
            std::error_code ec;
            const std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
            return ec ? path.lexically_normal() : normalized;
        }

    }

    bool loadExposed(const std::filesystem::path& gdatPath, std::vector<ScriptProperty>& out,
                     std::string& error) {
        out.clear();

        graph::GraphAsset asset;
        if (!readGraph(gdatPath, asset, error)) {
            return false;
        }

        for (const graph::GraphVariable& variable : asset.variables) {
            if (!variable.exposed) continue;

            ScriptPropertyType type;
            if (!graphTypeToScript(variable.type, type)) {
                Log::warn("GraphScriptProperties: variable '%s' has an unsupported type",
                          variable.name.c_str());
                continue;
            }

            ScriptProperty property;
            property.name = variable.name;
            property.displayName = variable.displayName.empty() ? variable.name : variable.displayName;
            property.type = type;
            property.defaultValue = graphValueToScript(variable.defaultValue, type);
            property.value = property.defaultValue;
            out.push_back(std::move(property));
        }

        error.clear();
        return true;
    }

    bool refresh(ScriptEntry& entry, const Project& project) {
        if (entry.type != ScriptType::GRAPH || entry.path.empty()) {
            return false;
        }

        std::vector<ScriptProperty> exposed;
        std::string error;
        if (!loadExposed(project.resolveAssetPath(entry.path), exposed, error)) {
            Log::error("GraphScriptProperties: '%s': %s", entry.path.c_str(), error.c_str());
            return false;
        }

        for (ScriptProperty& property : exposed) {
            for (const ScriptProperty& old : entry.properties) {
                if (old.name != property.name) continue;
                if (old.type == property.type) {
                    property.value = old.value;
                }
                break;
            }
        }

        entry.properties = std::move(exposed);
        return true;
    }

    int refreshAll(Project& project, const std::filesystem::path& gdatPath) {
        const std::filesystem::path target = normalizePath(gdatPath);
        int updated = 0;

        for (SceneProject& sceneProject : project.getScenes()) {
            if (!sceneProject.scene) continue;
            auto scripts = sceneProject.scene->getComponentArray<ScriptComponent>();
            if (!scripts) continue;

            bool sceneUpdated = false;
            for (size_t i = 0; i < scripts->size(); ++i) {
                ScriptComponent& component = scripts->getComponentFromIndex(i);
                for (ScriptEntry& entry : component.scripts) {
                    if (entry.type != ScriptType::GRAPH || entry.path.empty()) continue;
                    if (normalizePath(project.resolveAssetPath(entry.path)) != target) continue;
                    if (refresh(entry, project)) {
                        ++updated;
                        sceneUpdated = true;
                    }
                }
            }

            if (sceneUpdated) {
                sceneProject.isModified = true;
            }
        }

        return updated;
    }

}
}
