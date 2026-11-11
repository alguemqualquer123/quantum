// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef meshreport_h
#define meshreport_h

#include "Export.h"
#include "math/Vector3.h"
#include "model/MeshExport.h"
#include "model/MeshTopology.h"

#include <cstdint>
#include <string>
#include <vector>

namespace quantum {
namespace model {

    // Everything a tool shows about a mesh without asking the mesh for it twice. The counts
    // are kept separate on purpose: `faces` is what an editor edits, `triangles` is what a
    // renderer draws, and a format that has no polygons (glTF) moves the first while leaving
    // the second alone - which is the only reason a round trip can be compared at all.
    struct QUANTUM_API MeshStats {

        size_t vertices = 0;
        size_t faces = 0;
        size_t triangles = 0;
        size_t edges = 0;
        // An edge one face runs along: an open surface, not necessarily a mistake.
        size_t boundaryEdges = 0;
        // An edge three or more faces run along, which no modelling operation can walk.
        size_t nonManifoldEdges = 0;
        int64_t eulerCharacteristic = 0;
        size_t textureCoordinates = 0;
        size_t materialSlots = 0;
        size_t unusedMaterialSlots = 0;
        bool hasVertexNormals = false;
        // Nothing to repair: closed, manifold and every face usable.
        bool clean = false;

        Vector3 minimum;
        Vector3 maximum;
    };

    QUANTUM_API MeshStats meshStats(const MeshTopology& mesh, const ExportAttribs& attribs);

    // One sentence per thing a modeller would want to know before editing: faces the
    // topology cannot hold, an open surface, non-manifold edges, missing channels. English,
    // because they are shown in a panel and written to the output log, not parsed.
    QUANTUM_API std::vector<std::string> meshWarnings(const MeshTopology& mesh,
                                                     const MeshStats& stats);

    // Whether a file written from `source` still holds the same geometry. Faces, edges and
    // the manifold counts are deliberately not compared: a format without polygons fans every
    // n-gon out and so invents a diagonal edge per extra corner. A mesh whose UV seam got
    // welded shut shows up here as a vertex-count difference rather than silently.
    QUANTUM_API bool compareMeshStats(const MeshStats& source, const MeshStats& written,
                                     std::vector<std::string>& differences);

}
}

#endif
