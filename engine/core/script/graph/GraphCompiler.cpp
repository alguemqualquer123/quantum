// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "GraphCompiler.h"

#include <cstring>
#include <fstream>
#include <map>

namespace quantum {
namespace graph {

    namespace {

        class ByteWriter {
        public:
            std::vector<uint8_t> data;

            void u8(uint8_t value) {
                data.push_back(value);
            }

            void u16(uint16_t value) {
                data.push_back(static_cast<uint8_t>(value & 0xff));
                data.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
            }

            void u32(uint32_t value) {
                for (int i = 0; i < 4; ++i) {
                    data.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xff));
                }
            }

            void u64(uint64_t value) {
                for (int i = 0; i < 8; ++i) {
                    data.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xff));
                }
            }

            void f32(float value) {
                uint32_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                u32(bits);
            }

            void f64(double value) {
                uint64_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                u64(bits);
            }

            void patchU32(size_t offset, uint32_t value) {
                for (int i = 0; i < 4; ++i) {
                    data[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
                }
            }
        };

        class StringPool {
        public:
            std::string data;
            std::map<std::string, std::pair<uint32_t, uint32_t>> refs;

            void add(const std::string& value) {
                if (refs.count(value)) {
                    return;
                }
                std::pair<uint32_t, uint32_t> ref(static_cast<uint32_t>(data.size()),
                                                  static_cast<uint32_t>(value.size()));
                refs[value] = ref;
                data += value;
            }

            bool writeRef(ByteWriter& writer, const std::string& value, std::string& error) const {
                std::map<std::string, std::pair<uint32_t, uint32_t>>::const_iterator it = refs.find(value);
                if (it == refs.end()) {
                    error = "internal error: string missing from pool";
                    return false;
                }
                writer.u32(it->second.first);
                writer.u32(it->second.second);
                return true;
            }
        };

        void collectValue(StringPool& pool, const GraphValue& value) {
            if (value.type == GraphValueType::String) {
                pool.add(value.asString());
            }
        }

        bool writeValue(ByteWriter& writer, const StringPool& pool, const GraphValue& value,
                        std::string& error) {
            writer.u8(static_cast<uint8_t>(value.type));
            switch (value.type) {
                case GraphValueType::None:
                    return true;
                case GraphValueType::Bool:
                    writer.u8(value.asBool() ? 1 : 0);
                    return true;
                case GraphValueType::Int:
                    writer.u64(static_cast<uint64_t>(value.asInt()));
                    return true;
                case GraphValueType::Float:
                    writer.f64(value.asFloat());
                    return true;
                case GraphValueType::String:
                    return pool.writeRef(writer, value.asString(), error);
                case GraphValueType::Vector2: {
                    Vector2 v = value.asVector2();
                    writer.f32(v.x);
                    writer.f32(v.y);
                    return true;
                }
                case GraphValueType::Vector3:
                case GraphValueType::Color3: {
                    Vector3 v = value.asVector3();
                    writer.f32(v.x);
                    writer.f32(v.y);
                    writer.f32(v.z);
                    return true;
                }
                case GraphValueType::Vector4:
                case GraphValueType::Color4: {
                    Vector4 v = value.asVector4();
                    writer.f32(v.x);
                    writer.f32(v.y);
                    writer.f32(v.z);
                    writer.f32(v.w);
                    return true;
                }
                case GraphValueType::Quaternion: {
                    Quaternion q = value.asQuaternion();
                    writer.f32(q.w);
                    writer.f32(q.x);
                    writer.f32(q.y);
                    writer.f32(q.z);
                    return true;
                }
                case GraphValueType::Rect: {
                    Rect r = value.asRect();
                    writer.f32(r.getX());
                    writer.f32(r.getY());
                    writer.f32(r.getWidth());
                    writer.f32(r.getHeight());
                    return true;
                }
                case GraphValueType::Entity:
                    writer.u32(static_cast<uint32_t>(value.asEntity()));
                    return true;
                case GraphValueType::Any:
                    error = "value of type 'any' cannot be serialized";
                    return false;
            }
            error = "unknown value type";
            return false;
        }

        void collectStrings(const GraphAsset& asset, StringPool& pool) {
            pool.add(asset.name);
            for (const GraphVariable& variable : asset.variables) {
                pool.add(variable.name);
                pool.add(variable.displayName);
                collectValue(pool, variable.defaultValue);
            }
            for (const GraphNode& node : asset.nodes) {
                pool.add(node.kind);
                pool.add(node.title);
                for (const GraphParam& param : node.params) {
                    pool.add(param.name);
                    collectValue(pool, param.value);
                }
            }
            for (const GraphEdge& edge : asset.edges) {
                pool.add(edge.fromPin);
                pool.add(edge.toPin);
            }
            for (const GraphEventBinding& binding : asset.events) {
                pool.add(binding.id);
            }
            for (const GraphComment& comment : asset.comments) {
                pool.add(comment.text);
            }
        }

    }

    bool GraphCompiler::compile(const GraphAsset& asset, uint64_t sourceHash,
                                std::vector<uint8_t>& out, std::string& error) {
        out.clear();
        error.clear();

        if (asset.version != GRAPH_SCHEMA_VERSION) {
            error = "unsupported graph schema version";
            return false;
        }

        StringPool pool;
        collectStrings(asset, pool);

        ByteWriter writer;
        const uint8_t* magic = GRAPH_BLOB_MAGIC;
        for (int i = 0; i < 4; ++i) {
            writer.u8(magic[i]);
        }
        writer.u16(static_cast<uint16_t>(GRAPH_SCHEMA_VERSION));
        writer.u16(0);
        writer.u32(0);
        writer.u64(sourceHash);
        if (!pool.writeRef(writer, asset.name, error)) {
            return false;
        }
        writer.u32(static_cast<uint32_t>(pool.data.size()));
        writer.u32(static_cast<uint32_t>(asset.variables.size()));
        writer.u32(static_cast<uint32_t>(asset.nodes.size()));
        writer.u32(static_cast<uint32_t>(asset.edges.size()));
        writer.u32(static_cast<uint32_t>(asset.events.size()));
        writer.u32(static_cast<uint32_t>(asset.comments.size()));

        writer.data.insert(writer.data.end(), pool.data.begin(), pool.data.end());

        for (const GraphVariable& variable : asset.variables) {
            if (!pool.writeRef(writer, variable.name, error) ||
                !pool.writeRef(writer, variable.displayName, error)) {
                return false;
            }
            writer.u8(static_cast<uint8_t>(variable.type));
            writer.u8(variable.exposed ? 1 : 0);
            writer.u16(0);
            if (!writeValue(writer, pool, variable.defaultValue, error)) {
                return false;
            }
        }

        for (const GraphNode& node : asset.nodes) {
            writer.u32(node.id);
            if (!pool.writeRef(writer, node.kind, error) ||
                !pool.writeRef(writer, node.title, error)) {
                return false;
            }
            writer.f32(node.x);
            writer.f32(node.y);
            writer.u32(static_cast<uint32_t>(node.params.size()));
            for (const GraphParam& param : node.params) {
                if (!pool.writeRef(writer, param.name, error) ||
                    !writeValue(writer, pool, param.value, error)) {
                    return false;
                }
            }
        }

        for (const GraphEdge& edge : asset.edges) {
            writer.u32(edge.fromNode);
            if (!pool.writeRef(writer, edge.fromPin, error)) {
                return false;
            }
            writer.u32(edge.toNode);
            if (!pool.writeRef(writer, edge.toPin, error)) {
                return false;
            }
        }

        for (const GraphEventBinding& binding : asset.events) {
            if (!pool.writeRef(writer, binding.id, error)) {
                return false;
            }
            writer.u32(binding.entryNode);
        }

        for (const GraphComment& comment : asset.comments) {
            writer.f32(comment.x);
            writer.f32(comment.y);
            writer.f32(comment.width);
            writer.f32(comment.height);
            if (!pool.writeRef(writer, comment.text, error)) {
                return false;
            }
        }

        writer.patchU32(8, static_cast<uint32_t>(writer.data.size()));
        out.swap(writer.data);
        return true;
    }

    bool GraphCompiler::writeFile(const std::string& path, const GraphAsset& asset,
                                  uint64_t sourceHash, std::string& error) {
        std::vector<uint8_t> blob;
        if (!compile(asset, sourceHash, blob, error)) {
            return false;
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            error = "cannot open '" + path + "' for writing";
            return false;
        }
        file.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
        if (!file.good()) {
            error = "failed to write '" + path + "'";
            return false;
        }
        return true;
    }

}
}
