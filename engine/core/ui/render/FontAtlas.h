// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef FontAtlas_h
#define FontAtlas_h

#include <memory>
#include <string>
#include <vector>

#include "Export.h"
#include "buffer/InterleavedBuffer.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "texture/Texture.h"
#include "ui/render/PaintList.h"
#include "util/CharExtent.h"

namespace quantum {
    class STBText;
}

namespace quantum::ui {

    // One glyph atlas per (font path, pixel size), shared by every Label that
    // uses it. Wraps STBText (HarfBuzz shaping + stb_truetype rasterization,
    // with the built-in faces as fallback) and publishes it as a Quantum
    // Texture under a versioned id, like the legacy scene UI does.
    class QUANTUM_API FontAtlas {
    public:
        //Cached; returns nullptr when the font cannot be loaded (logged once).
        //No cache invalidation yet: hot reload arrives with the resource phase.
        static FontAtlas* get(const std::string& fontPath, unsigned int fontSize);

        //Lays out a single line at `origin` (canvas coordinates, top-left) and
        //appends a Text command with the glyph quads. Returns the line width.
        float layout(const std::string& text, Vector2 origin, const Vector4& color,
                     const Rect& clip, bool hasClip, PaintList& list);

        //Advances and line height of a single line, without painting. Used by
        //the layout pass to size content-driven elements (Label measure).
        Vector2 measure(const std::string& text);

        //Font ascent above the first baseline, in pixels. STBText anchors the
        //glyphs on the baseline; controls drawing from the element's top edge
        //use this to convert between the two.
        float getAscent();

        const Texture& getTexture() const;
        bool isValid() const;

    private:
        FontAtlas(const std::string& fontPath, unsigned int fontSize);
        FontAtlas(const FontAtlas&) = delete;
        FontAtlas& operator=(const FontAtlas&) = delete;

        void syncTexture();

        std::shared_ptr<STBText> font;
        std::string fontPath;
        unsigned int fontSize;
        bool valid;

        Texture texture;
        unsigned long publishedVersion;
        unsigned long uploadedVersion;

        //scratch reused across calls so steady-state frames do not allocate
        InterleavedBuffer scratch;
        std::vector<uint16_t> scratchIndices;
        std::vector<Vector2> scratchPositions;
        std::vector<CharExtent> scratchExtents;
        std::vector<GlyphQuad> scratchQuads;
    };
}

#endif /* FontAtlas_h */
