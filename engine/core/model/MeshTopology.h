// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef meshtopology_h
#define meshtopology_h

#include "Export.h"
#include "math/Vector3.h"
#include "math/Matrix4.h"
#include "math/AABB.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace quantum {

    // A polygon mesh in the form a modelling tool needs: n-gon faces over one shared
    // position list, so an edge has two faces and a vertex has a ring.
    //
    // MeshComponent is not this. It is an interleaved byte buffer plus an index buffer
    // (engine/core/component/MeshComponent.h), and the geometry of an imported model is a
    // non-owning pointer into the tinygltf model that produced it. Nothing in the engine
    // can answer "which faces touch this edge", so every edit operation would have to
    // deduce adjacency from float comparison on every call. This is the structure they
    // work on instead, and it is deliberately free of GPU, scene and entity state: the
    // same code runs in the editor, in a game build and in the console test runner.
    class QUANTUM_API MeshTopology {

    public:

        static constexpr uint32_t INVALID_INDEX = 0xFFFFFFFFu;

        // Positions, one per vertex.
        std::vector<Vector3> vertices;
        // Corner lists, wound counter-clockwise seen from the outside. A face may hold any
        // number of corners; the modelling operations preserve quads and n-gons rather than
        // collapsing everything to triangles.
        std::vector<std::vector<uint32_t>> faces;
        // Area-weighted vertex normals, parallel to `vertices`. Filled by recalcNormals(),
        // which every operation that changes connectivity calls before it returns.
        std::vector<Vector3> normals;

        MeshTopology() = default;

        void clear();
        bool empty() const;
        size_t faceCount() const;
        size_t vertexCount() const;

        // ---------- building ----------

        uint32_t addVertex(const Vector3& position);
        // Appends a face; returns its index, or INVALID_INDEX when it has under three corners.
        uint32_t addFace(const std::vector<uint32_t>& corners);
        uint32_t addTriangle(uint32_t a, uint32_t b, uint32_t c);
        void reserveVertices(size_t count);
        void reserveFaces(size_t count);

        // A triangle soup becomes a mesh with shared corners: two triangles that agree on a
        // position become one vertex, which is what makes an edge ring and a vertex ring
        // meaningful. `epsilon` is the weld distance, and 0 means "trust the index buffer".
        static MeshTopology fromTriangles(const std::vector<Vector3>& positions,
            const std::vector<uint32_t>& indices, float epsilon = 0.0f);

        // The other way: a fan-split copy, one entry per triangle. Faces are not preserved.
        void toTriangles(std::vector<Vector3>& positions, std::vector<uint32_t>& indices) const;
        bool isTriMesh() const;

        // ---------- geometry ----------

        const Vector3& position(uint32_t vertex) const;
        AABB bounds() const;
        // Newell's method: correct for a non-planar n-gon, where crossing the diagonals is not.
        Vector3 faceNormal(uint32_t face) const;
        void recalcNormals();
        const Vector3& vertexNormal(uint32_t vertex) const;
        void translate(const Vector3& delta);
        // Positions only; normals follow from recalcNormals().
        void transform(const Matrix4& matrix);

        // ---------- topology ----------

        uint64_t edgeCount() const;
        // Unique undirected edges, as vertex pairs. Order is stable for a given mesh.
        void getEdges(std::vector<std::array<uint32_t, 2>>& edges) const;
        bool isBoundaryEdge(uint32_t a, uint32_t b) const;
        // How many faces use this vertex; the valence that Catmull-Clark branches on.
        uint32_t faceValence(uint32_t vertex) const;
        bool isBoundaryVertex(uint32_t vertex) const;
        // Vertices joined to this one by an edge, in no particular order.
        void vertexNeighbours(uint32_t vertex, std::vector<uint32_t>& out) const;
        void faceNeighbours(uint32_t face, std::vector<uint32_t>& out) const;
        // V - E + F, the invariant a topology edit must not break on a closed mesh.
        int64_t eulerCharacteristic() const;

        // ---------- element operations ----------

        // Welds corners that agree within `epsilon`. Returns the number of vertices removed.
        size_t mergeByDistance(float epsilon);
        // Splits every face with more than three corners into triangles.
        void triangulate();
        bool flipFace(uint32_t face);
        void flipAll();
        // Removes faces (and the faces touching a removed vertex). Returns faces deleted.
        size_t deleteFaces(const std::vector<uint32_t>& selected, bool purgeUnusedVertices = true);
        size_t deleteVertices(const std::vector<uint32_t>& selected);
        size_t removeUnusedVertices();

        // ---------- checks ----------

        // Out-of-range corners, faces under three corners, and repeated corners inside one
        // face. `error` gets the first problem found, in English, for the tool that ran it.
        bool validate(std::string& error) const;
        // Drops faces that validate() would reject, so a loaded file never poisons an edit.
        size_t pruneInvalid();

    private:

        // The shared key of an undirected edge, ordered so both directions hash together.
        static uint64_t edgeKey(uint32_t a, uint32_t b);
        // How many faces run along this edge: 1 is a boundary, 2 is manifold, more is not.
        uint32_t edgeUses(uint32_t a, uint32_t b) const;
        void remapVertices(const std::vector<uint32_t>& mapping);
    };

}

#endif
