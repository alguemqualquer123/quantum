// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "ui/UIRuntime.h"

#include <algorithm>

#include "Engine.h"
#include "ui/layout/YogaLayout.h"
#include "ui/render/UIRenderer.h"

using namespace doriax;
using namespace doriax::ui;

std::unique_ptr<VisualElement> UIRuntime::root;
PaintList UIRuntime::paintList;
UIMesh UIRuntime::mesh;
UIRenderer* UIRuntime::renderer = nullptr;
UIRuntime::Stats UIRuntime::stats;

namespace {

    Rect intersectRect(const Rect& a, const Rect& b) {
        const float x0 = std::max(a.getX(), b.getX());
        const float y0 = std::max(a.getY(), b.getY());
        const float x1 = std::min(a.getX() + a.getWidth(), b.getX() + b.getWidth());
        const float y1 = std::min(a.getY() + a.getHeight(), b.getY() + b.getHeight());
        return Rect(x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0));
    }

    void paintElement(VisualElement& element, const PaintContext& parentContext, UIRuntime::Stats& stats) {
        stats.elements++;

        //resolved by the layout pass (absolute canvas coordinates)
        const Rect rect = element.getLayoutRect();

        if (!element.isVisible())
            return;

        PaintContext context = parentContext;
        context.opacity = parentContext.opacity * element.getOpacity();

        element.paint(context, rect);

        if (element.getChildCount() == 0)
            return;

        PaintContext childContext = context;
        if (element.getOverflow() == Overflow::Hidden) {
            childContext.clip = childContext.hasClip ? intersectRect(childContext.clip, rect) : rect;
            childContext.hasClip = true;
        }

        for (const auto& child : element.getChildren()) {
            paintElement(*child, childContext, stats);
        }
    }
}

void UIRuntime::setRoot(std::unique_ptr<VisualElement> root) {
    UIRuntime::root = std::move(root);
}

VisualElement* UIRuntime::getRoot() {
    return root.get();
}

bool UIRuntime::hasRoot() {
    return root != nullptr;
}

void UIRuntime::buildFrame() {
    stats = Stats();
    paintList.clear();

    if (root) {
        //flexbox layout: consumes the layout dirty flags, recalculates when
        //anything changed and stores the absolute rects on the elements
        YogaLayout::update(*root, (float)Engine::getCanvasWidth(), (float)Engine::getCanvasHeight());

        PaintContext context;
        context.list = &paintList;
        paintElement(*root, context, stats);
    }

    VertexBuilder::build(paintList, mesh);

    stats.commands = paintList.getCommandCount();
    stats.glyphQuads = paintList.getGlyphQuadCount();
    stats.vertices = (unsigned int)mesh.vertices.size();
    stats.indices = (unsigned int)mesh.indices.size();
    stats.ranges = (unsigned int)mesh.ranges.size();
}

const UIRuntime::Stats& UIRuntime::getStats() {
    return stats;
}

const PaintList& UIRuntime::getPaintList() {
    return paintList;
}

const UIMesh& UIRuntime::getMesh() {
    return mesh;
}

void UIRuntime::render() {
    if (!root || !Engine::isViewLoaded())
        return;

    buildFrame();

    if (mesh.ranges.empty())
        return;

    if (!renderer)
        renderer = createUIRenderer().release();

    const float canvasWidth = (float)Engine::getCanvasWidth();
    const float canvasHeight = (float)Engine::getCanvasHeight();

    renderer->render(mesh, Vector2(canvasWidth, canvasHeight), Engine::getViewRect(), Engine::getFramebuffer());
}

void UIRuntime::clear() {
    root.reset();
    paintList.clear();
    mesh.clear();
    stats = Stats();
}
