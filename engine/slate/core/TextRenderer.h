// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef SlateTextRenderer_h
#define SlateTextRenderer_h

#include <memory>
#include <string>
#include <vector>

#include "Export.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "texture/Texture.h"
#include "slate/core/PaintList.h"

namespace doriax {
    class STBText;
}

namespace doriax::slate {

    // One font at one pixel size. Wraps STBText - HarfBuzz shaping, kerning,
    // the built-in fallback face chain and SheenBidi bidi - and publishes its
    // atlas as a doriax Texture, the same way the legacy scene UI does. No
    // FreeType and no SDF: reusing what the engine already ships.
    class TextRenderer {
    public:
        //Cached per (path, size). Returns nullptr when the face cannot be
        //loaded (the reason is logged once, not per call).
        static TextRenderer* get(const std::string& fontPath, unsigned int fontSize);
        static void clearCache();

        //Single-line metrics, no painting.
        Vector2 measure(const std::string& text) const;

        //Breaks `text` into lines that fit `maxWidth` (0 = no wrap) and returns
        //the total size. Laid-out lines are cached by (text,width) so a static
        //label costs nothing to re-measure across frames.
        Vector2 measureWrapped(const std::string& text, float maxWidth, bool multiline) const;

        //Lays out at `origin` (top-left, canvas pixels), honouring align and
        //overflow, and appends Text commands to `list`. Returns the painted
        //block size. `clip`/`hasClip` bound glyph quads before they are emitted.
        Vector2 layout(const std::string& text, Vector2 origin, const Vector4& color,
                       float maxWidth, bool multiline, TextAlign align,
                       TextOverflow overflow, const Rect& clip, bool hasClip,
                       PaintList& list) const;

        float getAscent() const;
        float getLineHeight() const;
        const Texture& getAtlasTexture() const;
        bool isValid() const;

    private:
        TextRenderer(const std::string& fontPath, unsigned int fontSize);
        TextRenderer(const TextRenderer&) = delete;
        TextRenderer& operator=(const TextRenderer&) = delete;

        struct Line {
            std::string text;
            float width;
        };

        void syncAtlasTexture() const;
        const std::vector<Line>& wrapLines(const std::string& text, float maxWidth, bool multiline) const;

        std::shared_ptr<STBText> font;
        std::string fontPath;
        unsigned int fontSize;
        bool valid;

        mutable Texture atlasTexture;
        mutable unsigned long publishedVersion;
        mutable unsigned long uploadedVersion;

        //wrap cache: key = maxWidth flag + text, small LRU by insertion order
        struct WrappedCache {
            Vector2 size;
            std::vector<Line> lines;
        };
        mutable std::vector<std::pair<std::string, WrappedCache>> wrapCache;
    };
}

#endif /* SlateTextRenderer_h */
