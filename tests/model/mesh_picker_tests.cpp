// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT
//
// GPU-free unit tests of the element picker (engine/core/model/MeshPicker). Clicking a
// vertex, edge or face of a mesh in the viewport is the entry point of every modelling
// tool, and it is the one piece of the kernel that cannot be reasoned about by reading the
// topology: it depends on the ray, on the Möller-Trumbore sign conventions, and on the clip
// divide that turns a world point into an ImGui mouse coordinate.

#include "doctest.h"

#include "math/Matrix4.h"
#include "math/Ray.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "model/MeshPicker.h"
#include "model/MeshTopology.h"

#include <cmath>
#include <string>
#include <vector>

using namespace quantum;

namespace {

    // The same canonical cube as the topology tests: 8 welded corners, 6 outward quads,
    // face 5 being the +Y top and face 4 the -Y floor.
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

    // One quad in the z = 0 plane, wound so its normal points at +Z. A single face is the
    // only way to test culling without a second shell blocking the ray first.
    MeshTopology makeZFacingQuad() {
        MeshTopology mesh;
        mesh.addVertex(Vector3(0.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(1.0f, 0.0f, 0.0f));
        mesh.addVertex(Vector3(1.0f, 1.0f, 0.0f));
        mesh.addVertex(Vector3(0.0f, 1.0f, 0.0f));
        mesh.addFace({ 0, 1, 2, 3 });
        mesh.recalcNormals();
        return mesh;
    }

    const Vector2 kViewport(100.0f, 100.0f);

    // A view that actually separates depth: clip.x = 0.5x + 0.25z, clip.y = 0.5y + 0.25z.
    // The identity projects a cube along its z axis, which folds four of its corners onto
    // one pixel and makes an edge test ambiguous; this one keeps every corner and every
    // edge midpoint distinct, which is what a real perspective or ortho view does.
    Matrix4 makeObliqueView() {
        Matrix4 view;
        view.set(0, 0, 0.5f);
        view.set(2, 0, 0.25f);
        view.set(1, 1, 0.5f);
        view.set(2, 1, 0.25f);
        return view;
    }

}

TEST_SUITE("MeshPicker") {

    TEST_CASE("a fan-triangulated cube builds one primitive per triangle") {
        const MeshTopology cube = makeCube();
        MeshPicker picker;
        picker.build(cube);
        CHECK(picker.triangleCount() == 12);   // 6 quads, two triangles each
        CHECK_FALSE(picker.empty());

        MeshPicker none;
        none.build(MeshTopology());
        CHECK(none.empty());
        MeshRayHit hit;
        CHECK_FALSE(none.raycast(Ray(Vector3(0.5f, 5.0f, 0.5f), Vector3(0.0f, -1.0f, 0.0f)), hit));
        CHECK(hit.face == MeshTopology::INVALID_INDEX);

        // Rebuilding after an edit replaces the old primitives instead of adding to them.
        picker.build(makeZFacingQuad());
        CHECK(picker.triangleCount() == 2);
    }

    TEST_CASE("a ray from above hits the top face at the right distance") {
        MeshPicker picker;
        picker.build(makeCube());

        MeshRayHit hit;
        const Ray ray(Vector3(0.5f, 5.0f, 0.5f), Vector3(0.0f, -5.0f, 0.0f));
        REQUIRE(picker.raycast(ray, hit));
        CHECK(hit.face == 5);
        // The parameter, not the world length: the top sits at 4/5 of this ray.
        CHECK(hit.distance == doctest::Approx(0.8f));
        CHECK(hit.point.x == doctest::Approx(0.5f));
        CHECK(hit.point.y == doctest::Approx(1.0f));
        CHECK(hit.point.z == doctest::Approx(0.5f));
        // The contract the editor's own rays use: the point is the ray evaluated at the
        // reported distance, so a hit can be turned back into a RayReturn without re-solving.
        const Vector3 evaluated = ray.getPoint(hit.distance);
        CHECK(hit.point.distance(evaluated) < 1e-4f);
        CHECK(hit.normal.y == doctest::Approx(1.0f));
        // The weights sum to one, which is what lets a tool interpolate a UV or a weight.
        CHECK(hit.barycentric.x + hit.barycentric.y + hit.barycentric.z == doctest::Approx(1.0f));
    }

    TEST_CASE("the ray hits whichever shell it reaches first") {
        MeshPicker picker;
        picker.build(makeCube());

        MeshRayHit hit;
        const Ray ray(Vector3(0.25f, 5.0f, 0.40f), Vector3(0.0f, -6.0f, 0.0f));
        REQUIRE(picker.raycast(ray, hit));
        CHECK(hit.face == 5);
        CHECK(hit.distance == doctest::Approx(4.0f / 6.0f));   // the floor would be at 5/6

        // Off to the other side of the fan's diagonal, so the ray crosses the second
        // triangle of the split rather than running along its shared edge.
        REQUIRE(picker.raycast(Ray(Vector3(0.75f, 5.0f, 0.60f), Vector3(0.0f, -6.0f, 0.0f)), hit));
        CHECK(hit.face == 5);
    }

    TEST_CASE("the ray keeps its own length as the search bound") {
        MeshPicker picker;
        picker.build(makeCube());

        MeshRayHit hit;
        // Two units long from y = 5 reaches y = 3, so the top of the cube is out of range -
        // the parameter would be 2, and t is capped at 1.
        CHECK_FALSE(picker.raycast(Ray(Vector3(0.5f, 5.0f, 0.5f), Vector3(0.0f, -2.0f, 0.0f)), hit));
        CHECK(hit.face == MeshTopology::INVALID_INDEX);
        // Long enough now: the top is at t = 4/4.5.
        REQUIRE(picker.raycast(Ray(Vector3(0.5f, 5.0f, 0.5f), Vector3(0.0f, -4.5f, 0.0f)), hit));
        CHECK(hit.distance == doctest::Approx(4.0f / 4.5f));
        // A zero-length ray has no direction to travel.
        CHECK_FALSE(picker.raycast(Ray(Vector3(0.5f, 5.0f, 0.5f), Vector3(0.0f, 0.0f, 0.0f)), hit));
    }

    TEST_CASE("a ray that misses reports no face") {
        MeshPicker picker;
        picker.build(makeCube());

        MeshRayHit hit;
        CHECK_FALSE(picker.raycast(Ray(Vector3(5.0f, 5.0f, 5.0f), Vector3(0.0f, -9.0f, 0.0f)), hit));
        CHECK_FALSE(picker.raycast(Ray(Vector3(-1.0f, 0.5f, 0.5f), Vector3(0.0f, 0.0f, 1.0f)), hit));
    }

    TEST_CASE("a ray coplanar with a face is not a phantom hit") {
        MeshPicker picker;
        picker.build(makeCube());

        MeshRayHit grazing;
        // Parallel to the floor and level with it, but beside the cube: the determinant is
        // zero for every coplanar triangle, so the slab test must not turn a ray that lies
        // in a face's plane into a selection.
        CHECK_FALSE(picker.raycast(
            Ray(Vector3(-1.0f, 0.0f, -0.5f), Vector3(4.0f, 0.0f, 0.0f)), grazing));
        CHECK(grazing.face == MeshTopology::INVALID_INDEX);

        // Exactly along a corner of the floor, by contrast, genuinely is a hit: the ray runs
        // into vertex 0 on its way past the cube.
        MeshRayHit corner;
        CHECK(picker.raycast(Ray(Vector3(-1.0f, 0.0f, 0.0f), Vector3(4.0f, 0.0f, 0.0f)), corner));
        CHECK(corner.face != MeshTopology::INVALID_INDEX);
    }

    TEST_CASE("back-face culling keeps the far side of a one-sided surface") {
        MeshPicker picker;
        picker.build(makeZFacingQuad());

        // The quad's normal points at +Z, so its front face is the side a camera at +Z sees,
        // and a ray coming from -Z strikes the back of the sheet. `distance` is again the
        // parameter: the sheet sits halfway along a four-unit direction.
        MeshRayHit fromFront;
        REQUIRE(picker.raycast(Ray(Vector3(0.5f, 0.5f, 2.0f), Vector3(0.0f, 0.0f, -4.0f)), fromFront));
        CHECK(fromFront.face == 0);
        CHECK(fromFront.normal.z == doctest::Approx(1.0f));
        CHECK(fromFront.distance == doctest::Approx(0.5f));

        MeshRayHit fromBehind;
        REQUIRE(picker.raycast(Ray(Vector3(0.5f, 0.5f, -1.0f), Vector3(0.0f, 0.0f, 4.0f)), fromBehind));
        CHECK(fromBehind.distance == doctest::Approx(0.25f));

        // Same ray, culled: the tool that picks the surface under the cursor must not select
        // the inside of a single-sided sheet - which is how a hidden wall gets clicked.
        MeshRayHit culled;
        CHECK_FALSE(picker.raycast(Ray(Vector3(0.5f, 0.5f, -1.0f), Vector3(0.0f, 0.0f, 4.0f)),
                                   culled, true));
        MeshRayHit stillFront;
        CHECK(picker.raycast(Ray(Vector3(0.5f, 0.5f, 2.0f), Vector3(0.0f, 0.0f, -4.0f)),
                             stillFront, true));

        // A closed shell is unaffected: from above, the top face is front-facing either way.
        MeshPicker cube;
        cube.build(makeCube());
        MeshRayHit solid;
        CHECK(cube.raycast(Ray(Vector3(0.5f, 5.0f, 0.5f), Vector3(0.0f, -5.0f, 0.0f)), solid, true));
        CHECK(solid.face == 5);
    }

    TEST_CASE("projectPoint divides by w and flips y for the mouse") {
        Vector2 pixels(0.0f, 0.0f);

        // Clip origin is the centre of the viewport, and +y in clip space is -y in pixels.
        REQUIRE(MeshPicker::projectPoint(Matrix4(), kViewport, Vector3(0.0f, 0.0f, 0.0f), pixels));
        CHECK(pixels.x == doctest::Approx(50.0f));
        CHECK(pixels.y == doctest::Approx(50.0f));
        REQUIRE(MeshPicker::projectPoint(Matrix4(), kViewport, Vector3(0.5f, 0.5f, 0.0f), pixels));
        CHECK(pixels.x == doctest::Approx(75.0f));
        CHECK(pixels.y == doctest::Approx(25.0f));
        REQUIRE(MeshPicker::projectPoint(Matrix4(), kViewport, Vector3(-1.0f, 1.0f, 0.0f), pixels));
        CHECK(pixels.x == doctest::Approx(0.0f));
        CHECK(pixels.y == doctest::Approx(0.0f));

        // The engine keeps the translation in the fourth column (matrix[col][row], so
        // Matrix4::translateMatrix sets matrix[3][0..2]), so a translate pushes a point
        // right: half a clip unit is a quarter of the viewport.
        REQUIRE(MeshPicker::projectPoint(Matrix4::translateMatrix(0.2f, 0.0f, 0.0f), kViewport,
                                         Vector3(0.0f, 0.0f, 0.0f), pixels));
        CHECK(pixels.x == doctest::Approx(60.0f));
        CHECK(pixels.y == doctest::Approx(50.0f));

        // Every projection is the clip divide of the matrix product the engine itself does.
        const Matrix4 view = makeObliqueView();
        const Vector3 world(0.3f, -0.2f, 0.1f);
        const Vector4 clip = view * Vector4(world.x, world.y, world.z, 1.0f);
        REQUIRE(MeshPicker::projectPoint(view, kViewport, world, pixels));
        CHECK(pixels.x == doctest::Approx((clip.x / clip.w * 0.5f + 0.5f) * kViewport.x));
        CHECK(pixels.y == doctest::Approx((1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * kViewport.y));
    }

    TEST_CASE("projectPoint refuses a point outside the depth range") {
        Vector2 pixels(0.0f, 0.0f);
        // Identity leaves z as the clip depth, so anything past the near/far pair is out.
        CHECK_FALSE(MeshPicker::projectPoint(Matrix4(), kViewport, Vector3(0.0f, 0.0f, 2.0f), pixels));
        CHECK_FALSE(MeshPicker::projectPoint(Matrix4(), kViewport, Vector3(0.0f, 0.0f, -2.0f), pixels));
        CHECK(MeshPicker::projectPoint(Matrix4(), kViewport, Vector3(0.0f, 0.0f, 0.5f), pixels));
        // x and y are not clipped here: the viewport widget itself decides whether a point
        // is under the panel, and a vertex dragged off-screen must stay pickable by number.
        CHECK(MeshPicker::projectPoint(Matrix4(), kViewport, Vector3(4.0f, 4.0f, 0.0f), pixels));
    }

    TEST_CASE("pickVertex returns the nearest corner within the tolerance") {
        const MeshTopology cube = makeCube();
        const Matrix4 view = makeObliqueView();
        MeshPicker picker;
        picker.build(cube);

        // This view maps the cube to eight distinct pixels, so each corner is reachable on
        // its own - the check the editor will rely on when it highlights a vertex.
        for (size_t i = 0; i < cube.vertices.size(); ++i) {
            Vector2 expected(0.0f, 0.0f);
            REQUIRE(MeshPicker::projectPoint(view, kViewport, cube.vertices[i], expected));
            float pixels = -1.0f;
            CHECK(picker.pickVertex(cube, view, kViewport, expected, 4.0f, &pixels) == (uint32_t)i);
            CHECK(pixels == doctest::Approx(0.0f));
        }

        const Vector2 corner = Vector2(87.5f, 12.5f);   // vertex 6 under this view
        CHECK(picker.pickVertex(cube, view, kViewport, corner + Vector2(2.0f, 0.0f), 8.0f) == 6);
        CHECK(picker.pickVertex(cube, view, kViewport, corner + Vector2(2.0f, 0.0f), 1.0f) ==
              MeshTopology::INVALID_INDEX);

        // The middle of the cube is 8.8 pixels from its nearest corner: no hit in a 2-pixel
        // brush, which is what stops a click on a face from grabbing a vertex.
        Vector2 centre(0.0f, 0.0f);
        REQUIRE(MeshPicker::projectPoint(view, kViewport, Vector3(0.5f, 0.5f, 0.5f), centre));
        CHECK(picker.pickVertex(cube, view, kViewport, centre, 2.0f) ==
              MeshTopology::INVALID_INDEX);
    }

    TEST_CASE("pickEdge reports the index getEdges uses, which is what edge drags select") {
        const MeshTopology quad = makeZFacingQuad();
        std::vector<std::array<uint32_t, 2>> edges;
        quad.getEdges(edges);
        REQUIRE(edges.size() == 4);

        const Matrix4 view = makeObliqueView();
        MeshPicker picker;
        picker.build(quad);

        Vector2 a, b;
        for (size_t i = 0; i < edges.size(); ++i) {
            REQUIRE(MeshPicker::projectPoint(view, kViewport, quad.vertices[edges[i][0]], a));
            REQUIRE(MeshPicker::projectPoint(view, kViewport, quad.vertices[edges[i][1]], b));
            const Vector2 middle = (a + b) * 0.5f;
            float pixels = -1.0f;
            CHECK(picker.pickEdge(quad, view, kViewport, middle, 3.0f, &pixels) == (uint32_t)i);
            CHECK(pixels == doctest::Approx(0.0f));
        }

        // A cube's twelve edges, measured against the same ordering translateElements uses:
        // the silhouette centre of a box is genuinely ambiguous, so only a corner-adjacent
        // edge is asserted here, and the tolerance bounds how far off it the click may be.
        const MeshTopology cube = makeCube();
        std::vector<std::array<uint32_t, 2>> cubeEdges;
        cube.getEdges(cubeEdges);
        REQUIRE(cubeEdges.size() == 12);
        REQUIRE(MeshPicker::projectPoint(view, kViewport, cube.vertices[cubeEdges[0][0]], a));
        REQUIRE(MeshPicker::projectPoint(view, kViewport, cube.vertices[cubeEdges[0][1]], b));
        const Vector2 firstMiddle = (a + b) * 0.5f;
        CHECK(cubeEdges[0] == std::array<uint32_t, 2>({ 0, 1 }));
        CHECK(picker.pickEdge(cube, view, kViewport, firstMiddle, 3.0f) == 0);
        CHECK(picker.pickEdge(cube, view, kViewport, firstMiddle + Vector2(0.0f, 20.0f), 3.0f) ==
              MeshTopology::INVALID_INDEX);
    }

    TEST_CASE("a moved vertex is picked where it now is, not where it was") {
        MeshTopology cube = makeCube();
        const Matrix4 view = makeObliqueView();
        Vector2 before(0.0f, 0.0f);
        REQUIRE(MeshPicker::projectPoint(view, kViewport, cube.vertices[6], before));

        cube.vertices[6] += Vector3(0.4f, -0.4f, 0.0f);
        MeshPicker picker;
        picker.build(cube);

        Vector2 after(0.0f, 0.0f);
        REQUIRE(MeshPicker::projectPoint(view, kViewport, cube.vertices[6], after));
        CHECK(picker.pickVertex(cube, view, kViewport, before, 4.0f) != 6);
        CHECK(picker.pickVertex(cube, view, kViewport, after, 4.0f) == 6);
    }

}
