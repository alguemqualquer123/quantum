// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "slate/core/Geometry.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "slate/core/PaintList.h"

namespace quantum::slate {

    namespace {
        constexpr float kPi = 3.14159265358979323846f;

        // A shadow is kShadowLayerCount flat pieces: the solid core behind the
        // caster plus (kShadowLayerCount - 1) annuli expanding out to `blur`.
        // Layers never overlap, so per-layer alpha only darkens its own band;
        // that is why a plain tessellated shadow is acceptable without a shader.
        // 5 pieces is the sweet spot: 4 bands are visually smooth for the blur
        // radii UI uses (4..24 px) and keep the mesh cheap (max ~350 indices).
        constexpr unsigned int kShadowLayerCount = 5;

        //Geometric falloff: each band is half as dark as the one inside it.
        float shadowLayerAlphaMultiplier(unsigned int layer) {
            return std::pow(0.5f, (float)layer);
        }

        bool hasVisibleSize(const Rect& rect) {
            return rect.getWidth() > 0.0f && rect.getHeight() > 0.0f;
        }

        bool rectsEqual(const Rect& a, const Rect& b) {
            return a.getX() == b.getX() && a.getY() == b.getY() &&
                   a.getWidth() == b.getWidth() && a.getHeight() == b.getHeight();
        }

        //Strict disjointness test; touching edges do not count as overlap so a
        //command sitting exactly on the clip border still culls.
        bool rectsDisjoint(const Rect& a, const Rect& b) {
            return a.getX() + a.getWidth() <= b.getX() ||
                   b.getX() + b.getWidth() <= a.getX() ||
                   a.getY() + a.getHeight() <= b.getY() ||
                   b.getY() + b.getHeight() <= a.getY();
        }

        Rect expandRect(const Rect& rect, float expansion) {
            return Rect(rect.getX() - expansion, rect.getY() - expansion,
                        rect.getWidth() + expansion * 2.0f, rect.getHeight() + expansion * 2.0f);
        }

        Vector4 lerp(const Vector4& a, const Vector4& b, float t) {
            return Vector4(a.x + (b.x - a.x) * t,
                           a.y + (b.y - a.y) * t,
                           a.z + (b.z - a.z) * t,
                           a.w + (b.w - a.w) * t);
        }

        //A radius bigger than half the rect would make the corner arcs cross;
        //clamping keeps the outline simple (and makes pills out of thin rects).
        float clampRadius(float radius, const Rect& rect) {
            float r = radius < 0.0f ? 0.0f : radius;
            if (r > rect.getWidth() * 0.5f)
                r = rect.getWidth() * 0.5f;
            if (r > rect.getHeight() * 0.5f)
                r = rect.getHeight() * 0.5f;
            return r;
        }

        //Outline of a rounded rectangle, y-down screen order, starting at the
        //top-left corner arc. segments == 0 emits the four corners; a positive
        //count always emits exactly 4 * segments points, so two outlines built
        //with the same segment count (outer/inner ring, shadow bands) can be
        //zipped point-by-point even when their radii differ.
        void buildOutline(const Rect& rect, float radius, unsigned int segments, std::vector<Vector2>& out) {
            const float x = rect.getX();
            const float y = rect.getY();
            const float w = rect.getWidth();
            const float h = rect.getHeight();

            const float r = clampRadius(radius, rect);

            out.resize(0);
            if (segments == 0) {
                out.push_back(Vector2(x, y));
                out.push_back(Vector2(x + w, y));
                out.push_back(Vector2(x + w, y + h));
                out.push_back(Vector2(x, y + h));
                return;
            }

            const float step = (kPi * 0.5f) / (float)segments;
            const float centersX[4] = { x + r, x + w - r, x + w - r, x + r };
            const float centersY[4] = { y + r, y + r, y + h - r, y + h - r };
            const float starts[4] = { kPi, kPi * 1.5f, 0.0f, kPi * 0.5f };

            for (unsigned int corner = 0; corner < 4; corner++) {
                for (unsigned int i = 0; i < segments; i++) {
                    const float angle = starts[corner] + step * (float)i;
                    out.push_back(Vector2(centersX[corner] + std::cos(angle) * r,
                                          centersY[corner] + std::sin(angle) * r));
                }
            }
        }

        unsigned int addVertex(SlateMesh& mesh, const Vector2& position, const Vector2& uv, const Vector4& color) {
            SlateVertex vertex;
            vertex.position = Vector3(position.x, position.y, 0.0f);
            vertex.uv = uv;
            vertex.color = color;
            mesh.vertices.push_back(vertex);
            return (unsigned int)mesh.vertices.size() - 1;
        }

        void addTriangle(SlateMesh& mesh, uint32_t a, uint32_t b, uint32_t c) {
            mesh.indices.push_back(a);
            mesh.indices.push_back(b);
            mesh.indices.push_back(c);
        }

        //Quad with the text-quad vertex order: (x0,y0) (x1,y0) (x1,y1) (x0,y1).
        //Face culling is disabled for the UI passes, so any convex winding works.
        void addQuad(SlateMesh& mesh,
                     const Vector2& p0, const Vector2& p1, const Vector2& p2, const Vector2& p3,
                     const Vector2& uv0, const Vector2& uv1, const Vector2& uv2, const Vector2& uv3,
                     const Vector4& c0, const Vector4& c1, const Vector4& c2, const Vector4& c3) {
            const uint32_t base = (uint32_t)mesh.vertices.size();
            addVertex(mesh, p0, uv0, c0);
            addVertex(mesh, p1, uv1, c1);
            addVertex(mesh, p2, uv2, c2);
            addVertex(mesh, p3, uv3, c3);
            addTriangle(mesh, base, base + 1, base + 2);
            addTriangle(mesh, base, base + 2, base + 3);
        }

        void addQuadUniform(SlateMesh& mesh,
                            const Vector2& p0, const Vector2& p1, const Vector2& p2, const Vector2& p3,
                            const Vector2& uv0, const Vector2& uv1, const Vector2& uv2, const Vector2& uv3,
                            const Vector4& color) {
            addQuad(mesh, p0, p1, p2, p3, uv0, uv1, uv2, uv3, color, color, color, color);
        }

        //Rounded rect fill as one centre vertex plus an outline fan: every pixel
        //of the shape is covered by exactly one triangle, so there is no alpha
        //overdraw (square + circles stacked on the corners would darken seams).
        //colorAt maps a position to its colour; solid fills pass a constant.
        template <typename ColorFn>
        unsigned int addRoundedRectFill(SlateMesh& mesh, const Rect& rect, float radius,
                                        unsigned int segments, const ColorFn& colorAt) {
            const uint32_t firstIndex = (uint32_t)mesh.indices.size();

            static thread_local std::vector<Vector2> outlineScratch;
            std::vector<Vector2>& outline = outlineScratch;
            buildOutline(rect, radius, segments, outline);

            if (segments == 0) {
                addQuad(mesh,
                        outline[0], outline[1], outline[2], outline[3],
                        Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), Vector2(1.0f, 1.0f), Vector2(0.0f, 1.0f),
                        colorAt(outline[0]), colorAt(outline[1]), colorAt(outline[2]), colorAt(outline[3]));
            }else{
                const Vector2 center(rect.getX() + rect.getWidth() * 0.5f,
                                     rect.getY() + rect.getHeight() * 0.5f);
                const uint32_t centerVertex = addVertex(mesh, center, Vector2(0.5f, 0.5f), colorAt(center));
                const uint32_t ringBase = (uint32_t)mesh.vertices.size();
                for (const Vector2& point : outline)
                    addVertex(mesh, point, Vector2(0.5f, 0.5f), colorAt(point));

                const uint32_t count = (uint32_t)outline.size();
                for (uint32_t i = 0; i < count; i++)
                    addTriangle(mesh, centerVertex, ringBase + i, ringBase + (i + 1) % count);
            }

            return (uint32_t)mesh.indices.size() - firstIndex;
        }

        unsigned int addRoundedRectFillSolid(SlateMesh& mesh, const Rect& rect, const Vector4& color,
                                             float radius, unsigned int segments) {
            return addRoundedRectFill(mesh, rect, radius, segments,
                                      [color](const Vector2&) -> Vector4 { return color; });
        }

        //Stroke ring between two outlines sharing one segment count; the
        //interior is never emitted, so borders are single-layer (no overdraw).
        unsigned int addRoundedRectRing(SlateMesh& mesh,
                                        const Rect& outer, float outerRadius,
                                        const Rect& inner, float innerRadius,
                                        unsigned int segments, const Vector4& color) {
            const uint32_t firstIndex = (uint32_t)mesh.indices.size();

            static thread_local std::vector<Vector2> outerScratch;
            static thread_local std::vector<Vector2> innerScratch;
            std::vector<Vector2>& outerPoints = outerScratch;
            std::vector<Vector2>& innerPoints = innerScratch;

            buildOutline(outer, outerRadius, segments, outerPoints);
            buildOutline(inner, innerRadius, segments, innerPoints);

            const uint32_t count = (uint32_t)outerPoints.size();
            for (uint32_t i = 0; i < count; i++) {
                const uint32_t next = (i + 1) % count;
                addQuadUniform(mesh,
                               outerPoints[i], outerPoints[next], innerPoints[next], innerPoints[i],
                               Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), Vector2(1.0f, 1.0f), Vector2(0.0f, 1.0f),
                               color);
            }

            return (uint32_t)mesh.indices.size() - firstIndex;
        }

        unsigned int addRectCommand(SlateMesh& mesh, const PaintCommand& command) {
            return addRoundedRectFillSolid(mesh, command.rect, command.color,
                                           command.radius, Geometry::cornerSegmentsForRadius(command.radius));
        }

        unsigned int addBorderCommand(SlateMesh& mesh, const PaintCommand& command) {
            const float maxWidth = std::min(command.rect.getWidth(), command.rect.getHeight()) * 0.5f;
            const float width = std::min(command.borderWidth, maxWidth);
            if (width <= 0.0f)
                return 0;

            const float radius = clampRadius(command.radius, command.rect);
            const unsigned int segments = Geometry::cornerSegmentsForRadius(radius);

            const Rect inner(command.rect.getX() + width, command.rect.getY() + width,
                             command.rect.getWidth() - width * 2.0f, command.rect.getHeight() - width * 2.0f);

            return addRoundedRectRing(mesh, command.rect, radius, inner, radius - width, segments, command.color);
        }

        unsigned int addGradientCommand(SlateMesh& mesh, const PaintCommand& command) {
            //Ramp axis: (0,1) = vertical, (1,0) = horizontal, anything else is
            //normalised and projected; a degenerate direction falls back to vertical.
            Vector2 axis = command.gradientDirection;
            const float length = axis.length();
            if (length > 0.000001f)
                axis = axis * (1.0f / length);
            else
                axis = Vector2(0.0f, 1.0f);

            const float span = axis.x * command.rect.getWidth() + axis.y * command.rect.getHeight();
            const Vector2 origin(command.rect.getX(), command.rect.getY());

            const Vector4 from = command.color;
            const Vector4 to = command.secondColor;

            auto colorAt = [&from, &to, &axis, &origin, span](const Vector2& p) -> Vector4 {
                float t = 0.5f;
                if (std::fabs(span) > 0.000001f) {
                    t = ((p.x - origin.x) * axis.x + (p.y - origin.y) * axis.y) / span;
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                }
                return lerp(from, to, t);
            };

            //Same tessellation as Rect: the fan triangles never overlap, and the
            //linear ramp is exact at the vertices the interpolator blends.
            return addRoundedRectFill(mesh, command.rect, command.radius,
                                      Geometry::cornerSegmentsForRadius(command.radius), colorAt);
        }

        unsigned int addShadowCommand(SlateMesh& mesh, const PaintCommand& command) {
            const float blur = std::max(0.0f, command.shadowBlur);
            const float radius = std::max(0.0f, command.radius);

            const Rect core(command.rect.getX() + command.shadowOffset.x,
                             command.rect.getY() + command.shadowOffset.y,
                             command.rect.getWidth(), command.rect.getHeight());
            if (!hasVisibleSize(core))
                return 0;

            const uint32_t firstIndex = (uint32_t)mesh.indices.size();

            addRoundedRectFillSolid(mesh, core, command.color, radius,
                                    Geometry::cornerSegmentsForRadius(radius));

            if (blur <= 0.0f)
                return (uint32_t)mesh.indices.size() - firstIndex;

            //All bands share one segment count (the outermost radius) so every
            //outline zips with the next; bands are drawn outside the core only,
            //hence no two layers ever cover the same pixel.
            const unsigned int segments = Geometry::cornerSegmentsForRadius(radius + blur);

            for (unsigned int layer = 1; layer < kShadowLayerCount; layer++) {
                const float innerExpansion = blur * (float)(layer - 1) / (float)(kShadowLayerCount - 1);
                const float outerExpansion = blur * (float)layer / (float)(kShadowLayerCount - 1);

                const Vector4 bandColor = Vector4(command.color.x, command.color.y, command.color.z,
                                                  command.color.w * shadowLayerAlphaMultiplier(layer));

                addRoundedRectRing(mesh,
                                   expandRect(core, outerExpansion), radius + outerExpansion,
                                   expandRect(core, innerExpansion), radius + innerExpansion,
                                   segments, bandColor);
            }

            return (uint32_t)mesh.indices.size() - firstIndex;
        }

        //Nine-slice margins are in texture pixels; they can never cover more
        //than half the texture, otherwise the middle strip UV would flip.
        void clampSlices(float& left, float& top, float& right, float& bottom,
                         float textureWidth, float textureHeight) {
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

        unsigned int addImageSimple(SlateMesh& mesh, const PaintCommand& command) {
            const uint32_t firstIndex = (uint32_t)mesh.indices.size();
            const Rect& rect = command.rect;

            addQuadUniform(mesh,
                           Vector2(rect.getX(), rect.getY()),
                           Vector2(rect.getX() + rect.getWidth(), rect.getY()),
                           Vector2(rect.getX() + rect.getWidth(), rect.getY() + rect.getHeight()),
                           Vector2(rect.getX(), rect.getY() + rect.getHeight()),
                           Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), Vector2(1.0f, 1.0f), Vector2(0.0f, 1.0f),
                           command.color);

            return (uint32_t)mesh.indices.size() - firstIndex;
        }

        unsigned int addImageSliced(SlateMesh& mesh, const PaintCommand& command,
                                    float textureWidth, float textureHeight) {
            const uint32_t firstIndex = (uint32_t)mesh.indices.size();
            const Rect& rect = command.rect;

            float left = std::max(0.0f, command.slices.left);
            float top = std::max(0.0f, command.slices.top);
            float right = std::max(0.0f, command.slices.right);
            float bottom = std::max(0.0f, command.slices.bottom);
            clampSlices(left, top, right, bottom, textureWidth, textureHeight);

            //A margin wider than the rect halves clamps to a hairline strip:
            //the middle band collapses and only the surviving strips tessellate.
            const float rectLeft = std::min(left, rect.getWidth() * 0.5f);
            const float rectRight = std::min(right, rect.getWidth() * 0.5f);
            const float rectTop = std::min(top, rect.getHeight() * 0.5f);
            const float rectBottom = std::min(bottom, rect.getHeight() * 0.5f);

            const float xs[4] = { rect.getX(), rect.getX() + rectLeft,
                                  rect.getX() + rect.getWidth() - rectRight, rect.getX() + rect.getWidth() };
            const float ys[4] = { rect.getY(), rect.getY() + rectTop,
                                  rect.getY() + rect.getHeight() - rectBottom, rect.getY() + rect.getHeight() };
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

                    addQuadUniform(mesh,
                                   Vector2(x0, y0), Vector2(x1, y0), Vector2(x1, y1), Vector2(x0, y1),
                                   Vector2(us[col], vs[row]), Vector2(us[col + 1], vs[row]),
                                   Vector2(us[col + 1], vs[row + 1]), Vector2(us[col], vs[row + 1]),
                                   command.color);
                }
            }

            return (uint32_t)mesh.indices.size() - firstIndex;
        }

        unsigned int addImageTiled(SlateMesh& mesh, const PaintCommand& command,
                                   float textureWidth, float textureHeight) {
            const uint32_t firstIndex = (uint32_t)mesh.indices.size();
            const Rect& rect = command.rect;

            //Repeat period: brush design size first, texture pixels as fallback.
            float tileW = command.brushSize.x > 0.0f ? command.brushSize.x : textureWidth;
            float tileH = command.brushSize.y > 0.0f ? command.brushSize.y : textureHeight;
            if (tileW <= 0.0f || tileH <= 0.0f)
                return addImageSimple(mesh, command);

            const float right = rect.getX() + rect.getWidth();
            const float bottom = rect.getY() + rect.getHeight();

            //Edge tiles are clamped to the rect and sample only the matching
            //fraction of the brush; the scissor of the batch handles the clip.
            for (float y = rect.getY(); y < bottom - 0.000001f; y += tileH) {
                const float rowH = std::min(tileH, bottom - y);
                const float v1 = rowH / tileH;
                for (float x = rect.getX(); x < right - 0.000001f; x += tileW) {
                    const float colW = std::min(tileW, right - x);
                    const float u1 = colW / tileW;

                    addQuadUniform(mesh,
                                   Vector2(x, y), Vector2(x + colW, y),
                                   Vector2(x + colW, y + rowH), Vector2(x, y + rowH),
                                   Vector2(0.0f, 0.0f), Vector2(u1, 0.0f),
                                   Vector2(u1, v1), Vector2(0.0f, v1),
                                   command.color);
                }
            }

            return (uint32_t)mesh.indices.size() - firstIndex;
        }

        unsigned int addImageCommand(SlateMesh& mesh, const PaintCommand& command) {
            const float textureWidth = (float)command.texture.getWidth();
            const float textureHeight = (float)command.texture.getHeight();

            switch (command.drawMode) {
                case BrushDrawMode::Slice:
                    //Nine-slice UVs need texture pixels; an unloaded (GPU-free)
                    //texture cannot slice, so degrade to one full-UV quad.
                    if (command.slices.isZero() || textureWidth <= 0.0f || textureHeight <= 0.0f)
                        return addImageSimple(mesh, command);
                    return addImageSliced(mesh, command, textureWidth, textureHeight);

                case BrushDrawMode::Tile:
                    return addImageTiled(mesh, command, textureWidth, textureHeight);

                case BrushDrawMode::Simple:
                default:
                    return addImageSimple(mesh, command);
            }
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

            return !(maxX <= clip.getX() || minX >= clip.getX() + clip.getWidth() ||
                     maxY <= clip.getY() || minY >= clip.getY() + clip.getHeight());
        }

        unsigned int addTextCommand(SlateMesh& mesh, const PaintList& paintList, const PaintCommand& command) {
            const uint32_t firstIndex = (uint32_t)mesh.indices.size();
            const std::vector<GlyphQuad>& quads = paintList.getGlyphQuads();

            for (unsigned int i = 0; i < command.quadCount; i++) {
                const GlyphQuad& quad = quads[command.quadOffset + i];
                if (command.hasClip && !quadIntersectsClip(quad, command.clip))
                    continue;

                addQuadUniform(mesh,
                               quad.position[0], quad.position[1], quad.position[2], quad.position[3],
                               quad.uv[0], quad.uv[1], quad.uv[2], quad.uv[3],
                               command.color);
            }

            return (uint32_t)mesh.indices.size() - firstIndex;
        }

        //True when the command cannot touch the clip region at all. The covered
        //area is inflated by everything the tessellation may emit outside the
        //rect (stroke stays inside, shadow bands expand by blur).
        bool isCulled(const PaintCommand& command) {
            if (!command.hasClip)
                return false;

            //Text quads carry their own bounds and are culled per quad.
            if (command.type == PaintCommand::Type::Text)
                return false;

            Rect area = command.rect;
            if (command.type == PaintCommand::Type::Shadow) {
                area.setX(area.getX() + command.shadowOffset.x);
                area.setY(area.getY() + command.shadowOffset.y);
                const float blur = std::max(0.0f, command.shadowBlur);
                area = expandRect(area, blur);
            }

            if (area.getWidth() <= 0.0f || area.getHeight() <= 0.0f)
                return true;

            return rectsDisjoint(area, command.clip);
        }

        bool sameDrawState(const SlateDrawRange& range, const PaintCommand& command, bool fontAtlas) {
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

    void SlateMesh::clear() {
        //resize(0) keeps the capacity, so steady-state frames (same widget count)
        //reuse the buffers allocated on the first frame; the mesh is rebuilt
        //from scratch every frame but never reallocated.
        vertices.resize(0);
        indices.resize(0);
        ranges.resize(0);
    }

    unsigned int Geometry::cornerSegmentsForRadius(float radius) {
        if (radius <= 0.5f)
            return 0;

        //Arc resolution grows with the radius: 3 segments minimum to read as a
        //curve, 12 maximum to keep the vertex budget of heavily rounded panels.
        float segments = std::ceil(radius * 0.5f);
        if (segments < 3.0f)
            segments = 3.0f;
        if (segments > 12.0f)
            segments = 12.0f;
        return (unsigned int)segments;
    }

    void Geometry::build(const PaintList& paintList, SlateMesh& mesh) {
        //mesh is cleared once per frame by the caller and may already hold
        //geometry here; ranges are extended only at the back, so the appended
        //index span stays contiguous either way.
        const unsigned int commandCount = paintList.getCommandCount();
        const std::vector<PaintCommand>& commands = paintList.getCommands();

        for (unsigned int i = 0; i < commandCount; i++) {
            const PaintCommand& command = commands[i];

            if (isCulled(command))
                continue;

            if (command.type != PaintCommand::Type::Text && !hasVisibleSize(command.rect))
                continue;

            unsigned int indexCount = 0;
            switch (command.type) {
                case PaintCommand::Type::Rect:
                    indexCount = addRectCommand(mesh, command);
                    break;
                case PaintCommand::Type::Border:
                    indexCount = addBorderCommand(mesh, command);
                    break;
                case PaintCommand::Type::Image:
                    indexCount = addImageCommand(mesh, command);
                    break;
                case PaintCommand::Type::Text:
                    indexCount = addTextCommand(mesh, paintList, command);
                    break;
                case PaintCommand::Type::Gradient:
                    indexCount = addGradientCommand(mesh, command);
                    break;
                case PaintCommand::Type::Shadow:
                    indexCount = addShadowCommand(mesh, command);
                    break;
            }

            if (indexCount == 0)
                continue;

            const bool fontAtlas = command.type == PaintCommand::Type::Text;

            if (!mesh.ranges.empty() && sameDrawState(mesh.ranges.back(), command, fontAtlas)) {
                mesh.ranges.back().indexCount += indexCount;
            }else{
                SlateDrawRange range;
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
}
