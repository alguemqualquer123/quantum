// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "model/MeshTopology.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>

namespace quantum {

    namespace {

        // A float key that treats 0.0 and -0.0 as the same position, so an exact weld does
        // not split a vertex that only crossed a sign bit.
        uint32_t positionKey(float value) {
            if (value == 0.0f) return 0u;
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return bits;
        }

        uint32_t hashCombine(uint32_t seed, uint32_t value) {
            return seed ^ (value + 0x9e3779b9u + (seed << 6) + (seed >> 2));
        }

        struct PositionHash {
            size_t operator()(const std::array<uint32_t, 3>& key) const {
                uint32_t hash = key[0];
                hash = hashCombine(hash, key[1]);
                hash = hashCombine(hash, key[2]);
                return (size_t)hash;
            }
        };

        struct CellHash {
            size_t operator()(const std::array<int64_t, 3>& key) const {
                uint64_t hash = (uint64_t)key[0];
                hash = hash * 31u + (uint64_t)key[1];
                hash = hash * 31u + (uint64_t)key[2];
                return (size_t)hash;
            }
        };

        std::array<uint32_t, 3> quantize(const Vector3& position) {
            return { positionKey(position.x), positionKey(position.y), positionKey(position.z) };
        }

        std::array<int64_t, 3> cellOf(const Vector3& position, float cell) {
            return { (int64_t)std::floor(position.x / cell),
                     (int64_t)std::floor(position.y / cell),
                     (int64_t)std::floor(position.z / cell) };
        }

        // The normal of a polygon by Newell's method: the accumulated cross products of
        // consecutive corners about the origin. It is the area-weighted normal of a fan, so
        // it is still meaningful when the polygon is not planar, and unnormalized it doubles
        // as the weight a corner needs when its faces are averaged.
        Vector3 newellNormal(const std::vector<Vector3>& vertices, const std::vector<uint32_t>& corners) {
            Vector3 total(0.0f, 0.0f, 0.0f);
            for (size_t i = 0; i < corners.size(); ++i) {
                const Vector3& current = vertices[corners[i]];
                const Vector3& next = vertices[corners[(i + 1) % corners.size()]];
                total += Vector3(current.y * next.z - current.z * next.y,
                                 current.z * next.x - current.x * next.z,
                                 current.x * next.y - current.y * next.x);
            }
            return total;
        }

        uint64_t pairKey(uint32_t a, uint32_t b) {
            return a <= b ? ((uint64_t)a << 32) | (uint64_t)b
                          : ((uint64_t)b << 32) | (uint64_t)a;
        }

    }

    void MeshTopology::clear() {
        vertices.clear();
        faces.clear();
        normals.clear();
    }

    bool MeshTopology::empty() const {
        return faces.empty();
    }

    size_t MeshTopology::faceCount() const {
        return faces.size();
    }

    size_t MeshTopology::vertexCount() const {
        return vertices.size();
    }

    uint32_t MeshTopology::addVertex(const Vector3& position) {
        vertices.push_back(position);
        return (uint32_t)vertices.size() - 1u;
    }

    uint32_t MeshTopology::addFace(const std::vector<uint32_t>& corners) {
        if (corners.size() < 3) return INVALID_INDEX;
        faces.push_back(corners);
        return (uint32_t)faces.size() - 1u;
    }

    uint32_t MeshTopology::addTriangle(uint32_t a, uint32_t b, uint32_t c) {
        const std::vector<uint32_t> corners = { a, b, c };
        return addFace(corners);
    }

    void MeshTopology::reserveVertices(size_t count) {
        vertices.reserve(count);
    }

    void MeshTopology::reserveFaces(size_t count) {
        faces.reserve(count);
    }

    MeshTopology MeshTopology::fromTriangles(const std::vector<Vector3>& positions,
                                             const std::vector<uint32_t>& indices, float epsilon) {
        MeshTopology mesh;
        mesh.reserveVertices(positions.size());
        mesh.reserveFaces(indices.size() / 3u);

        mesh.vertices = positions;
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const std::vector<uint32_t> corners = { indices[i], indices[i + 1], indices[i + 2] };
            mesh.addFace(corners);
        }

        // 0 welds only corners at exactly the same position, which is what turns a shared
        // index into adjacency; a positive epsilon is the Merge by Distance tool.
        mesh.mergeByDistance(epsilon);
        return mesh;
    }

    void MeshTopology::toTriangles(std::vector<Vector3>& positions, std::vector<uint32_t>& indices) const {
        positions = vertices;
        indices.clear();
        for (const std::vector<uint32_t>& face : faces) {
            for (size_t i = 2; i < face.size(); ++i) {
                indices.push_back(face[0]);
                indices.push_back(face[i - 1]);
                indices.push_back(face[i]);
            }
        }
    }

    bool MeshTopology::isTriMesh() const {
        for (const std::vector<uint32_t>& face : faces) {
            if (face.size() != 3) return false;
        }
        return true;
    }

    const Vector3& MeshTopology::position(uint32_t vertex) const {
        return vertices[vertex];
    }

    AABB MeshTopology::bounds() const {
        AABB box;
        if (vertices.empty()) return box;
        box.setExtents(vertices[0], vertices[0]);
        for (size_t i = 1; i < vertices.size(); ++i) box.merge(vertices[i]);
        return box;
    }

    Vector3 MeshTopology::faceNormal(uint32_t face) const {
        Vector3 normal = newellNormal(vertices, faces[face]);
        normal.normalize();
        return normal;
    }

    void MeshTopology::recalcNormals() {
        normals.assign(vertices.size(), Vector3(0.0f, 0.0f, 0.0f));
        for (const std::vector<uint32_t>& face : faces) {
            const Vector3 weighted = newellNormal(vertices, face);
            for (uint32_t corner : face) normals[corner] += weighted;
        }
        for (Vector3& normal : normals) normal.normalize();
    }

    const Vector3& MeshTopology::vertexNormal(uint32_t vertex) const {
        return normals[vertex];
    }

    void MeshTopology::translate(const Vector3& delta) {
        for (Vector3& vertex : vertices) vertex += delta;
    }

    void MeshTopology::transform(const Matrix4& matrix) {
        for (Vector3& vertex : vertices) vertex = matrix * vertex;
        recalcNormals();
    }

    uint64_t MeshTopology::edgeKey(uint32_t a, uint32_t b) {
        return pairKey(a, b);
    }

    uint64_t MeshTopology::edgeCount() const {
        std::set<uint64_t> edges;
        for (const std::vector<uint32_t>& face : faces) {
            for (size_t i = 0; i < face.size(); ++i) {
                edges.insert(pairKey(face[i], face[(i + 1) % face.size()]));
            }
        }
        return (uint64_t)edges.size();
    }

    void MeshTopology::getEdges(std::vector<std::array<uint32_t, 2>>& edges) const {
        std::set<uint64_t> unique;
        for (const std::vector<uint32_t>& face : faces) {
            for (size_t i = 0; i < face.size(); ++i) {
                unique.insert(pairKey(face[i], face[(i + 1) % face.size()]));
            }
        }
        edges.clear();
        edges.reserve(unique.size());
        for (uint64_t key : unique) {
            edges.push_back({ (uint32_t)(key >> 32), (uint32_t)(key & 0xFFFFFFFFu) });
        }
    }

    uint32_t MeshTopology::edgeUses(uint32_t a, uint32_t b) const {
        const uint64_t key = pairKey(a, b);
        uint32_t uses = 0;
        for (const std::vector<uint32_t>& face : faces) {
            for (size_t i = 0; i < face.size(); ++i) {
                if (pairKey(face[i], face[(i + 1) % face.size()]) == key) {
                    ++uses;
                    break;
                }
            }
        }
        return uses;
    }

    bool MeshTopology::isBoundaryEdge(uint32_t a, uint32_t b) const {
        return edgeUses(a, b) < 2;
    }

    uint32_t MeshTopology::faceValence(uint32_t vertex) const {
        uint32_t valence = 0;
        for (const std::vector<uint32_t>& face : faces) {
            for (uint32_t corner : face) {
                if (corner == vertex) {
                    ++valence;
                    break;
                }
            }
        }
        return valence;
    }

    bool MeshTopology::isBoundaryVertex(uint32_t vertex) const {
        std::vector<std::array<uint32_t, 2>> edges;
        getEdges(edges);
        for (const std::array<uint32_t, 2>& edge : edges) {
            if ((edge[0] == vertex || edge[1] == vertex) && edgeUses(edge[0], edge[1]) < 2) return true;
        }
        return false;
    }

    void MeshTopology::vertexNeighbours(uint32_t vertex, std::vector<uint32_t>& out) const {
        std::vector<std::array<uint32_t, 2>> edges;
        getEdges(edges);
        out.clear();
        for (const std::array<uint32_t, 2>& edge : edges) {
            if (edge[0] == vertex) out.push_back(edge[1]);
            else if (edge[1] == vertex) out.push_back(edge[0]);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    void MeshTopology::faceNeighbours(uint32_t face, std::vector<uint32_t>& out) const {
        out.clear();
        const std::vector<uint32_t>& corners = faces[face];
        for (size_t i = 0; i < corners.size(); ++i) {
            const uint32_t a = corners[i];
            const uint32_t b = corners[(i + 1) % corners.size()];
            const uint64_t key = pairKey(a, b);
            for (size_t other = 0; other < faces.size(); ++other) {
                if (other == face) continue;
                const std::vector<uint32_t>& candidate = faces[other];
                for (size_t j = 0; j < candidate.size(); ++j) {
                    if (pairKey(candidate[j], candidate[(j + 1) % candidate.size()]) == key) {
                        out.push_back((uint32_t)other);
                        break;
                    }
                }
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    int64_t MeshTopology::eulerCharacteristic() const {
        return (int64_t)vertices.size() - (int64_t)edgeCount() + (int64_t)faces.size();
    }

    // `representative[old]` is the index of the corner that survives, always an earlier one
    // or the vertex itself. Faces are rewritten to the compacted slots and the dead positions
    // are dropped, so the mesh stays dense and renderable.
    void MeshTopology::remapVertices(const std::vector<uint32_t>& representative) {
        std::vector<uint32_t> oldToNew(vertices.size(), INVALID_INDEX);
        std::vector<Vector3> welded;
        welded.reserve(vertices.size());

        for (size_t old = 0; old < representative.size(); ++old) {
            const uint32_t survivor = representative[old];
            if (survivor >= vertices.size()) continue;
            if (oldToNew[survivor] == INVALID_INDEX) {
                oldToNew[survivor] = (uint32_t)welded.size();
                welded.push_back(vertices[survivor]);
            }
            oldToNew[old] = oldToNew[survivor];
        }

        for (std::vector<uint32_t>& face : faces) {
            for (uint32_t& corner : face) {
                if (corner < oldToNew.size()) corner = oldToNew[corner];
            }
        }
        vertices.swap(welded);

        pruneInvalid();
        removeUnusedVertices();
        recalcNormals();
    }

    size_t MeshTopology::mergeByDistance(float epsilon) {
        if (vertices.empty()) return 0;
        const size_t before = vertices.size();

        std::vector<uint32_t> representative(vertices.size());

        if (epsilon <= 0.0f) {
            std::unordered_map<std::array<uint32_t, 3>, uint32_t, PositionHash> seen;
            for (size_t i = 0; i < vertices.size(); ++i) {
                auto found = seen.find(quantize(vertices[i]));
                if (found == seen.end()) {
                    seen.insert({ quantize(vertices[i]), (uint32_t)i });
                    representative[i] = (uint32_t)i;
                } else {
                    representative[i] = found->second;
                }
            }
        } else {
            // Cells of exactly epsilon: any vertex within epsilon of another shares a cell or
            // sits in one of the 26 around it, so the neighbour scan is complete.
            const float cell = epsilon;
            const float squared = epsilon * epsilon;
            std::unordered_map<std::array<int64_t, 3>, std::vector<uint32_t>, CellHash> buckets;

            for (size_t i = 0; i < vertices.size(); ++i) {
                const std::array<int64_t, 3> center = cellOf(vertices[i], cell);
                uint32_t survivor = INVALID_INDEX;
                for (int64_t dx = -1; dx <= 1 && survivor == INVALID_INDEX; ++dx) {
                    for (int64_t dy = -1; dy <= 1 && survivor == INVALID_INDEX; ++dy) {
                        for (int64_t dz = -1; dz <= 1 && survivor == INVALID_INDEX; ++dz) {
                            auto found = buckets.find({ center[0] + dx, center[1] + dy, center[2] + dz });
                            if (found == buckets.end()) continue;
                            for (uint32_t candidate : found->second) {
                                if (vertices[candidate].squaredDistance(vertices[i]) <= squared) {
                                    survivor = candidate;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (survivor == INVALID_INDEX) {
                    survivor = (uint32_t)i;
                    buckets[center].push_back(survivor);
                }
                representative[i] = survivor;
            }
        }

        remapVertices(representative);
        return before - vertices.size();
    }

    void MeshTopology::triangulate() {
        std::vector<std::vector<uint32_t>> triangles;
        triangles.reserve(faces.size() * 2);
        for (const std::vector<uint32_t>& face : faces) {
            for (size_t i = 2; i < face.size(); ++i) {
                triangles.push_back({ face[0], face[i - 1], face[i] });
            }
        }
        faces.swap(triangles);
        recalcNormals();
    }

    bool MeshTopology::flipFace(uint32_t face) {
        if (face >= faces.size()) return false;
        std::reverse(faces[face].begin(), faces[face].end());
        recalcNormals();
        return true;
    }

    void MeshTopology::flipAll() {
        for (std::vector<uint32_t>& face : faces) std::reverse(face.begin(), face.end());
        recalcNormals();
    }

    size_t MeshTopology::deleteFaces(const std::vector<uint32_t>& selected, bool purgeUnusedVertices) {
        std::vector<bool> removed(faces.size(), false);
        for (uint32_t face : selected) {
            if (face < faces.size()) removed[face] = true;
        }

        std::vector<std::vector<uint32_t>> kept;
        kept.reserve(faces.size());
        size_t deleted = 0;
        for (size_t f = 0; f < faces.size(); ++f) {
            if (removed[f]) { ++deleted; continue; }
            kept.push_back(faces[f]);
        }
        faces.swap(kept);

        if (purgeUnusedVertices) removeUnusedVertices();
        recalcNormals();
        return deleted;
    }

    size_t MeshTopology::deleteVertices(const std::vector<uint32_t>& selected) {
        std::vector<bool> gone(vertices.size(), false);
        for (uint32_t vertex : selected) {
            if (vertex < vertices.size()) gone[vertex] = true;
        }

        std::vector<std::vector<uint32_t>> kept;
        kept.reserve(faces.size());
        size_t deleted = 0;
        for (const std::vector<uint32_t>& face : faces) {
            bool touches = false;
            for (uint32_t corner : face) {
                if (corner < gone.size() && gone[corner]) { touches = true; break; }
            }
            if (touches) { ++deleted; continue; }
            kept.push_back(face);
        }
        faces.swap(kept);

        removeUnusedVertices();
        recalcNormals();
        return deleted;
    }

    size_t MeshTopology::removeUnusedVertices() {
        std::vector<uint32_t> oldToNew(vertices.size(), INVALID_INDEX);
        std::vector<Vector3> kept;
        kept.reserve(vertices.size());
        for (const std::vector<uint32_t>& face : faces) {
            for (uint32_t corner : face) {
                if (corner >= oldToNew.size()) continue;
                if (oldToNew[corner] == INVALID_INDEX) {
                    oldToNew[corner] = (uint32_t)kept.size();
                    kept.push_back(vertices[corner]);
                }
            }
        }
        if (kept.size() == vertices.size()) return 0;

        size_t dropped = vertices.size() - kept.size();
        for (std::vector<uint32_t>& face : faces) {
            for (uint32_t& corner : face) {
                if (corner < oldToNew.size()) corner = oldToNew[corner];
            }
        }
        vertices.swap(kept);
        return dropped;
    }

    bool MeshTopology::validate(std::string& error) const {
        for (size_t f = 0; f < faces.size(); ++f) {
            const std::vector<uint32_t>& face = faces[f];
            if (face.size() < 3) {
                error = "face " + std::to_string(f) + " has fewer than three corners";
                return false;
            }
            for (size_t i = 0; i < face.size(); ++i) {
                if (face[i] >= vertices.size()) {
                    error = "face " + std::to_string(f) + " refers to vertex " + std::to_string(face[i]) +
                            " which does not exist";
                    return false;
                }
                if (face[i] == face[(i + 1) % face.size()]) {
                    error = "face " + std::to_string(f) + " repeats a corner, so it has a zero-length edge";
                    return false;
                }
            }
        }
        return true;
    }

    size_t MeshTopology::pruneInvalid() {
        std::vector<std::vector<uint32_t>> kept;
        kept.reserve(faces.size());
        size_t dropped = 0;
        for (const std::vector<uint32_t>& face : faces) {
            bool ok = face.size() >= 3;
            for (size_t i = 0; ok && i < face.size(); ++i) {
                if (face[i] >= vertices.size()) ok = false;
                else if (face[i] == face[(i + 1) % face.size()]) ok = false;
            }
            if (ok) kept.push_back(face);
            else ++dropped;
        }
        faces.swap(kept);
        return dropped;
    }

}
