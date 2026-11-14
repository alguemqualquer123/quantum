// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef PaintList_h
#define PaintList_h

#include <vector>

#include "Export.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "texture/Texture.h"

namespace quantum::ui {

    // Glyph quad produced by the text renderer, in absolute canvas
    // coordinates, ordered (x0,y0) (x1,y0) (x1,y1) (x0,y1).
    struct GlyphQuad {
        Vector2 position[4];
        Vector2 uv[4];
    };

    // One resolved paint command, in absolute canvas coordinates.
    struct PaintCommand {
        enum class Type : unsigned char {
            Rect,   //rounded rectangle fill
            Border, //rounded rectangle stroke
            Image,  //texture quad or nine-slice
            Text    //glyph quads at quadOffset/quadCount
        };

        Type type = Type::Rect;
        Rect rect;
        Vector4 color = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        float radius = 0.0f;
        float borderWidth = 0.0f;
        Texture texture;
        Vector4 nineSlice = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        Rect clip;
        bool hasClip = false;
        unsigned int quadOffset = 0;
        unsigned int quadCount = 0;
    };

    // Flat command list produced by the paint pass and consumed by the vertex
    // builder. Kept GPU-free so it can be unit tested.
    class QUANTUM_API PaintList {
    public:
        void clear();

        PaintCommand& addCommand(PaintCommand::Type type);
        unsigned int addGlyphQuads(const GlyphQuad* quads, unsigned int count);

        void addRect(const Rect& rect, const Vector4& color, float radius, const Rect& clip, bool hasClip);
        void addBorder(const Rect& rect, const Vector4& color, float width, float radius, const Rect& clip, bool hasClip);
        void addImage(const Rect& rect, const Texture& texture, const Vector4& color, const Vector4& nineSlice, const Rect& clip, bool hasClip);

        const std::vector<PaintCommand>& getCommands() const;
        const std::vector<GlyphQuad>& getGlyphQuads() const;
        unsigned int getCommandCount() const;
        unsigned int getGlyphQuadCount() const;

    private:
        std::vector<PaintCommand> commands;
        std::vector<GlyphQuad> glyphQuads;
    };
}

#endif /* PaintList_h */
