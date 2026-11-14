// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT
//
// GPU-free unit tests of the report layer (engine/core/model/MeshReport). The panel that shows
// these numbers cannot be clicked in a test run, so everything here is the part the panel only
// formats: what a mesh is made of, what is wrong with it, and whether a written file still holds
// the same thing. The last case is the one that matters most - it is the check the export window
// runs on every file it writes.

#include "doctest.h"

#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "model/MeshExport.h"
#include "model/MeshImport.h"
#include "model/MeshReport.h"
#include "model/MeshTopology.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace quantum;
using namespace quantum::model;

namespace {

    // The same canonical cube as the export and import tests: 8 welded corners, 6 outward quads.
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

    // One quad in the z = 0 plane: four border edges, and nothing else to report.
    MeshTopology makeQuad() {
        MeshTopology mesh;
        mesh.addVertex(Vector3(0.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(1.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(1.0f, 1.0f, 0.0f));
        mesh.addVertex(Vector3(0.0f, 1.0f, 0.0f));
        mesh.addFace({ 0, 1, 2, 3 });
        mesh.recalcNormals();
        return mesh;
    }

    // Three triangles around the 0-1 edge, which is the smallest mesh whose edge count is not
    // the one a walk of the surface expects.
    MeshTopology makeNonManifold() {
        MeshTopology mesh;
        mesh.addVertex(Vector3(0.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(1.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(0.5f, 1.0f, 0.0f));
        mesh.addVertex(Vector3(0.5f, -1.0f, 0.0f));
        mesh.addVertex(Vector3(0.5f, 0.0f, 1.0f));
        mesh.addTriangle(0, 1, 2);
        mesh.addTriangle(0, 3, 1);
        mesh.addTriangle(0, 1, 4);
        mesh.recalcNormals();
        return mesh;
    }

    ExportAttribs oneMaterial(const MeshTopology& mesh) {
        ExportAttribs attribs;
        attribs.materials.push_back(ExportMaterial());
        for (size_t f = 0; f < mesh.faces.size(); ++f) attribs.faceMaterials.push_back(0u);
        for (const Vector3& vertex : mesh.vertices)
            attribs.uvs.push_back(Vector2(vertex.x, vertex.y));
        return attribs;
    }

    ExportOptions named(const std::string& name) {
        ExportOptions options;
        options.name = name;
        return options;
    }

    bool mentions(const std::vector<std::string>& lines, const std::string& needle) {
        for (const std::string& line : lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    std::filesystem::path tempDir() {
        return std::filesystem::temp_directory_path() / "quantum-model-report-tests";
    }

}

TEST_CASE("a welded cube reports a closed and clean mesh") {
    const MeshTopology cube = makeCube();
    const MeshStats stats = meshStats(cube, oneMaterial(cube));

    CHECK(stats.vertices == 8);
    CHECK(stats.faces == 6);
    // Six quads fan out to twelve triangles, which is what a renderer is handed.
    CHECK(stats.triangles == 12);
    CHECK(stats.edges == 12);
    CHECK(stats.boundaryEdges == 0);
    CHECK(stats.nonManifoldEdges == 0);
    // V - E + F of a sphere-topologised cube.
    CHECK(stats.eulerCharacteristic == 2);
    CHECK(stats.hasVertexNormals);
    CHECK(stats.textureCoordinates == 8);
    CHECK(stats.materialSlots == 1);
    CHECK(stats.unusedMaterialSlots == 0);
    CHECK(stats.clean);

    CHECK(stats.minimum.x == doctest::Approx(0.0f));
    CHECK(stats.maximum.z == doctest::Approx(1.0f));

    CHECK(meshWarnings(cube, stats).empty());
}

TEST_CASE("an open plane reports its border and its missing channels") {
    const MeshTopology quad = makeQuad();

    ExportAttribs attribs;
    attribs.materials.push_back(ExportMaterial());
    attribs.faceMaterials.push_back(0u);

    const MeshStats stats = meshStats(quad, attribs);
    CHECK(stats.vertices == 4);
    CHECK(stats.faces == 1);
    CHECK(stats.triangles == 2);
    CHECK(stats.edges == 4);
    CHECK(stats.boundaryEdges == 4);
    CHECK(stats.eulerCharacteristic == 1);
    CHECK_FALSE(stats.clean);

    const std::vector<std::string> warnings = meshWarnings(quad, stats);
    CHECK(mentions(warnings, "open"));
    CHECK(mentions(warnings, "texture coordinates"));
}

TEST_CASE("three faces along one edge are reported as non-manifold") {
    const MeshTopology mesh = makeNonManifold();
    const MeshStats stats = meshStats(mesh, oneMaterial(mesh));

    CHECK(stats.vertices == 5);
    CHECK(stats.faces == 3);
    CHECK(stats.triangles == 3);
    // The shared edge plus six around the outside.
    CHECK(stats.edges == 7);
    CHECK(stats.boundaryEdges == 6);
    CHECK(stats.nonManifoldEdges == 1);
    CHECK(stats.eulerCharacteristic == 1);
    CHECK_FALSE(stats.clean);

    CHECK(mentions(meshWarnings(mesh, stats), "three or more faces"));
}

TEST_CASE("a material slot no face uses is counted and named") {
    const MeshTopology quad = makeQuad();
    ExportAttribs attribs = oneMaterial(quad);
    attribs.materials.push_back(ExportMaterial());

    const MeshStats stats = meshStats(quad, attribs);
    CHECK(stats.materialSlots == 2);
    CHECK(stats.unusedMaterialSlots == 1);
    CHECK(mentions(meshWarnings(quad, stats), "no face uses"));
}

TEST_CASE("an empty mesh reports itself empty rather than a list of zeroes") {
    const MeshTopology mesh;
    const ExportAttribs attribs;

    const MeshStats stats = meshStats(mesh, attribs);
    CHECK(stats.vertices == 0);
    CHECK(stats.edges == 0);
    CHECK_FALSE(stats.clean);

    const std::vector<std::string> warnings = meshWarnings(mesh, stats);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("empty") != std::string::npos);
}

TEST_CASE("a face that repeats a corner is reported as invalid geometry") {
    MeshTopology mesh;
    mesh.addVertex(Vector3(0.0f, 0.0f, 0.0f));
    mesh.addVertex(Vector3(1.0f, 0.0f, 0.0f));
    mesh.addVertex(Vector3(0.0f, 1.0f, 0.0f));
    mesh.addFace({ 0, 1, 0 });
    mesh.addFace({ 0, 1, 2 });

    const MeshStats stats = meshStats(mesh, oneMaterial(mesh));
    CHECK_FALSE(stats.clean);
    CHECK(mentions(meshWarnings(mesh, stats), "invalid geometry"));
}

TEST_CASE("compareMeshStats ignores the counts a format is allowed to change") {
    const MeshTopology cube = makeCube();
    const ExportAttribs attribs = oneMaterial(cube);

    MeshStats source = meshStats(cube, attribs);

    MeshTopology fans = cube;
    fans.triangulate();
    const MeshStats triangulated = meshStats(fans, attribs);

    // The numbers that must agree, and the ones that must not, per the header's promise.
    CHECK(triangulated.faces == 12);
    CHECK(triangulated.edges == 18);
    CHECK(source.triangles == triangulated.triangles);

    std::vector<std::string> differences;
    CHECK(compareMeshStats(source, triangulated, differences));
    CHECK(differences.empty());

    // A file that lost a corner is reported in the direction the writer went: the source's
    // 8 became the written 7.
    MeshStats shortWritten = triangulated;
    shortWritten.vertices = 7;
    CHECK_FALSE(compareMeshStats(source, shortWritten, differences));
    REQUIRE(differences.size() == 1);
    CHECK(differences[0].find("vertices") != std::string::npos);
    CHECK(differences[0].find("8 became 7") != std::string::npos);
}

TEST_CASE("a UV seam welded shut is a vertex difference, not a silent one") {
    // A square in two triangles, with the corner at the origin kept twice because the texture
    // wraps there: the same position, a different UV. That split is what a UV editor lives on.
    MeshTopology seam;
    seam.addVertex(Vector3(0.0f, 0.0f, 0.0f));
    seam.addVertex(Vector3(1.0f, 0.0f, 0.0f));
    seam.addVertex(Vector3(1.0f, 1.0f, 0.0f));
    seam.addVertex(Vector3(0.0f, 1.0f, 0.0f));
    seam.addVertex(Vector3(0.0f, 0.0f, 0.0f));
    seam.addFace({ 0, 1, 2 });
    seam.addFace({ 4, 2, 3 });
    seam.recalcNormals();

    ExportAttribs seamAttribs;
    seamAttribs.uvs.push_back(Vector2(0.0f, 0.0f));
    seamAttribs.uvs.push_back(Vector2(0.5f, 0.0f));
    seamAttribs.uvs.push_back(Vector2(0.5f, 1.0f));
    seamAttribs.uvs.push_back(Vector2(0.0f, 1.0f));
    seamAttribs.uvs.push_back(Vector2(1.0f, 0.0f));
    seamAttribs.materials.push_back(ExportMaterial());
    seamAttribs.faceMaterials.push_back(0u);
    seamAttribs.faceMaterials.push_back(0u);

    // The same square with the seam closed: one corner fewer, one UV fewer, nothing else moved.
    MeshTopology welded = seam;
    CHECK(welded.mergeByDistance(0.0001f) == 1);
    ExportAttribs weldedAttribs = seamAttribs;
    weldedAttribs.uvs.pop_back();

    REQUIRE(welded.vertices.size() == 4);
    REQUIRE(meshStats(seam, seamAttribs).triangles == meshStats(welded, weldedAttribs).triangles);

    std::vector<std::string> differences;
    CHECK_FALSE(compareMeshStats(meshStats(welded, weldedAttribs),
                                 meshStats(seam, seamAttribs), differences));
    CHECK(mentions(differences, "vertices"));
    CHECK(mentions(differences, "texture coordinates"));
}

TEST_CASE("a .glb the writer produced reports the same geometry it was written from") {
    std::error_code code;
    std::filesystem::remove_all(tempDir(), code);
    std::filesystem::create_directories(tempDir(), code);
    REQUIRE(std::filesystem::exists(tempDir()));

    const MeshTopology cube = makeCube();
    const ExportAttribs attribs = oneMaterial(cube);
    const std::string path = (tempDir() / "cube.glb").string();

    std::string error;
    REQUIRE_MESSAGE(writeGlb(path, cube, attribs, named("cube"), error), error);

    ImportedModel back;
    REQUIRE_MESSAGE(importModelFile(path, back, error), error);

    std::vector<std::string> differences;
    const bool preserved = compareMeshStats(meshStats(cube, attribs),
                                           meshStats(back.topology, back.attribs), differences);
    const std::string why = differences.empty() ? std::string("the counts match")
                                                 : differences.front();
    CHECK_MESSAGE(preserved, why);

    // The face count is the one that legitimately moves: glTF has no polygons.
    CHECK(back.topology.faceCount() == 12);
    std::filesystem::remove_all(tempDir(), code);
}

TEST_CASE("a .obj the writer produced keeps its polygons through the report") {
    std::error_code code;
    std::filesystem::remove_all(tempDir(), code);
    std::filesystem::create_directories(tempDir(), code);
    REQUIRE(std::filesystem::exists(tempDir()));

    const MeshTopology cube = makeCube();
    const ExportAttribs attribs = oneMaterial(cube);
    const std::string path = (tempDir() / "cube.obj").string();

    std::string error;
    REQUIRE_MESSAGE(writeObj(path, cube, attribs, named("cube"), error), error);

    ImportedModel back;
    REQUIRE_MESSAGE(importModelFile(path, back, error), error);

    const MeshStats written = meshStats(back.topology, back.attribs);
    CHECK(written.faces == cube.faceCount());
    CHECK(written.edges == meshStats(cube, attribs).edges);

    std::vector<std::string> differences;
    CHECK_MESSAGE(compareMeshStats(meshStats(cube, attribs), written, differences),
                  (differences.empty() ? std::string("the counts match") : differences.front()));

    std::filesystem::remove_all(tempDir(), code);
}

TEST_CASE("the report of a mesh without normals says where they come from") {
    MeshTopology cube = makeCube();
    cube.normals.clear();

    const MeshStats stats = meshStats(cube, oneMaterial(cube));
    CHECK_FALSE(stats.hasVertexNormals);
    // Still closed and still usable: only the source of the normals changed.
    CHECK(stats.clean);
    CHECK(mentions(meshWarnings(cube, stats), "no vertex normals"));

    // The same mesh with its UVs and materials gone is what a bare file reads back as.
    ExportAttribs bare;
    const MeshStats bareStats = meshStats(cube, bare);
    CHECK(bareStats.materialSlots == 0);
    const std::vector<std::string> bareWarnings = meshWarnings(cube, bareStats);
    CHECK(mentions(bareWarnings, "no texture coordinates"));
    CHECK(mentions(bareWarnings, "no vertex normals"));
}
