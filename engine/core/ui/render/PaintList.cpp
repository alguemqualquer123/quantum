// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ui/render/PaintList.h"

using namespace quantum;
using namespace quantum::ui;

void PaintList::clear() {
    //clear() keeps the capacity, so steady-state frames do not allocate
    commands.clear();
    glyphQuads.clear();
}

PaintCommand& PaintList::addCommand(PaintCommand::Type type) {
    commands.push_back(PaintCommand());
    PaintCommand& command = commands.back();
    command.type = type;
    return command;
}

unsigned int PaintList::addGlyphQuads(const GlyphQuad* quads, unsigned int count) {
    const unsigned int offset = (unsigned int)glyphQuads.size();
    glyphQuads.insert(glyphQuads.end(), quads, quads + count);
    return offset;
}

void PaintList::addRect(const Rect& rect, const Vector4& color, float radius, const Rect& clip, bool hasClip) {
    PaintCommand& command = addCommand(PaintCommand::Type::Rect);
    command.rect = rect;
    command.color = color;
    command.radius = radius;
    command.clip = clip;
    command.hasClip = hasClip;
}

void PaintList::addBorder(const Rect& rect, const Vector4& color, float width, float radius, const Rect& clip, bool hasClip) {
    PaintCommand& command = addCommand(PaintCommand::Type::Border);
    command.rect = rect;
    command.color = color;
    command.borderWidth = width;
    command.radius = radius;
    command.clip = clip;
    command.hasClip = hasClip;
}

void PaintList::addImage(const Rect& rect, const Texture& texture, const Vector4& color, const Vector4& nineSlice, const Rect& clip, bool hasClip) {
    PaintCommand& command = addCommand(PaintCommand::Type::Image);
    command.rect = rect;
    command.texture = texture;
    command.color = color;
    command.nineSlice = nineSlice;
    command.clip = clip;
    command.hasClip = hasClip;
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
