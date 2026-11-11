// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "model/MeshExport.h"

#include "tiny_gltf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace quantum {
namespace model {

    namespace {

        namespace fs = std::filesystem;

        constexpr uint32_t kInvalid = MeshTopology::INVALID_INDEX;

        std::string number(float value) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.6f", static_cast<double>(value));
            return text;
        }

        bool isFinite(const Vector3& value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool isFinite(const Vector2& value) {
            return std::isfinite(value.x) && std::isfinite(value.y);
        }

        // A name with a space or a newline in it splits into two tokens once it is in the
        // file, and those characters come straight from a text field in the editor.
        std::string safeName(const std::string& name, const std::string& fallback) {
            std::string clean = name;
            for (char& c : clean) {
                if (static_cast<unsigned char>(c) <= static_cast<unsigned char>(' ')) c = '_';
            }
            return clean.empty() ? fallback : clean;
        }

        std::string textOr(const std::string& value, const std::string& fallback) {
            return value.empty() ? fallback : value;
        }

        // One slot even when the caller named none: a mesh with no materials still needs a
        // material in the file, or every viewer invents its own.
        std::vector<ExportMaterial> resolveSlots(const ExportAttribs& attribs, const std::string& objectName) {
            std::vector<ExportMaterial> slots = attribs.materials;
            if (slots.empty()) {
                ExportMaterial material;
                material.name = objectName + "_material";
                slots.push_back(material);
            }
            return slots;
        }

        // The normals a file needs when the mesh never had them computed. recalcNormals()
        // weights each face by its area; this averages the face normals instead, which is the
        // same direction for the uniform meshes an export sees and keeps the kernel's
        // accumulator private. Exporting never mutates the mesh it reads.
        std::vector<Vector3> resolveNormals(const MeshTopology& mesh) {
            if (mesh.normals.size() == mesh.vertices.size() && !mesh.normals.empty()) return mesh.normals;

            std::vector<Vector3> normals(mesh.vertices.size(), Vector3(0.0f, 0.0f, 0.0f));
            for (size_t f = 0; f < mesh.faces.size(); ++f) {
                const Vector3 normal = mesh.faceNormal((uint32_t)f);
                for (uint32_t corner : mesh.faces[f]) {
                    if (corner < normals.size()) normals[corner] += normal;
                }
            }
            for (Vector3& normal : normals) normal.normalize();
            return normals;
        }

        bool checkExportable(const MeshTopology& mesh, const ExportAttribs& attribs, std::string& error) {
            if (mesh.faces.empty()) {
                error = "the mesh has no faces to export";
                return false;
            }
            std::string problem;
            if (!mesh.validate(problem)) {
                error = "the mesh is not exportable: " + problem;
                return false;
            }
            for (size_t i = 0; i < mesh.vertices.size(); ++i) {
                if (!isFinite(mesh.vertices[i])) {
                    error = "vertex " + std::to_string(i) + " is not a finite position";
                    return false;
                }
            }
            if (!attribs.uvs.empty()) {
                if (attribs.uvs.size() != mesh.vertices.size()) {
                    error = "the uv list has " + std::to_string(attribs.uvs.size()) +
                            " pairs but the mesh has " + std::to_string(mesh.vertices.size()) +
                            " vertices";
                    return false;
                }
                for (size_t i = 0; i < attribs.uvs.size(); ++i) {
                    if (!isFinite(attribs.uvs[i])) {
                        error = "uv " + std::to_string(i) + " is not a finite pair";
                        return false;
                    }
                }
            }
            if (!attribs.faceMaterials.empty()) {
                if (attribs.faceMaterials.size() != mesh.faces.size()) {
                    error = "the material list has " + std::to_string(attribs.faceMaterials.size()) +
                            " slots but the mesh has " + std::to_string(mesh.faces.size()) + " faces";
                    return false;
                }
                const size_t slots = attribs.materials.empty() ? 1u : attribs.materials.size();
                for (size_t f = 0; f < attribs.faceMaterials.size(); ++f) {
                    if (attribs.faceMaterials[f] >= slots) {
                        error = "face " + std::to_string(f) + " uses material slot " +
                                std::to_string(attribs.faceMaterials[f]) + " and there are only " +
                                std::to_string(slots);
                        return false;
                    }
                }
            }
            return true;
        }

        // glTF has no polygons: every face becomes (corners - 2) triangles from its first
        // corner, which is the fan MeshTopology::toTriangles uses.
        void appendFan(const std::vector<uint32_t>& corners, uint32_t slot,
                       std::vector<std::vector<uint32_t>>& bySlot) {
            for (size_t i = 2; i < corners.size(); ++i) {
                bySlot[slot].push_back(corners[0]);
                bySlot[slot].push_back(corners[i - 1]);
                bySlot[slot].push_back(corners[i]);
            }
        }

        int addBufferView(tinygltf::Model& model, const void* data, size_t size, int target) {
            std::vector<unsigned char>& bytes = model.buffers[0].data;
            while (bytes.size() % 4 != 0) bytes.push_back(0);
            tinygltf::BufferView view;
            view.buffer = 0;
            view.byteOffset = bytes.size();
            view.byteLength = size;
            view.target = target;
            const unsigned char* source = static_cast<const unsigned char*>(data);
            bytes.insert(bytes.end(), source, source + size);
            model.bufferViews.push_back(view);
            return static_cast<int>(model.bufferViews.size()) - 1;
        }

        int addAccessor(tinygltf::Model& model, int view, int componentType, size_t count, int type) {
            tinygltf::Accessor accessor;
            accessor.bufferView = view;
            accessor.componentType = componentType;
            accessor.count = count;
            accessor.type = type;
            model.accessors.push_back(accessor);
            return static_cast<int>(model.accessors.size()) - 1;
        }

        // `bounds` is required for POSITION and useless for the rest, so it is a flag rather
        // than always paid for.
        int addFloatAccessor(tinygltf::Model& model, const std::vector<float>& values, int type,
                             bool bounds, int componentType) {
            const int components = tinygltf::GetNumComponentsInType(type);
            const size_t count = values.size() / static_cast<size_t>(components);
            const int view = addBufferView(model, values.data(), values.size() * sizeof(float),
                                           TINYGLTF_TARGET_ARRAY_BUFFER);
            const int index = addAccessor(model, view, componentType, count, type);
            if (bounds && count > 0) {
                tinygltf::Accessor& accessor = model.accessors[index];
                accessor.minValues.assign(components, 0.0);
                accessor.maxValues.assign(components, 0.0);
                for (int c = 0; c < components; ++c) {
                    float low = values[c];
                    float high = values[c];
                    for (size_t i = 1; i < count; ++i) {
                        const float value = values[i * components + c];
                        low = std::min(low, value);
                        high = std::max(high, value);
                    }
                    accessor.minValues[c] = low;
                    accessor.maxValues[c] = high;
                }
            }
            return index;
        }

        int addIndexAccessor(tinygltf::Model& model, const std::vector<uint32_t>& indices, bool narrow) {
            if (narrow) {
                std::vector<uint16_t> short_(indices.size());
                for (size_t i = 0; i < indices.size(); ++i) short_[i] = (uint16_t)indices[i];
                const int view = addBufferView(model, short_.data(), short_.size() * sizeof(uint16_t),
                                               TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
                return addAccessor(model, view, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, short_.size(),
                                   TINYGLTF_TYPE_SCALAR);
            }
            const int view = addBufferView(model, indices.data(), indices.size() * sizeof(uint32_t),
                                           TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
            return addAccessor(model, view, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, indices.size(),
                               TINYGLTF_TYPE_SCALAR);
        }

        void addMaterial(tinygltf::Model& model, const ExportMaterial& material) {
            tinygltf::Material entry;
            entry.name = material.name;
            entry.pbrMetallicRoughness.baseColorFactor = {
                static_cast<double>(material.baseColorFactor.x),
                static_cast<double>(material.baseColorFactor.y),
                static_cast<double>(material.baseColorFactor.z),
                static_cast<double>(material.baseColorFactor.w)
            };
            entry.pbrMetallicRoughness.metallicFactor = static_cast<double>(material.metallicFactor);
            entry.pbrMetallicRoughness.roughnessFactor = static_cast<double>(material.roughnessFactor);
            entry.emissiveFactor = {
                static_cast<double>(material.emissiveFactor.x),
                static_cast<double>(material.emissiveFactor.y),
                static_cast<double>(material.emissiveFactor.z)
            };
            entry.doubleSided = material.doubleSided;
            // glTF reads an absent alphaMode as OPAQUE, so a base colour with a real alpha has
            // to say otherwise or every viewer throws the fourth component away.
            if (material.baseColorFactor.w < 1.0f) entry.alphaMode = "BLEND";
            model.materials.push_back(entry);
        }

        // Wavefront has no metallic workflow. The exact glTF values travel as scalars in the
        // PBR extension tags that tinyobjloader documents at tiny_obj_loader.h:224-238
        // (Pr = roughness, Pm = metallic, map_Pr/map_Pm are their textures), while Kd/Ns/Ke
        // below are the fall-back an older viewer shades with. Ns = 2^(10 * roughness
        // complement) is the usual mapping, and the round trip reads Pr/Pm back first.
        void addMtlEntry(std::ostringstream& out, const ExportMaterial& material) {
            const double roughness = std::max(0.0, std::min(1.0, (double)material.roughnessFactor));
            const double shininess = std::pow(2.0, 10.0 * (1.0 - roughness));
            out << "newmtl " << safeName(material.name, "material") << "\n";
            out << "Ka 0.000000 0.000000 0.000000\n";
            out << "Kd " << number(material.baseColorFactor.x) << " "
                << number(material.baseColorFactor.y) << " "
                << number(material.baseColorFactor.z) << "\n";
            out << "Ks 1.000000 1.000000 1.000000\n";
            out << "Ke " << number(material.emissiveFactor.x) << " "
                << number(material.emissiveFactor.y) << " "
                << number(material.emissiveFactor.z) << "\n";
            out << "Ns " << number((float)shininess) << "\n";
            out << "d " << number(material.baseColorFactor.w) << "\n";
            out << "illum 2\n";
            out << "Pr " << number(material.roughnessFactor) << "\n";
            out << "Pm " << number(material.metallicFactor) << "\n";
        }

        bool writeFile(const std::string& path, const void* data, size_t size, std::string& error) {
            const fs::path target(path);
            std::error_code code;
            if (target.has_parent_path()) fs::create_directories(target.parent_path(), code);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) {
                error = "cannot open " + path + " for writing";
                return false;
            }
            out.write(static_cast<const char*>(data), (std::streamsize)size);
            out.close();
            if (!out) {
                error = "the write of " + path + " stopped short";
                return false;
            }
            return true;
        }

    }

    std::string buildObj(const MeshTopology& mesh, const ExportAttribs& attribs,
                         const ExportOptions& options, std::string& mtlText, std::string& error) {
        mtlText.clear();
        if (!checkExportable(mesh, attribs, error)) return std::string();

        const std::string objectName = safeName(textOr(options.name, "mesh"), "mesh");
        const std::vector<ExportMaterial> slots = resolveSlots(attribs, objectName);
        const bool writesMaterials = !attribs.materials.empty() || !attribs.faceMaterials.empty();

        const bool withUVs = options.writeUVs && !attribs.uvs.empty();
        const std::vector<Vector3> normals =
            options.writeNormals ? resolveNormals(mesh) : std::vector<Vector3>();
        const bool withNormals = normals.size() == mesh.vertices.size();

        std::ostringstream out;
        out << "# Wavefront OBJ written by quantum (engine/core/model/MeshExport.cpp)\n";
        out << "o " << objectName << "\n";
        if (writesMaterials) out << "mtllib " << objectName << ".mtl\n";

        for (const Vector3& vertex : mesh.vertices) {
            out << "v " << number(vertex.x) << " " << number(vertex.y) << " " << number(vertex.z) << "\n";
        }
        // Wavefront's V axis points up out of the texture while glTF, every image viewer and
        // the engine's own UVs measure from the top, so the flip lands on the same texel.
        if (withUVs) {
            for (const Vector2& uv : attribs.uvs) {
                out << "vt " << number(uv.x) << " " << number(1.0f - uv.y) << "\n";
            }
        }
        if (withNormals) {
            for (const Vector3& normal : normals) {
                out << "vn " << number(normal.x) << " " << number(normal.y) << " "
                    << number(normal.z) << "\n";
            }
        }

        const int cornerStyle = (withUVs ? 1 : 0) * 2 + (withNormals ? 1 : 0);
        uint32_t previousSlot = kInvalid;
        for (size_t f = 0; f < mesh.faces.size(); ++f) {
            const uint32_t slot = attribs.faceMaterials.empty() ? 0u : attribs.faceMaterials[f];
            if (writesMaterials && slot != previousSlot) {
                out << "usemtl " << safeName(slots[std::min<size_t>(slot, slots.size() - 1u)].name,
                                            "material") << "\n";
                previousSlot = slot;
            }
            out << "f";
            for (uint32_t corner : mesh.faces[f]) {
                const int index = (int)corner + 1;
                out << ' ';
                if (cornerStyle == 3) out << index << '/' << index << '/' << index;
                else if (cornerStyle == 2) out << index << '/' << index;
                else if (cornerStyle == 1) out << index << "//" << index;
                else out << index;
            }
            out << "\n";
        }

        if (writesMaterials) {
            std::ostringstream library;
            library << "# Material library for " << objectName << ".obj, written by quantum\n";
            for (const ExportMaterial& material : slots) addMtlEntry(library, material);
            mtlText = library.str();
        }
        return out.str();
    }

    bool writeObj(const std::string& path, const MeshTopology& mesh, const ExportAttribs& attribs,
                  const ExportOptions& options, std::string& error) {
        std::string mtlText;
        std::string reason;
        const std::string text = buildObj(mesh, attribs, options, mtlText, reason);
        if (text.empty()) {
            error = reason;
            return false;
        }
        if (!writeFile(path, text.data(), text.size(), error)) return false;

        if (!mtlText.empty()) {
            // Named after the object rather than after the file, because that is the name the
            // mtllib line inside the .obj asks for, and loaders resolve it next to the .obj.
            const fs::path library = fs::path(path).parent_path() /
                (safeName(textOr(options.name, "mesh"), "mesh") + ".mtl");
            if (!writeFile(library.string(), mtlText.data(), mtlText.size(), error)) return false;
        }
        return true;
    }

    std::vector<unsigned char> buildGlb(const MeshTopology& mesh, const ExportAttribs& attribs,
                                        const ExportOptions& options, std::string& error) {
        if (!checkExportable(mesh, attribs, error)) return {};

        const std::string objectName = safeName(textOr(options.name, "mesh"), "mesh");
        const std::vector<ExportMaterial> slots = resolveSlots(attribs, objectName);
        const bool withUVs = options.writeUVs && !attribs.uvs.empty();
        const std::vector<Vector3> normals =
            options.writeNormals ? resolveNormals(mesh) : std::vector<Vector3>();
        const bool withNormals = normals.size() == mesh.vertices.size();

        tinygltf::Model model;
        model.asset.version = "2.0";
        model.asset.generator = "quantum (engine/core/model/MeshExport.cpp)";
        model.buffers.push_back(tinygltf::Buffer());
        model.buffers[0].name = objectName + "_buffer";

        std::vector<float> positions(mesh.vertices.size() * 3u);
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            positions[i * 3u] = mesh.vertices[i].x;
            positions[i * 3u + 1] = mesh.vertices[i].y;
            positions[i * 3u + 2] = mesh.vertices[i].z;
        }
        const int positionAccessor = addFloatAccessor(model, positions, TINYGLTF_TYPE_VEC3, true,
                                                     TINYGLTF_COMPONENT_TYPE_FLOAT);

        int normalAccessor = -1;
        if (withNormals) {
            std::vector<float> data(normals.size() * 3u);
            for (size_t i = 0; i < normals.size(); ++i) {
                data[i * 3u] = normals[i].x;
                data[i * 3u + 1] = normals[i].y;
                data[i * 3u + 2] = normals[i].z;
            }
            normalAccessor = addFloatAccessor(model, data, TINYGLTF_TYPE_VEC3, false,
                                              TINYGLTF_COMPONENT_TYPE_FLOAT);
        }

        int uvAccessor = -1;
        if (withUVs) {
            std::vector<float> data(attribs.uvs.size() * 2u);
            for (size_t i = 0; i < attribs.uvs.size(); ++i) {
                data[i * 2u] = attribs.uvs[i].x;
                data[i * 2u + 1] = attribs.uvs[i].y;
            }
            uvAccessor = addFloatAccessor(model, data, TINYGLTF_TYPE_VEC2, false,
                                          TINYGLTF_COMPONENT_TYPE_FLOAT);
        }

        std::vector<std::vector<uint32_t>> bySlot(slots.size());
        for (size_t f = 0; f < mesh.faces.size(); ++f) {
            const uint32_t slot = attribs.faceMaterials.empty() ? 0u : attribs.faceMaterials[f];
            appendFan(mesh.faces[f], std::min<uint32_t>(slot, (uint32_t)slots.size() - 1u), bySlot);
        }

        // A quantum primitive index buffer is 16-bit, so a mesh that needs more than that is
        // still exported as valid glTF but warned about by whoever reads it back.
        const bool narrow = mesh.vertices.size() < 65536u;

        tinygltf::Mesh entry;
        entry.name = objectName;
        for (size_t s = 0; s < slots.size(); ++s) {
            if (bySlot[s].empty()) continue;
            addMaterial(model, slots[s]);

            tinygltf::Primitive primitive;
            primitive.mode = TINYGLTF_MODE_TRIANGLES;
            primitive.attributes["POSITION"] = positionAccessor;
            if (normalAccessor >= 0) primitive.attributes["NORMAL"] = normalAccessor;
            if (uvAccessor >= 0) primitive.attributes["TEXCOORD_0"] = uvAccessor;
            primitive.indices = addIndexAccessor(model, bySlot[s], narrow);
            primitive.material = (int)model.materials.size() - 1;
            entry.primitives.push_back(primitive);
        }
        model.meshes.push_back(entry);

        tinygltf::Node node;
        node.name = objectName;
        node.mesh = 0;
        model.nodes.push_back(node);

        tinygltf::Scene scene;
        scene.name = objectName;
        scene.nodes.push_back(0);
        model.scenes.push_back(scene);
        model.defaultScene = 0;

        std::vector<unsigned char>& bytes = model.buffers[0].data;
        while (bytes.size() % 4 != 0) bytes.push_back(0);

        tinygltf::TinyGLTF writer;
        std::ostringstream stream;
        if (!writer.WriteGltfSceneToStream(&model, stream, false, true)) {
            error = "the glTF writer refused the document";
            return {};
        }
        const std::string data = stream.str();
        return std::vector<unsigned char>(data.begin(), data.end());
    }

    bool writeGlb(const std::string& path, const MeshTopology& mesh, const ExportAttribs& attribs,
                  const ExportOptions& options, std::string& error) {
        std::string reason;
        const std::vector<unsigned char> bytes = buildGlb(mesh, attribs, options, reason);
        if (bytes.empty()) {
            error = reason;
            return false;
        }
        return writeFile(path, bytes.data(), bytes.size(), error);
    }

}
}
