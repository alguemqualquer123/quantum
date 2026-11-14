// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "GraphTypes.h"

#include <cstring>

namespace quantum {
namespace graph {

    const char* graphValueTypeName(GraphValueType type) {
        switch (type) {
            case GraphValueType::None: return "none";
            case GraphValueType::Bool: return "bool";
            case GraphValueType::Int: return "int";
            case GraphValueType::Float: return "float";
            case GraphValueType::String: return "string";
            case GraphValueType::Vector2: return "vec2";
            case GraphValueType::Vector3: return "vec3";
            case GraphValueType::Vector4: return "vec4";
            case GraphValueType::Quaternion: return "quat";
            case GraphValueType::Rect: return "rect";
            case GraphValueType::Color3: return "color3";
            case GraphValueType::Color4: return "color4";
            case GraphValueType::Entity: return "entity";
            case GraphValueType::Any: return "any";
        }
        return "none";
    }

    bool graphValueTypeFromName(const std::string& name, GraphValueType& out) {
        for (uint8_t i = 0; i <= static_cast<uint8_t>(GraphValueType::Any); ++i) {
            GraphValueType type = static_cast<GraphValueType>(i);
            if (name == graphValueTypeName(type)) {
                out = type;
                return true;
            }
        }
        return false;
    }

    uint64_t graphSourceHash(const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        uint64_t hash = 14695981039346656037ull;
        for (size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

}
}
