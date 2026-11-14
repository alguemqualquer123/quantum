// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef meshpicker_h
#define meshpicker_h

#include "Export.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Ray.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "model/MeshTopology.h"

#include <cstdint>
#include <vector>

namespace quantum {

    // Where a ray met a mesh, in the terms a modelling tool needs: the face, not just the
    // entity. The engine's own Ray only answers for AABB, OBB, sphere and physics bodies
    // (engine/core/math/Ray.h:67-92), which is enough to click an object but not enough to
    // select the triangle, edge or vertex under the cursor.
    struct MeshRayHit {
        bool hit = false;
        float distance = 0.0f;
        Vector3 point;
        Vector3 normal;
        uint32_t face = MeshTopology::INVALID_INDEX;
        // Barycentric weights of the hit inside that triangle, so a tool can interpolate any
        // per-vertex attribute (a UV for texture projection, a weight for skin painting).
        Vector3 barycentric;

        operator bool() const { return hit; }
    };

    // A static acceleration structure over one mesh, rebuilt whenever the mesh changes.
    // Editor-sized meshes (a few tens of thousands of triangles) build in well under a frame,
    // and a drag that mutates geometry rebuilds per gesture rather than per frame.
    class QUANTUM_API MeshPicker {

    public:

        void build(const MeshTopology& mesh);
        void clear();
        bool empty() const { return m_triangles.empty(); }
        size_t triangleCount() const { return m_triangles.size(); }

        // The nearest hit along the ray. `hit.distance` is the parameter along the ray's own
        // direction, exactly as RayReturn reports it, so ray.getPoint(hit.distance) is
        // hit.point. The direction carries the ray's reach: t runs from 0 to 1, and anything
        // the ray would meet beyond its own length is not a hit.
        bool raycast(const Ray& ray, MeshRayHit& out, bool cullBackfaces = false) const;

        // Screen-space element selection. `viewProjection` maps world to clip space with the
        // same matrix the viewport renders with, `viewport` is the image size in pixels, and
        // `mouse` is measured from the top-left, as ImGui reports it.
        // The returned index is into the mesh itself, and the nearest element within
        // `tolerancePixels` wins even when the cursor is not exactly on it.
        uint32_t pickVertex(const MeshTopology& mesh, const Matrix4& viewProjection,
            const Vector2& viewport, const Vector2& mouse, float tolerancePixels,
            float* outPixels = nullptr) const;
        uint32_t pickEdge(const MeshTopology& mesh, const Matrix4& viewProjection,
            const Vector2& viewport, const Vector2& mouse, float tolerancePixels,
            float* outPixels = nullptr) const;

        // Projects one world point into pixel coordinates. False when it is behind the camera.
        static bool projectPoint(const Matrix4& viewProjection, const Vector2& viewport,
            const Vector3& world, Vector2& outPixels);

    private:

        struct Triangle {
            uint32_t a, b, c;
            uint32_t face;
        };

        struct Node {
            AABB bounds;
            uint32_t first = 0;
            uint32_t count = 0;   // a leaf holds a run of primitives; an interior node holds none
            uint32_t left = MeshTopology::INVALID_INDEX;
            uint32_t right = MeshTopology::INVALID_INDEX;
        };

        uint32_t buildNode(uint32_t first, uint32_t count);
        bool hitTriangle(const Triangle& triangle, const Vector3& origin, const Vector3& direction,
            float maxDistance, MeshRayHit& out, bool cullBackfaces) const;

        std::vector<Triangle> m_triangles;
        std::vector<uint32_t> m_order;
        std::vector<Node> m_nodes;
        std::vector<Vector3> m_positions;
    };

}

#endif
