// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ui/render/VertexBuilder.h"

#include <algorithm>
#include <cmath>

using namespace quantum;
using namespace quantum::ui;

namespace {

    constexpr float kPi = 3.14159265358979323846f;

    // Corner arc resolution; grows with the radius, capped for the vertex budget.
    int cornerSegments(float radius) {
        if (radius <= 0.5f)
            return 0;

        int segments = (int)std::ceil(radius * 0.5f);
        if (segments < 3)
            segments = 3;
        if (segments > 12)
            segments = 12;
        return segments;
    }

    bool rectsEqual(const Rect& a, const Rect& b) {
        return a.getX() == b.getX() && a.getY() == b.getY() &&
               a.getWidth() == b.getWidth() && a.getHeight() == b.getHeight();
    }

    bool hasVisibleSize(const Rect& rect) {
        return rect.getWidth() > 0.0f && rect.getHeight() > 0.0f;
    }

    // Outline of a rounded rectangle in screen order (y down), starting at the
    // top-left corner arc. segments == 0 emits the four corners; any positive
    // count always emits 4 * segments points so outer/inner rings match.
    void buildOutline(const Rect& rect, float radius, int segments, std::vector<Vector2>& out) {
        const float x = rect.getX();
        const float y = rect.getY();
        const float w = rect.getWidth();
        const float h = rect.getHeight();

        float r = radius;
        if (r > w * 0.5f)
            r = w * 0.5f;
        if (r > h * 0.5f)
            r = h * 0.5f;
        if (r < 0.0f)
            r = 0.0f;

        if (segments <= 0) {
            out.push_back(Vector2(x, y));
            out.push_back(Vector2(x + w, y));
            out.push_back(Vector2(x + w, y + h));
            out.push_back(Vector2(x, y + h));
            return;
        }

        const float step = (kPi * 0.5f) / segments;
        const float centersX[4] = { x + r, x + w - r, x + w - r, x + r };
        const float centersY[4] = { y + r, y + r, y + h - r, y + h - r };
        const float starts[4] = { kPi, kPi * 1.5f, 0.0f, kPi * 0.5f };

        for (int corner = 0; corner < 4; corner++) {
            for (int i = 0; i < segments; i++) {
                const float angle = starts[corner] + step * i;
                out.push_back(Vector2(centersX[corner] + std::cos(angle) * r,
                                      centersY[corner] + std::sin(angle) * r));
            }
        }
    }

    unsigned int addVertex(UIMesh& mesh, const Vector2& position, const Vector2& uv, const Vector4& color) {
        UIVertex vertex;
        vertex.position = Vector3(position.x, position.y, 0.0f);
        vertex.uv = uv;
        vertex.color = color;
        mesh.vertices.push_back(vertex);
        return (unsigned int)mesh.vertices.size() - 1;
    }

    void addTriangle(UIMesh& mesh, uint32_t a, uint32_t b, uint32_t c) {
        mesh.indices.push_back(a);
        mesh.indices.push_back(b);
        mesh.indices.push_back(c);
    }

    // Quad with the text-quad winding: (x0,y0) (x1,y0) (x1,y1) (x0,y1).
    // Face culling is disabled for the UI, so any convex winding works.
    void addQuad(UIMesh& mesh,
                 const Vector2& p0, const Vector2& p1, const Vector2& p2, const Vector2& p3,
                 const Vector2& uv0, const Vector2& uv1, const Vector2& uv2, const Vector2& uv3,
                 const Vector4& color) {
        const uint32_t base = (uint32_t)mesh.vertices.size();
        addVertex(mesh, p0, uv0, color);
        addVertex(mesh, p1, uv1, color);
        addVertex(mesh, p2, uv2, color);
        addVertex(mesh, p3, uv3, color);
        addTriangle(mesh, base, base + 1, base + 2);
        addTriangle(mesh, base, base + 2, base + 3);
    }

    unsigned int addRoundedRectFill(UIMesh& mesh, const Rect& rect, const Vector4& color, float radius) {
        const uint32_t firstIndex = (uint32_t)mesh.indices.size();

        static thread_local std::vector<Vector2> outlineScratch;
        std::vector<Vector2>& outline = outlineScratch;
        outline.clear();
        buildOutline(rect, radius, cornerSegments(radius), outline);

        if (outline.size() == 4) {
            addQuad(mesh,
                    outline[0], outline[1], outline[2], outline[3],
                    Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), Vector2(1.0f, 1.0f), Vector2(0.0f, 1.0f),
                    color);
        }else{
            const uint32_t center = addVertex(mesh,
                    Vector2(rect.getX() + rect.getWidth() * 0.5f, rect.getY() + rect.getHeight() * 0.5f),
                    Vector2(0.5f, 0.5f), color);
            const uint32_t ringBase = (uint32_t)mesh.vertices.size();
            for (const Vector2& point : outline)
                addVertex(mesh, point, Vector2(0.5f, 0.5f), color);

            const uint32_t count = (uint32_t)outline.size();
            for (uint32_t i = 0; i < count; i++)
                addTriangle(mesh, center, ringBase + i, ringBase + (i + 1) % count);
        }

        return (uint32_t)mesh.indices.size() - firstIndex;
    }

    unsigned int addBorderRing(UIMesh& mesh, const Rect& rect, const Vector4& color, float width, float radius) {
        const float maxWidth = std::min(rect.getWidth(), rect.getHeight()) * 0.5f;
        const float borderWidth = std::min(width, maxWidth);
        if (borderWidth <= 0.0f)
            return 0;

        const uint32_t firstIndex = (uint32_t)mesh.indices.size();

        static thread_local std::vector<Vector2> outerScratch;
        static thread_local std::vector<Vector2> innerScratch;
        std::vector<Vector2>& outer = outerScratch;
        std::vector<Vector2>& inner = innerScratch;

        float clampedRadius = radius;
        if (clampedRadius > maxWidth)
            clampedRadius = maxWidth;
        const int segments = cornerSegments(clampedRadius);

        outer.clear();
        buildOutline(rect, radius, segments, outer);

        const Rect innerRect(rect.getX() + borderWidth, rect.getY() + borderWidth,
                             rect.getWidth() - borderWidth * 2.0f, rect.getHeight() - borderWidth * 2.0f);
        inner.clear();
        buildOutline(innerRect, radius - borderWidth, segments, inner);

        const uint32_t count = (uint32_t)outer.size();
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t next = (i + 1) % count;
            addQuad(mesh,
                    outer[i], outer[next], inner[next], inner[i],
                    Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), Vector2(1.0f, 1.0f), Vector2(0.0f, 1.0f),
                    color);
        }

        return (uint32_t)mesh.indices.size() - firstIndex;
    }

    unsigned int addImage(UIMesh& mesh, const PaintCommand& command) {
        const uint32_t firstIndex = (uint32_t)mesh.indices.size();

        const Rect& rect = command.rect;
        const Texture& texture = command.texture;

        const float textureWidth = (float)texture.getWidth();
        const float textureHeight = (float)texture.getHeight();

        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        if (textureWidth > 0.0f && textureHeight > 0.0f) {
            left = std::max(0.0f, command.nineSlice.x);
            top = std::max(0.0f, command.nineSlice.y);
            right = std::max(0.0f, command.nineSlice.z);
            bottom = std::max(0.0f, command.nineSlice.w);

            //insets can never cover more than half of the texture
            if (left + right > textureWidth) {
                const float scale = textureWidth / (left + right);
                left *= scale;
                right *= scale;
            }
            if (top + bottom > textureHeight) {
                const float scale = textureHeight / (top + bottom);
                top *= scale;
                bottom *= scale;
            }
        }

        if (left <= 0.0f && top <= 0.0f && right <= 0.0f && bottom <= 0.0f) {
            if (!hasVisibleSize(rect))
                return 0;

            addQuad(mesh,
                    Vector2(rect.getX(), rect.getY()),
                    Vector2(rect.getX() + rect.getWidth(), rect.getY()),
                    Vector2(rect.getX() + rect.getWidth(), rect.getY() + rect.getHeight()),
                    Vector2(rect.getX(), rect.getY() + rect.getHeight()),
                    Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), Vector2(1.0f, 1.0f), Vector2(0.0f, 1.0f),
                    command.color);
            return (uint32_t)mesh.indices.size() - firstIndex;
        }

        if (!hasVisibleSize(rect))
            return 0;

        const float rectLeft = std::min(left, rect.getWidth() * 0.5f);
        const float rectRight = std::min(right, rect.getWidth() * 0.5f);
        const float rectTop = std::min(top, rect.getHeight() * 0.5f);
        const float rectBottom = std::min(bottom, rect.getHeight() * 0.5f);

        const float xs[4] = { rect.getX(), rect.getX() + rectLeft, rect.getX() + rect.getWidth() - rectRight, rect.getX() + rect.getWidth() };
        const float ys[4] = { rect.getY(), rect.getY() + rectTop, rect.getY() + rect.getHeight() - rectBottom, rect.getY() + rect.getHeight() };
        const float us[4] = { 0.0f, left / textureWidth, 1.0f - right / textureWidth, 1.0f };
        const float vs[4] = { 0.0f, top / textureHeight, 1.0f - bottom / textureHeight, 1.0f };

        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                const float x0 = xs[col];
                const float x1 = xs[col + 1];
                const float y0 = ys[row];
                const float y1 = ys[row + 1];
                if (x1 <= x0 || y1 <= y0)
                    continue;

                const float u0 = us[col];
                const float u1 = us[col + 1];
                const float v0 = vs[row];
                const float v1 = vs[row + 1];
                addQuad(mesh,
                        Vector2(x0, y0), Vector2(x1, y0), Vector2(x1, y1), Vector2(x0, y1),
                        Vector2(u0, v0), Vector2(u1, v0), Vector2(u1, v1), Vector2(u0, v1),
                        command.color);
            }
        }

        return (uint32_t)mesh.indices.size() - firstIndex;
    }

    bool quadIntersectsClip(const GlyphQuad& quad, const Rect& clip) {
        float minX = quad.position[0].x;
        float maxX = minX;
        float minY = quad.position[0].y;
        float maxY = minY;
        for (int i = 1; i < 4; i++) {
            minX = std::min(minX, quad.position[i].x);
            maxX = std::max(maxX, quad.position[i].x);
            minY = std::min(minY, quad.position[i].y);
            maxY = std::max(maxY, quad.position[i].y);
        }

        return minX < clip.getX() + clip.getWidth() && maxX > clip.getX() &&
               minY < clip.getY() + clip.getHeight() && maxY > clip.getY();
    }

    unsigned int addText(UIMesh& mesh, const PaintList& paintList, const PaintCommand& command) {
        const uint32_t firstIndex = (uint32_t)mesh.indices.size();
        const std::vector<GlyphQuad>& quads = paintList.getGlyphQuads();

        for (unsigned int i = 0; i < command.quadCount; i++) {
            const GlyphQuad& quad = quads[command.quadOffset + i];
            if (command.hasClip && !quadIntersectsClip(quad, command.clip))
                continue;

            addQuad(mesh,
                    quad.position[0], quad.position[1], quad.position[2], quad.position[3],
                    quad.uv[0], quad.uv[1], quad.uv[2], quad.uv[3],
                    command.color);
        }

        return (uint32_t)mesh.indices.size() - firstIndex;
    }

    bool sameDrawState(const UIDrawRange& range, const PaintCommand& command, bool fontAtlas) {
        if (range.fontAtlas != fontAtlas)
            return false;

        if (range.hasClip != command.hasClip)
            return false;

        if (range.hasClip && !rectsEqual(range.clip, command.clip))
            return false;

        if (range.texture.empty() != command.texture.empty())
            return false;

        if (!range.texture.empty() && !(range.texture == command.texture))
            return false;

        //the text tint travels in the FS uniform, so it is part of the draw state
        if (fontAtlas && !(range.color == command.color))
            return false;

        return true;
    }
}

void UIMesh::clear() {
    //clear() keeps the capacities, so steady-state frames do not allocate
    vertices.clear();
    indices.clear();
    ranges.clear();
}

void VertexBuilder::build(const PaintList& paintList, UIMesh& mesh) {
    mesh.clear();

    for (const PaintCommand& command : paintList.getCommands()) {
        if ((command.type == PaintCommand::Type::Rect || command.type == PaintCommand::Type::Border) &&
            !hasVisibleSize(command.rect)) {
            continue;
        }

        unsigned int indexCount = 0;
        switch (command.type) {
            case PaintCommand::Type::Rect:
                indexCount = addRoundedRectFill(mesh, command.rect, command.color, command.radius);
                break;
            case PaintCommand::Type::Border:
                indexCount = addBorderRing(mesh, command.rect, command.color, command.borderWidth, command.radius);
                break;
            case PaintCommand::Type::Image:
                indexCount = addImage(mesh, command);
                break;
            case PaintCommand::Type::Text:
                indexCount = addText(mesh, paintList, command);
                break;
        }

        if (indexCount == 0)
            continue;

        const bool fontAtlas = command.type == PaintCommand::Type::Text;

        if (!mesh.ranges.empty() && sameDrawState(mesh.ranges.back(), command, fontAtlas)) {
            mesh.ranges.back().indexCount += indexCount;
        }else{
            UIDrawRange range;
            range.indexOffset = (unsigned int)mesh.indices.size() - indexCount;
            range.indexCount = indexCount;
            range.texture = command.texture;
            range.fontAtlas = fontAtlas;
            range.color = command.color;
            range.clip = command.clip;
            range.hasClip = command.hasClip;
            mesh.ranges.push_back(range);
        }
    }
}
