// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef meshexport_h
#define meshexport_h

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "model/MeshTopology.h"

#include <cstdint>
#include <string>
#include <vector>

namespace quantum {
namespace model {

    // A material slot the way a file format sees it. There are no image textures here yet:
    // an authored mesh gets its maps after the UV tool exists, and a factor-only material is
    // still a complete, loadable file rather than a half-written one.
    struct QUANTUM_API ExportMaterial {
        std::string name = "material";
        Vector4 baseColorFactor = Vector4(0.8f, 0.8f, 0.8f, 1.0f);
        Vector3 emissiveFactor = Vector3(0.0f, 0.0f, 0.0f);
        float metallicFactor = 0.0f;
        float roughnessFactor = 0.5f;
        bool doubleSided = false;
    };

    // The channels a topology does not carry: one texture coordinate per vertex, one material
    // slot per face, and the slots those indices look up. Each list is either empty or exactly
    // parallel to the mesh it describes, and a mismatch is reported instead of guessed at - a
    // short list would silently repaint or re-uv the faces past its end.
    //
    // They live beside the mesh rather than inside it because the operations that change
    // connectivity have no correct rule for carrying them: the wall faces an extrude creates
    // inherit nothing, and a Catmull-Clark vertex needs its UV interpolated by a rule the UV
    // tool owns, not the subdivider.
    struct QUANTUM_API ExportAttribs {
        std::vector<Vector2> uvs;
        std::vector<uint32_t> faceMaterials;
        std::vector<ExportMaterial> materials;
    };

    struct QUANTUM_API ExportOptions {
        // The object name, and the stem the Wavefront writer gives the .mtl it saves beside
        // the .obj.
        std::string name = "mesh";
        bool writeNormals = true;
        bool writeUVs = true;
    };

    // ---------- Wavefront ----------

    // The .obj text, polygons and all: Wavefront keeps n-gons, so a quad authored in the
    // editor survives the file as a quad. `mtlText` receives the .mtl body the header
    // references with `mtllib`, so a caller that never touches the disk still gets a
    // loadable pair. Returns an empty string and fills `error` when the mesh cannot be
    // exported.
    QUANTUM_API std::string buildObj(const MeshTopology& mesh, const ExportAttribs& attribs,
                                    const ExportOptions& options, std::string& mtlText,
                                    std::string& error);

    // Saves the .obj at `path` and, when there is a material to describe, the .mtl beside it.
    QUANTUM_API bool writeObj(const std::string& path, const MeshTopology& mesh,
                             const ExportAttribs& attribs, const ExportOptions& options,
                             std::string& error);

    // ---------- binary glTF ----------

    // A complete glTF 2.0 binary container: 12-byte header, JSON chunk, BIN chunk. glTF has
    // no polygons, only triangles, so every face is fan-split here the same way
    // MeshTopology::toTriangles does. One primitive per material slot, all of them sharing
    // the position, normal and UV accessors. Empty with `error` filled on failure.
    QUANTUM_API std::vector<unsigned char> buildGlb(const MeshTopology& mesh,
                                                   const ExportAttribs& attribs,
                                                   const ExportOptions& options,
                                                   std::string& error);

    QUANTUM_API bool writeGlb(const std::string& path, const MeshTopology& mesh,
                             const ExportAttribs& attribs, const ExportOptions& options,
                             std::string& error);

}
}

#endif
