// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "GraphReader.h"

#include <cstring>
#include <fstream>

namespace quantum {
namespace graph {

    namespace {

        class ByteReader {
        public:
            ByteReader(const std::vector<uint8_t>& data, size_t start)
                : data(data), pos(start) {
            }

            bool u8(uint8_t& value) {
                if (pos + 1 > data.size()) return false;
                value = data[pos++];
                return true;
            }

            bool u16(uint16_t& value) {
                if (pos + 2 > data.size()) return false;
                value = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
                pos += 2;
                return true;
            }

            bool u32(uint32_t& value) {
                if (pos + 4 > data.size()) return false;
                value = static_cast<uint32_t>(data[pos]) |
                        (static_cast<uint32_t>(data[pos + 1]) << 8) |
                        (static_cast<uint32_t>(data[pos + 2]) << 16) |
                        (static_cast<uint32_t>(data[pos + 3]) << 24);
                pos += 4;
                return true;
            }

            bool u64(uint64_t& value) {
                if (pos + 8 > data.size()) return false;
                value = 0;
                for (int i = 0; i < 8; ++i) {
                    value |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
                }
                pos += 8;
                return true;
            }

            bool f32(float& value) {
                uint32_t bits = 0;
                if (!u32(bits)) return false;
                std::memcpy(&value, &bits, sizeof(value));
                return true;
            }

            bool f64(double& value) {
                uint64_t bits = 0;
                if (!u64(bits)) return false;
                std::memcpy(&value, &bits, sizeof(value));
                return true;
            }

            bool poolString(uint32_t offset, uint32_t length, uint32_t poolSize,
                            size_t poolBase, std::string& out) {
                if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(length) > poolSize) {
                    return false;
                }
                if (poolBase + offset + length > data.size()) {
                    return false;
                }
                out.assign(reinterpret_cast<const char*>(&data[poolBase + offset]), length);
                return true;
            }

            size_t getPos() const { return pos; }

        private:
            const std::vector<uint8_t>& data;
            size_t pos;
        };

        bool readString(ByteReader& reader, uint32_t poolSize, size_t poolBase,
                        std::string& out) {
            uint32_t offset = 0;
            uint32_t length = 0;
            if (!reader.u32(offset) || !reader.u32(length)) {
                return false;
            }
            return reader.poolString(offset, length, poolSize, poolBase, out);
        }

        bool readValue(ByteReader& reader, uint32_t poolSize, size_t poolBase,
                       GraphValue& value) {
            uint8_t tag = 0;
            if (!reader.u8(tag)) {
                return false;
            }
            value = GraphValue();
            GraphValueType type = static_cast<GraphValueType>(tag);
            switch (type) {
                case GraphValueType::None:
                    return true;
                case GraphValueType::Bool: {
                    uint8_t raw = 0;
                    if (!reader.u8(raw)) return false;
                    value = GraphValue::makeBool(raw != 0);
                    return true;
                }
                case GraphValueType::Int: {
                    uint64_t raw = 0;
                    if (!reader.u64(raw)) return false;
                    value = GraphValue::makeInt(static_cast<int64_t>(raw));
                    return true;
                }
                case GraphValueType::Float: {
                    double raw = 0.0;
                    if (!reader.f64(raw)) return false;
                    value = GraphValue::makeFloat(raw);
                    return true;
                }
                case GraphValueType::String: {
                    std::string text;
                    if (!readString(reader, poolSize, poolBase, text)) return false;
                    value = GraphValue::makeString(text);
                    return true;
                }
                case GraphValueType::Vector2: {
                    float x = 0.0f, y = 0.0f;
                    if (!reader.f32(x) || !reader.f32(y)) return false;
                    value = GraphValue::makeVector2(Vector2(x, y));
                    return true;
                }
                case GraphValueType::Vector3:
                case GraphValueType::Color3: {
                    float x = 0.0f, y = 0.0f, z = 0.0f;
                    if (!reader.f32(x) || !reader.f32(y) || !reader.f32(z)) return false;
                    value = type == GraphValueType::Color3
                        ? GraphValue::makeColor3(Vector3(x, y, z))
                        : GraphValue::makeVector3(Vector3(x, y, z));
                    return true;
                }
                case GraphValueType::Vector4:
                case GraphValueType::Color4: {
                    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
                    if (!reader.f32(x) || !reader.f32(y) || !reader.f32(z) || !reader.f32(w)) return false;
                    value = type == GraphValueType::Color4
                        ? GraphValue::makeColor4(Vector4(x, y, z, w))
                        : GraphValue::makeVector4(Vector4(x, y, z, w));
                    return true;
                }
                case GraphValueType::Quaternion: {
                    float w = 0.0f, x = 0.0f, y = 0.0f, z = 0.0f;
                    if (!reader.f32(w) || !reader.f32(x) || !reader.f32(y) || !reader.f32(z)) return false;
                    value = GraphValue::makeQuaternion(Quaternion(w, x, y, z));
                    return true;
                }
                case GraphValueType::Rect: {
                    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
                    if (!reader.f32(x) || !reader.f32(y) || !reader.f32(width) || !reader.f32(height)) return false;
                    value = GraphValue::makeRect(Rect(x, y, width, height));
                    return true;
                }
                case GraphValueType::Entity: {
                    uint32_t raw = 0;
                    if (!reader.u32(raw)) return false;
                    value = GraphValue::makeEntity(static_cast<Entity>(raw));
                    return true;
                }
                case GraphValueType::Any:
                    return false;
            }
            return false;
        }

    }

    bool GraphReader::read(const std::vector<uint8_t>& data, GraphAsset& out,
                           GraphBlobInfo& info, std::string& error) {
        out = GraphAsset();
        info = GraphBlobInfo();
        error.clear();

        const size_t headerSize = 52;
        if (data.size() < headerSize) {
            error = "graph blob is truncated";
            return false;
        }
        if (std::memcmp(data.data(), GRAPH_BLOB_MAGIC, 4) != 0) {
            error = "not a graph blob (bad magic)";
            return false;
        }

        ByteReader header(data, 4);
        uint16_t version = 0;
        uint16_t flags = 0;
        uint32_t blobSize = 0;
        uint64_t sourceHash = 0;
        uint32_t nameOffset = 0;
        uint32_t nameLength = 0;
        uint32_t poolSize = 0;
        uint32_t variableCount = 0;
        uint32_t nodeCount = 0;
        uint32_t edgeCount = 0;
        uint32_t eventCount = 0;
        uint32_t commentCount = 0;

        if (!header.u16(version) || !header.u16(flags) || !header.u32(blobSize) ||
            !header.u64(sourceHash) || !header.u32(nameOffset) || !header.u32(nameLength) ||
            !header.u32(poolSize) || !header.u32(variableCount) || !header.u32(nodeCount) ||
            !header.u32(edgeCount) || !header.u32(eventCount) ||
            !header.u32(commentCount)) {
            error = "graph blob header is truncated";
            return false;
        }

        if (version != GRAPH_SCHEMA_VERSION) {
            error = "unsupported graph blob version " + std::to_string(version) +
                    " (expected " + std::to_string(GRAPH_SCHEMA_VERSION) + ")";
            return false;
        }
        if (flags != 0) {
            error = "unsupported graph blob flags";
            return false;
        }
        if (blobSize != data.size()) {
            error = "graph blob size does not match its header";
            return false;
        }
        if (static_cast<uint64_t>(headerSize) + poolSize > data.size()) {
            error = "graph string pool is out of bounds";
            return false;
        }

        const size_t poolBase = headerSize;
        ByteReader reader(data, headerSize + poolSize);

        out.version = version;
        {
            ByteReader nameReader(data, 0);
            if (!nameReader.poolString(nameOffset, nameLength, poolSize, poolBase, out.name)) {
                error = "graph name is out of bounds";
                return false;
            }
        }
        out.variables.clear();
        out.nodes.clear();
        out.edges.clear();
        out.events.clear();
        out.comments.clear();

        for (uint32_t i = 0; i < variableCount; ++i) {
            GraphVariable variable;
            uint8_t type = 0;
            uint8_t exposed = 0;
            uint16_t reserved = 0;
            if (!readString(reader, poolSize, poolBase, variable.name) ||
                !readString(reader, poolSize, poolBase, variable.displayName) ||
                !reader.u8(type) || !reader.u8(exposed) || !reader.u16(reserved)) {
                error = "truncated variable table";
                return false;
            }
            if (type > static_cast<uint8_t>(GraphValueType::Any)) {
                error = "invalid variable type";
                return false;
            }
            variable.type = static_cast<GraphValueType>(type);
            variable.exposed = exposed != 0;
            if (!readValue(reader, poolSize, poolBase, variable.defaultValue)) {
                error = "truncated variable default value";
                return false;
            }
            out.variables.push_back(variable);
        }

        for (uint32_t i = 0; i < nodeCount; ++i) {
            GraphNode node;
            uint32_t paramCount = 0;
            if (!reader.u32(node.id) ||
                !readString(reader, poolSize, poolBase, node.kind) ||
                !readString(reader, poolSize, poolBase, node.title) ||
                !reader.f32(node.x) || !reader.f32(node.y) ||
                !reader.u32(paramCount)) {
                error = "truncated node table";
                return false;
            }
            if (paramCount > data.size()) {
                error = "invalid parameter count";
                return false;
            }
            for (uint32_t p = 0; p < paramCount; ++p) {
                GraphParam param;
                if (!readString(reader, poolSize, poolBase, param.name) ||
                    !readValue(reader, poolSize, poolBase, param.value)) {
                    error = "truncated node parameter";
                    return false;
                }
                node.params.push_back(param);
            }
            out.nodes.push_back(node);
        }

        for (uint32_t i = 0; i < edgeCount; ++i) {
            GraphEdge edge;
            if (!reader.u32(edge.fromNode) ||
                !readString(reader, poolSize, poolBase, edge.fromPin) ||
                !reader.u32(edge.toNode) ||
                !readString(reader, poolSize, poolBase, edge.toPin)) {
                error = "truncated edge table";
                return false;
            }
            out.edges.push_back(edge);
        }

        for (uint32_t i = 0; i < eventCount; ++i) {
            GraphEventBinding binding;
            if (!readString(reader, poolSize, poolBase, binding.id) ||
                !reader.u32(binding.entryNode)) {
                error = "truncated event table";
                return false;
            }
            out.events.push_back(binding);
        }

        for (uint32_t i = 0; i < commentCount; ++i) {
            GraphComment comment;
            if (!reader.f32(comment.x) || !reader.f32(comment.y) ||
                !reader.f32(comment.width) || !reader.f32(comment.height) ||
                !readString(reader, poolSize, poolBase, comment.text)) {
                error = "truncated comment table";
                return false;
            }
            out.comments.push_back(comment);
        }

        if (reader.getPos() != data.size()) {
            error = "graph blob has trailing data";
            return false;
        }

        info.version = version;
        info.flags = flags;
        info.blobSize = blobSize;
        info.sourceHash = sourceHash;
        return true;
    }

    bool GraphReader::readFile(const std::string& path, GraphAsset& out,
                               GraphBlobInfo& info, std::string& error) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            error = "cannot open '" + path + "' for reading";
            return false;
        }
        std::streamsize size = file.tellg();
        if (size < 0) {
            error = "cannot read '" + path + "'";
            return false;
        }
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> data(static_cast<size_t>(size));
        if (size > 0) {
            file.read(reinterpret_cast<char*>(data.data()), size);
            if (!file.good()) {
                error = "failed to read '" + path + "'";
                return false;
            }
        }
        return read(data, out, info, error);
    }

}
}
