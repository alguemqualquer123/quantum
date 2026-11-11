// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef meshmodeling_h
#define meshmodeling_h

#include "Export.h"
#include "math/Vector3.h"
#include "model/MeshTopology.h"

#include <cstdint>
#include <string>
#include <vector>

namespace quantum {
namespace model {

    // The weight a proportional-editing brush puts on a vertex by its distance to the
    // selection, in the same order Blender lists them in the toolbar.
    enum class Falloff {
        None,
        Linear,
        Smooth,
        Sphere,
        InverseSquare,
        Sharp,
        Root,
        Cubic
    };

    // Operations report instead of throwing: the process is built with
    // _HAS_EXCEPTIONS=0 (engine/libs/joltphysics/CMakeLists.txt), so a throw that escapes a
    // modelling tool aborts the editor instead of telling the user what went wrong.
    struct QUANTUM_API ModelingResult {
        bool ok = true;
        // Empty when ok. English, because it is shown in the tool's own status line.
        std::string message;
        // What the operation created, so the tool can select it the way Blender does.
        std::vector<uint32_t> newFaces;
        std::vector<uint32_t> newVertices;
        std::vector<uint32_t> movedVertices;

        static ModelingResult failed(const std::string& reason);
        void absorb(const ModelingResult& other);
    };

    // ---------- free transforms ----------

    // Moves elements of one kind. Moving faces moves their corners; moving an edge moves
    // both corners; a vertex moves alone. `faces` is the whole mesh's face list.
    enum class Element { Vertex, Edge, Face };

    QUANTUM_API ModelingResult translateElements(MeshTopology& mesh, Element element,
        const std::vector<uint32_t>& indices, const Vector3& delta);

    // Blender's G with O on: the selection moves fully, everything within `radius` of it
    // moves by the falloff of its distance to the nearest selected vertex.
    QUANTUM_API ModelingResult proportionalTranslate(MeshTopology& mesh, const std::vector<uint32_t>& selection,
        const Vector3& delta, float radius, Falloff falloff);

    // ---------- mesh operations ----------

    // Region extrude: the selected faces are lifted by `offset` on duplicated corners, and
    // the seam they leave behind is filled with side quads. With `individual` every face
    // extrudes on its own corners, so a shared wall is not welded and the region splits.
    // Returns the wall faces in newFaces.
    QUANTUM_API ModelingResult extrudeFaces(MeshTopology& mesh, const std::vector<uint32_t>& selected,
        const Vector3& offset, bool individual = false);

    // Pushes a ring of quads inside each face, optionally moving the inner face along the
    // face normal by `depth`. `amount` is a fraction of the distance to the face centroid.
    QUANTUM_API ModelingResult insetFaces(MeshTopology& mesh, const std::vector<uint32_t>& selected,
        float amount, float depth = 0.0f);

    // A centre vertex with a triangle fan over the face's outline.
    QUANTUM_API ModelingResult pokeFaces(MeshTopology& mesh, const std::vector<uint32_t>& selected);

    // Midpoint split with no smoothing: every face becomes a fan of quads through shared
    // edge midpoints and its centroid. Preserves the Euler characteristic.
    QUANTUM_API ModelingResult subdivideSimple(MeshTopology& mesh, uint32_t iterations = 1);

    // Catmull-Clark: face points, edge points, and vertex points pulled by the valence rule.
    // Boundary edges and boundary vertices use the open-mesh rules; a non-manifold vertex
    // keeps its position rather than collapsing. Quads stay quads, n-gons gain poles.
    QUANTUM_API ModelingResult subdivideCatmullClark(MeshTopology& mesh, uint32_t iterations = 1);

    // Gives the mesh thickness: a flipped copy offset along the vertex normals, welded to the
    // original along the boundary. `thickness` is measured along the surface normal.
    QUANTUM_API ModelingResult solidify(MeshTopology& mesh, float thickness);

    // Splits every face into triangles in place.
    QUANTUM_API ModelingResult triangulate(MeshTopology& mesh);

    // Welds corners closer than `epsilon` and drops the faces that become degenerate.
    QUANTUM_API ModelingResult mergeByDistance(MeshTopology& mesh, float epsilon);

    // ---------- helpers exposed for the tools' own previews ----------

    QUANTUM_API float falloffWeight(Falloff falloff, float t);
    QUANTUM_API Vector3 faceCentroid(const MeshTopology& mesh, uint32_t face);

}
}

#endif
