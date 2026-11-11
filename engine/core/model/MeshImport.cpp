// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "model/MeshImport.h"

#include "tiny_gltf.h"
#include "tiny_obj_loader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace quantum {
namespace model {

    namespace {

        namespace fs = std::filesystem;

        // A float that decides whether two corners are the same point. 0.0 and -0.0 have to
        // agree or a vertex that only crossed a sign bit splits in two - the same rule
        // MeshTopology::positionKey follows, and the reason a weld here is exact rather than
        // measured: an epsilon would close the gap a UV seam deliberately leaves open.
        uint32_t bitKey(float value) {
            if (value == 0.0f) return 0u;
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return bits;
        }

        // The identity of a corner: its position, its texture coordinate and its normal, with a
        // pattern no real value reaches standing in for a channel the file did not store. Two
        // corners that agree on all three are one vertex; two that differ only in UV are two,
        // which is what a seam is and what a renderer needs anyway.
        //
        // It is keyed on the values and not on the file's own indices because glTF indexes each
        // attribute accessor separately, so element 5 of one primitive's positions and element 5
        // of another's are different points that an index key would weld into one.
        using CornerKey = std::array<uint32_t, 8>;

        CornerKey keyOf(const Vector3& position, const Vector2* uv, const Vector3* normal) {
            CornerKey key = { bitKey(position.x), bitKey(position.y), bitKey(position.z),
                              0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
            if (uv) {
                key[3] = bitKey(uv->x);
                key[4] = bitKey(uv->y);
            }
            if (normal) {
                key[5] = bitKey(normal->x);
                key[6] = bitKey(normal->y);
                key[7] = bitKey(normal->z);
            }
            return key;
        }

        // One corner as a reader sees it, before anything is welded.
        struct FileCorner {
            Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
            Vector2 uv = Vector2(0.0f, 0.0f);
            Vector3 normal = Vector3(0.0f, 0.0f, 0.0f);
            bool hasUV = false;
            bool hasNormal = false;
            CornerKey key = {};

            const Vector2* uvPointer() const { return hasUV ? &uv : nullptr; }
            const Vector3* normalPointer() const { return hasNormal ? &normal : nullptr; }
        };

        struct Welder {
            MeshTopology topology;
            std::vector<Vector2> uvs;
            std::vector<Vector3> normals;
            std::map<CornerKey, uint32_t> slots;
            size_t withoutUV = 0;
            size_t withoutNormal = 0;

            // A half-filled channel is worse than an empty one: the exporter derives a missing
            // normal from the faces for every vertex, and a list with holes would shade the
            // corners the file did not describe flat instead of like the rest of the mesh.
            //
            // The faces are built here and not in the caller's model because this assigns the
            // whole topology: a reader that had already appended faces to `out` would have them
            // erased from under the material slot list that describes them.
            void finish(ImportedModel& out) const {
                out.topology = topology;
                out.attribs.uvs.clear();
                if (withoutNormal == 0) out.topology.normals = normals;
                else out.topology.recalcNormals();
                if (withoutUV < topology.vertices.size()) out.attribs.uvs = uvs;
            }

            uint32_t add(const FileCorner& corner) {
                auto found = slots.find(corner.key);
                if (found != slots.end()) return found->second;
                const uint32_t slot = topology.addVertex(corner.position);
                slots.insert({ corner.key, slot });
                uvs.push_back(corner.uv);
                normals.push_back(corner.normal);
                if (!corner.hasUV) ++withoutUV;
                if (!corner.hasNormal) ++withoutNormal;
                return slot;
            }
        };

        // A face is accepted or refused whole, before any of its corners is welded, because the
        // material slots have to stay parallel to the faces and MeshTopology cannot drop a face
        // without shifting the list that describes it. Two corners that are the same vertex leave
        // the face with no area, so they are refused here rather than pruned later.
        void buildFace(const std::vector<FileCorner>& corners, std::vector<uint32_t>& face,
                       Welder& welder) {
            face.clear();
            face.reserve(corners.size());
            for (const FileCorner& corner : corners) face.push_back(welder.add(corner));
        }

        bool collectFace(const std::vector<FileCorner>& corners, std::vector<FileCorner>& kept) {
            kept.clear();
            for (const FileCorner& corner : corners) {
                if (std::any_of(kept.begin(), kept.end(),
                                [&corner](const FileCorner& other) { return other.key == corner.key; })) {
                    return false;
                }
                kept.push_back(corner);
            }
            return kept.size() >= 3;
        }

        float componentOf(const std::vector<double>& values, size_t index, float fallback) {
            return index < values.size() ? (float)values[index] : fallback;
        }

        std::string lower(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            return text;
        }

        std::string fileExtension(const std::string& path) {
            std::string ext = lower(fs::path(path).extension().string());
            if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
            return ext;
        }

        std::string stemOf(const std::string& path) {
            const std::string stem = fs::path(path).stem().string();
            return stem.empty() ? std::string("mesh") : stem;
        }

        // A file that names nothing still has to produce a mesh the editor can put in a list.
        std::string textOr(const std::string& value, const std::string& fallback) {
            return value.empty() ? fallback : value;
        }

        std::string countWarning(size_t count, const std::string& what) {
            return std::to_string(count) + " " + what;
        }

        bool readText(const std::string& path, std::string& text, std::string& error) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                error = "cannot open " + path + " for reading";
                return false;
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            text = buffer.str();
            return true;
        }

        ExportMaterial toMaterial(const tinyobj::material_t& source) {
            ExportMaterial material;
            material.name = source.name.empty() ? std::string("material") : source.name;
            material.baseColorFactor = Vector4(source.diffuse[0], source.diffuse[1], source.diffuse[2],
                                               source.dissolve);
            material.emissiveFactor = Vector3(source.emission[0], source.emission[1], source.emission[2]);
            // This fork parses the PBR extension tags into their own fields (Pr at
            // tiny_obj_loader.h:2270, Pm at :2277), so they are read there and not as text. An
            // absent Pr leaves the field at zero, which is indistinguishable from a declared one,
            // so the Blinn exponent wins whenever the file carried it: our own writer derives Ns
            // from the same roughness, and the two answers agree to the last digit.
            if (source.roughness > 0.0f) {
                material.roughnessFactor = std::max(0.0f, std::min(1.0f, source.roughness));
            } else if (source.shininess > 0.0f) {
                const double exponent = std::log2((double)source.shininess) / 10.0;
                material.roughnessFactor = std::max(0.0f, std::min(1.0f, (float)(1.0 - exponent)));
            }
            material.metallicFactor = std::max(0.0f, std::min(1.0f, source.metallic));
            return material;
        }

        ExportMaterial toMaterial(const tinygltf::Material& source, bool& masked) {
            ExportMaterial material;
            material.name = source.name.empty() ? std::string("material") : source.name;
            const std::vector<double>& color = source.pbrMetallicRoughness.baseColorFactor;
            material.baseColorFactor = Vector4(componentOf(color, 0, 1.0f), componentOf(color, 1, 1.0f),
                                               componentOf(color, 2, 1.0f), componentOf(color, 3, 1.0f));
            material.metallicFactor = (float)source.pbrMetallicRoughness.metallicFactor;
            material.roughnessFactor = (float)source.pbrMetallicRoughness.roughnessFactor;
            material.emissiveFactor = Vector3(componentOf(source.emissiveFactor, 0, 0.0f),
                                              componentOf(source.emissiveFactor, 1, 0.0f),
                                              componentOf(source.emissiveFactor, 2, 0.0f));
            material.doubleSided = source.doubleSided;
            // glTF spells out how a surface blends and a file-level material has only an alpha, so
            // the two non-opaque modes collapse onto it: OPAQUE ignores the factor, as the spec
            // says, and MASK keeps its cutoff, which is the nearest thing to it that survives.
            masked = false;
            if (source.alphaMode == "MASK") {
                material.baseColorFactor.w = (float)source.alphaCutoff;
                masked = true;
            } else if (source.alphaMode == "OPAQUE") {
                material.baseColorFactor.w = 1.0f;
            }
            return material;
        }

        bool hasTexture(const tinygltf::Material& source) {
            return source.pbrMetallicRoughness.baseColorTexture.index >= 0 ||
                source.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0 ||
                source.emissiveTexture.index >= 0 ||
                source.normalTexture.index >= 0 ||
                source.occlusionTexture.index >= 0;
        }

        const tinygltf::Accessor* accessorOf(const tinygltf::Model& model, int index,
                                             std::string& reason) {
            if (index < 0 || index >= (int)model.accessors.size()) {
                reason = "it refers to accessor " + std::to_string(index) + " and the file has " +
                    std::to_string(model.accessors.size());
                return nullptr;
            }
            return &model.accessors[index];
        }

        int attributeOf(const tinygltf::Primitive& primitive, const char* name) {
            auto found = primitive.attributes.find(name);
            return found == primitive.attributes.end() ? -1 : found->second;
        }

        // One accessor's floats, with the buffer view's own stride and the component count of its
        // type both taken from the file. A read that would run past the buffer is an error and not
        // a clamp: a truncated file has to fail rather than invent positions.
        bool readFloats(const tinygltf::Model& model, const tinygltf::Accessor& accessor,
                        size_t needed, std::vector<float>& values, std::string& reason) {
            if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
                reason = "a float attribute cannot be component type " +
                    std::to_string(accessor.componentType);
                return false;
            }
            const size_t available = (size_t)tinygltf::GetNumComponentsInType(accessor.type);
            if (available < needed) {
                reason = "the accessor has " + std::to_string(available) + " components and " +
                    std::to_string(needed) + " are needed";
                return false;
            }
            if (accessor.bufferView < 0 || accessor.bufferView >= (int)model.bufferViews.size()) {
                reason = "the accessor has no buffer view to read";
                return false;
            }
            const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
            if (view.buffer < 0 || view.buffer >= (int)model.buffers.size()) {
                reason = "the buffer view points at buffer " + std::to_string(view.buffer) +
                    " and the file has " + std::to_string(model.buffers.size());
                return false;
            }
            const std::vector<unsigned char>& data = model.buffers[view.buffer].data;
            const size_t packed = sizeof(float) * available;
            const size_t stride = view.byteStride >= packed ? view.byteStride : packed;

            values.assign(accessor.count * needed, 0.0f);
            for (size_t i = 0; i < accessor.count; ++i) {
                for (size_t c = 0; c < needed; ++c) {
                    const size_t at = view.byteOffset + accessor.byteOffset + i * stride +
                        c * sizeof(float);
                    if (at + sizeof(float) > data.size()) {
                        reason = "the accessor runs past the end of its buffer at element " +
                            std::to_string(i);
                        return false;
                    }
                    float value = 0.0f;
                    std::memcpy(&value, data.data() + at, sizeof(value));
                    values[i * needed + c] = value;
                }
            }
            return true;
        }

        bool readIndices(const tinygltf::Model& model, const tinygltf::Accessor& accessor,
                         std::vector<uint32_t>& indices, std::string& reason) {
            size_t element = 0;
            if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) element = 1;
            else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) element = 2;
            else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) element = 4;
            else {
                reason = "an index accessor cannot be component type " +
                    std::to_string(accessor.componentType);
                return false;
            }
            if (accessor.type != TINYGLTF_TYPE_SCALAR) {
                reason = "the index accessor is not a scalar";
                return false;
            }
            if (accessor.bufferView < 0 || accessor.bufferView >= (int)model.bufferViews.size()) {
                reason = "the index accessor has no buffer view to read";
                return false;
            }
            const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
            if (view.buffer < 0 || view.buffer >= (int)model.buffers.size()) {
                reason = "the index buffer view points outside the file's buffers";
                return false;
            }
            const std::vector<unsigned char>& data = model.buffers[view.buffer].data;
            const size_t stride = view.byteStride >= element ? view.byteStride : element;

            indices.assign(accessor.count, 0u);
            for (size_t i = 0; i < accessor.count; ++i) {
                const size_t at = view.byteOffset + accessor.byteOffset + i * stride;
                if (at + element > data.size()) {
                    reason = "the index accessor runs past the end of its buffer at element " +
                        std::to_string(i);
                    return false;
                }
                uint32_t value = 0;
                // glTF buffers are little-endian, which is every platform this engine builds on.
                for (size_t b = 0; b < element; ++b) value |= (uint32_t)data[at + b] << (8u * b);
                indices[i] = value;
            }
            return true;
        }

        bool fromGltf(const std::string& name, const tinygltf::Model& model, ImportedModel& out,
                      std::string& error) {
            if (model.meshes.empty()) {
                error = "the glTF has no mesh";
                return false;
            }

            Welder welder;
            // Slots are numbered in the order the file's materials are first drawn with, so a
            // material no primitive uses does not become an empty slot of the imported mesh.
            std::map<int, uint32_t> slotByMaterial;
            int defaultSlot = -1;
            size_t skippedPrimitives = 0;
            size_t skippedAttributes = 0;
            size_t morphed = 0;
            size_t textured = 0;
            size_t masked = 0;

            for (const tinygltf::Mesh& mesh : model.meshes) {
                for (const tinygltf::Primitive& primitive : mesh.primitives) {
                    // glTF 2.0 has no polygons, and a mode of -1 is the spec default of triangles.
                    if (primitive.mode != -1 && primitive.mode != TINYGLTF_MODE_TRIANGLES) {
                        ++skippedPrimitives;
                        continue;
                    }

                    std::string reason;
                    const tinygltf::Accessor* positionAccessor = accessorOf(model,
                        attributeOf(primitive, "POSITION"), reason);
                    if (!positionAccessor) {
                        error = "a primitive has no usable POSITION because " + reason;
                        return false;
                    }
                    std::vector<float> positions;
                    if (!readFloats(model, *positionAccessor, 3, positions, reason)) {
                        error = "the POSITION accessor cannot be read because " + reason;
                        return false;
                    }
                    const size_t positionCount = positions.size() / 3u;

                    std::vector<float> normals;
                    const int normalIndex = attributeOf(primitive, "NORMAL");
                    if (normalIndex >= 0) {
                        if (const tinygltf::Accessor* accessor = accessorOf(model, normalIndex, reason)) {
                            if (!readFloats(model, *accessor, 3, normals, reason)) normals.clear();
                        }
                    }
                    std::vector<float> coords;
                    const int uvIndex = attributeOf(primitive, "TEXCOORD_0");
                    if (uvIndex >= 0) {
                        if (const tinygltf::Accessor* accessor = accessorOf(model, uvIndex, reason)) {
                            if (!readFloats(model, *accessor, 2, coords, reason)) coords.clear();
                        }
                    }
                    for (const auto& attribute : primitive.attributes) {
                        const std::string& channel = attribute.first;
                        if (channel != "POSITION" && channel != "NORMAL" && channel != "TEXCOORD_0") {
                            ++skippedAttributes;
                        }
                    }
                    morphed += primitive.targets.size();

                    std::vector<uint32_t> indices;
                    if (primitive.indices >= 0) {
                        const tinygltf::Accessor* accessor = accessorOf(model, primitive.indices, reason);
                        if (!accessor) {
                            error = "the index accessor cannot be read because " + reason;
                            return false;
                        }
                        if (!readIndices(model, *accessor, indices, reason)) {
                            error = "the index accessor cannot be read because " + reason;
                            return false;
                        }
                    } else {
                        // A non-indexed primitive is legal glTF: its corners run in order.
                        indices.resize(positionCount);
                        for (size_t i = 0; i < indices.size(); ++i) indices[i] = (uint32_t)i;
                    }
                    for (uint32_t index : indices) {
                        if ((size_t)index >= positionCount) {
                            error = "the index accessor refers to position " + std::to_string(index) +
                                " and the primitive has " + std::to_string(positionCount);
                            return false;
                        }
                    }

                    uint32_t slot = 0;
                    if (primitive.material >= 0) {
                        if (primitive.material >= (int)model.materials.size()) {
                            error = "a primitive refers to material " +
                                std::to_string(primitive.material) + " and the file has " +
                                std::to_string(model.materials.size());
                            return false;
                        }
                        auto found = slotByMaterial.find(primitive.material);
                        if (found == slotByMaterial.end()) {
                            const tinygltf::Material& source = model.materials[primitive.material];
                            bool isMasked = false;
                            out.attribs.materials.push_back(toMaterial(source, isMasked));
                            masked += isMasked ? 1u : 0u;
                            textured += hasTexture(source) ? 1u : 0u;
                            const uint32_t created = (uint32_t)(out.attribs.materials.size() - 1);
                            slotByMaterial.insert({ primitive.material, created });
                            slot = created;
                        } else {
                            slot = found->second;
                        }
                    } else if (defaultSlot >= 0) {
                        slot = (uint32_t)defaultSlot;
                    } else {
                        // The spec's default material: white, fully metallic, fully rough.
                        ExportMaterial material;
                        material.name = "gltf_default_material";
                        material.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
                        material.metallicFactor = 1.0f;
                        material.roughnessFactor = 1.0f;
                        out.attribs.materials.push_back(material);
                        defaultSlot = (int)out.attribs.materials.size() - 1;
                        slot = (uint32_t)defaultSlot;
                    }

                    const size_t normalCount = normals.size() / 3u;
                    const size_t uvCount = coords.size() / 2u;
                    std::vector<FileCorner> corners(3);
                    std::vector<FileCorner> kept;
                    std::vector<uint32_t> face;
                    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                        for (size_t c = 0; c < 3; ++c) {
                            const size_t index = indices[i + c];
                            FileCorner& corner = corners[c];
                            corner = FileCorner();
                            corner.position = Vector3(positions[index * 3], positions[index * 3 + 1],
                                                      positions[index * 3 + 2]);
                            if (index < uvCount) {
                                corner.hasUV = true;
                                corner.uv = Vector2(coords[index * 2], coords[index * 2 + 1]);
                            }
                            if (index < normalCount) {
                                corner.hasNormal = true;
                                corner.normal = Vector3(normals[index * 3], normals[index * 3 + 1],
                                                        normals[index * 3 + 2]);
                            }
                            corner.key = keyOf(corner.position, corner.uvPointer(), corner.normalPointer());
                        }
                        if (!collectFace(corners, kept)) continue;
                        buildFace(kept, face, welder);
                        welder.topology.addFace(face);
                        out.attribs.faceMaterials.push_back(slot);
                    }
                }
            }

            if (welder.topology.faces.empty()) {
                error = skippedPrimitives ? "the glTF has only primitives that are not triangles"
                                          : "the glTF has no triangles";
                return false;
            }

            out.name = name.empty() ? textOr(model.meshes[0].name, "mesh") : name;
            welder.finish(out);

            size_t transformed = 0;
            for (const tinygltf::Node& node : model.nodes) {
                if (!node.translation.empty() || !node.rotation.empty() || !node.scale.empty()) ++transformed;
            }
            if (transformed) {
                out.warnings.push_back(countWarning(transformed,
                    " node transforms were ignored; a topology is one mesh in its own space"));
            }
            if (skippedPrimitives) {
                out.warnings.push_back(countWarning(skippedPrimitives,
                    " primitives were skipped because glTF draws them as points or lines"));
            }
            if (skippedAttributes) {
                out.warnings.push_back(countWarning(skippedAttributes,
                    " vertex attributes were skipped (tangents, colours and extra UV sets)"));
            }
            if (morphed) {
                out.warnings.push_back(countWarning(morphed,
                    " morph targets were skipped; a topology has no shape keys"));
            }
            if (textured) {
                out.warnings.push_back(countWarning(textured,
                    " materials carry image textures that a factor-only material cannot hold"));
            }
            if (masked) {
                out.warnings.push_back(countWarning(masked,
                    " materials use alphaMode MASK, kept as an alpha equal to their cutoff"));
            }
            if (!model.skins.empty()) {
                out.warnings.push_back(countWarning(model.skins.size(),
                    " skins were skipped; a topology has no bone weights"));
            }
            if (!model.animations.empty()) {
                out.warnings.push_back(countWarning(model.animations.size(),
                    " animations were skipped; an import is geometry only"));
            }
            return true;
        }

        bool fromObj(const std::string& name, const std::string& objText, const std::string& mtlText,
                     ImportedModel& out, std::string& error) {
            tinyobj::attrib_t attrib;
            std::vector<tinyobj::shape_t> shapes;
            std::vector<tinyobj::material_t> materials;
            std::string warn;
            std::string err;

            std::istringstream objStream(objText);
            std::istringstream mtlStream(mtlText);
            tinyobj::MaterialStreamReader mtlReader(mtlStream);

            // triangulate = false: an authored quad has to come back as a quad. With no material
            // callback the reader skips every mtllib line (tiny_obj_loader.h:2900) instead of
            // opening a file, because this fork resolves every path through a process-wide
            // function pointer that is null until a caller installs it (tiny_obj_loader.h:145-147,
            // :700) and calling it unset is a null dereference, not an error return.
            const bool loaded = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
                                                 &objStream,
                                                 mtlText.empty() ? nullptr : &mtlReader,
                                                 /*triangulate=*/false);
            if (!loaded || !err.empty()) {
                error = "the .obj could not be parsed: " + err;
                return false;
            }
            if (!warn.empty()) out.warnings.push_back("the .obj reader warned: " + warn);
            if (shapes.empty()) {
                error = "the .obj has no geometry";
                return false;
            }

            out.name = textOr(name, textOr(shapes[0].name, "mesh"));

            Welder welder;
            // Slots are numbered in the order the file's materials are first drawn with, exactly as
            // the glTF reader does it, so a library entry no surviving face uses costs no slot.
            std::map<int, uint32_t> slotByMaterial;
            int unmaterialisedSlot = -1;
            size_t pointsAndLines = 0;
            size_t refused = 0;
            size_t unmaterialised = 0;

            std::vector<FileCorner> corners;
            std::vector<FileCorner> kept;
            std::vector<uint32_t> face;

            for (const tinyobj::shape_t& shape : shapes) {
                const std::vector<tinyobj::index_t>& source = shape.mesh.indices;
                size_t offset = 0;
                for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
                    const size_t span = shape.mesh.num_face_vertices[f];
                    if (offset + span > source.size()) break;
                    if (span < 3) {   // a point or a line is not a face of a topology
                        ++pointsAndLines;
                        ++offset;
                        continue;
                    }

                    corners.clear();
                    corners.reserve(span);
                    bool usable = true;
                    for (size_t c = 0; c < span; ++c) {
                        const tinyobj::index_t& corner = source[offset + c];
                        if (corner.vertex_index < 0 ||
                            (size_t)corner.vertex_index * 3 + 2 >= attrib.vertices.size()) {
                            usable = false;
                            break;
                        }
                        FileCorner made;
                        made.position = Vector3(attrib.vertices[corner.vertex_index * 3],
                                                attrib.vertices[corner.vertex_index * 3 + 1],
                                                attrib.vertices[corner.vertex_index * 3 + 2]);
                        // Wavefront measures V from the bottom of the image and the engine, like
                        // glTF, from the top; MeshExport flips the same way on the way out.
                        if (corner.texcoord_index >= 0 &&
                            (size_t)corner.texcoord_index * 2 + 1 < attrib.texcoords.size()) {
                            made.hasUV = true;
                            made.uv = Vector2(attrib.texcoords[corner.texcoord_index * 2],
                                              1.0f - attrib.texcoords[corner.texcoord_index * 2 + 1]);
                        }
                        if (corner.normal_index >= 0 &&
                            (size_t)corner.normal_index * 3 + 2 < attrib.normals.size()) {
                            made.hasNormal = true;
                            made.normal = Vector3(attrib.normals[corner.normal_index * 3],
                                                  attrib.normals[corner.normal_index * 3 + 1],
                                                  attrib.normals[corner.normal_index * 3 + 2]);
                        }
                        made.key = keyOf(made.position, made.uvPointer(), made.normalPointer());
                        corners.push_back(made);
                    }
                    offset += span;

                    if (!usable || !collectFace(corners, kept)) {
                        ++refused;
                        continue;
                    }
                    buildFace(kept, face, welder);
                    welder.topology.addFace(face);

                    const int fileSlot = f < shape.mesh.material_ids.size()
                        ? shape.mesh.material_ids[f] : -1;
                    if (fileSlot < 0 || fileSlot >= (int)materials.size()) {
                        // A face the file never gave a material to - which is every face of an
                        // .obj that has no .mtl at all - shares one plain slot, so the lists stay
                        // parallel either way.
                        ++unmaterialised;
                        if (unmaterialisedSlot < 0) {
                            out.attribs.materials.push_back(ExportMaterial());
                            unmaterialisedSlot = (int)out.attribs.materials.size() - 1;
                        }
                        out.attribs.faceMaterials.push_back((uint32_t)unmaterialisedSlot);
                    } else {
                        auto found = slotByMaterial.find(fileSlot);
                        if (found == slotByMaterial.end()) {
                            out.attribs.materials.push_back(toMaterial(materials[fileSlot]));
                            const uint32_t created = (uint32_t)(out.attribs.materials.size() - 1);
                            slotByMaterial.insert({ fileSlot, created });
                            out.attribs.faceMaterials.push_back(created);
                        } else {
                            out.attribs.faceMaterials.push_back(found->second);
                        }
                    }
                }
            }

            if (welder.topology.faces.empty()) {
                error = pointsAndLines ? "the .obj has no faces, only points and lines"
                                       : "the .obj has no usable faces";
                return false;
            }

            welder.finish(out);

            if (unmaterialised) {
                const std::string& given = out.attribs.materials[(size_t)unmaterialisedSlot].name;
                out.warnings.push_back(countWarning(unmaterialised,
                    " faces had no material and were given " + given));
            }
            if (pointsAndLines) {
                out.warnings.push_back(countWarning(pointsAndLines,
                    " points and lines were skipped; a topology is made of faces"));
            }
            if (refused) {
                out.warnings.push_back(countWarning(refused,
                    " faces were skipped because their corners are out of range or repeated"));
            }
            if (welder.withoutUV && welder.withoutUV < out.topology.vertices.size()) {
                out.warnings.push_back(countWarning(welder.withoutUV,
                    " corners had no texture coordinate and were given (0, 0)"));
            }
            if (welder.withoutNormal) {
                out.warnings.push_back(countWarning(welder.withoutNormal,
                    " corners had no normal, so the normals the mesh carries are derived from its faces"));
            }
            if (shapes.size() > 1) {
                out.warnings.push_back(countWarning(shapes.size(),
                    " groups were merged into one mesh; a topology is a single object"));
            }
            return true;
        }

    }

    bool importObjText(const std::string& name, const std::string& objText,
                       const std::string& mtlText, ImportedModel& out, std::string& error) {
        out = ImportedModel();
        if (objText.empty()) {
            error = "the .obj text is empty";
            return false;
        }
        return fromObj(name, objText, mtlText, out, error);
    }

    bool importObjFile(const std::string& path, ImportedModel& out, std::string& error) {
        out = ImportedModel();
        std::string objText;
        if (!readText(path, objText, error)) return false;

        // The mtllib names are collected here rather than left to the library's own file reader,
        // which resolves through the null-by-default hook fromObj() above describes.
        std::string mtlText;
        const fs::path directory = fs::path(path).parent_path();
        std::istringstream lines(objText);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, 7, "mtllib ") != 0) continue;
            std::istringstream words(line.substr(7));
            std::string name;
            while (words >> name) {
                const fs::path mtl = fs::path(name).is_absolute() ? fs::path(name) : directory / name;
                std::string body;
                std::string reason;
                if (readText(mtl.string(), body, reason)) {
                    mtlText += body;
                    if (!body.empty() && body.back() != '\n') mtlText += "\n";
                } else {
                    out.warnings.push_back("the material library " + mtl.string() + " was not found");
                }
            }
        }
        return fromObj(stemOf(path), objText, mtlText, out, error);
    }

    bool importGlbBytes(const std::string& name, const std::vector<unsigned char>& bytes,
                        ImportedModel& out, std::string& error) {
        out = ImportedModel();
        if (bytes.empty()) {
            error = "the glTF bytes are empty";
            return false;
        }
        tinygltf::Model model;
        std::string warn;
        std::string parseError;
        tinygltf::TinyGLTF loader;
        if (!loader.LoadBinaryFromMemory(&model, &parseError, &warn, bytes.data(),
                                         (unsigned int)bytes.size(), std::string())) {
            error = "the glTF could not be parsed: " + (parseError.empty() ? warn : parseError);
            return false;
        }
        return fromGltf(name, model, out, error);
    }

    bool importGltfFile(const std::string& path, ImportedModel& out, std::string& error) {
        out = ImportedModel();
        tinygltf::Model model;
        std::string warn;
        std::string parseError;
        tinygltf::TinyGLTF loader;
        // This version's file loaders take a callback rather than a base dir, so an .gltf with an
        // external .bin is read by tinygltf's own default, which resolves against the process's
        // working folder. Every file this engine exports embeds its buffer, and a self-contained
        // .glb has nothing to resolve.
        const bool loaded = fileExtension(path) == "glb"
            ? loader.LoadBinaryFromFile(&model, &parseError, &warn, path)
            : loader.LoadASCIIFromFile(&model, &parseError, &warn, path);
        if (!loaded) {
            error = "the glTF could not be parsed: " + (parseError.empty() ? warn : parseError);
            return false;
        }
        return fromGltf(stemOf(path), model, out, error);
    }

    bool importModelFile(const std::string& path, ImportedModel& out, std::string& error) {
        const std::string ext = fileExtension(path);
        if (ext == "obj") return importObjFile(path, out, error);
        if (ext == "glb" || ext == "gltf") return importGltfFile(path, out, error);
        out = ImportedModel();
        if (ext == "fbx") {
            error = "a .fbx needs ufbx, which is linked into the editor and not into the engine; "
                    "convert it to .glb first";
        } else {
            error = "no reader for a ." + ext + " file; .obj, .glb and .gltf are supported";
        }
        return false;
    }

}
}
