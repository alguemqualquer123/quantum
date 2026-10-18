// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "slate/core/TextRenderer.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include "Engine.h"
#include "Log.h"
#include "buffer/Buffer.h"
#include "buffer/InterleavedBuffer.h"
#include "pool/FontPool.h"
#include "texture/TextureData.h"
#include "util/CharExtent.h"
#include "util/STBText.h"
#include "util/StringUtils.h"

namespace doriax::slate {

    namespace {
        //(path,size) -> renderer. Failed loads stay in the map so the error is
        //logged once per key, matching the legacy UI atlas cache behaviour.
        using CacheKey = std::pair<std::string, unsigned int>;

        std::map<CacheKey, std::unique_ptr<TextRenderer>>& rendererCache() {
            static std::map<CacheKey, std::unique_ptr<TextRenderer>> cache;
            return cache;
        }

        //Shared createText scratch. The widget tree lives on the main thread
        //only (engine rule), so one static instance is safe and keeps the
        //steady-state paint walk allocation-free.
        struct ShapeScratch {
            InterleavedBuffer buffer;
            std::vector<uint16_t> indices;
            std::vector<Vector2> positions;
            std::vector<CharExtent> extents;

            ShapeScratch() {
                buffer.addAttribute(AttributeType::POSITION, 3);
                buffer.addAttribute(AttributeType::TEXCOORD1, 2);
                buffer.setUsage(BufferUsage::STREAM);
            }

            void reset() {
                buffer.clear();
                indices.clear();
                positions.clear();
                extents.clear();
            }
        };

        ShapeScratch& shapeScratch() {
            static ShapeScratch scratch;
            return scratch;
        }

        //Per-codepoint advance sum: the same first-guess metric STBText's
        //private shapeLines uses to pick break points. Shaping (kerning,
        //ligatures, final-form widening) is re-applied exactly when the line
        //is measured/painted through createText.
        float approxWidth(STBText* font, const std::string& text) {
            if (!font || text.empty())
                return 0.0f;

            bool hadInvalid = false;
            std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints(text, hadInvalid);

            float width = 0.0f;
            for (uint32_t cp : codepoints)
                width += font->getCharWidth(cp);
            return width;
        }

        //Ellipsis glyph, U+2026 in UTF-8 bytes (kept as escapes so the source
        //stays pure ASCII regardless of the /utf-8 flag).
        const char* const kEllipsis = "\xE2\x80\xA6";

        //Clamps an already-positioned glyph quad to `clip`, remapping the UVs
        //proportionally so a cropped glyph keeps its shape. Returns false when
        //the quad ends up with no visible area (the full-cull case).
        bool clipGlyphQuad(GlyphQuad& q, const Rect& clip) {
            const float x0 = q.position[0].x;
            const float y0 = q.position[0].y;
            const float x1 = q.position[2].x;
            const float y1 = q.position[2].y;

            const float cx0 = clip.getX();
            const float cy0 = clip.getY();
            const float cx1 = cx0 + clip.getWidth();
            const float cy1 = cy0 + clip.getHeight();

            if (x1 <= cx0 || x0 >= cx1 || y1 <= cy0 || y0 >= cy1)
                return false;

            const float nx0 = std::max(x0, cx0);
            const float nx1 = std::min(x1, cx1);
            const float ny0 = std::max(y0, cy0);
            const float ny1 = std::min(y1, cy1);
            if (nx1 <= nx0 || ny1 <= ny0)
                return false;

            const float du = (x1 > x0) ? (q.uv[1].x - q.uv[0].x) / (x1 - x0) : 0.0f;
            const float dv = (y1 > y0) ? (q.uv[3].y - q.uv[0].y) / (y1 - y0) : 0.0f;
            const float nu0 = q.uv[0].x + (nx0 - x0) * du;
            const float nu1 = q.uv[0].x + (nx1 - x0) * du;
            const float nv0 = q.uv[0].y + (ny0 - y0) * dv;
            const float nv1 = q.uv[0].y + (ny1 - y0) * dv;

            q.position[0] = Vector2(nx0, ny0); q.uv[0] = Vector2(nu0, nv0);
            q.position[1] = Vector2(nx1, ny0); q.uv[1] = Vector2(nu1, nv0);
            q.position[2] = Vector2(nx1, ny1); q.uv[2] = Vector2(nu1, nv1);
            q.position[3] = Vector2(nx0, ny1); q.uv[3] = Vector2(nu0, nv1);
            return true;
        }

        const size_t maxWrapCacheEntries = 64;

        //glyph quads reused across calls so a steady-state frame allocates
        //nothing. File scope, not a class member: the class must stay free of
        //static data so the header works for both a static and a shared link.
        std::vector<GlyphQuad> scratchQuads;
    }

    TextRenderer* TextRenderer::get(const std::string& fontPath, unsigned int fontSize) {
        if (fontSize == 0)
            fontSize = 16;

        auto& cache = rendererCache();
        const CacheKey key(fontPath, fontSize);
        auto it = cache.find(key);
        if (it == cache.end()) {
            std::unique_ptr<TextRenderer> renderer(new TextRenderer(fontPath, fontSize));
            it = cache.emplace(key, std::move(renderer)).first;
        }

        if (!it->second->isValid())
            return nullptr;

        return it->second.get();
    }

    void TextRenderer::clearCache() {
        rendererCache().clear();
    }

    TextRenderer::TextRenderer(const std::string& fontPath, unsigned int fontSize) :
        fontPath(fontPath),
        fontSize(fontSize),
        valid(false),
        publishedVersion(0),
        uploadedVersion(0) {

        const std::string id = "doriax_slate|" + fontPath + "|" + std::to_string(fontSize);

        font = FontPool::get(id);
        if (!font) {
            //Empty fallback list is the engine's existing chain: STBText::load
            //appends the built-in Latin and Arabic faces after the requested
            //file, and the whole chain shares one atlas. No FreeType (D3).
            font = FontPool::get(id, fontPath, std::vector<std::string>(), fontSize);
        }

        if (!font || font->getTextureData() == nullptr) {
            Log::error("Slate TextRenderer: cannot load font '%s' (size %u)",
                       fontPath.empty() ? "<built-in>" : fontPath.c_str(), fontSize);
            font.reset();
            return;
        }

        valid = true;
        syncAtlasTexture();
    }

    bool TextRenderer::isValid() const {
        return valid;
    }

    float TextRenderer::getAscent() const {
        return font ? font->getAscent() : 0.0f;
    }

    float TextRenderer::getLineHeight() const {
        return font ? (float)font->getLineHeight() : 0.0f;
    }

    const Texture& TextRenderer::getAtlasTexture() const {
        syncAtlasTexture();
        return atlasTexture;
    }

    void TextRenderer::syncAtlasTexture() const {
        if (!font)
            return;

        TextureData* data = font->getTextureData();
        if (data == nullptr)
            return;

        const unsigned long version = font->getAtlasVersion();
        if (version != publishedVersion) {
            //Versioned id: each atlas growth publishes fresh pixels into a new
            //pool entry, so texts still bound to the older texture keep working.
            atlasTexture.setData("doriax_slate_atlas|" + fontPath + "|" +
                                 std::to_string(fontSize) + "#" + std::to_string(version), *data);
            publishedVersion = version;
            uploadedVersion = 0;
        }

        if (uploadedVersion != publishedVersion && Engine::isViewLoaded()) {
            atlasTexture.load();
            uploadedVersion = publishedVersion;
        }
    }

    //createText over a getCharWidth sum: per-codepoint advances ignore kerning,
    //ligatures and fallback-face metrics, so measured width would drift from
    //the painted line. The full shaping pass costs one buffer fill, the result
    //is cached by the callers' wrap cache, and rasterising unseen glyphs is
    //something the atlas needs anyway.
    Vector2 TextRenderer::measure(const std::string& text) const {
        if (!font || text.empty())
            return Vector2(0.0f, 0.0f);

        ShapeScratch& scratch = shapeScratch();
        scratch.reset();

        unsigned int width = 0;
        unsigned int height = 0;
        font->createText(text, &scratch.buffer, scratch.indices, scratch.positions,
                         scratch.extents, width, height, false, false, false, false);

        //Glyphs rasterized above may have grown the atlas.
        syncAtlasTexture();

        return Vector2((float)width, (float)height);
    }

    //Small LRU by insertion order: wrapLines always leaves the touched entry at
    //the back, so measureWrapped can read the size from back().
    const std::vector<TextRenderer::Line>& TextRenderer::wrapLines(
            const std::string& text, float maxWidth, bool multiline) const {

        //Key: quantised width + wrap flag + length guard + payload, so two
        //different (width, text) pairs cannot collide.
        const std::string key = std::to_string((long long)(maxWidth * 64.0f)) + '|' +
                                (multiline ? '1' : '0') + '|' +
                                std::to_string(text.size()) + '|' + text;

        for (size_t i = 0; i < wrapCache.size(); i++) {
            if (wrapCache[i].first == key) {
                if (i + 1 < wrapCache.size()) {
                    //Move out of the element before touching the vector: a
                    //push_back reallocation would invalidate the reference.
                    auto touched = std::move(wrapCache[i]);
                    wrapCache.erase(wrapCache.begin() + (std::ptrdiff_t)i);
                    wrapCache.push_back(std::move(touched));
                }
                return wrapCache.back().second.lines;
            }
        }

        WrappedCache entry;

        if (!text.empty()) {
            //Paragraphs always break on '\n'; width wrapping is the optional
            //layer, exactly like STBText::shapeLines structures its pass.
            std::vector<std::string> paragraphs;
            size_t start = 0;
            while (true) {
                size_t nl = text.find('\n', start);
                if (nl == std::string::npos) {
                    paragraphs.push_back(text.substr(start));
                    break;
                }
                paragraphs.push_back(text.substr(start, nl - start));
                start = nl + 1;
            }

            const float spaceWidth = approxWidth(font.get(), " ");
            const bool wrap = multiline && maxWidth > 0.0f;

            for (const std::string& paragraph : paragraphs) {
                if (!wrap || paragraph.empty() || approxWidth(font.get(), paragraph) <= maxWidth) {
                    Line line;
                    line.text = paragraph;
                    line.width = paragraph.empty() ? 0.0f : measure(paragraph).x;
                    entry.lines.push_back(line);
                    continue;
                }

                //Greedy word wrap on single spaces, the same approximation
                //shapeLines uses before it re-shapes and gives ground.
                std::string current;
                float currentWidth = 0.0f;
                size_t pos = 0;
                while (pos < paragraph.size()) {
                    while (pos < paragraph.size() && paragraph[pos] == ' ')
                        pos++;
                    size_t wordEnd = pos;
                    while (wordEnd < paragraph.size() && paragraph[wordEnd] != ' ')
                        wordEnd++;
                    if (wordEnd <= pos)
                        break;

                    const std::string word = paragraph.substr(pos, wordEnd - pos);
                    const float wordWidth = approxWidth(font.get(), word);
                    pos = wordEnd;

                    if (current.empty()) {
                        current = word;
                        currentWidth = wordWidth;
                        continue;
                    }

                    const float trial = currentWidth + spaceWidth + wordWidth;
                    if (trial <= maxWidth) {
                        current += ' ';
                        current += word;
                        currentWidth = trial;
                    } else {
                        //A word alone wider than the budget still gets its own
                        //line and is allowed to overflow, like shapeLines does
                        //when backing off reaches a single cluster.
                        Line line;
                        line.text = current;
                        line.width = measure(current).x;
                        entry.lines.push_back(line);

                        current = word;
                        currentWidth = wordWidth;
                    }
                }

                if (!current.empty()) {
                    Line line;
                    line.text = current;
                    line.width = measure(current).x;
                    entry.lines.push_back(line);
                }
            }

            for (const Line& line : entry.lines)
                entry.size.x = std::max(entry.size.x, line.width);
            entry.size.y = (float)entry.lines.size() * (float)font->getLineHeight();
        }

        if (wrapCache.size() >= maxWrapCacheEntries)
            wrapCache.erase(wrapCache.begin());
        wrapCache.emplace_back(key, std::move(entry));
        return wrapCache.back().second.lines;
    }

    Vector2 TextRenderer::measureWrapped(const std::string& text, float maxWidth, bool multiline) const {
        if (!font)
            return Vector2(0.0f, 0.0f);

        wrapLines(text, maxWidth, multiline);
        return wrapCache.back().second.size;
    }

    Vector2 TextRenderer::layout(const std::string& text, Vector2 origin, const Vector4& color,
                                 float maxWidth, bool multiline, TextAlign align,
                                 TextOverflow overflow, const Rect& clip, bool hasClip,
                                 PaintList& list) const {
        if (!font || text.empty())
            return Vector2(0.0f, 0.0f);

        const std::vector<Line>& lines = wrapLines(text, maxWidth, multiline);
        if (lines.empty())
            return Vector2(0.0f, 0.0f);

        const float lineHeight = (float)font->getLineHeight();
        const float ascent = font->getAscent();
        const bool bounded = maxWidth > 0.0f;
        const float ellipsisWidth = (bounded && overflow == TextOverflow::Ellipsis)
            ? approxWidth(font.get(), kEllipsis) : 0.0f;

        scratchQuads.clear();
        float paintedWidth = 0.0f;

        for (size_t i = 0; i < lines.size(); i++) {
            std::string lineText = lines[i].text;
            float lineWidth = lines[i].width;

            //Ellipsis per line: only lines still wider than the budget after
            //wrapping (long words, or any line when wrapping is off).
            if (bounded && lineWidth > maxWidth && overflow == TextOverflow::Ellipsis) {
                while (!lineText.empty() &&
                       approxWidth(font.get(), lineText) + ellipsisWidth > maxWidth) {
                    StringUtils::eraseLastCodepointUtf8(lineText);
                }
                while (!lineText.empty() && lineText.back() == ' ')
                    lineText.pop_back();
                lineText += kEllipsis;
                //Even an ellipsis alone can exceed a very narrow budget; it is
                //measured honestly and left for the clip rect to trim.
                lineWidth = measure(lineText).x;
            }

            float penX = origin.x;
            if (bounded) {
                if (align == TextAlign::Center)
                    penX += (maxWidth - lineWidth) * 0.5f;
                else if (align == TextAlign::Far)
                    penX += maxWidth - lineWidth;
            }

            const float baseline = origin.y + ascent + (float)i * lineHeight;

            ShapeScratch& scratch = shapeScratch();
            scratch.reset();

            unsigned int lineW = 0;
            unsigned int lineH = 0;
            font->createText(lineText, &scratch.buffer, scratch.indices, scratch.positions,
                             scratch.extents, lineW, lineH, false, false, false, false);

            //createText emits (x0,y0)(x1,y0)(x1,y1)(x0,y1) per glyph, positions
            //relative to the pen with y measured down from the baseline.
            const unsigned int vertexCount = scratch.buffer.getCount();
            const unsigned int stride = scratch.buffer.getStride();
            const unsigned char* base = scratch.buffer.getData();
            for (unsigned int v = 0; v + 3 < vertexCount; v += 4) {
                GlyphQuad quad;
                for (int c = 0; c < 4; c++) {
                    const float* values = (const float*)(base + (size_t)(v + (unsigned int)c) * stride);
                    quad.position[c] = Vector2(penX + values[0], baseline + values[1]);
                    quad.uv[c] = Vector2(values[3], values[4]);
                }

                if (hasClip && !clipGlyphQuad(quad, clip))
                    continue;

                scratchQuads.push_back(quad);
            }

            paintedWidth = std::max(paintedWidth, lineWidth);
        }

        //A glyph rasterized during this pass may have grown the atlas; publish
        //before the renderer ever binds the texture from the command path.
        syncAtlasTexture();

        if (!scratchQuads.empty()) {
            list.addText(scratchQuads, 0, (unsigned int)scratchQuads.size(), atlasTexture, color, clip, hasClip);
        }

        //The block occupies the full budget when bounded (that is the width the
        //widget asked to wrap into), otherwise the widest line.
        return Vector2(bounded ? maxWidth : paintedWidth, (float)lines.size() * lineHeight);
    }
}
