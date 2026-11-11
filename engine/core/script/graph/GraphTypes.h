// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef GRAPH_TYPES_H
#define GRAPH_TYPES_H

#include "Export.h"

#include <cstdint>
#include <string>

namespace quantum {
namespace graph {

    // Value kinds a pin or variable can carry. Color3/Color4 are stored as
    // Vector3/Vector4 respectively, which is what the engine already uses for
    // material and light colors.
    enum class GraphValueType : uint8_t {
        None = 0,
        Bool,
        Int,
        Float,
        String,
        Vector2,
        Vector3,
        Vector4,
        Quaternion,
        Rect,
        Color3,
        Color4,
        Entity,
        Any
    };

    enum class GraphPinKind : uint8_t {
        Exec = 0,
        Data
    };

    enum class GraphPinDir : uint8_t {
        Input = 0,
        Output
    };

    // On disk the graph is a DGR1 blob; the schema version is bumped whenever a
    // table layout changes, so a stale .gdat from a previous editor build is
    // rejected with a readable error instead of being misread.
    constexpr uint32_t GRAPH_SCHEMA_VERSION = 1;
    constexpr uint8_t GRAPH_BLOB_MAGIC[4] = {'D', 'G', 'R', '1'};

    // Authoring files live next to the other project assets.
    constexpr const char* GRAPH_FILE_EXTENSION = ".graph";

    // Stable node kind ids are dotted strings ("flow.branch", "var.get",
    // "method.PhysicsSystem.applyImpulse"). The core ids live in NodeCatalog;
    // reflection-generated ones follow "method.<Class>.<method>".
    constexpr const char* GRAPH_EVENT_PREFIX = "event.";
    constexpr const char* GRAPH_METHOD_PREFIX = "method.";

    // A custom event id carries this prefix in the runtime table, so "hit" is
    // dispatched as "custom:hit" and never collides with an engine signal name.
    constexpr const char* GRAPH_CUSTOM_EVENT_PREFIX = "custom:";

    QUANTUM_API const char* graphValueTypeName(GraphValueType type);
    QUANTUM_API bool graphValueTypeFromName(const std::string& name, GraphValueType& out);

    // FNV-1a 64: the source checksum stored in the .gdat header so the editor
    // can tell a derived blob from a hand-edited one. Kept in the engine (not
    // the editor's SHA1) so the headless compiler and the runtime agree.
    QUANTUM_API uint64_t graphSourceHash(const void* data, size_t size);

}
}

#endif //GRAPH_TYPES_H
