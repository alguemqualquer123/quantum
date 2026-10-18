// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef SlatePaintList_h
#define SlatePaintList_h

#include <vector>

#include "Export.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "texture/Texture.h"
#include "slate/core/SlateTypes.h"

namespace doriax::slate {

    // One glyph quad, absolute canvas coordinates, ordered
    // (x0,y0) (x1,y0) (x1,y1) (x0,y1).
    struct GlyphQuad {
        Vector2 position[4];
        Vector2 uv[4];
    };

    // A resolved draw command in absolute canvas coordinates. Kept GPU-free so
    // the paint pass is unit testable without a window or a GL context.
    struct PaintCommand {
        enum class Type : unsigned char {
            Rect,     //rounded rectangle fill
            Border,   //rounded rectangle stroke
            Image,    //brush quad, nine-slice or tiled
            Text,     //glyph quads at quadOffset/quadCount
            Gradient, //vertical or horizontal two-colour ramp
            Shadow    //soft rounded drop shadow behind a rect
        };

        Type type = Type::Rect;
        Rect rect;
        Vector4 color = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        Vector4 secondColor = Vector4(0.0f, 0.0f, 0.0f, 0.0f); //gradient end
        Vector2 gradientDirection = Vector2(0.0f, 1.0f);       //normalized ramp axis
        Vector2 shadowOffset = Vector2(0.0f, 0.0f);
        float shadowBlur = 0.0f;
        float radius = 0.0f;
        float borderWidth = 0.0f;
        Texture texture;
        Insets slices;             //nine-slice margins, texture pixels
        Vector2 brushSize;         //design size, used for tiling
        BrushDrawMode drawMode = BrushDrawMode::Simple;
        Rect clip;
        bool hasClip = false;
        unsigned int quadOffset = 0;
        unsigned int quadCount = 0;
    };

    // Flat command buffer produced by the paint walk, consumed by Geometry.
    class PaintList {
    public:
        void clear();

        unsigned int addGlyphQuads(const GlyphQuad* quads, unsigned int count);

        void addRect(const Rect& rect, const Vector4& color, float radius,
                     const Rect& clip, bool hasClip);
        void addBorder(const Rect& rect, const Vector4& color, float width, float radius,
                       const Rect& clip, bool hasClip);
        void addImage(const Rect& rect, const SlateBrush& brush, const Vector4& tint,
                      const Rect& clip, bool hasClip);
        void addGradient(const Rect& rect, const Vector4& from, const Vector4& to,
                         const Vector2& direction, float radius,
                         const Rect& clip, bool hasClip);
        void addShadow(const Rect& rect, const Vector4& color, float radius,
                       const Vector2& offset, float blur,
                       const Rect& clip, bool hasClip);
        //`atlas` is the font atlas the glyph quads' UVs index into: the command
        //has to carry it, or the renderer has no texture to bind for the range.
        void addText(const std::vector<GlyphQuad>& quads, unsigned int offset, unsigned int count,
                     const Texture& atlas, const Vector4& color, const Rect& clip, bool hasClip);

        const std::vector<PaintCommand>& getCommands() const;
        const std::vector<GlyphQuad>& getGlyphQuads() const;
        unsigned int getCommandCount() const;
        unsigned int getGlyphQuadCount() const;

    private:
        std::vector<PaintCommand> commands;
        std::vector<GlyphQuad> glyphQuads;
    };
}

#endif /* SlatePaintList_h */
