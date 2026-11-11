// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "model/MeshModeling.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace quantum {
namespace model {

    namespace {

        constexpr uint32_t kInvalid = MeshTopology::INVALID_INDEX;

        // Tags a (face, corner) pair so it cannot collide with a shared-vertex key, which
        // sets the top bit instead.
        uint64_t cornerKey(uint32_t face, uint32_t corner) {
            return ((uint64_t)face << 32) | (uint64_t)corner;
        }

        uint64_t sharedKey(uint32_t vertex) {
            return (1ull << 63) | (uint64_t)vertex;
        }

        uint64_t undirected(uint32_t a, uint32_t b) {
            return a <= b ? ((uint64_t)a << 32) | (uint64_t)b
                          : ((uint64_t)b << 32) | (uint64_t)a;
        }

        uint32_t highOf(uint64_t key) { return (uint32_t)(key >> 32); }
        uint32_t lowOf(uint64_t key) { return (uint32_t)(key & 0xFFFFFFFFu); }

        Vector3 lerp(const Vector3& a, const Vector3& b, float t) {
            return a + (b - a) * t;
        }

        Vector3 surfaceNormal(const MeshTopology& mesh, uint32_t vertex) {
            if (vertex < mesh.normals.size()) {
                const Vector3& normal = mesh.normals[vertex];
                if (normal.length() > 1e-8f) return normal.normalized();
            }
            return Vector3(0.0f, 1.0f, 0.0f);
        }

        // Solidify reads vertex normals, and a mesh that came from a file may not have had
        // them recomputed since it was loaded.
        void ensureNormals(MeshTopology& mesh) {
            if (mesh.normals.size() != mesh.vertices.size()) mesh.recalcNormals();
        }

        struct TopologyCounters {
            // How many faces run along each edge, and which vertices each vertex touches.
            std::unordered_map<uint64_t, uint32_t> edgeUses;
            std::unordered_map<uint32_t, uint32_t> valence;
            std::unordered_map<uint32_t, std::vector<uint32_t>> neighbours;
        };

        TopologyCounters collectCounters(const MeshTopology& mesh) {
            TopologyCounters counters;
            // Walked from the faces, not from getEdges(): getEdges reports each edge once, and
            // the number of faces running along an edge is exactly what separates a manifold
            // join from a boundary - which is the rule Catmull-Clark and the extrude walls both
            // branch on.
            for (const std::vector<uint32_t>& face : mesh.faces) {
                const size_t corners = face.size();
                for (size_t i = 0; i < corners; ++i) {
                    const uint32_t a = face[i];
                    const uint32_t b = face[(i + 1) % corners];
                    ++counters.edgeUses[undirected(a, b)];
                    std::vector<uint32_t>& ring = counters.neighbours[a];
                    if (std::find(ring.begin(), ring.end(), b) == ring.end()) ring.push_back(b);
                    std::vector<uint32_t>& other = counters.neighbours[b];
                    if (std::find(other.begin(), other.end(), a) == other.end()) other.push_back(a);
                    ++counters.valence[a];
                }
            }
            return counters;
        }

    }

    ModelingResult ModelingResult::failed(const std::string& reason) {
        ModelingResult result;
        result.ok = false;
        result.message = reason;
        return result;
    }

    void ModelingResult::absorb(const ModelingResult& other) {
        if (!other.ok) {
            ok = false;
            if (message.empty()) message = other.message;
            return;
        }
        newFaces.insert(newFaces.end(), other.newFaces.begin(), other.newFaces.end());
        newVertices.insert(newVertices.end(), other.newVertices.begin(), other.newVertices.end());
        movedVertices.insert(movedVertices.end(), other.movedVertices.begin(), other.movedVertices.end());
    }

    float falloffWeight(Falloff falloff, float t) {
        if (t <= 0.0f) return 1.0f;
        if (t >= 1.0f) return 0.0f;
        const float inside = 1.0f - t;
        switch (falloff) {
            case Falloff::None: return 1.0f;
            case Falloff::Linear: return inside;
            case Falloff::Smooth: return inside * inside * (3.0f - 2.0f * inside);
            case Falloff::Sphere: return std::sqrt(1.0f - t * t);
            case Falloff::InverseSquare: return 1.0f / (1.0f + 10.0f * t * t);
            case Falloff::Sharp: return inside * inside;
            case Falloff::Root: return std::sqrt(inside);
            case Falloff::Cubic: return inside * inside * inside;
        }
        return inside;
    }

    Vector3 faceCentroid(const MeshTopology& mesh, uint32_t face) {
        const std::vector<uint32_t>& corners = mesh.faces[face];
        Vector3 total(0.0f, 0.0f, 0.0f);
        for (uint32_t corner : corners) total += mesh.vertices[corner];
        return total * (1.0f / (float)corners.size());
    }

    ModelingResult translateElements(MeshTopology& mesh, Element element,
                                     const std::vector<uint32_t>& indices, const Vector3& delta) {
        if (mesh.faces.empty()) return ModelingResult::failed("the mesh is empty");

        std::vector<bool> picked(mesh.vertices.size(), false);
        std::vector<uint32_t> moved;

        if (element == Element::Vertex) {
            for (uint32_t vertex : indices) {
                if (vertex >= mesh.vertices.size()) return ModelingResult::failed(
                    "vertex " + std::to_string(vertex) + " does not exist");
                if (!picked[vertex]) { picked[vertex] = true; moved.push_back(vertex); }
            }
        } else if (element == Element::Edge) {
            std::vector<std::array<uint32_t, 2>> edges;
            mesh.getEdges(edges);
            for (uint32_t edge : indices) {
                if (edge >= edges.size()) return ModelingResult::failed(
                    "edge " + std::to_string(edge) + " does not exist");
                for (uint32_t corner : edges[edge]) {
                    if (!picked[corner]) { picked[corner] = true; moved.push_back(corner); }
                }
            }
        } else {
            for (uint32_t face : indices) {
                if (face >= mesh.faces.size()) return ModelingResult::failed(
                    "face " + std::to_string(face) + " does not exist");
                for (uint32_t corner : mesh.faces[face]) {
                    if (!picked[corner]) { picked[corner] = true; moved.push_back(corner); }
                }
            }
        }

        if (moved.empty()) return ModelingResult::failed("nothing is selected");
        for (uint32_t vertex : moved) mesh.vertices[vertex] += delta;
        mesh.recalcNormals();

        ModelingResult result;
        result.movedVertices = moved;
        return result;
    }

    ModelingResult proportionalTranslate(MeshTopology& mesh, const std::vector<uint32_t>& selection,
                                         const Vector3& delta, float radius, Falloff falloff) {
        if (mesh.vertices.empty()) return ModelingResult::failed("the mesh is empty");
        if (selection.empty()) return ModelingResult::failed("nothing is selected");

        std::vector<bool> selected(mesh.vertices.size(), false);
        std::vector<uint32_t> seeds;
        for (uint32_t vertex : selection) {
            if (vertex >= mesh.vertices.size()) {
                return ModelingResult::failed("vertex " + std::to_string(vertex) + " does not exist");
            }
            if (!selected[vertex]) { selected[vertex] = true; seeds.push_back(vertex); }
        }

        if (radius <= 0.0f || falloff == Falloff::None) {
            for (uint32_t seed : seeds) mesh.vertices[seed] += delta;
            mesh.recalcNormals();
            ModelingResult result;
            result.movedVertices = seeds;
            return result;
        }

        ModelingResult result;
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            if (selected[i]) continue;
            float nearest = radius;
            for (uint32_t seed : seeds) {
                const float distance = mesh.vertices[i].distance(mesh.vertices[seed]);
                if (distance < nearest) nearest = distance;
            }
            const float weight = falloffWeight(falloff, nearest / radius);
            if (weight <= 0.0f) continue;
            mesh.vertices[i] += delta * weight;
            result.movedVertices.push_back((uint32_t)i);
        }

        for (uint32_t seed : seeds) mesh.vertices[seed] += delta;
        result.movedVertices.insert(result.movedVertices.begin(), seeds.begin(), seeds.end());
        mesh.recalcNormals();
        return result;
    }

    ModelingResult extrudeFaces(MeshTopology& mesh, const std::vector<uint32_t>& selected,
                               const Vector3& offset, bool individual) {
        if (mesh.faces.empty()) return ModelingResult::failed("the mesh is empty");

        std::vector<uint32_t> region;
        {
            std::unordered_set<uint32_t> seen;
            for (uint32_t face : selected) {
                if (face >= mesh.faces.size()) {
                    return ModelingResult::failed("face " + std::to_string(face) + " does not exist");
                }
                if (seen.insert(face).second) region.push_back(face);
            }
        }
        if (region.empty()) return ModelingResult::failed("no face is selected");

        const TopologyCounters counters = collectCounters(mesh);

        // How many selected faces each corner belongs to, and how many selected faces run
        // along each edge: an edge with two is inside the region and must not get a wall.
        std::unordered_map<uint32_t, uint32_t> selectedVertexUses;
        std::unordered_map<uint64_t, uint32_t> selectedEdgeUses;
        std::unordered_map<uint32_t, std::vector<uint32_t>> outlines;
        for (uint32_t face : region) {
            const std::vector<uint32_t>& corners = mesh.faces[face];
            outlines[face] = corners;
            for (uint32_t corner : corners) ++selectedVertexUses[corner];
            for (size_t i = 0; i < corners.size(); ++i) {
                ++selectedEdgeUses[undirected(corners[i], corners[(i + 1) % corners.size()])];
            }
        }

        std::unordered_map<uint64_t, uint32_t> duplicates;
        ModelingResult result;

        for (uint32_t face : region) {
            const std::vector<uint32_t>& outline = outlines[face];
            std::vector<uint32_t> lifted;
            lifted.reserve(outline.size());

            for (uint32_t corner : outline) {
                // A corner every one of whose faces is selected has nothing left to stick to,
                // so the whole ring lifts as one vertex. Otherwise each face needs its own, or
                // the wall would be welded to the neighbour that stayed behind.
                const uint32_t totalUses = counters.valence.count(corner) ? counters.valence.at(corner) : 0u;
                const bool whollyInside = !individual && selectedVertexUses[corner] >= totalUses;
                const uint64_t key = whollyInside ? sharedKey(corner) : cornerKey(face, corner);

                auto found = duplicates.find(key);
                if (found == duplicates.end()) {
                    const uint32_t created = mesh.addVertex(mesh.vertices[corner] + offset);
                    duplicates[key] = created;
                    lifted.push_back(created);
                    result.newVertices.push_back(created);
                } else {
                    lifted.push_back(found->second);
                }
            }
            mesh.faces[face] = lifted;
        }

        for (uint32_t face : region) {
            const std::vector<uint32_t>& outline = outlines[face];
            for (size_t i = 0; i < outline.size(); ++i) {
                const uint32_t a = outline[i];
                const uint32_t b = outline[(i + 1) % outline.size()];
                if (!individual && selectedEdgeUses[undirected(a, b)] != 1) continue;

                const uint32_t usesA = counters.valence.count(a) ? counters.valence.at(a) : 0u;
                const uint32_t usesB = counters.valence.count(b) ? counters.valence.at(b) : 0u;
                const bool whollyA = !individual && selectedVertexUses[a] >= usesA;
                const bool whollyB = !individual && selectedVertexUses[b] >= usesB;
                const uint32_t liftedA = duplicates[whollyA ? sharedKey(a) : cornerKey(face, a)];
                const uint32_t liftedB = duplicates[whollyB ? sharedKey(b) : cornerKey(face, b)];

                // [a, b, b', a'] is the winding that faces away from the extruded region.
                const std::vector<uint32_t> wall = { a, b, liftedB, liftedA };
                const uint32_t created = mesh.addFace(wall);
                if (created != kInvalid) result.newFaces.push_back(created);
            }
        }

        mesh.removeUnusedVertices();
        mesh.recalcNormals();
        return result;
    }

    ModelingResult insetFaces(MeshTopology& mesh, const std::vector<uint32_t>& selected,
                             float amount, float depth) {
        if (mesh.faces.empty()) return ModelingResult::failed("the mesh is empty");
        if (amount <= 0.0f) return ModelingResult::failed("an inset needs an amount above zero");
        if (amount >= 1.0f) return ModelingResult::failed("an inset of 1 collapses the face to its centroid");

        ensureNormals(mesh);

        ModelingResult result;
        std::unordered_set<uint32_t> seen;
        for (uint32_t face : selected) {
            if (face >= mesh.faces.size()) {
                return ModelingResult::failed("face " + std::to_string(face) + " does not exist");
            }
            if (!seen.insert(face).second) continue;

            const std::vector<uint32_t> outline = mesh.faces[face];
            const Vector3 center = faceCentroid(mesh, face);
            Vector3 normal = mesh.faceNormal(face);
            if (normal.length() <= 1e-8f) normal = Vector3(0.0f, 1.0f, 0.0f);

            std::vector<uint32_t> inner;
            inner.reserve(outline.size());
            for (uint32_t corner : outline) {
                const uint32_t created = mesh.addVertex(
                    lerp(mesh.vertices[corner], center, amount) + normal * depth);
                inner.push_back(created);
                result.newVertices.push_back(created);
            }

            // The face becomes the inner polygon and the strip it left behind is the ring:
            // each outer edge and the edge between the two inset corners that replaced it.
            for (size_t i = 0; i < outline.size(); ++i) {
                const std::vector<uint32_t> ring = { outline[i], outline[(i + 1) % outline.size()],
                                                     inner[(i + 1) % inner.size()], inner[i] };
                const uint32_t created = mesh.addFace(ring);
                if (created != kInvalid) result.newFaces.push_back(created);
            }
            mesh.faces[face] = inner;
        }

        if (result.newVertices.empty()) return ModelingResult::failed("no face is selected");
        mesh.removeUnusedVertices();
        mesh.recalcNormals();
        return result;
    }

    ModelingResult pokeFaces(MeshTopology& mesh, const std::vector<uint32_t>& selected) {
        if (mesh.faces.empty()) return ModelingResult::failed("the mesh is empty");

        std::unordered_set<uint32_t> poked;
        for (uint32_t face : selected) {
            if (face >= mesh.faces.size()) {
                return ModelingResult::failed("face " + std::to_string(face) + " does not exist");
            }
            poked.insert(face);
        }
        if (poked.empty()) return ModelingResult::failed("no face is selected");

        std::vector<std::vector<uint32_t>> rebuilt;
        rebuilt.reserve(mesh.faces.size() + poked.size() * 4);
        ModelingResult result;

        for (size_t f = 0; f < mesh.faces.size(); ++f) {
            if (poked.find((uint32_t)f) == poked.end()) {
                rebuilt.push_back(mesh.faces[f]);
                continue;
            }
            const std::vector<uint32_t>& outline = mesh.faces[f];
            Vector3 center(0.0f, 0.0f, 0.0f);
            for (uint32_t corner : outline) center += mesh.vertices[corner];
            center = center * (1.0f / (float)outline.size());

            const uint32_t centerIndex = (uint32_t)mesh.vertices.size();
            mesh.vertices.push_back(center);
            result.newVertices.push_back(centerIndex);

            for (size_t i = 0; i < outline.size(); ++i) {
                rebuilt.push_back({ outline[i], outline[(i + 1) % outline.size()], centerIndex });
                result.newFaces.push_back((uint32_t)rebuilt.size() - 1u);
            }
        }

        mesh.faces.swap(rebuilt);
        mesh.recalcNormals();
        return result;
    }

    ModelingResult subdivideSimple(MeshTopology& mesh, uint32_t iterations) {
        for (uint32_t pass = 0; pass < iterations; ++pass) {
            std::unordered_map<uint64_t, uint32_t> edgeMids;
            std::vector<std::vector<uint32_t>> rebuilt;
            rebuilt.reserve(mesh.faces.size() * 4);

            for (const std::vector<uint32_t>& face : mesh.faces) {
                const size_t corners = face.size();
                std::vector<uint32_t> mids(corners, kInvalid);
                for (size_t i = 0; i < corners; ++i) {
                    const uint32_t a = face[i];
                    const uint32_t b = face[(i + 1) % corners];
                    const uint64_t key = undirected(a, b);
                    auto found = edgeMids.find(key);
                    if (found != edgeMids.end()) {
                        mids[i] = found->second;
                    } else {
                        const uint32_t created = (uint32_t)mesh.vertices.size();
                        mesh.vertices.push_back(mesh.vertices[a].midPoint(mesh.vertices[b]));
                        edgeMids[key] = created;
                        mids[i] = created;
                    }
                }

                Vector3 center(0.0f, 0.0f, 0.0f);
                for (uint32_t corner : face) center += mesh.vertices[corner];
                center = center * (1.0f / (float)corners);
                const uint32_t centerIndex = (uint32_t)mesh.vertices.size();
                mesh.vertices.push_back(center);

                for (size_t i = 0; i < corners; ++i) {
                    rebuilt.push_back({ face[i], mids[i], centerIndex,
                                        mids[(i + corners - 1) % corners] });
                }
            }

            mesh.faces.swap(rebuilt);
            mesh.removeUnusedVertices();
            mesh.recalcNormals();
        }

        ModelingResult result;
        return result;
    }

    ModelingResult subdivideCatmullClark(MeshTopology& mesh, uint32_t iterations) {
        for (uint32_t pass = 0; pass < iterations; ++pass) {
            const std::vector<std::vector<uint32_t>> oldFaces = mesh.faces;
            const std::vector<Vector3> oldPositions = mesh.vertices;
            if (oldFaces.empty()) return ModelingResult::failed("the mesh is empty");

            const TopologyCounters counters = collectCounters(mesh);

            std::unordered_map<uint32_t, std::vector<uint32_t>> facesOfVertex;
            for (size_t f = 0; f < oldFaces.size(); ++f) {
                for (uint32_t corner : oldFaces[f]) facesOfVertex[corner].push_back((uint32_t)f);
            }

            std::unordered_map<uint64_t, uint32_t> facePoint;
            std::unordered_map<uint64_t, uint32_t> edgePoint;
            std::unordered_map<uint32_t, uint32_t> vertexPoint;
            std::vector<Vector3> positions;
            positions.reserve(oldPositions.size() * 4);

            auto append = [&](const Vector3& position) {
                const uint32_t index = (uint32_t)positions.size();
                positions.push_back(position);
                return index;
            };

            for (size_t f = 0; f < oldFaces.size(); ++f) {
                Vector3 total(0.0f, 0.0f, 0.0f);
                for (uint32_t corner : oldFaces[f]) total += oldPositions[corner];
                facePoint[f] = append(total * (1.0f / (float)oldFaces[f].size()));
            }

            // Boundary edges first, because a boundary vertex counts them to pick its rule.
            std::unordered_map<uint32_t, std::vector<uint32_t>> boundaryRing;
            for (const auto& entry : counters.edgeUses) {
                const uint32_t a = highOf(entry.first);
                const uint32_t b = lowOf(entry.first);
                if (entry.second == 1) {
                    boundaryRing[a].push_back(b);
                    boundaryRing[b].push_back(a);
                }
            }

            for (const auto& entry : counters.edgeUses) {
                const uint32_t a = highOf(entry.first);
                const uint32_t b = lowOf(entry.first);
                const uint64_t key = undirected(a, b);
                const Vector3 midpoint = oldPositions[a].midPoint(oldPositions[b]);
                if (entry.second == 2) {
                    // The manifold rule: an edge point is a quarter of each endpoint and each
                    // adjacent face point, which is what makes the surface converge rather
                    // than shrink. Both neighbouring faces have to be found before the average.
                    const std::vector<uint32_t>& neighbours = facesOfVertex[a];
                    uint32_t first = kInvalid, second = kInvalid;
                    for (uint32_t f : neighbours) {
                        bool touches = false;
                        for (uint32_t corner : oldFaces[f]) {
                            if (corner == b) { touches = true; break; }
                        }
                        if (!touches) continue;
                        if (first == kInvalid) first = f;
                        else if (f != first) second = f;
                    }
                    if (first != kInvalid && second != kInvalid) {
                        const Vector3 point = (oldPositions[a] + oldPositions[b] +
                                               positions[facePoint[first]] +
                                               positions[facePoint[second]]) * 0.25f;
                        edgePoint[key] = append(point);
                        continue;
                    }
                }
                // A boundary edge, or a non-manifold one: the midpoint keeps the seam sharp.
                edgePoint[key] = append(midpoint);
            }

            for (size_t v = 0; v < oldPositions.size(); ++v) {
                const auto ring = boundaryRing.find((uint32_t)v);
                if (ring != boundaryRing.end() && ring->second.size() == 2) {
                    const Vector3 point = (oldPositions[v] * 3.0f +
                                           oldPositions[ring->second[0]] +
                                           oldPositions[ring->second[1]]) * 0.25f;
                    vertexPoint[(uint32_t)v] = append(point);
                    continue;
                }

                const uint32_t valence = counters.valence.count((uint32_t)v)
                                       ? counters.valence.at((uint32_t)v) : 0u;
                const auto neighbours = counters.neighbours.find((uint32_t)v);
                bool manifold = valence >= 3 && ring == boundaryRing.end();
                if (manifold && neighbours != counters.neighbours.end()) {
                    for (uint32_t other : neighbours->second) {
                        auto use = counters.edgeUses.find(undirected((uint32_t)v, other));
                        if (use == counters.edgeUses.end() || use->second != 2) manifold = false;
                    }
                }
                if (!manifold) {
                    // Anything the standard rules do not define keeps its position rather than
                    // averaging across a seam or collapsing a non-manifold join.
                    vertexPoint[(uint32_t)v] = append(oldPositions[v]);
                    continue;
                }

                Vector3 faceTotal(0.0f, 0.0f, 0.0f);
                const std::vector<uint32_t>& faces = facesOfVertex[(uint32_t)v];
                for (uint32_t f : faces) faceTotal += positions[facePoint[f]];
                const Vector3 q = faceTotal * (1.0f / (float)faces.size());

                Vector3 edgeTotal(0.0f, 0.0f, 0.0f);
                uint32_t edgeCount = 0;
                if (neighbours != counters.neighbours.end()) {
                    for (uint32_t other : neighbours->second) {
                        edgeTotal += oldPositions[v].midPoint(oldPositions[other]);
                        ++edgeCount;
                    }
                }
                if (edgeCount == 0) {
                    vertexPoint[(uint32_t)v] = append(oldPositions[v]);
                    continue;
                }
                const Vector3 r = edgeTotal * (1.0f / (float)edgeCount);
                const Vector3 point = (q + r * 2.0f +
                                        oldPositions[v] * (float)((int)valence - 3)) / (float)valence;
                vertexPoint[(uint32_t)v] = append(point);
            }

            std::vector<std::vector<uint32_t>> rebuilt;
            rebuilt.reserve(oldFaces.size() * 4);
            for (size_t f = 0; f < oldFaces.size(); ++f) {
                const size_t corners = oldFaces[f].size();
                for (size_t i = 0; i < corners; ++i) {
                    const uint32_t current = oldFaces[f][i];
                    const uint32_t next = oldFaces[f][(i + 1) % corners];
                    const uint32_t previous = oldFaces[f][(i + corners - 1) % corners];
                    rebuilt.push_back({ vertexPoint[current],
                                        edgePoint[undirected(current, next)],
                                        facePoint[f],
                                        edgePoint[undirected(previous, current)] });
                }
            }

            mesh.vertices.swap(positions);
            mesh.faces.swap(rebuilt);
            mesh.recalcNormals();
        }

        ModelingResult result;
        return result;
    }

    ModelingResult solidify(MeshTopology& mesh, float thickness) {
        if (mesh.faces.empty()) return ModelingResult::failed("the mesh is empty");
        if (thickness == 0.0f) return ModelingResult::failed("a solidify of zero thickness makes no shell");

        ensureNormals(mesh);

        const size_t originalCount = mesh.vertices.size();

        // The rim has to be found before the shell is added: every boundary edge would
        // otherwise already have a second face using it.
        std::vector<std::array<uint32_t, 2>> rim;
        {
            std::vector<std::array<uint32_t, 2>> edges;
            mesh.getEdges(edges);
            for (const std::array<uint32_t, 2>& edge : edges) {
                if (mesh.isBoundaryEdge(edge[0], edge[1])) rim.push_back(edge);
            }
        }
        // Which face owns each rim edge decides the wall's winding, and the wall must run
        // along the outline of that face, not the mirrored one.
        std::unordered_map<uint64_t, uint32_t> rimOwner;
        for (size_t f = 0; f < mesh.faces.size(); ++f) {
            const std::vector<uint32_t>& corners = mesh.faces[f];
            for (size_t i = 0; i < corners.size(); ++i) {
                rimOwner[undirected(corners[i], corners[(i + 1) % corners.size()])] = (uint32_t)f;
            }
        }

        ModelingResult result;

        std::vector<std::vector<uint32_t>> shell;
        shell.reserve(mesh.faces.size());
        for (const std::vector<uint32_t>& face : mesh.faces) {
            std::vector<uint32_t> flipped(face.rbegin(), face.rend());
            for (uint32_t& corner : flipped) corner += (uint32_t)originalCount;
            shell.push_back(flipped);
        }

        for (size_t v = 0; v < originalCount; ++v) {
            result.newVertices.push_back((uint32_t)mesh.vertices.size());
            mesh.vertices.push_back(mesh.vertices[v] - surfaceNormal(mesh, (uint32_t)v) * thickness);
        }

        // The wall goes in before the shell: [a, b, b', a'] faces away from the solid, and an
        // edge of the flipped shell would wind it inward.
        for (const std::array<uint32_t, 2>& edge : rim) {
            auto owner = rimOwner.find(undirected(edge[0], edge[1]));
            if (owner == rimOwner.end()) continue;
            uint32_t a = edge[0], b = edge[1];
            const std::vector<uint32_t>& corners = mesh.faces[owner->second];
            bool sameDirection = false;
            for (size_t i = 0; i < corners.size(); ++i) {
                if (corners[i] == a && corners[(i + 1) % corners.size()] == b) { sameDirection = true; break; }
            }
            if (!sameDirection) std::swap(a, b);

            const std::vector<uint32_t> wall = { a, b,
                                                 b + (uint32_t)originalCount,
                                                 a + (uint32_t)originalCount };
            const uint32_t created = mesh.addFace(wall);
            if (created != kInvalid) result.newFaces.push_back(created);
        }

        for (const std::vector<uint32_t>& face : shell) {
            const uint32_t created = mesh.addFace(face);
            if (created != kInvalid) result.newFaces.push_back(created);
        }

        mesh.recalcNormals();
        return result;
    }

    ModelingResult triangulate(MeshTopology& mesh) {
        if (mesh.faces.empty()) return ModelingResult::failed("the mesh is empty");
        mesh.triangulate();
        ModelingResult result;
        return result;
    }

    ModelingResult mergeByDistance(MeshTopology& mesh, float epsilon) {
        if (mesh.vertices.empty()) return ModelingResult::failed("the mesh is empty");
        mesh.mergeByDistance(epsilon);
        if (mesh.faces.empty()) return ModelingResult::failed("welding removed every face");
        ModelingResult result;
        return result;
    }

}
}
