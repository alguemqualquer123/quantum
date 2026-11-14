// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "model/MeshReport.h"

#include "math/AABB.h"

#include <algorithm>
#include <unordered_map>

namespace quantum {
namespace model {

    namespace {

        using EdgeCounts = std::unordered_map<uint64_t, uint32_t>;

        uint64_t edgeKey(uint32_t a, uint32_t b) {
            return a < b ? (uint64_t(a) << 32) | uint64_t(b)
                         : (uint64_t(b) << 32) | uint64_t(a);
        }

        // Counted here rather than taken from MeshTopology::getEdges because the interesting
        // number is how many faces run along each edge, and edgeUses() is private. A face
        // that validate() would reject contributes nothing: two corners make no edge ring.
        void countEdges(const MeshTopology& mesh, EdgeCounts& out) {
            out.clear();
            for (const std::vector<uint32_t>& face : mesh.faces) {
                if (face.size() < 3) continue;
                for (size_t i = 0; i < face.size(); ++i) {
                    const uint32_t a = face[i];
                    const uint32_t b = face[(i + 1) % face.size()];
                    if (a == b) continue;
                    if (a >= mesh.vertices.size() || b >= mesh.vertices.size()) continue;
                    ++out[edgeKey(a, b)];
                }
            }
        }

        size_t countTriangles(const MeshTopology& mesh) {
            size_t triangles = 0;
            for (const std::vector<uint32_t>& face : mesh.faces) {
                if (face.size() >= 3) triangles += face.size() - 2;
            }
            return triangles;
        }

        size_t countUsedSlots(const ExportAttribs& attribs) {
            std::unordered_map<uint32_t, uint32_t> uses;
            for (size_t f = 0; f < attribs.faceMaterials.size(); ++f) {
                const uint32_t slot = attribs.faceMaterials[f];
                if (slot < attribs.materials.size()) ++uses[slot];
            }
            return uses.size();
        }

    }

    MeshStats meshStats(const MeshTopology& mesh, const ExportAttribs& attribs) {
        MeshStats stats;

        stats.vertices = mesh.vertices.size();
        stats.faces = mesh.faces.size();
        stats.triangles = countTriangles(mesh);

        EdgeCounts edges;
        countEdges(mesh, edges);
        stats.edges = edges.size();
        for (const auto& edge : edges) {
            if (edge.second == 1u) ++stats.boundaryEdges;
            else if (edge.second > 2u) ++stats.nonManifoldEdges;
        }

        stats.eulerCharacteristic = mesh.eulerCharacteristic();
        stats.textureCoordinates = attribs.uvs.size();
        stats.materialSlots = attribs.materials.size();
        stats.unusedMaterialSlots = stats.materialSlots - countUsedSlots(attribs);
        stats.hasVertexNormals = !mesh.normals.empty() &&
                                 mesh.normals.size() == mesh.vertices.size();

        const AABB box = mesh.bounds();
        stats.minimum = box.getMinimum();
        stats.maximum = box.getMaximum();

        std::string invalid;
        stats.clean = stats.vertices > 0 && !mesh.faces.empty() &&
                      stats.boundaryEdges == 0 && stats.nonManifoldEdges == 0 &&
                      mesh.validate(invalid);
        return stats;
    }

    std::vector<std::string> meshWarnings(const MeshTopology& mesh, const MeshStats& stats) {
        std::vector<std::string> out;

        auto count = [](size_t value, const std::string& plural, const std::string& one) {
            return std::to_string(value) + " " + (value == 1 ? one : plural);
        };

        if (stats.vertices == 0 || stats.faces == 0) {
            out.push_back("the mesh is empty: " + count(stats.vertices, "vertices", "vertex") +
                          " and " + count(stats.faces, "faces", "face"));
            return out;
        }

        std::string invalid;
        if (!mesh.validate(invalid)) out.push_back("invalid geometry: " + invalid);

        if (stats.boundaryEdges) {
            out.push_back(count(stats.boundaryEdges, "edges", "edge") +
                          " with only one face on it, so the surface is open");
        }
        if (stats.nonManifoldEdges) {
            out.push_back(count(stats.nonManifoldEdges, "edges", "edge") +
                          " shared by three or more faces; operations that walk an edge "
                          "ring stop early there");
        }
        if (!stats.hasVertexNormals) {
            out.push_back("no vertex normals are stored, so every normal is derived from the "
                          "faces around it");
        }
        if (stats.textureCoordinates == 0) {
            out.push_back("no texture coordinates: an exported file will be untextured");
        } else if (stats.textureCoordinates != stats.vertices) {
            out.push_back(std::to_string(stats.textureCoordinates) +
                          " texture coordinates for " + std::to_string(stats.vertices) +
                          " vertices; the exporter needs one per vertex");
        }
        if (stats.unusedMaterialSlots) {
            out.push_back(count(stats.unusedMaterialSlots, "material slots", "material slot") +
                          " no face uses");
        }
        return out;
    }

    bool compareMeshStats(const MeshStats& source, const MeshStats& written,
                          std::vector<std::string>& differences) {
        differences.clear();

        auto compare = [&differences](const char* label, size_t from, size_t to) {
            if (from == to) return;
            differences.push_back(std::string(label) + ": " + std::to_string(from) +
                                  " became " + std::to_string(to));
        };

        compare("vertices", source.vertices, written.vertices);
        compare("triangles", source.triangles, written.triangles);
        compare("texture coordinates", source.textureCoordinates, written.textureCoordinates);
        compare("material slots", source.materialSlots, written.materialSlots);

        // Faces, edges and the manifold counts are not compared: a format without polygons
        // fans every n-gon out, which invents a diagonal edge per extra corner - that is what
        // `triangles` is for. Normals are not compared either: a file that carries none is
        // answered with derived ones on the way back in, and both are correct.
        return differences.empty();
    }

}
}
