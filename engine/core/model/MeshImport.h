// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef meshimport_h
#define meshimport_h

#include "Export.h"
#include "model/MeshExport.h"
#include "model/MeshTopology.h"

#include <string>
#include <vector>

namespace quantum {
namespace model {

    // A file turned into the structure the modelling operations work on, which is the other
    // half of MeshExport: whatever the editor can write it can open, edit and write again.
    //
    // It is not MeshSystem::loadOBJ/loadGLTF. Those build a MeshComponent and a ModelComponent
    // for the GPU and the scene, with pools, submeshes, textures and entity state. This reads
    // geometry only, into a topology the CPU can edit, and it deliberately returns no entity,
    // no texture and no render handle so that the same call works in the editor, in a game
    // build and in the console test runner.
    struct QUANTUM_API ImportedModel {

        std::string name;
        MeshTopology topology;
        // One UV per vertex and one slot per face, in the lists MeshExport consumes.
        ExportAttribs attribs;
        // What the file asked for that a topology cannot hold: textures, skin weights, node
        // transforms, morph targets. A loaded model that silently loses these is a bug the
        // tool cannot see, so each one is named here instead.
        std::vector<std::string> warnings;
    };

    // ---------- Wavefront ----------

    // .obj text in hand, with the .mtl body it describes already read into `mtlText` (empty
    // when there is none: an `mtllib` line is then reported as a warning rather than resolved
    // from disk). Polygons survive as polygons - the reader is called with triangulation off -
    // and a corner is welded to a vertex only when position, UV and normal all match, which is
    // what keeps a UV seam split the way a renderer needs it.
    QUANTUM_API bool importObjText(const std::string& name, const std::string& objText,
                                  const std::string& mtlText, ImportedModel& out,
                                  std::string& error);

    // Reads the .obj and then every .mtl its mtllib lines name, beside the file.
    QUANTUM_API bool importObjFile(const std::string& path, ImportedModel& out, std::string& error);

    // ---------- glTF ----------

    // A complete binary glTF container in memory. Triangles only, because that is all glTF
    // has: the faces a topology keeps are the ones the file's corners imply, welded by the
    // same (position, UV, normal) rule.
    QUANTUM_API bool importGlbBytes(const std::string& name,
                                   const std::vector<unsigned char>& bytes, ImportedModel& out,
                                   std::string& error);

    // .glb by its container, .gltf by its JSON, chosen by extension. This version of tinygltf's
    // file loaders take a resolve callback instead of a base directory, so a .gltf that keeps its
    // buffer in a separate .bin is resolved against the process's working folder; every file the
    // exporter above writes embeds its buffer, and a .glb has nothing to resolve.
    QUANTUM_API bool importGltfFile(const std::string& path, ImportedModel& out, std::string& error);

    // ---------- dispatch ----------

    // .obj, .gltf or .glb by extension. Everything else fails with a message naming the
    // extension: .fbx needs ufbx, which is linked into the editor and not into the engine.
    QUANTUM_API bool importModelFile(const std::string& path, ImportedModel& out, std::string& error);

}
}

#endif
