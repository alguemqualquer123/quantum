// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef VertexBuilder_h
#define VertexBuilder_h

#include <cstdint>
#include <vector>

#include "Export.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "texture/Texture.h"
#include "ui/render/PaintList.h"

namespace doriax::ui {

    // Vertex layout consumed by the UI shader: POSITION(3) + TEXCOORD1(2) + COLOR(4).
    struct UIVertex {
        Vector3 position;
        Vector2 uv;
        Vector4 color;
    };

    // Consecutive triangles that share texture + clip state (one draw call).
    struct UIDrawRange {
        unsigned int indexOffset = 0;
        unsigned int indexCount = 0;
        Texture texture;
        bool fontAtlas = false;
        //font atlas ranges have no usable per-vertex color (Ftx shader), so the
        //text tint travels with the range and drives the FS uniform color
        Vector4 color;
        Rect clip;
        bool hasClip = false;
    };

    struct UIMesh {
        std::vector<UIVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<UIDrawRange> ranges;

        void clear();
    };

    // Turns resolved paint commands into triangles; GPU-free, unit tested.
    class DORIAX_API VertexBuilder {
    public:
        static void build(const PaintList& paintList, UIMesh& mesh);
    };
}

#endif /* VertexBuilder_h */
