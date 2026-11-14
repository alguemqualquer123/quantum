// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "slate/core/PaintList.h"

namespace {
    //The header keeps the command storage private and exposes no addCommand hook,
    //so command creation is a translation-unit helper over the caller's vector.
    quantum::slate::PaintCommand& pushCommand(std::vector<quantum::slate::PaintCommand>& commands, quantum::slate::PaintCommand::Type type) {
        commands.push_back(quantum::slate::PaintCommand());
        quantum::slate::PaintCommand& command = commands.back();
        command.type = type;
        return command;
    }
}

namespace quantum::slate {

    void PaintList::clear() {
        //resize(0) keeps the capacity, so steady-state frames do not allocate;
        //the paint walk must never free and rebuild these buffers.
        commands.resize(0);
        glyphQuads.resize(0);
    }

    unsigned int PaintList::addGlyphQuads(const GlyphQuad* quads, unsigned int count) {
        const unsigned int offset = (unsigned int)glyphQuads.size();
        glyphQuads.insert(glyphQuads.end(), quads, quads + count);
        return offset;
    }

    void PaintList::addRect(const Rect& rect, const Vector4& color, float radius,
                            const Rect& clip, bool hasClip) {
        PaintCommand& command = pushCommand(commands, PaintCommand::Type::Rect);
        command.rect = rect;
        command.color = color;
        command.radius = radius;
        command.clip = clip;
        command.hasClip = hasClip;
    }

    void PaintList::addBorder(const Rect& rect, const Vector4& color, float width, float radius,
                              const Rect& clip, bool hasClip) {
        PaintCommand& command = pushCommand(commands, PaintCommand::Type::Border);
        command.rect = rect;
        command.color = color;
        command.borderWidth = width;
        command.radius = radius;
        command.clip = clip;
        command.hasClip = hasClip;
    }

    void PaintList::addImage(const Rect& rect, const SlateBrush& brush, const Vector4& tint,
                             const Rect& clip, bool hasClip) {
        PaintCommand& command = pushCommand(commands, PaintCommand::Type::Image);
        command.rect = rect;
        command.texture = brush.texture;
        command.color = tint;
        command.slices = brush.slices;
        command.brushSize = brush.size;
        command.drawMode = brush.drawMode;
        command.clip = clip;
        command.hasClip = hasClip;
    }

    void PaintList::addGradient(const Rect& rect, const Vector4& from, const Vector4& to,
                                const Vector2& direction, float radius,
                                const Rect& clip, bool hasClip) {
        PaintCommand& command = pushCommand(commands, PaintCommand::Type::Gradient);
        command.rect = rect;
        command.color = from;
        command.secondColor = to;
        command.gradientDirection = direction;
        command.radius = radius;
        command.clip = clip;
        command.hasClip = hasClip;
    }

    void PaintList::addShadow(const Rect& rect, const Vector4& color, float radius,
                              const Vector2& offset, float blur,
                              const Rect& clip, bool hasClip) {
        PaintCommand& command = pushCommand(commands, PaintCommand::Type::Shadow);
        command.rect = rect;
        command.color = color;
        command.radius = radius;
        command.shadowOffset = offset;
        command.shadowBlur = blur;
        command.clip = clip;
        command.hasClip = hasClip;
    }

    void PaintList::addText(const std::vector<GlyphQuad>& quads, unsigned int offset, unsigned int count,
                            const Texture& atlas, const Vector4& color, const Rect& clip, bool hasClip) {
        PaintCommand& command = pushCommand(commands, PaintCommand::Type::Text);
        command.texture = atlas;
        command.color = color;
        command.clip = clip;
        command.hasClip = hasClip;
        command.quadCount = count;

        //the caller hands over its shaping buffer; the command always refers to
        //this list's own glyph storage, so append and record the internal offset
        command.quadOffset = addGlyphQuads(&quads[offset], count);
    }

    const std::vector<PaintCommand>& PaintList::getCommands() const {
        return commands;
    }

    const std::vector<GlyphQuad>& PaintList::getGlyphQuads() const {
        return glyphQuads;
    }

    unsigned int PaintList::getCommandCount() const {
        return (unsigned int)commands.size();
    }

    unsigned int PaintList::getGlyphQuadCount() const {
        return (unsigned int)glyphQuads.size();
    }
}
