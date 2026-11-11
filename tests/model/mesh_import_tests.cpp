// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT
//
// GPU-free unit tests of the importer (engine/core/model/MeshImport). The proof of an import is
// the round trip: a mesh the exporter wrote has to come back as the mesh it was written from, so
// most cases here build with MeshExport and read the result with MeshImport, then compare
// geometry, UVs and material factors. Files the exporter would never write - a UV seam, a .mtl
// that only carries a Blinn exponent, a face that repeats a corner - are handed to the reader as
// text, because that is what a model from anywhere else in the world looks like.

#include "doctest.h"

#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "model/MeshExport.h"
#include "model/MeshImport.h"
#include "model/MeshTopology.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace quantum;
using namespace quantum::model;

namespace {

    // The same canonical cube as the export tests: 8 welded corners, 6 outward quads.
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

    // One five-sided face: the cheapest thing glTF cannot hold and a topology can.
    MeshTopology makePentagon() {
        MeshTopology mesh;
        for (int i = 0; i < 5; ++i) {
            const double angle = 2.0 * 3.1415926535897931 * (double)i / 5.0;
            mesh.addVertex(Vector3((float)std::cos(angle), (float)std::sin(angle), 0.0f));
        }
        mesh.addFace({ 0, 1, 2, 3, 4 });
        mesh.recalcNormals();
        return mesh;
    }

    // Six decimals is all the .obj text carries, so a round trip is only ever that exact.
    bool near(float a, float b) { return std::fabs(a - b) <= 1e-5f; }

    // Corners are welded in the order they appear in the file's faces, which is not the order the
    // positions were listed in, so a round trip compares a set of points and not a prefix.
    size_t indexOfPosition(const MeshTopology& mesh, const Vector3& position) {
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            const Vector3& found = mesh.vertices[i];
            if (near(found.x, position.x) && near(found.y, position.y) && near(found.z, position.z)) {
                return i;
            }
        }
        return mesh.vertices.size();
    }

    std::vector<Vector3> sorted(std::vector<Vector3> values) {
        std::sort(values.begin(), values.end(), [](const Vector3& a, const Vector3& b) {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            return a.z < b.z;
        });
        return values;
    }

    size_t facesWithCorners(const MeshTopology& mesh, size_t corners) {
        size_t found = 0;
        for (const std::vector<uint32_t>& face : mesh.faces) {
            if (face.size() == corners) ++found;
        }
        return found;
    }

    bool hasWarning(const ImportedModel& model, const std::string& needle) {
        for (const std::string& warning : model.warnings) {
            if (warning.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    ExportOptions named(const std::string& name) {
        ExportOptions options;
        options.name = name;
        return options;
    }

    // One UV per corner of the cube, derived from the position so that every vertex's coordinate
    // is distinct and can be looked up again by position after the weld reorders the list.
    ExportAttribs cubeAttribs(const MeshTopology& mesh) {
        ExportAttribs attribs;
        attribs.uvs.reserve(mesh.vertices.size());
        for (const Vector3& vertex : mesh.vertices) {
            attribs.uvs.push_back(Vector2(vertex.x * 0.25f + 0.5f * vertex.z,
                                          vertex.y * 0.5f + 0.25f));
        }

        ExportMaterial red;
        red.name = "red";
        red.baseColorFactor = Vector4(0.8f, 0.1f, 0.1f, 0.5f);
        red.emissiveFactor = Vector3(0.0f, 0.05f, 0.0f);
        red.metallicFactor = 0.25f;
        red.roughnessFactor = 0.6f;
        red.doubleSided = true;
        ExportMaterial blue;
        blue.name = "blue";
        blue.baseColorFactor = Vector4(0.1f, 0.2f, 0.9f, 1.0f);
        blue.metallicFactor = 0.0f;
        blue.roughnessFactor = 0.1f;
        attribs.materials.push_back(red);
        attribs.materials.push_back(blue);

        for (size_t f = 0; f < mesh.faces.size(); ++f) {
            attribs.faceMaterials.push_back((uint32_t)(f % 2u));
        }
        return attribs;
    }

    std::filesystem::path tempDir() {
        return std::filesystem::temp_directory_path() / "quantum-model-import-tests";
    }

}

TEST_CASE("a Wavefront file the exporter wrote reads back as the same mesh") {
    const MeshTopology cube = makeCube();
    const ExportAttribs attribs = cubeAttribs(cube);

    std::string mtlText;
    std::string reason;
    const std::string obj = buildObj(cube, attribs, named("cube"), mtlText, reason);
    REQUIRE_MESSAGE(!obj.empty(), reason);

    ImportedModel imported;
    std::string error;
    REQUIRE(importObjText("cube", obj, mtlText, imported, error));
    CHECK_MESSAGE(error.empty(), error);

    CHECK(imported.name == "cube");
    REQUIRE(imported.topology.vertexCount() == cube.vertexCount());
    REQUIRE(imported.topology.faceCount() == cube.faceCount());
    CHECK(facesWithCorners(imported.topology, 4) == 6);
    CHECK(facesWithCorners(imported.topology, 3) == 0);

    const std::vector<Vector3> expected = sorted(cube.vertices);
    const std::vector<Vector3> actual = sorted(imported.topology.vertices);
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        CHECK(near(actual[i].x, expected[i].x));
        CHECK(near(actual[i].y, expected[i].y));
        CHECK(near(actual[i].z, expected[i].z));
    }

    // Nothing was lost on the way out and back: no texture, no group, no missing channel.
    for (const std::string& warning : imported.warnings) {
        FAIL_CHECK("a clean Wavefront round trip warned: " << warning);
    }

    REQUIRE(imported.attribs.uvs.size() == imported.topology.vertices.size());
    for (size_t i = 0; i < cube.vertices.size(); ++i) {
        const size_t at = indexOfPosition(imported.topology, cube.vertices[i]);
        REQUIRE(at < imported.topology.vertices.size());
        CHECK(near(imported.attribs.uvs[at].x, attribs.uvs[i].x));
        CHECK(near(imported.attribs.uvs[at].y, attribs.uvs[i].y));
    }

    REQUIRE(imported.attribs.materials.size() == 2);
    CHECK(imported.attribs.materials[0].name == "red");
    CHECK(imported.attribs.materials[1].name == "blue");
    CHECK(near(imported.attribs.materials[0].baseColorFactor.x, 0.8f));
    CHECK(near(imported.attribs.materials[0].baseColorFactor.y, 0.1f));
    CHECK(near(imported.attribs.materials[0].baseColorFactor.z, 0.1f));
    // The fourth component travels as `d` and comes back as the alpha, not as a mask.
    CHECK(near(imported.attribs.materials[0].baseColorFactor.w, 0.5f));
    CHECK(near(imported.attribs.materials[0].emissiveFactor.y, 0.05f));
    // Pr and Pm are the exact glTF scalars, so this is a comparison and not an approximation.
    CHECK(near(imported.attribs.materials[0].roughnessFactor, 0.6f));
    CHECK(near(imported.attribs.materials[0].metallicFactor, 0.25f));
    CHECK(near(imported.attribs.materials[1].roughnessFactor, 0.1f));
    // doubleSided has no Wavefront spelling, so it is one thing a .obj cannot carry.

    // Faces keep their order, so a slot list that stayed parallel is a slot list that survived.
    REQUIRE(imported.attribs.faceMaterials.size() == attribs.faceMaterials.size());
    for (size_t f = 0; f < attribs.faceMaterials.size(); ++f) {
        CHECK(imported.attribs.faceMaterials[f] == attribs.faceMaterials[f]);
    }

    // The normals the file stored beat the ones a face-derived mesh would have invented.
    REQUIRE(imported.topology.normals.size() == imported.topology.vertices.size());
    for (size_t i = 0; i < cube.vertices.size(); ++i) {
        const size_t at = indexOfPosition(imported.topology, cube.vertices[i]);
        REQUIRE(at < imported.topology.vertices.size());
        CHECK(near(imported.topology.normals[at].x, cube.normals[i].x));
        CHECK(near(imported.topology.normals[at].y, cube.normals[i].y));
        CHECK(near(imported.topology.normals[at].z, cube.normals[i].z));
    }
}

TEST_CASE("a .obj reads its material factors out of the .mtl it was given") {
    // The file an older viewer writes: no PBR extension tags at all, only a Blinn exponent. The
    // importer inverts the mapping MeshExport uses, so the look survives even though the number
    // it is stored under does not.
    const std::string obj =
        "o old\n"
        "mtllib old.mtl\n"
        "usemtl old_shiny\n"
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "f 1 2 3\n";
    const std::string mtl =
        "newmtl old_shiny\n"
        "Kd 0.200000 0.300000 0.400000\n"
        "d 0.500000\n"
        "Ns 16.000000\n";

    ImportedModel imported;
    std::string error;
    REQUIRE(importObjText("old", obj, mtl, imported, error));
    REQUIRE(imported.attribs.materials.size() == 1);
    const ExportMaterial& material = imported.attribs.materials[0];
    CHECK(material.name == "old_shiny");
    CHECK(near(material.baseColorFactor.x, 0.2f));
    CHECK(near(material.baseColorFactor.w, 0.5f));
    // Ns = 2^(10 * (1 - roughness)), so 16 is a roughness of exactly 0.6.
    CHECK(near(material.roughnessFactor, 0.6f));
    CHECK(near(material.metallicFactor, 0.0f));
    CHECK(imported.name == "old");
}

TEST_CASE("polygons come back from a .obj and never from a .glb") {
    const MeshTopology pentagon = makePentagon();
    ExportAttribs attribs;
    attribs.uvs = { Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), Vector2(1.0f, 1.0f),
                    Vector2(0.5f, 1.0f), Vector2(0.0f, 0.5f) };

    std::string mtlText;
    std::string reason;
    const std::string obj = buildObj(pentagon, attribs, named("pentagon"), mtlText, reason);
    REQUIRE(!obj.empty());
    ImportedModel fromWavefront;
    std::string error;
    REQUIRE(importObjText("pentagon", obj, mtlText, fromWavefront, error));
    CHECK(fromWavefront.topology.faceCount() == 1);
    CHECK(fromWavefront.topology.faces[0].size() == 5);
    CHECK(fromWavefront.topology.vertexCount() == 5);

    const std::vector<unsigned char> glb = buildGlb(pentagon, attribs, named("pentagon"), reason);
    REQUIRE(!glb.empty());
    ImportedModel fromGltf;
    REQUIRE(importGlbBytes("pentagon", glb, fromGltf, error));
    // glTF has no polygons, so the fan split the exporter applied is all the file can carry.
    CHECK(fromGltf.topology.faceCount() == 3);
    CHECK(facesWithCorners(fromGltf.topology, 3) == 3);
    CHECK(fromGltf.topology.vertexCount() == 5);
    CHECK(fromGltf.attribs.uvs.size() == 5);
}

TEST_CASE("a UV seam stays a seam and the Wavefront V axis is flipped back") {
    // Two triangles over four positions, sharing the diagonal, with the corners on the diagonal
    // given different texture coordinates on either side. A renderer needs those as two vertices;
    // welding on position alone would fold the seam shut and stretch the island across itself.
    const std::string obj =
        "o seam\n"
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvt 0 0.5\n"
        "vn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\n"
        "f 1/4/1 3/3/1 4/5/1\n";

    ImportedModel imported;
    std::string error;
    REQUIRE(importObjText("seam", obj, std::string(), imported, error));
    CHECK(imported.topology.vertexCount() == 5);
    CHECK(indexOfPosition(imported.topology, Vector3(0.0f, 0.0f, 0.0f)) < 5);
    CHECK(indexOfPosition(imported.topology, Vector3(1.0f, 0.0f, 0.0f)) < 5);
    REQUIRE(imported.attribs.uvs.size() == 5);

    // `vt 1 0` is measured from the bottom of the image, so the stored V is 1 - 0.
    const size_t at = indexOfPosition(imported.topology, Vector3(1.0f, 0.0f, 0.0f));
    REQUIRE(at < imported.topology.vertices.size());
    CHECK(near(imported.attribs.uvs[at].x, 1.0f));
    CHECK(near(imported.attribs.uvs[at].y, 1.0f));
    CHECK(imported.topology.faceCount() == 2);
}

TEST_CASE("a mesh with no material at all gets one plain slot and says so") {
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";

    ImportedModel imported;
    std::string error;
    REQUIRE(importObjText("bare", obj, std::string(), imported, error));
    REQUIRE(imported.attribs.materials.size() == 1);
    CHECK(imported.attribs.materials[0].name == "material");
    REQUIRE(imported.attribs.faceMaterials.size() == 1);
    CHECK(imported.attribs.faceMaterials[0] == 0u);
    CHECK(hasWarning(imported, "no material"));

    // Every corner of that file lacked a normal, so the mesh carries the faces' own.
    CHECK(hasWarning(imported, "derived from its faces"));
    REQUIRE(imported.topology.normals.size() == 3);
    CHECK(near(imported.topology.normals[0].z, 1.0f));
}

TEST_CASE("a library entry no face uses costs no slot") {
    const std::string obj =
        "mtllib two.mtl\n"
        "usemtl used\n"
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "f 1 2 3\n";
    const std::string mtl =
        "newmtl used\nKd 1 0 0\n"
        "newmtl unused\nKd 0 0 1\n";

    ImportedModel imported;
    std::string error;
    REQUIRE(importObjText("one", obj, mtl, imported, error));
    REQUIRE(imported.attribs.materials.size() == 1);
    CHECK(imported.attribs.materials[0].name == "used");
}

TEST_CASE("a glTF the exporter wrote reads back with its materials and its alpha") {
    const MeshTopology cube = makeCube();
    const ExportAttribs attribs = cubeAttribs(cube);
    std::string reason;
    const std::vector<unsigned char> glb = buildGlb(cube, attribs, named("cube"), reason);
    REQUIRE_MESSAGE(!glb.empty(), reason);

    ImportedModel imported;
    std::string error;
    REQUIRE(importGlbBytes("cube", glb, imported, error));
    CHECK(imported.name == "cube");
    CHECK(imported.topology.vertexCount() == 8);
    CHECK(imported.topology.faceCount() == 12);
    CHECK(imported.attribs.materials.size() == 2);
    for (const std::string& warning : imported.warnings) {
        FAIL_CHECK("a clean glTF round trip warned: " << warning);
    }

    // A base colour with real alpha left the exporter as alphaMode BLEND, so the fourth
    // component is the alpha the mesh went in with rather than a cutoff.
    const ExportMaterial& red = imported.attribs.materials[0];
    CHECK(near(red.baseColorFactor.w, 0.5f));
    CHECK(near(red.roughnessFactor, 0.6f));
    CHECK(near(red.metallicFactor, 0.25f));
    CHECK(red.doubleSided == true);

    size_t onRed = 0;
    size_t onBlue = 0;
    for (uint32_t slot : imported.attribs.faceMaterials) {
        if (slot == 0u) ++onRed;
        else if (slot == 1u) ++onBlue;
    }
    CHECK(onRed == 6);
    CHECK(onBlue == 6);
    REQUIRE(imported.attribs.uvs.size() == 8);
    for (size_t i = 0; i < cube.vertices.size(); ++i) {
        const size_t at = indexOfPosition(imported.topology, cube.vertices[i]);
        REQUIRE(at < 8);
        CHECK(near(imported.attribs.uvs[at].x, attribs.uvs[i].x));
        CHECK(near(imported.attribs.uvs[at].y, attribs.uvs[i].y));
    }
}

TEST_CASE("a glTF that carries only positions imports as geometry and nothing else") {
    // One triangle with neither UVs nor normals: the channels the topology keeps beside it have
    // to stay empty rather than be filled with an invented zero per vertex.
    MeshTopology mesh;
    mesh.addVertex(Vector3(0.0f, 0.0f, 0.0f));
    mesh.addVertex(Vector3(1.0f, 0.0f, 0.0f));
    mesh.addVertex(Vector3(0.0f, 1.0f, 0.0f));
    mesh.addFace({ 0, 1, 2 });
    mesh.recalcNormals();

    ExportOptions options = named("plain");
    options.writeNormals = false;
    options.writeUVs = false;
    std::string reason;
    const std::vector<unsigned char> glb = buildGlb(mesh, ExportAttribs(), options, reason);
    REQUIRE(!glb.empty());

    ImportedModel imported;
    std::string error;
    // The exporter still writes one material for an unassigned mesh, because a file with no
    // material at all is a file every viewer shades differently.
    REQUIRE(importGlbBytes("plain", glb, imported, error));
    CHECK(imported.topology.vertexCount() == 3);
    CHECK(imported.topology.faceCount() == 1);
    CHECK(imported.attribs.uvs.empty());
    CHECK(imported.attribs.materials.size() == 1);
    REQUIRE(imported.attribs.faceMaterials.size() == 1);
    CHECK(imported.attribs.faceMaterials[0] == 0u);
}

TEST_CASE("the files on disk are found by their extension and read with their library") {
    std::error_code code;
    std::filesystem::remove_all(tempDir(), code);
    std::filesystem::create_directories(tempDir(), code);
    REQUIRE(std::filesystem::exists(tempDir()));

    const MeshTopology cube = makeCube();
    const ExportAttribs attribs = cubeAttribs(cube);
    std::string error;

    const std::string objPath = (tempDir() / "cube.obj").string();
    REQUIRE_MESSAGE(writeObj(objPath, cube, attribs, named("cube"), error), error);
    REQUIRE(std::filesystem::exists(tempDir() / "cube.mtl"));

    ImportedModel fromWavefront;
    REQUIRE_MESSAGE(importModelFile(objPath, fromWavefront, error), error);
    CHECK(fromWavefront.name == "cube");
    CHECK(fromWavefront.topology.faceCount() == 6);
    // The mtllib line was resolved next to the .obj rather than left to the library's own reader.
    REQUIRE(fromWavefront.attribs.materials.size() == 2);
    CHECK(fromWavefront.attribs.materials[1].name == "blue");
    CHECK_FALSE(hasWarning(fromWavefront, "was not found"));

    const std::string glbPath = (tempDir() / "cube.glb").string();
    REQUIRE_MESSAGE(writeGlb(glbPath, cube, attribs, named("cube"), error), error);
    ImportedModel fromGltf;
    REQUIRE_MESSAGE(importModelFile(glbPath, fromGltf, error), error);
    CHECK(fromGltf.topology.vertexCount() == 8);
    CHECK(fromGltf.topology.faceCount() == 12);

    ImportedModel byItsOwnReader;
    REQUIRE(importGltfFile(glbPath, byItsOwnReader, error));
    CHECK(byItsOwnReader.topology.faceCount() == 12);
    CHECK(byItsOwnReader.name == "cube");

    std::filesystem::remove_all(tempDir(), code);
}

TEST_CASE("a file that cannot be a mesh is refused with the reason") {
    ImportedModel imported;
    std::string error;

    CHECK_FALSE(importObjText("nothing", std::string(), std::string(), imported, error));
    CHECK(error.find("empty") != std::string::npos);

    error.clear();
    CHECK_FALSE(importObjText("points", "v 0 0 0\nv 1 0 0\nv 0 1 0\n", std::string(),
                              imported, error));
    CHECK_MESSAGE(error.find("no geometry") != std::string::npos, error);

    // A face that repeats a corner has no area; the whole face goes, and with it the mesh.
    error.clear();
    CHECK_FALSE(importObjText("flat", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 1 2\n", std::string(),
                              imported, error));
    CHECK_MESSAGE(error.find("no usable faces") != std::string::npos, error);

    error.clear();
    CHECK_FALSE(importGlbBytes("empty", std::vector<unsigned char>(), imported, error));
    CHECK(error.find("empty") != std::string::npos);

    error.clear();
    const std::vector<unsigned char> garbage(64, 0x7fu);
    CHECK_FALSE(importGlbBytes("garbage", garbage, imported, error));
    CHECK_MESSAGE(error.find("glTF") != std::string::npos, error);

    error.clear();
    CHECK_FALSE(importModelFile("C:/somewhere/notes.txt", imported, error));
    CHECK_MESSAGE(error.find(".txt") != std::string::npos, error);

    error.clear();
    CHECK_FALSE(importModelFile("C:/somewhere/character.fbx", imported, error));
    CHECK_MESSAGE(error.find("ufbx") != std::string::npos, error);
}

TEST_CASE("points and lines never become faces") {
    // The Wavefront reader is asked for the file's own polygons, and in that mode tinyobjloader
    // does not hand `p` and `l` entries to the caller at all - the guard in the importer is what
    // keeps a build of the library that does emit them from making a face of one or two corners.
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "l 1 2\n"
        "p 3\n"
        "f 1 2 3\n";

    ImportedModel imported;
    std::string error;
    REQUIRE_MESSAGE(importObjText("mixed", obj, std::string(), imported, error), error);
    CHECK(imported.topology.faceCount() == 1);
    CHECK(imported.attribs.faceMaterials.size() == 1);
    CHECK(indexOfPosition(imported.topology, Vector3(0.0f, 1.0f, 0.0f)) == 2);
}

TEST_CASE("a group is merged into one mesh and the importer says so") {
    const std::string obj =
        "o first\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
        "o second\nv 2 0 0\nv 3 0 0\nv 2 1 0\nf 4 5 6\n";

    ImportedModel imported;
    std::string error;
    REQUIRE_MESSAGE(importObjText("pair", obj, std::string(), imported, error), error);
    CHECK(imported.topology.faceCount() == 2);
    CHECK(imported.topology.vertexCount() == 6);
    CHECK(hasWarning(imported, "merged into one mesh"));
}
