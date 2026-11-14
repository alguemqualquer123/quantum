// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ui/render/FontAtlas.h"

#include <map>
#include <utility>

#include "Engine.h"
#include "Log.h"
#include "pool/FontPool.h"
#include "util/STBText.h"

using namespace quantum;
using namespace quantum::ui;

namespace {

    using AtlasKey = std::pair<std::string, unsigned int>;

    std::map<AtlasKey, std::unique_ptr<FontAtlas>>& atlasCache() {
        static std::map<AtlasKey, std::unique_ptr<FontAtlas>> cache;
        return cache;
    }
}

FontAtlas* FontAtlas::get(const std::string& fontPath, unsigned int fontSize) {
    if (fontSize == 0)
        fontSize = 16;

    auto& cache = atlasCache();
    const AtlasKey key(fontPath, fontSize);
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::unique_ptr<FontAtlas> atlas(new FontAtlas(fontPath, fontSize));
        it = cache.emplace(key, std::move(atlas)).first;
    }

    if (!it->second->isValid())
        return nullptr;

    return it->second.get();
}

FontAtlas::FontAtlas(const std::string& fontPath, unsigned int fontSize) :
    fontPath(fontPath),
    fontSize(fontSize),
    valid(false),
    publishedVersion(0),
    uploadedVersion(0) {

    const std::string baseId = "quantum_ui_atlas|" + fontPath + "|" + std::to_string(fontSize);

    font = FontPool::get(baseId);
    if (!font)
        font = FontPool::get(baseId, fontPath, std::vector<std::string>(), fontSize);

    if (!font || font->getTextureData() == nullptr) {
        Log::error("Quantum UI: cannot load the font '%s' (size %u) for the UI atlas",
                   fontPath.empty() ? "<built-in>" : fontPath.c_str(), fontSize);
        font.reset();
        return;
    }

    scratch.addAttribute(AttributeType::POSITION, 3);
    scratch.addAttribute(AttributeType::TEXCOORD1, 2);
    scratch.setUsage(BufferUsage::STREAM);

    valid = true;
    syncTexture();
}

bool FontAtlas::isValid() const {
    return valid;
}

float FontAtlas::getAscent() {
    return font ? font->getAscent() : 0.0f;
}

const Texture& FontAtlas::getTexture() const {
    return texture;
}

void FontAtlas::syncTexture() {
    if (!font)
        return;

    TextureData* data = font->getTextureData();
    if (data == nullptr)
        return;

    const unsigned long version = font->getAtlasVersion();
    if (version != publishedVersion) {
        //Versioned id: each atlas growth publishes fresh pixels (the data pool
        //keeps earlier versions), exactly like the legacy scene UI does.
        texture.setData("quantum_ui_atlas|" + fontPath + "|" + std::to_string(fontSize) + "#" + std::to_string(version), *data);
        publishedVersion = version;
        uploadedVersion = 0;
    }

    if (uploadedVersion != publishedVersion && Engine::isViewLoaded()) {
        texture.load();
        uploadedVersion = publishedVersion;
    }
}

float FontAtlas::layout(const std::string& text, Vector2 origin, const Vector4& color,
                        const Rect& clip, bool hasClip, PaintList& list) {
    if (!font || text.empty())
        return 0.0f;

    scratch.clear();
    scratchIndices.clear();
    scratchPositions.clear();
    scratchExtents.clear();

    unsigned int width = 0;
    unsigned int height = 0;
    font->createText(text, &scratch, scratchIndices, scratchPositions, scratchExtents,
                     width, height, false, false, false, false);

    //glyphs rasterized above may have grown the atlas, publish before copying
    syncTexture();

    scratchQuads.clear();
    const unsigned int vertexCount = scratch.getCount();
    const unsigned int stride = scratch.getStride();
    const unsigned char* base = scratch.getData();
    for (unsigned int vertex = 0; vertex + 3 < vertexCount; vertex += 4) {
        GlyphQuad quad;
        for (int i = 0; i < 4; i++) {
            const float* values = (const float*)(base + (size_t)(vertex + i) * stride);
            quad.position[i] = Vector2(origin.x + values[0], origin.y + values[1]);
            quad.uv[i] = Vector2(values[3], values[4]);
        }
        scratchQuads.push_back(quad);
    }

    if (scratchQuads.empty())
        return (float)width;

    PaintCommand& command = list.addCommand(PaintCommand::Type::Text);
    command.rect = Rect(origin.x, origin.y, (float)width, (float)height);
    command.color = color;
    command.texture = texture;
    command.clip = clip;
    command.hasClip = hasClip;
    command.quadOffset = list.addGlyphQuads(scratchQuads.data(), (unsigned int)scratchQuads.size());
    command.quadCount = (unsigned int)scratchQuads.size();

    return (float)width;
}

Vector2 FontAtlas::measure(const std::string& text) {
    if (!font || text.empty())
        return Vector2(0.0f, 0.0f);

    scratch.clear();
    scratchIndices.clear();
    scratchPositions.clear();
    scratchExtents.clear();

    unsigned int width = 0;
    unsigned int height = 0;
    font->createText(text, &scratch, scratchIndices, scratchPositions, scratchExtents,
                     width, height, false, false, false, false);

    return Vector2((float)width, (float)height);
}
