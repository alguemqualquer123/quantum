// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT
//
// GPU-free unit tests of the exporters (engine/core/model/MeshExport). A file an export
// writes is only correct if a reader accepts it, so nothing here compares bytes to a stored
// copy of itself: the .obj is read back with tinyobjloader and the .glb with tinygltf, which
// are the two libraries the engine itself loads models with (MeshSystem.cpp:1327 and :1331).
// The glTF container gets a second, independent check by hand, because a writer and a reader
// from the same library can agree on a wrong answer.

#include "doctest.h"

#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "model/MeshExport.h"
#include "model/MeshTopology.h"

#include "tiny_gltf.h"
#include "tiny_obj_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace quantum;
using namespace quantum::model;

namespace {

    // The same canonical cube as the topology tests: 8 welded corners, 6 outward quads.
    MeshTopology makeCube() {
        MeshTopology mesh;
        mesh.addVertex(Vector3(0.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(1.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(1.0f, 1.0f, 0.0f));
        mesh.addVertex(Vector3(0.0f, 1.0f, 0.0f));
        mesh.addVertex(Vector3(0.0f, 0.0f, 1.0f));
        mesh.addVertex(Vector3(1.0f, 0.0f, 1.0f));
        mesh.addVertex(Vector3(1.0f, 1.0f, 1.0f));
        mesh.addVertex(Vector3(0.0f, 1.0f, 1.0f));

        mesh.addFace({ 0, 3, 2, 1 });
        mesh.addFace({ 4, 5, 6, 7 });
        mesh.addFace({ 0, 4, 7, 3 });
        mesh.addFace({ 1, 2, 6, 5 });
        mesh.addFace({ 0, 1, 5, 4 });
        mesh.addFace({ 3, 7, 6, 2 });
        mesh.recalcNormals();
        return mesh;
    }

    // One triangle in the z = 0 plane, wound so its normal points at +Z. Small enough that
    // the whole file can be compared as a string.
    MeshTopology makeTriangle() {
        MeshTopology mesh;
        mesh.addVertex(Vector3(0.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(1.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(0.0f, 1.0f, 0.0f));
        mesh.addFace({ 0, 1, 2 });
        mesh.recalcNormals();
        return mesh;
    }

    // A size x size grid of quads, used to cross the 16-bit index limit: 256 is the first
    // side length that needs more than 65535 vertices.
    MeshTopology makeGrid(int size) {
        MeshTopology mesh;
        mesh.reserveVertices((size_t)size * size);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                mesh.addVertex(Vector3((float)x, (float)y, 0.0f));
            }
        }
        mesh.reserveFaces((size_t)(size - 1) * (size - 1));
        for (int y = 0; y + 1 < size; ++y) {
            for (int x = 0; x + 1 < size; ++x) {
                const uint32_t a = (uint32_t)(y * size + x);
                mesh.addFace({ a, a + 1, a + 1 + (uint32_t)size, a + (uint32_t)size });
            }
        }
        mesh.recalcNormals();
        return mesh;
    }

    std::vector<Vector2> planarUVs(const MeshTopology& mesh) {
        std::vector<Vector2> uvs;
        uvs.reserve(mesh.vertices.size());
        for (const Vector3& vertex : mesh.vertices) uvs.push_back(Vector2(vertex.x, vertex.y));
        return uvs;
    }

    ExportMaterial makeMaterial(const std::string& name, float r, float g, float b,
                                float metallic, float roughness) {
        ExportMaterial material;
        material.name = name;
        material.baseColorFactor = Vector4(r, g, b, 1.0f);
        material.metallicFactor = metallic;
        material.roughnessFactor = roughness;
        return material;
    }

    int count(const std::string& text, const std::string& needle) {
        int found = 0;
        size_t at = 0;
        while ((at = text.find(needle, at)) != std::string::npos) { ++found; at += needle.size(); }
        return found;
    }

    bool has(const std::string& text, const std::string& needle) {
        return text.find(needle) != std::string::npos;
    }

    // A line the writer emits, not a substring that could come from anywhere: the leading
    // and trailing newlines are part of the check.
    bool hasLine(const std::string& text, const std::string& line) {
        return has(text, "\n" + line + "\n") || text == line ||
               text.compare(0, line.size() + 1, line + "\n") == 0;
    }

    std::vector<float> readFloats(const tinygltf::Model& model, const tinygltf::Accessor& accessor) {
        const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
        const tinygltf::Buffer& buffer = model.buffers[view.buffer];
        const int components = tinygltf::GetNumComponentsInType(accessor.type);
        const size_t stride = view.byteStride ? view.byteStride : sizeof(float) * (size_t)components;
        std::vector<float> out;
        out.reserve(accessor.count * (size_t)components);
        for (size_t i = 0; i < accessor.count; ++i) {
            for (int c = 0; c < components; ++c) {
                const size_t at = view.byteOffset + accessor.byteOffset + i * stride +
                                  (size_t)c * sizeof(float);
                float value = 0.0f;
                std::memcpy(&value, buffer.data.data() + at, sizeof(float));
                out.push_back(value);
            }
        }
        return out;
    }

    std::vector<uint32_t> readIndices(const tinygltf::Model& model,
                                      const tinygltf::Accessor& accessor) {
        const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
        const tinygltf::Buffer& buffer = model.buffers[view.buffer];
        const size_t element = accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT
            ? sizeof(uint16_t) : sizeof(uint32_t);
        std::vector<uint32_t> out;
        out.reserve(accessor.count);
        for (size_t i = 0; i < accessor.count; ++i) {
            const size_t at = view.byteOffset + accessor.byteOffset + i * element;
            if (element == sizeof(uint16_t)) {
                uint16_t value = 0;
                std::memcpy(&value, buffer.data.data() + at, sizeof(value));
                out.push_back(value);
            } else {
                uint32_t value = 0;
                std::memcpy(&value, buffer.data.data() + at, sizeof(value));
                out.push_back(value);
            }
        }
        return out;
    }

    tinygltf::Model parseGlb(const std::vector<unsigned char>& bytes, std::string& error) {
        tinygltf::Model model;
        std::string warning;
        if (!tinygltf::TinyGLTF().LoadBinaryFromMemory(&model, &error, &warning, bytes.data(),
                                                       (unsigned int)bytes.size())) {
            error = "tinygltf refused the container: " + error;
        }
        return model;
    }

    uint32_t chunkAt(const std::vector<unsigned char>& bytes, size_t at) {
        uint32_t value = 0;
        std::memcpy(&value, bytes.data() + at, sizeof(value));
        return value;
    }

    // The first argument of a library line, e.g. "Ns 15.999998" -> 15.999998. Checked as a
    // number because the writer derives it: a float roughness of 0.6 is a hair above 0.6, so
    // 2^(10 * (1 - roughness)) lands just under 16 and the digits are not the contract.
    float mtlNumber(const std::string& mtl, const std::string& key) {
        const size_t at = mtl.find("\n" + key + " ");
        REQUIRE_MESSAGE(at != std::string::npos, "the library has no " << key << " line");
        return (float)std::atof(mtl.c_str() + at + key.size() + 2);
    }

    std::filesystem::path tempDir() {
        return std::filesystem::temp_directory_path() / "quantum-model-export-tests";
    }

    // The vendored tinyobjloader is a fork that reads every file through this one hook
    // (tiny_obj_loader.h:700, used at :2576 and :2500), and the pointer is null until a
    // caller installs it - which is why MeshSystem.cpp:1324 sets it before LoadObj. Calling
    // LoadObj without it is a null dereference, not an error return.
    std::string readWholeFile(const char* path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

}

TEST_CASE("a triangle is written as the exact Wavefront text") {
    const MeshTopology mesh = makeTriangle();
    std::string mtl;
    std::string error;
    const std::string text = buildObj(mesh, ExportAttribs(), ExportOptions(), mtl, error);

    REQUIRE_FALSE(text.empty());
    CHECK(error.empty());
    CHECK(mtl.empty());   // no material was given, so no mtllib and no library body
    CHECK_FALSE(has(text, "mtllib"));
    CHECK_FALSE(has(text, "vt "));

    const std::string expected =
        "# Wavefront OBJ written by quantum (engine/core/model/MeshExport.cpp)\n"
        "o mesh\n"
        "v 0.000000 0.000000 0.000000\n"
        "v 1.000000 0.000000 0.000000\n"
        "v 0.000000 1.000000 0.000000\n"
        "vn 0.000000 0.000000 1.000000\n"
        "vn 0.000000 0.000000 1.000000\n"
        "vn 0.000000 0.000000 1.000000\n"
        "f 1//1 2//2 3//3\n";
    CHECK(text == expected);
}

TEST_CASE("polygons survive an OBJ export and only an export") {
    const MeshTopology mesh = makeCube();
    std::string mtl;
    std::string error;
    const std::string text = buildObj(mesh, ExportAttribs(), ExportOptions(), mtl, error);

    REQUIRE_FALSE(text.empty());
    CHECK(count(text, "\nv ") == 8);
    CHECK(count(text, "\nvn ") == 8);
    CHECK(count(text, "\nf ") == 6);
    // A quad is four corners, so a fan-split would have written twelve lines of three.
    CHECK(hasLine(text, "f 1//1 4//4 3//3 2//2"));
    // The corner of a cube where three faces meet, averaged and normalized.
    CHECK(hasLine(text, "vn -0.577350 -0.577350 -0.577350"));

    MeshTopology flat = mesh;
    flat.normals.clear();
    const std::string fromFaces = buildObj(flat, ExportAttribs(), ExportOptions(), mtl, error);
    CHECK(count(fromFaces, "\nvn ") == 8);   // the fallback derives them instead of dropping them
    CHECK(fromFaces == text);
}

TEST_CASE("UVs are written once per vertex with the V axis flipped") {
    MeshTopology mesh = makeTriangle();
    ExportAttribs attribs;
    attribs.uvs = { Vector2(0.0f, 0.0f), Vector2(1.0f, 0.25f), Vector2(0.5f, 0.75f) };

    std::string mtl;
    std::string error;
    const std::string text = buildObj(mesh, attribs, ExportOptions(), mtl, error);

    REQUIRE_FALSE(text.empty());
    CHECK(count(text, "\nvt ") == 3);
    CHECK(hasLine(text, "vt 0.000000 1.000000"));
    CHECK(hasLine(text, "vt 1.000000 0.750000"));
    CHECK(hasLine(text, "vt 0.500000 0.250000"));

    ExportOptions noUVs;
    noUVs.writeUVs = false;
    const std::string without = buildObj(mesh, attribs, noUVs, mtl, error);
    CHECK(count(without, "\nvt ") == 0);
    CHECK(hasLine(without, "f 1//1 2//2 3//3"));
}

TEST_CASE("a material assignment becomes usemtl groups and a library body") {
    const MeshTopology mesh = makeCube();
    ExportAttribs attribs;
    attribs.materials = { makeMaterial("stone", 0.8f, 0.2f, 0.1f, 0.25f, 0.6f),
                          makeMaterial("glass", 0.1f, 0.2f, 0.9f, 0.0f, 0.1f) };
    attribs.faceMaterials = { 0, 1, 1, 0, 0, 1 };

    std::string mtl;
    std::string error;
    ExportOptions options;
    options.name = "cube";
    const std::string text = buildObj(mesh, attribs, options, mtl, error);

    REQUIRE_FALSE(text.empty());
    CHECK(hasLine(text, "o cube"));
    CHECK(hasLine(text, "mtllib cube.mtl"));
    // Slots interleave in face order, so every change re-issues usemtl: 0, 1, 1(no), 0, 0(no), 1.
    CHECK(count(text, "\nusemtl ") == 4);
    CHECK(text.compare(text.find("usemtl"), 12, "usemtl stone") == 0);

    CHECK(count(mtl, "newmtl ") == 2);
    CHECK(has(mtl, "newmtl stone"));
    CHECK(has(mtl, "Kd 0.800000 0.200000 0.100000"));
    // The exact glTF values, as the PBR extension scalars tinyobjloader documents.
    CHECK(hasLine(mtl, "Pr 0.600000"));
    CHECK(hasLine(mtl, "Pm 0.250000"));
    CHECK(mtlNumber(mtl, "Ns") == doctest::Approx(16.0f).epsilon(0.0001));   // the roughness fall-back
}

TEST_CASE("a name typed into a text field cannot split a line") {
    MeshTopology mesh = makeTriangle();
    ExportAttribs attribs;
    attribs.materials = { makeMaterial("stone wall\nKd 1 1 1", 1.0f, 1.0f, 1.0f, 0.0f, 0.5f) };
    attribs.faceMaterials = { 0 };

    std::string mtl;
    std::string error;
    ExportOptions options;
    options.name = "my mesh";
    const std::string text = buildObj(mesh, attribs, options, mtl, error);

    REQUIRE_FALSE(text.empty());
    CHECK(hasLine(text, "o my_mesh"));
    CHECK(hasLine(text, "mtllib my_mesh.mtl"));
    CHECK(has(mtl, "newmtl stone_wall_Kd_1_1_1"));
    CHECK_FALSE(has(mtl, "newmtl stone wall"));
    CHECK(count(mtl, "newmtl ") == 1);
}

TEST_CASE("an empty mesh and a channel out of parallel are refused") {
    std::string mtl;
    std::string error;

    MeshTopology nothing;
    CHECK(buildObj(nothing, ExportAttribs(), ExportOptions(), mtl, error).empty());
    CHECK(has(error, "no faces"));
    CHECK(buildGlb(nothing, ExportAttribs(), ExportOptions(), error).empty());
    CHECK(has(error, "no faces"));

    const MeshTopology mesh = makeCube();

    ExportAttribs shortUVs;
    shortUVs.uvs = { Vector2(0.0f, 0.0f) };
    CHECK(buildObj(mesh, shortUVs, ExportOptions(), mtl, error).empty());
    CHECK(has(error, "uv list has 1 pairs but the mesh has 8 vertices"));

    ExportAttribs shortMaterials;
    shortMaterials.faceMaterials = { 0, 0 };
    CHECK(buildGlb(mesh, shortMaterials, ExportOptions(), error).empty());
    CHECK(has(error, "material list has 2 slots but the mesh has 6 faces"));

    ExportAttribs missingSlot;
    missingSlot.materials = { makeMaterial("stone", 1, 1, 1, 0, 1), makeMaterial("glass", 1, 1, 1, 0, 1) };
    missingSlot.faceMaterials = { 0, 1, 0, 1, 0, 5 };
    CHECK(buildObj(mesh, missingSlot, ExportOptions(), mtl, error).empty());
    CHECK(has(error, "face 5 uses material slot 5 and there are only 2"));

    MeshTopology broken = makeCube();
    broken.vertices[3] = Vector3(std::nanf(""), 1.0f, 0.0f);
    CHECK(buildGlb(broken, ExportAttribs(), ExportOptions(), error).empty());
    CHECK(has(error, "vertex 3 is not a finite position"));

    MeshTopology poisoned = makeCube();
    poisoned.faces.push_back({ 0, 0, 1 });
    CHECK(buildObj(poisoned, ExportAttribs(), ExportOptions(), mtl, error).empty());
    CHECK(has(error, "repeats a corner"));
}

TEST_CASE("the glTF binary container is the one the spec describes") {
    const MeshTopology mesh = makeCube();
    std::string error;
    ExportAttribs attribs;
    attribs.uvs = planarUVs(mesh);
    const std::vector<unsigned char> bytes = buildGlb(mesh, attribs, ExportOptions(), error);

    REQUIRE(bytes.size() > 24);
    CHECK(error.empty());
    CHECK(mesh.vertices.size() == 8);   // the reader below counts against this

    // 12-byte header: magic, version 2, whole file length.
    CHECK(chunkAt(bytes, 0) == 0x46546C67u);   // "glTF"
    CHECK(chunkAt(bytes, 4) == 2u);
    CHECK(chunkAt(bytes, 8) == (uint32_t)bytes.size());

    // JSON chunk, then the BIN chunk, each padded to a multiple of four.
    const uint32_t jsonLength = chunkAt(bytes, 12);
    CHECK(chunkAt(bytes, 16) == 0x4E4F534Au);   // "JSON"
    CHECK(jsonLength % 4 == 0u);
    const size_t binHeader = 20u + jsonLength;
    REQUIRE(binHeader + 8 <= bytes.size());
    const uint32_t binLength = chunkAt(bytes, binHeader);
    CHECK(chunkAt(bytes, binHeader + 4) == 0x004E4942u);   // "BIN\0"
    CHECK(binLength % 4 == 0u);
    CHECK(binHeader + 8 + binLength == bytes.size());
    CHECK(bytes[20] == '{');
}

TEST_CASE("the engine's own glTF reader loads the export back") {
    const MeshTopology mesh = makeCube();
    ExportAttribs attribs;
    attribs.uvs = planarUVs(mesh);
    std::string error;
    const std::vector<unsigned char> bytes = buildGlb(mesh, attribs, ExportOptions(), error);
    REQUIRE_FALSE(bytes.empty());

    std::string problem;
    const tinygltf::Model parsed = parseGlb(bytes, problem);
    REQUIRE_MESSAGE(problem.empty(), problem);
    REQUIRE(parsed.meshes.size() == 1);
    REQUIRE(parsed.meshes[0].primitives.size() == 1);
    CHECK(parsed.asset.version == "2.0");
    CHECK(parsed.defaultScene == 0);
    REQUIRE(parsed.nodes.size() == 1);
    CHECK(parsed.nodes[0].name == "mesh");

    const tinygltf::Primitive& primitive = parsed.meshes[0].primitives[0];
    CHECK(primitive.mode == TINYGLTF_MODE_TRIANGLES);
    REQUIRE(primitive.attributes.size() == 3);
    REQUIRE(primitive.indices >= 0);

    const tinygltf::Accessor& position = parsed.accessors[primitive.attributes.at("POSITION")];
    CHECK(position.type == TINYGLTF_TYPE_VEC3);
    CHECK(position.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT);
    CHECK(position.count == 8);
    const std::vector<float> positions = readFloats(parsed, position);
    REQUIRE(positions.size() == 24);
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        CHECK(positions[i * 3] == doctest::Approx(mesh.vertices[i].x));
        CHECK(positions[i * 3 + 1] == doctest::Approx(mesh.vertices[i].y));
        CHECK(positions[i * 3 + 2] == doctest::Approx(mesh.vertices[i].z));
    }
    // POSITION bounds are required by the spec, and a viewer culls with them.
    REQUIRE(position.minValues.size() == 3);
    CHECK(position.minValues[0] == doctest::Approx(0.0));
    CHECK(position.maxValues[0] == doctest::Approx(1.0));
    CHECK(position.maxValues[2] == doctest::Approx(1.0));

    const tinygltf::Accessor& normal = parsed.accessors[primitive.attributes.at("NORMAL")];
    CHECK(normal.count == 8);
    const std::vector<float> normals = readFloats(parsed, normal);
    REQUIRE(normals.size() == 24);
    for (size_t i = 0; i < mesh.normals.size(); ++i) {
        CHECK(normals[i * 3] == doctest::Approx(mesh.normals[i].x));
        CHECK(normals[i * 3 + 1] == doctest::Approx(mesh.normals[i].y));
    }

    // glTF measures V from the top like the engine does, so the UVs arrive unflipped.
    const tinygltf::Accessor& uv = parsed.accessors[primitive.attributes.at("TEXCOORD_0")];
    CHECK(uv.type == TINYGLTF_TYPE_VEC2);
    const std::vector<float> uvs = readFloats(parsed, uv);
    REQUIRE(uvs.size() == 16);
    CHECK(uvs[2] == doctest::Approx(1.0));
    CHECK(uvs[3] == doctest::Approx(0.0));

    // Six quads, fan-split into twelve triangles, welded corners indexed the same way.
    const std::vector<uint32_t> indices = readIndices(parsed, parsed.accessors[primitive.indices]);
    CHECK(indices.size() == 36);
    CHECK(parsed.accessors[primitive.indices].componentType ==
          TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
    CHECK(indices[0] == 0);
    CHECK(indices[1] == 3);
    CHECK(indices[2] == 2);
}

TEST_CASE("a material slot becomes a primitive and keeps its factors") {
    const MeshTopology mesh = makeCube();
    ExportAttribs attribs;
    attribs.materials = { makeMaterial("stone", 0.8f, 0.2f, 0.1f, 0.25f, 0.6f),
                          makeMaterial("glass", 0.1f, 0.2f, 0.9f, 0.0f, 0.1f) };
    attribs.faceMaterials = { 0, 1, 1, 0, 0, 1 };

    std::string error;
    const std::vector<unsigned char> bytes = buildGlb(mesh, attribs, ExportOptions(), error);
    REQUIRE_FALSE(bytes.empty());

    std::string problem;
    const tinygltf::Model parsed = parseGlb(bytes, problem);
    REQUIRE_MESSAGE(problem.empty(), problem);

    REQUIRE(parsed.materials.size() == 2);
    REQUIRE(parsed.meshes[0].primitives.size() == 2);
    CHECK(parsed.meshes[0].primitives[0].material == 0);
    CHECK(parsed.meshes[0].primitives[1].material == 1);

    CHECK(parsed.materials[0].name == "stone");
    REQUIRE(parsed.materials[0].pbrMetallicRoughness.baseColorFactor.size() == 4);
    CHECK(parsed.materials[0].pbrMetallicRoughness.baseColorFactor[0] == doctest::Approx(0.8));
    CHECK(parsed.materials[0].pbrMetallicRoughness.baseColorFactor[1] == doctest::Approx(0.2));
    CHECK(parsed.materials[0].pbrMetallicRoughness.metallicFactor == doctest::Approx(0.25));
    CHECK(parsed.materials[0].pbrMetallicRoughness.roughnessFactor == doctest::Approx(0.6));
    CHECK(parsed.materials[1].name == "glass");
    CHECK(parsed.materials[1].pbrMetallicRoughness.baseColorFactor[2] == doctest::Approx(0.9));

    // Three faces per slot, each a quad, so six triangles and 18 indices apiece. Every
    // primitive shares the one position accessor rather than copying the vertices.
    for (const tinygltf::Primitive& primitive : parsed.meshes[0].primitives) {
        CHECK(readIndices(parsed, parsed.accessors[primitive.indices]).size() == 18);
        CHECK(primitive.attributes.at("POSITION") == 0);
    }
}

TEST_CASE("an unassigned mesh still gets a material and one primitive") {
    const MeshTopology mesh = makeCube();
    std::string error;
    const std::vector<unsigned char> bytes = buildGlb(mesh, ExportAttribs(), ExportOptions(), error);
    REQUIRE_FALSE(bytes.empty());

    std::string problem;
    const tinygltf::Model parsed = parseGlb(bytes, problem);
    REQUIRE_MESSAGE(problem.empty(), problem);
    CHECK(parsed.materials.size() == 1);
    CHECK(parsed.materials[0].name == "mesh_material");
    CHECK(parsed.meshes[0].primitives.size() == 1);
    CHECK(parsed.meshes[0].primitives[0].attributes.size() == 2);   // POSITION and NORMAL
}

TEST_CASE("an index buffer goes 32-bit past the 16-bit limit") {
    const MeshTopology mesh = makeGrid(256);
    REQUIRE(mesh.vertices.size() == 65536);

    std::string error;
    ExportOptions options;
    options.writeNormals = false;
    const std::vector<unsigned char> bytes = buildGlb(mesh, ExportAttribs(), options, error);
    REQUIRE_FALSE(bytes.empty());

    std::string problem;
    const tinygltf::Model parsed = parseGlb(bytes, problem);
    REQUIRE_MESSAGE(problem.empty(), problem);

    const tinygltf::Primitive& primitive = parsed.meshes[0].primitives[0];
    CHECK(parsed.accessors[primitive.attributes.at("POSITION")].count == 65536);
    CHECK(parsed.accessors[primitive.indices].componentType ==
          TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT);
    CHECK(parsed.accessors[primitive.indices].count == 65025 * 6);
    // A 32-bit index buffer is valid glTF but past what a quantum primitive can draw, which is
    // why the exporter is the one that has to keep the mesh under it rather than hide it.
    CHECK(primitive.attributes.size() == 1);
}

TEST_CASE("the files written to disk load back through the engine's readers") {
    std::error_code cleanup;
    const std::filesystem::path dir = tempDir();
    std::filesystem::remove_all(dir, cleanup);
    std::filesystem::create_directories(dir, cleanup);
    REQUIRE(std::filesystem::is_directory(dir));

    const MeshTopology mesh = makeCube();
    ExportAttribs attribs;
    attribs.uvs = planarUVs(mesh);
    attribs.materials = { makeMaterial("stone", 0.8f, 0.2f, 0.1f, 0.25f, 0.6f),
                          makeMaterial("glass", 0.1f, 0.2f, 0.9f, 0.0f, 0.1f) };
    attribs.faceMaterials = { 0, 1, 1, 0, 0, 1 };

    ExportOptions options;
    options.name = "cube";

    const std::filesystem::path objPath = dir / "cube.obj";
    std::string error;
    REQUIRE(writeObj(objPath.string(), mesh, attribs, options, error));
    CHECK(error.empty());
    // The library is named after the object, because that is the name the mtllib line asks for.
    REQUIRE(std::filesystem::exists(dir / "cube.mtl"));
    CHECK(std::filesystem::file_size(dir / "cube.mtl") > 0);

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warning;
    std::string objError;
    tinyobj::FileReader::externalFunc = &readWholeFile;
    // triangulate = false: the reader must accept the polygons as written, not be rescued.
    const bool loaded = tinyobj::LoadObj(&attrib, &shapes, &materials, &warning, &objError,
                                         objPath.string().c_str(), dir.string().c_str(), false);
    REQUIRE_MESSAGE(loaded, objError);
    REQUIRE(shapes.size() == 1);
    CHECK(attrib.vertices.size() == 24);
    CHECK(attrib.normals.size() == 24);
    CHECK(attrib.texcoords.size() == 16);
    CHECK(shapes[0].name == "cube");
    CHECK(shapes[0].mesh.indices.size() == 24);   // 6 quads of 4 corners
    REQUIRE(shapes[0].mesh.material_ids.size() == 6);
    CHECK(shapes[0].mesh.material_ids[0] == 0);
    CHECK(shapes[0].mesh.material_ids[1] == 1);
    REQUIRE(materials.size() == 2);
    CHECK(materials[0].name == "stone");
    CHECK(materials[0].diffuse[0] == doctest::Approx(0.8));
    CHECK(materials[1].name == "glass");

    const std::vector<float> expected = { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0,
                                          0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1 };
    REQUIRE(attrib.vertices.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        CHECK(attrib.vertices[i] == doctest::Approx(expected[i]));
    }

    const std::filesystem::path glbPath = dir / "cube.glb";
    REQUIRE(writeGlb(glbPath.string(), mesh, attribs, options, error));
    REQUIRE(std::filesystem::file_size(glbPath) > 12);

    tinygltf::Model fromDisk;
    std::string glbError;
    REQUIRE(tinygltf::TinyGLTF().LoadBinaryFromFile(&fromDisk, &glbError, &warning,
                                                    glbPath.string()));
    CHECK(fromDisk.meshes[0].primitives.size() == 2);
    CHECK(fromDisk.accessors[fromDisk.meshes[0].primitives[0].attributes.at("POSITION")].count == 8);
    CHECK(fromDisk.materials[0].name == "stone");

    std::string missing;
    MeshTopology nothing;
    error.clear();
    CHECK_FALSE(writeObj((dir / "empty.obj").string(), nothing, ExportAttribs(), options, missing));
    CHECK(has(missing, "no faces"));
    CHECK_FALSE(std::filesystem::exists(dir / "empty.obj"));

    std::filesystem::remove_all(dir, cleanup);
}
