// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef SlateGeometry_h
#define SlateGeometry_h

#include <cstdint>
#include <vector>

#include "Export.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "texture/Texture.h"
#include "slate/core/PaintList.h"

namespace doriax::slate {

    // Layout the UI shader consumes: POSITION(3) + TEXCOORD1(2) + COLOR(4).
    struct SlateVertex {
        Vector3 position;
        Vector2 uv;
        Vector4 color;
    };

    // Consecutive triangles sharing texture + clip state: one draw call.
    struct SlateDrawRange {
        unsigned int indexOffset = 0;
        unsigned int indexCount = 0;
        Texture texture;
        bool fontAtlas = false;
        //Text has no usable per-vertex color in the Ftx variant, so the tint
        //travels with the range and drives the fragment-shader uniform.
        Vector4 color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        Rect clip;
        bool hasClip = false;
    };

    struct SlateMesh {
        std::vector<SlateVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<SlateDrawRange> ranges;

        void clear();
    };

    // Turns paint commands into triangles and batches them. GPU-free.
    class Geometry {
    public:
        //Appends the whole command list to `mesh` (mesh is cleared by the caller
        //once per frame, so the buffers keep their capacity between frames).
        static void build(const PaintList& paintList, SlateMesh& mesh);

        //Corner segment count used to tessellate a rounded rect of this radius.
        static unsigned int cornerSegmentsForRadius(float radius);
    };
}

#endif /* SlateGeometry_h */
