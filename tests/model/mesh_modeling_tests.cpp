// (c) SR VINIX
// SPDX-License-Identifier: MIT
//
// GPU-free unit tests of the modelling operators (engine/core/model/MeshModeling). The
// invariants that matter are the Euler characteristic (an edit must not tear or seal the
// surface), the winding (a flipped face renders black), and the counts a tool shows the user.

#include "doctest.h"

#include "model/MeshModeling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace quantum;
using namespace quantum::model;

namespace {

    // Kept in sync with the cube in mesh_topology_tests.cpp: 8 welded corners, 6 outward
    // quads, face 5 being the +Y top.
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

    // A 2x2 open plane: 9 welded corners, 4 quads, boundary all around. Solidify needs an
    // open surface to have a rim at all.
    MeshTopology makePlane() {
        MeshTopology mesh;
        for (int z = 0; z < 3; ++z) {
            for (int x = 0; x < 3; ++x) {
                mesh.addVertex(Vector3((float)x, 0.0f, (float)z));
            }
        }
        auto corner = [](int x, int z) { return (uint32_t)(z * 3 + x); };
        // Wound so the normal is +Y: (a, b, c, d) runs counter-clockwise seen from above.
        mesh.addFace({ corner(0, 0), corner(0, 1), corner(1, 1), corner(1, 0) });
        mesh.addFace({ corner(1, 0), corner(1, 1), corner(2, 1), corner(2, 0) });
        mesh.addFace({ corner(0, 1), corner(0, 2), corner(1, 2), corner(1, 1) });
        mesh.addFace({ corner(1, 1), corner(1, 2), corner(2, 2), corner(2, 1) });
        mesh.recalcNormals();
        return mesh;
    }

    bool allQuads(const MeshTopology& mesh) {
        for (const std::vector<uint32_t>& face : mesh.faces) {
            if (face.size() != 4) return false;
        }
        return !mesh.faces.empty();
    }

    bool allOutwardFrom(const MeshTopology& mesh, const Vector3& center) {
        for (size_t f = 0; f < mesh.faces.size(); ++f) {
            const Vector3 normal = mesh.faceNormal((uint32_t)f);
            Vector3 middle(0.0f, 0.0f, 0.0f);
            for (uint32_t corner : mesh.faces[f]) middle += mesh.vertices[corner];
            middle = middle * (1.0f / (float)mesh.faces[f].size());
            if (normal.dotProduct(middle - center) <= 0.0f) return false;
        }
        return true;
    }

}

TEST_SUITE("MeshModeling") {

    TEST_CASE("extruding a face lifts it and walls the gap") {
        MeshTopology cube = makeCube();
        const std::vector<uint32_t> top = { 5 };

        const ModelingResult result = extrudeFaces(cube, top, Vector3(0.0f, 1.0f, 0.0f));
        REQUIRE(result.ok);
        CHECK(result.newFaces.size() == 4);
        CHECK(result.newVertices.size() == 4);
        CHECK(cube.faceCount() == 10);
        CHECK(cube.vertexCount() == 12);
        // A lifted top turns the cube into a box: still closed, still genus zero.
        CHECK(cube.eulerCharacteristic() == 2);
        CHECK(allQuads(cube));
        std::string error;
        CHECK(cube.validate(error));

        // The lid moved; the floor did not.
        CHECK(cube.vertices[0].y == doctest::Approx(0.0f));
        bool lifted = false;
        for (const Vector3& position : cube.vertices) {
            if (position.y > 1.5f) lifted = true;
        }
        CHECK(lifted);
    }

    TEST_CASE("an individual extrude does not weld the corners it shares") {
        MeshTopology cube = makeCube();
        const std::vector<uint32_t> top = { 5 };

        const ModelingResult result = extrudeFaces(cube, top, Vector3(0.0f, 1.0f, 0.0f), true);
        REQUIRE(result.ok);
        CHECK(result.newFaces.size() == 4);
        CHECK(result.newVertices.size() == 4);
        CHECK(cube.eulerCharacteristic() == 2);
    }

    TEST_CASE("extruding the whole closed shell has no boundary to wall") {
        MeshTopology cube = makeCube();
        const std::vector<uint32_t> all = { 0, 1, 2, 3, 4, 5 };

        const ModelingResult result = extrudeFaces(cube, all, Vector3(0.0f, 0.5f, 0.0f));
        REQUIRE(result.ok);
        CHECK(result.newFaces.empty());
        CHECK(result.newVertices.size() == 8);   // every corner is interior to the selection
        // The faces themselves move, so nothing stays behind: with no rim to wall, an extrude
        // of a closed shell is a translation of it. Leaving the emptied original in the mesh
        // would be geometry the user cannot select but can still raycast through.
        CHECK(cube.vertexCount() == 8);
        CHECK(cube.faceCount() == 6);
        CHECK(cube.eulerCharacteristic() == 2);
        float lowest = 10.0f;
        for (const Vector3& position : cube.vertices) lowest = std::min(lowest, position.y);
        CHECK(lowest == doctest::Approx(0.5f));
    }

    TEST_CASE("insetting a face rings it without opening the mesh") {
        MeshTopology cube = makeCube();
        const std::vector<uint32_t> top = { 5 };

        const ModelingResult result = insetFaces(cube, top, 0.25f);
        REQUIRE(result.ok);
        CHECK(result.newFaces.size() == 4);
        CHECK(cube.faceCount() == 10);
        CHECK(cube.eulerCharacteristic() == 2);
        CHECK(allQuads(cube));
        CHECK(allOutwardFrom(cube, Vector3(0.5f, 0.5f, 0.5f)));

        // The inner square sits a quarter of the way from each corner toward the centre of
        // the face, which is 0.5 - so the inset corner of a unit cube's top is at 0.125.
        bool foundInner = false;
        for (const Vector3& position : cube.vertices) {
            if (std::fabs(position.x - 0.125f) < 1e-4f && std::fabs(position.z - 0.125f) < 1e-4f &&
                position.y == doctest::Approx(1.0f)) {
                foundInner = true;
            }
        }
        CHECK(foundInner);
    }

    TEST_CASE("insetting with depth pushes the inner face along the normal") {
        MeshTopology cube = makeCube();
        const std::vector<uint32_t> top = { 5 };

        REQUIRE(insetFaces(cube, top, 0.5f, -0.25f).ok);
        // The depth is measured along the face normal (+Y here), so a negative depth buries
        // the inner face - while the ring it left behind and the floor stay at their heights.
        bool buried = false;
        for (const Vector3& position : cube.vertices) {
            if (position.y == doctest::Approx(0.75f)) buried = true;
        }
        CHECK(buried);
        float lowest = 10.0f;
        for (const Vector3& position : cube.vertices) lowest = std::min(lowest, position.y);
        CHECK(lowest == doctest::Approx(0.0f));
    }

    TEST_CASE("poke splits a face into a fan over its centre") {
        MeshTopology cube = makeCube();
        const std::vector<uint32_t> top = { 5 };

        const ModelingResult result = pokeFaces(cube, top);
        REQUIRE(result.ok);
        CHECK(result.newVertices.size() == 1);
        CHECK(cube.faceCount() == 9);   // five untouched + four triangles
        CHECK(cube.eulerCharacteristic() == 2);
        CHECK(allOutwardFrom(cube, Vector3(0.5f, 0.5f, 0.5f)));
    }

    TEST_CASE("simple subdivision preserves the Euler characteristic") {
        MeshTopology cube = makeCube();

        REQUIRE(subdivideSimple(cube).ok);
        CHECK(cube.vertexCount() == 26);   // V + E + F
        CHECK(cube.faceCount() == 24);
        CHECK(cube.edgeCount() == 48);
        CHECK(cube.eulerCharacteristic() == 2);
        CHECK(allQuads(cube));
        CHECK(allOutwardFrom(cube, Vector3(0.5f, 0.5f, 0.5f)));
        // No smoothing: the corners stay exactly where they were.
        CHECK(cube.vertices[0] == Vector3(0.0f, 0.0f, 0.0f));
    }

    TEST_CASE("catmull-clark rounds a cube and keeps it closed") {
        MeshTopology cube = makeCube();
        const Vector3 center(0.5f, 0.5f, 0.5f);

        REQUIRE(subdivideCatmullClark(cube).ok);
        CHECK(cube.vertexCount() == 26);
        CHECK(cube.faceCount() == 24);
        CHECK(cube.eulerCharacteristic() == 2);
        CHECK(allQuads(cube));
        CHECK(allOutwardFrom(cube, center));

        // The corner that was at the origin is pulled toward the middle by the valence rule:
        // Q is the mean of the three adjacent face points (1/3 each), R the mean of the three
        // edge midpoints (1/6 each), and the corner itself weighs n-3 = 0.
        float nearestSum = 100.0f;
        for (const Vector3& position : cube.vertices) {
            const float sum = position.x + position.y + position.z;
            if (sum < nearestSum) nearestSum = sum;
        }
        CHECK(nearestSum == doctest::Approx(3.0f * (2.0f / 9.0f)));

        // Rounder with each pass: the extremes come in, the middle goes out.
        float nearest = 10.0f;
        float farthest = 0.0f;
        for (const Vector3& position : cube.vertices) {
            const float distance = position.distance(center);
            nearest = std::min(nearest, distance);
            farthest = std::max(farthest, distance);
        }
        CHECK(farthest < 0.87f);   // a cube corner is sqrt(0.75) = 0.866 away
        CHECK(nearest > 0.4f);

        REQUIRE(subdivideCatmullClark(cube).ok);
        CHECK(cube.eulerCharacteristic() == 2);
        CHECK(cube.faceCount() == 96);
    }

    TEST_CASE("catmull-clark holds an open boundary flat instead of shrinking it") {
        MeshTopology plane = makePlane();
        REQUIRE(subdivideCatmullClark(plane).ok);

        // The rim vertices of a plane at y = 0 must stay at y = 0: the boundary rule is what
        // stops an open mesh from curling under when subdivided.
        for (const Vector3& position : plane.vertices) {
            CHECK(position.y == doctest::Approx(0.0f));
        }
        CHECK(plane.eulerCharacteristic() == 1);   // a disc: V - E + F = 1
    }

    TEST_CASE("solidify gives an open surface a rim and a shell") {
        MeshTopology plane = makePlane();

        const ModelingResult result = solidify(plane, 0.5f);
        REQUIRE(result.ok);
        CHECK(plane.vertexCount() == 18);
        // 4 original + 4 walls (the 2x2 plane has 8 boundary edges, and the 4 quads own 12
        // distinct edges: 4 interior, 8 rim) + 4 shell.
        CHECK(plane.faceCount() == 16);
        CHECK(plane.eulerCharacteristic() == 2);   // closed, so genus zero again
        std::string error;
        CHECK(plane.validate(error));

        // The shell really is 0.5 below the surface.
        bool foundBottom = false;
        for (const Vector3& position : plane.vertices) {
            if (position.y == doctest::Approx(-0.5f)) foundBottom = true;
        }
        CHECK(foundBottom);
        CHECK(result.newFaces.size() == 12);
    }

    TEST_CASE("translating elements moves the corners of the right kind") {
        MeshTopology cube = makeCube();
        const Vector3 delta(2.0f, 0.0f, 0.0f);

        REQUIRE(translateElements(cube, Element::Face, { 5 }, delta).ok);
        // The top face owns 3, 7, 6, 2 - and 2 and 3 also belong to faces that must not move.
        CHECK(cube.vertices[2].x == doctest::Approx(3.0f));
        CHECK(cube.vertices[6].x == doctest::Approx(3.0f));
        CHECK(cube.vertices[1].x == doctest::Approx(1.0f));

        MeshTopology other = makeCube();
        REQUIRE(translateElements(other, Element::Vertex, { 0 }, delta).ok);
        CHECK(other.vertices[0] == Vector3(2.0f, 0.0f, 0.0f));

        MeshTopology third = makeCube();
        std::vector<std::array<uint32_t, 2>> edges;
        third.getEdges(edges);
        REQUIRE(edges.size() == 12);
        const uint32_t endA = edges[0][0];
        const uint32_t endB = edges[0][1];
        const std::vector<Vector3> before = third.vertices;
        REQUIRE(translateElements(third, Element::Edge, { 0 }, delta).ok);
        CHECK(third.vertices[endA] == before[endA] + delta);
        CHECK(third.vertices[endB] == before[endB] + delta);
        // Exactly the two corners of that edge move; a face-mode drag would take four.
        size_t changed = 0;
        for (size_t v = 0; v < third.vertices.size(); ++v) {
            if (!(third.vertices[v] == before[v])) ++changed;
        }
        CHECK(changed == 2);
    }

    TEST_CASE("proportional editing falls off with distance") {
        MeshTopology grid;
        for (int z = 0; z < 5; ++z) {
            for (int x = 0; x < 5; ++x) grid.addVertex(Vector3((float)x, 0.0f, (float)z));
        }
        grid.addFace({ 0, 1, 6, 5 });
        grid.recalcNormals();

        const std::vector<uint32_t> centre = { 12 };   // (2, 0, 2)
        REQUIRE(proportionalTranslate(grid, centre, Vector3(0.0f, 1.0f, 0.0f), 2.0f,
                                      Falloff::Linear).ok);

        CHECK(grid.vertices[12].y == doctest::Approx(1.0f));
        CHECK(grid.vertices[11].y == doctest::Approx(0.5f));   // one step away
        CHECK(grid.vertices[10].y == doctest::Approx(0.0f));   // outside the radius
        CHECK(grid.vertices[0].y == doctest::Approx(0.0f));
    }

    TEST_CASE("a falloff of none is a plain move") {
        MeshTopology grid;
        for (int z = 0; z < 3; ++z) {
            for (int x = 0; x < 3; ++x) grid.addVertex(Vector3((float)x, 0.0f, (float)z));
        }
        grid.addFace({ 0, 1, 4, 3 });

        REQUIRE(proportionalTranslate(grid, { 0 }, Vector3(0.0f, 1.0f, 0.0f), 10.0f,
                                      Falloff::None).ok);
        CHECK(grid.vertices[0].y == doctest::Approx(1.0f));
        CHECK(grid.vertices[4].y == doctest::Approx(0.0f));
    }

    TEST_CASE("an operation that cannot run says so and leaves the mesh alone") {
        MeshTopology cube = makeCube();
        const size_t faces = cube.faceCount();
        const size_t vertices = cube.vertexCount();

        const ModelingResult missing = extrudeFaces(cube, { 99 }, Vector3(0.0f, 1.0f, 0.0f));
        CHECK(missing.ok == false);
        CHECK_FALSE(missing.message.empty());
        CHECK(cube.faceCount() == faces);
        CHECK(cube.vertexCount() == vertices);

        CHECK(insetFaces(cube, { 5 }, 0.0f).ok == false);
        CHECK(insetFaces(cube, { 5 }, 1.0f).ok == false);
        MeshTopology nothing;
        CHECK(solidify(nothing, 0.5f).ok == false);
        CHECK(translateElements(cube, Element::Vertex, { 404 }, Vector3::UNIT_X).ok == false);
        CHECK(mergeByDistance(cube, 2.0f).ok == false);   // welds the whole cube into a point
    }

    TEST_CASE("triangulate and weld round-trip a subdivided cube") {
        MeshTopology cube = makeCube();
        REQUIRE(subdivideSimple(cube).ok);
        REQUIRE(triangulate(cube).ok);
        CHECK(cube.isTriMesh());
        CHECK(cube.eulerCharacteristic() == 2);
        REQUIRE(mergeByDistance(cube, 1e-5f).ok);
        CHECK(cube.eulerCharacteristic() == 2);
    }

}
