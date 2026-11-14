// (c) SR VINIX
// SPDX-License-Identifier: MIT
//
// GPU-free unit tests of the modelling kernel (engine/core/model/). This file owns the
// doctest main; the sibling files add cases for the operators and the picker.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "model/MeshTopology.h"

#include <string>
#include <vector>

using namespace quantum;

namespace quantum {
namespace test {

    // A unit cube with welded corners, wound counter-clockwise seen from outside. The exact
    // corner order matters: every orientation assertion in the suite reads these normals.
    inline MeshTopology makeCube() {
        MeshTopology mesh;
        mesh.addVertex(Vector3(0.0f, 0.0f, 0.0f));   // 0
        mesh.addVertex(Vector3(1.0f, 0.0f, 0.0f));   // 1
        mesh.addVertex(Vector3(1.0f, 1.0f, 0.0f));   // 2
        mesh.addVertex(Vector3(0.0f, 1.0f, 0.0f));   // 3
        mesh.addVertex(Vector3(0.0f, 0.0f, 1.0f));   // 4
        mesh.addVertex(Vector3(1.0f, 0.0f, 1.0f));   // 5
        mesh.addVertex(Vector3(1.0f, 1.0f, 1.0f));   // 6
        mesh.addVertex(Vector3(0.0f, 1.0f, 1.0f));   // 7

        mesh.addFace({ 0, 3, 2, 1 });   // 0: z = 0, faces -Z
        mesh.addFace({ 4, 5, 6, 7 });   // 1: z = 1, faces +Z
        mesh.addFace({ 0, 4, 7, 3 });   // 2: x = 0, faces -X
        mesh.addFace({ 1, 2, 6, 5 });   // 3: x = 1, faces +X
        mesh.addFace({ 0, 1, 5, 4 });   // 4: y = 0, faces -Y
        mesh.addFace({ 3, 7, 6, 2 });   // 5: y = 1, faces +Y
        mesh.recalcNormals();
        return mesh;
    }

    // The same cube as a triangle soup with one position per corner of every triangle, which
    // is what a file with seams hands over: 36 positions, 12 triangles, nothing shared.
    inline std::vector<Vector3> makeUnweldedCubePositions(std::vector<uint32_t>& indices) {
        const MeshTopology cube = makeCube();
        std::vector<Vector3> weldedPositions;
        std::vector<uint32_t> weldedIndices;
        cube.toTriangles(weldedPositions, weldedIndices);

        indices.clear();
        std::vector<Vector3> positions;
        for (uint32_t index : weldedIndices) {
            indices.push_back((uint32_t)positions.size());
            positions.push_back(weldedPositions[index]);
        }
        return positions;
    }

}
}

TEST_SUITE("MeshTopology") {

    using namespace quantum::test;

    TEST_CASE("a cube reports the topology it has") {
        const MeshTopology cube = makeCube();

        CHECK(cube.vertexCount() == 8);
        CHECK(cube.faceCount() == 6);
        CHECK(cube.edgeCount() == 12);
        CHECK(cube.eulerCharacteristic() == 2);   // V - E + F, genus zero
        CHECK(cube.isTriMesh() == false);

        std::string error;
        CHECK(cube.validate(error));
        CHECK(cube.bounds().getMinimum() == Vector3(0.0f, 0.0f, 0.0f));
        CHECK(cube.bounds().getMaximum() == Vector3(1.0f, 1.0f, 1.0f));
    }

    TEST_CASE("faces are wound outward") {
        const MeshTopology cube = makeCube();

        CHECK(cube.faceNormal(0).z == doctest::Approx(-1.0f));
        CHECK(cube.faceNormal(1).z == doctest::Approx(1.0f));
        CHECK(cube.faceNormal(2).x == doctest::Approx(-1.0f));
        CHECK(cube.faceNormal(3).x == doctest::Approx(1.0f));
        CHECK(cube.faceNormal(4).y == doctest::Approx(-1.0f));
        CHECK(cube.faceNormal(5).y == doctest::Approx(1.0f));
    }

    TEST_CASE("a closed cube has no boundary") {
        const MeshTopology cube = makeCube();

        std::vector<std::array<uint32_t, 2>> edges;
        cube.getEdges(edges);
        REQUIRE(edges.size() == 12);
        for (const std::array<uint32_t, 2>& edge : edges) {
            CHECK(cube.isBoundaryEdge(edge[0], edge[1]) == false);
        }
        for (uint32_t v = 0; v < 8; ++v) {
            CHECK(cube.faceValence(v) == 3);
            CHECK(cube.isBoundaryVertex(v) == false);
        }
    }

    TEST_CASE("vertex normals average the faces around a corner") {
        const MeshTopology cube = makeCube();

        // Corner 0 is shared by the three faces pointing -X, -Y and -Z.
        const Vector3& corner = cube.vertexNormal(0);
        CHECK(corner.x == doctest::Approx(-0.57735f));
        CHECK(corner.y == doctest::Approx(-0.57735f));
        CHECK(corner.z == doctest::Approx(-0.57735f));
    }

    TEST_CASE("neighbour queries agree with the cube") {
        const MeshTopology cube = makeCube();

        std::vector<uint32_t> neighbours;
        cube.faceNeighbours(0, neighbours);
        CHECK(neighbours.size() == 4);   // the four side faces around the z = 0 quad

        cube.vertexNeighbours(0, neighbours);
        REQUIRE(neighbours.size() == 3);
        CHECK(neighbours[0] == 1);
        CHECK(neighbours[1] == 3);
        CHECK(neighbours[2] == 4);
    }

    TEST_CASE("flipping a face turns it inward") {
        MeshTopology cube = makeCube();
        REQUIRE(cube.flipFace(5));
        CHECK(cube.faceNormal(5).y == doctest::Approx(-1.0f));
    }

    TEST_CASE("a triangle soup welds into a cube") {
        std::vector<uint32_t> indices;
        const std::vector<Vector3> positions = quantum::test::makeUnweldedCubePositions(indices);
        REQUIRE(positions.size() == 36);

        const MeshTopology welded = MeshTopology::fromTriangles(positions, indices);
        CHECK(welded.vertexCount() == 8);
        CHECK(welded.faceCount() == 12);
        CHECK(welded.edgeCount() == 18);
        CHECK(welded.eulerCharacteristic() == 2);
    }

    TEST_CASE("merge by distance closes a gap and drops the face inside it") {
        MeshTopology mesh;
        mesh.addVertex(Vector3(0.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(1.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(0.0f, 1.0f, 0.0f));
        mesh.addVertex(Vector3(0.0f, 0.0f, 0.05f));   // a duplicate of corner 0, 5 cm away
        mesh.addTriangle(0, 1, 2);
        mesh.recalcNormals();

        CHECK(mesh.mergeByDistance(0.1f) == 1);
        CHECK(mesh.vertexCount() == 3);
        CHECK(mesh.faceCount() == 1);
        std::string error;
        CHECK(mesh.validate(error));
    }

    TEST_CASE("an exact weld merges only the corners that already share a place") {
        // Two triangles that meet at one corner written twice, once with negative zeros: the
        // weld is by bits, so it must not need a tolerance or move a single position.
        MeshTopology mesh;
        mesh.addVertex(Vector3(0.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(1.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(0.0f, 1.0f, 0.0f));
        mesh.addVertex(Vector3(0.0f, -0.0f, -0.0f));
        mesh.addTriangle(0, 1, 2);
        mesh.addTriangle(1, 2, 3);

        CHECK(mesh.mergeByDistance(0.0f) == 1);
        CHECK(mesh.vertexCount() == 3);
        CHECK(mesh.faceCount() == 2);
        CHECK(mesh.position(0) == Vector3(0.0f, 0.0f, 0.0f));
        std::string error;
        CHECK(mesh.validate(error));

        // Corners of one face that share a place make that face zero-area, and a weld reports
        // it by dropping the face rather than keeping a triangle with no normal.
        MeshTopology degenerate;
        degenerate.addVertex(Vector3(0.0f, 0.0f, 0.0f));
        degenerate.addVertex(Vector3(1.0f, 0.0f, 0.0f));
        degenerate.addVertex(Vector3(0.0f, 1.0f, 0.0f));
        degenerate.addVertex(Vector3(0.0f, -0.0f, 0.0f));
        degenerate.addTriangle(0, 1, 2);
        degenerate.addTriangle(1, 3, 0);

        CHECK(degenerate.mergeByDistance(0.0f) == 1);
        CHECK(degenerate.faceCount() == 1);
        CHECK(degenerate.vertexCount() == 3);
        CHECK(degenerate.validate(error));
    }

    TEST_CASE("triangulate keeps the surface and the Euler characteristic") {
        MeshTopology cube = makeCube();
        cube.triangulate();

        CHECK(cube.isTriMesh());
        CHECK(cube.faceCount() == 12);
        CHECK(cube.eulerCharacteristic() == 2);
    }

    TEST_CASE("deleting a face opens a hole and purges what it freed") {
        MeshTopology cube = makeCube();
        const std::vector<uint32_t> top = { 5 };

        CHECK(cube.deleteFaces(top) == 1);
        CHECK(cube.faceCount() == 5);
        CHECK(cube.vertexCount() == 8);   // every corner still carries another face
        CHECK(cube.eulerCharacteristic() == 1);
        CHECK(cube.isBoundaryEdge(3, 7));

        // Deleting the three faces that meet at a corner frees exactly that corner, and the
        // purge must renumber the faces that referenced it. Seven of the eight corners still
        // carry one of the three faces that remain.
        MeshTopology other = makeCube();
        const std::vector<uint32_t> aroundCorner = { 0, 2, 4 };
        other.deleteFaces(aroundCorner);
        CHECK(other.vertexCount() == 7);
        std::string error;
        CHECK(other.validate(error));
    }

    TEST_CASE("deleting a vertex takes its faces with it") {
        MeshTopology cube = makeCube();
        const std::vector<uint32_t> corner = { 0 };

        CHECK(cube.deleteVertices(corner) == 3);
        CHECK(cube.faceCount() == 3);
        CHECK(cube.vertexCount() == 7);   // only the deleted corner itself became unused
        std::string error;
        CHECK(cube.validate(error));
    }

    TEST_CASE("an invalid mesh is reported and can be pruned") {
        MeshTopology mesh = makeCube();
        mesh.faces.push_back({ 0, 1, 8 });   // out of range
        mesh.faces.push_back({ 0, 0, 1, 2 });   // repeated corner

        std::string error;
        CHECK(mesh.validate(error) == false);
        CHECK_FALSE(error.empty());
        CHECK(mesh.pruneInvalid() == 2);
        CHECK(mesh.validate(error));
    }

}
