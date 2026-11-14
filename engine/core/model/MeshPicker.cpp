// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "model/MeshPicker.h"

#include "math/Vector4.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace quantum {

    namespace {

        constexpr uint32_t kInvalid = MeshTopology::INVALID_INDEX;
        constexpr uint32_t kLeafSize = 4;

        AABB triangleBounds(const Vector3& a, const Vector3& b, const Vector3& c) {
            Vector3 minimum = a;
            Vector3 maximum = a;
            for (const Vector3* point : { &b, &c }) {
                minimum.x = std::min(minimum.x, point->x);
                minimum.y = std::min(minimum.y, point->y);
                minimum.z = std::min(minimum.z, point->z);
                maximum.x = std::max(maximum.x, point->x);
                maximum.y = std::max(maximum.y, point->y);
                maximum.z = std::max(maximum.z, point->z);
            }
            AABB box;
            box.setExtents(minimum, maximum);
            return box;
        }

        // The slab test: the ray's parameter interval clipped by each axis pair. AABB's own
        // intersects() compares two volumes, and a ray is not one.
        bool rayOverlapsBox(const Vector3& origin, const Vector3& direction, const AABB& box,
                            float maxDistance) {
            const Vector3& minimum = box.getMinimum();
            const Vector3& maximum = box.getMaximum();
            float nearT = 0.0f;
            float farT = maxDistance;

            for (int axis = 0; axis < 3; ++axis) {
                const float o = origin[axis];
                const float d = direction[axis];
                const float lo = minimum[axis];
                const float hi = maximum[axis];

                if (std::fabs(d) < 1e-8f) {
                    if (o < lo || o > hi) return false;
                    continue;
                }
                float t0 = (lo - o) / d;
                float t1 = (hi - o) / d;
                if (t0 > t1) std::swap(t0, t1);
                nearT = std::max(nearT, t0);
                farT = std::min(farT, t1);
                if (nearT > farT) return false;
            }
            return true;
        }

        float distanceToSegment(const Vector2& point, const Vector2& a, const Vector2& b) {
            const Vector2 ab = b - a;
            const float squared = ab.dotProduct(ab);
            if (squared <= 1e-12f) return point.distance(a);
            float t = (point - a).dotProduct(ab) / squared;
            t = std::max(0.0f, std::min(1.0f, t));
            return point.distance(a + ab * t);
        }

    }

    void MeshPicker::clear() {
        m_triangles.clear();
        m_order.clear();
        m_nodes.clear();
        m_positions.clear();
    }

    void MeshPicker::build(const MeshTopology& mesh) {
        clear();
        m_positions = mesh.vertices;

        for (size_t f = 0; f < mesh.faces.size(); ++f) {
            const std::vector<uint32_t>& corners = mesh.faces[f];
            for (size_t i = 2; i < corners.size(); ++i) {
                if (corners[0] >= mesh.vertices.size() || corners[i - 1] >= mesh.vertices.size() ||
                    corners[i] >= mesh.vertices.size()) {
                    continue;
                }
                const Triangle triangle = { corners[0], corners[i - 1], corners[i], (uint32_t)f };
                m_triangles.push_back(triangle);
            }
        }

        if (m_triangles.empty()) return;

        m_order.resize((uint32_t)m_triangles.size());
        for (uint32_t i = 0; i < m_order.size(); ++i) m_order[i] = i;

        m_nodes.reserve(m_order.size() / kLeafSize * 2);
        buildNode(0, (uint32_t)m_order.size());
    }

    uint32_t MeshPicker::buildNode(uint32_t first, uint32_t count) {
        const uint32_t node = (uint32_t)m_nodes.size();
        m_nodes.push_back(Node());

        AABB bounds;
        bool seeded = false;
        Vector3 centroidTotal(0.0f, 0.0f, 0.0f);
        for (uint32_t i = first; i < first + count; ++i) {
            const Triangle& triangle = m_triangles[m_order[i]];
            const AABB box = triangleBounds(m_positions[triangle.a], m_positions[triangle.b],
                                           m_positions[triangle.c]);
            if (!seeded) {
                bounds = box;
                seeded = true;
            } else {
                bounds.merge(box);
            }
            centroidTotal += (box.getMinimum() + box.getMaximum()) * 0.5f;
        }

        m_nodes[node].bounds = bounds;

        if (count <= kLeafSize) {
            m_nodes[node].first = first;
            m_nodes[node].count = count;
            return node;
        }

        // Split on the widest axis at the mean centroid. A median-of-centroids split is enough
        // here: the tree is rebuilt on every geometry edit, so build cost matters more than the
        // few percent a surface-area heuristic would buy.
        const Vector3 size = bounds.getMaximum() - bounds.getMinimum();
        int axis = 0;
        if (size.y > size.x && size.y >= size.z) axis = 1;
        else if (size.z > size.x && size.z > size.y) axis = 2;

        const float pivot = centroidTotal[axis] / (float)count;
        auto middle = std::partition(m_order.begin() + first, m_order.begin() + first + count,
            [&](uint32_t index) {
                const Triangle& triangle = m_triangles[index];
                const Vector3 center = (m_positions[triangle.a] + m_positions[triangle.b] +
                                        m_positions[triangle.c]) * (1.0f / 3.0f);
                return center[axis] < pivot;
            });

        const uint32_t split = (uint32_t)(middle - m_order.begin());
        if (split == first || split >= first + count) {
            // Degenerate distribution (a fan of triangles sharing a centroid plane): make a
            // leaf rather than recursing forever.
            m_nodes[node].first = first;
            m_nodes[node].count = count;
            return node;
        }

        // Children first, then the writes by index: the two recursive calls grow m_nodes and
        // any reference taken into it before them would dangle.
        const uint32_t leftChild = buildNode(first, split - first);
        const uint32_t rightChild = buildNode(split, first + count - split);
        m_nodes[node].left = leftChild;
        m_nodes[node].right = rightChild;
        return node;
    }

    bool MeshPicker::hitTriangle(const Triangle& triangle, const Vector3& origin,
                                 const Vector3& direction, float maxDistance, MeshRayHit& out,
                                 bool cullBackfaces) const {
        const Vector3& a = m_positions[triangle.a];
        const Vector3 edge1 = m_positions[triangle.b] - a;
        const Vector3 edge2 = m_positions[triangle.c] - a;

        const Vector3 p = direction.crossProduct(edge2);
        const float determinant = edge1.dotProduct(p);
        if (std::fabs(determinant) < 1e-8f) return false;

        if (cullBackfaces && determinant <= 0.0f) return false;

        const float inverse = 1.0f / determinant;
        const Vector3 s = origin - a;
        const float u = s.dotProduct(p) * inverse;
        if (u < 0.0f || u > 1.0f) return false;

        const Vector3 q = s.crossProduct(edge1);
        const float v = direction.dotProduct(q) * inverse;
        if (v < 0.0f || u + v > 1.0f) return false;

        const float t = edge2.dotProduct(q) * inverse;
        if (t < 0.0f || t > maxDistance) return false;

        out.distance = t;
        out.point = origin + direction * t;
        out.face = triangle.face;
        out.barycentric = Vector3(1.0f - u - v, u, v);
        out.normal = edge1.crossProduct(edge2);
        out.normal.normalize();
        return true;
    }

    bool MeshPicker::raycast(const Ray& ray, MeshRayHit& out, bool cullBackfaces) const {
        if (m_nodes.empty()) return false;

        const Vector3 origin = ray.getOrigin();
        const Vector3 direction = ray.getDirection();
        if (direction.squaredLength() <= 0.0f) return false;

        // `distance` is the parameter along the ray's own direction, which is what RayReturn
        // reports and what makes `ray.getPoint(hit.distance)` reproduce `hit.point`. The
        // direction carries the ray's reach, so the search ends at t = 1 and a unit-length
        // direction is a one-unit ray - the same rule as engine/core/math/Ray.h.
        constexpr float kRayEnd = 1.0f;

        MeshRayHit best;
        best.distance = kRayEnd;
        bool found = false;

        // Breadth-first on an explicit stack: the nearest hit is the one a modeller wants, and
        // an unsorted depth-first walk can find a far triangle first.
        std::vector<uint32_t> stack;
        stack.push_back(0);

        while (!stack.empty()) {
            const uint32_t nodeIndex = stack.back();
            stack.pop_back();
            const Node& node = m_nodes[nodeIndex];

            if (node.count > 0) {
                for (uint32_t i = node.first; i < node.first + node.count; ++i) {
                    MeshRayHit candidate;
                    if (!hitTriangle(m_triangles[m_order[i]], origin, direction, best.distance,
                                     candidate, cullBackfaces)) {
                        continue;
                    }
                    best = candidate;
                    found = true;
                }
                continue;
            }

            if (node.left != kInvalid &&
                rayOverlapsBox(origin, direction, m_nodes[node.left].bounds, best.distance)) {
                stack.push_back(node.left);
            }
            if (node.right != kInvalid &&
                rayOverlapsBox(origin, direction, m_nodes[node.right].bounds, best.distance)) {
                stack.push_back(node.right);
            }
        }

        out = best;
        return found;
    }

    bool MeshPicker::projectPoint(const Matrix4& viewProjection, const Vector2& viewport,
                                  const Vector3& world, Vector2& outPixels) {
        const Vector4 clip = viewProjection * Vector4(world.x, world.y, world.z, 1.0f);
        if (std::fabs(clip.w) < 1e-8f) return false;

        const float inverse = 1.0f / clip.w;
        const float x = clip.x * inverse;
        const float y = clip.y * inverse;
        const float z = clip.z * inverse;
        if (z < -1.0f || z > 1.0f) return false;

        outPixels.x = (x * 0.5f + 0.5f) * viewport.x;
        // ImGui measures the mouse from the top-left, clip space from the bottom.
        outPixels.y = (1.0f - (y * 0.5f + 0.5f)) * viewport.y;
        return true;
    }

    uint32_t MeshPicker::pickVertex(const MeshTopology& mesh, const Matrix4& viewProjection,
                                    const Vector2& viewport, const Vector2& mouse,
                                    float tolerancePixels, float* outPixels) const {
        uint32_t best = kInvalid;
        float bestDistance = tolerancePixels;
        Vector2 projected(0.0f, 0.0f);

        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            if (!projectPoint(viewProjection, viewport, mesh.vertices[i], projected)) continue;
            const float distance = mouse.distance(projected);
            if (distance <= bestDistance) {
                bestDistance = distance;
                best = (uint32_t)i;
            }
        }

        if (outPixels) *outPixels = best == kInvalid ? -1.0f : bestDistance;
        return best;
    }

    uint32_t MeshPicker::pickEdge(const MeshTopology& mesh, const Matrix4& viewProjection,
                                  const Vector2& viewport, const Vector2& mouse,
                                  float tolerancePixels, float* outPixels) const {
        std::vector<std::array<uint32_t, 2>> edges;
        mesh.getEdges(edges);

        uint32_t best = kInvalid;
        float bestDistance = tolerancePixels;
        Vector2 start(0.0f, 0.0f), end(0.0f, 0.0f);

        for (size_t i = 0; i < edges.size(); ++i) {
            if (edges[i][0] >= mesh.vertices.size() || edges[i][1] >= mesh.vertices.size()) continue;
            if (!projectPoint(viewProjection, viewport, mesh.vertices[edges[i][0]], start)) continue;
            if (!projectPoint(viewProjection, viewport, mesh.vertices[edges[i][1]], end)) continue;

            const float distance = distanceToSegment(mouse, start, end);
            if (distance <= bestDistance) {
                bestDistance = distance;
                best = (uint32_t)i;
            }
        }

        if (outPixels) *outPixels = best == kInvalid ? -1.0f : bestDistance;
        return best;
    }

}
