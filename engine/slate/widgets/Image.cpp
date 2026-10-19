// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "slate/widgets/Image.h"

#include <algorithm>
#include <utility>

#include "slate/core/StyleSet.h"

namespace doriax::slate {

    namespace {
        void clampSize(float& width, float& height, const Vector2& minSize, const Vector2& maxSize) {
            if (maxSize.x > 0.0f) {
                width = std::min(width, maxSize.x);
            }
            if (maxSize.y > 0.0f) {
                height = std::min(height, maxSize.y);
            }
            width = std::max(width, minSize.x);
            height = std::max(height, minSize.y);
        }

        //Componentwise multiply, the way a layer tint stacks on a brush tint.
        Vector4 multiply(const Vector4& a, const Vector4& b) {
            return Vector4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
        }
    }

    Image::Image() : Image(std::string()) {
    }

    Image::Image(std::string imageId) : Widget(std::move(imageId)),
        hasTint(false),
        tint(1.0f, 1.0f, 1.0f, 1.0f),
        hasDrawModeOverride(false),
        drawModeOverride(BrushDrawMode::Simple) {
    }

    Image& Image::setBrush(const std::string& brushName) {
        if (this->brushName == brushName) {
            return *this;
        }
        this->brushName = brushName;
        //the brush's design size is what the widget measures
        markSubtreeDirty(WidgetDirty::Layout | WidgetDirty::Paint);
        return *this;
    }

    const std::string& Image::getBrush() const {
        return brushName;
    }

    Image& Image::setTint(const Vector4& newTint) {
        hasTint = true;
        tint = newTint;
        markDirty(WidgetDirty::Paint);
        return *this;
    }

    Image& Image::setDrawMode(BrushDrawMode mode) {
        if (hasDrawModeOverride && drawModeOverride == mode) {
            return *this;
        }
        hasDrawModeOverride = true;
        drawModeOverride = mode;
        markDirty(WidgetDirty::Paint);
        return *this;
    }

    Vector2 Image::measure(Vector2 available) {
        const WidgetStyle& style = getStyle();
        float width = 0.0f;
        float height = 0.0f;

        //findBrush (not getBrush): measuring must not touch the disk, and the
        //design size is authored into the brush when it is registered.
        StyleSet* set = StyleSet::get();
        const std::string& source = !brushName.empty() ? brushName : style.backgroundBrush;
        if (set != nullptr && !source.empty()) {
            const SlateBrush* brush = set->findBrush(source);
            if (brush != nullptr) {
                width = brush->size.x;
                height = brush->size.y;
            }
        }

        width += style.padding.left + style.padding.right;
        height += style.padding.top + style.padding.bottom;

        clampSize(width, height, style.minSize, style.maxSize);
        clampSize(width, height, slot.minSize, slot.maxSize);
        if (available.x >= 0.0f) {
            width = std::min(width, available.x);
        }
        if (available.y >= 0.0f) {
            height = std::min(height, available.y);
        }

        desiredSize = Vector2(width, height);
        return desiredSize;
    }

    void Image::paint(PaintContext& context) {
        if (!visible || context.list == nullptr) {
            return;
        }

        const WidgetStyle& style = getStyle();
        StyleSet* set = StyleSet::get();
        const std::string& source = !brushName.empty() ? brushName : style.backgroundBrush;

        //Nothing to draw as a brush: the style's own colours (and, when it does
        //name a brush, the brush itself) are emitted by the base widget.
        if (set == nullptr || source.empty() ||
            (source == style.backgroundBrush && !hasTint && !hasDrawModeOverride)) {
            Widget::paint(context);
            return;
        }

        const Rect rect = applyPaintTransform(layoutRect, context);
        if (rect.getWidth() <= 0.0f || rect.getHeight() <= 0.0f) {
            return;
        }

        //A style background brush painted with an instance tint must not go through
        //Widget::paint, which already emits it: draw the border from the style and
        //the background ourselves so the brush is emitted exactly once.
        if (source != style.backgroundBrush) {
            paintBackground(context, rect);
        }
        paintBorder(context, rect);

        //getBrush: lazy texture load on first paint; an unresolvable brush returns
        //an empty one, so a missing icon never stops the paint pass.
        const SlateBrush& found = set->getBrush(source);
        if (found.isEmpty()) {
            return;
        }

        SlateBrush brush = found;
        if (hasDrawModeOverride) {
            brush.drawMode = drawModeOverride;
        }

        //the style colour tints the artwork first (a fully transparent colour would
        //erase it, so it falls back to white, as paintBackground does), then the
        //widget tint rides on top of it
        Vector4 tint = style.backgroundColor.w > 0.0f ? style.backgroundColor : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        if (hasTint) {
            tint = multiply(tint, this->tint);
        }

        context.list->addImage(rect, brush, applyOpacity(tint, context), context.clip, context.hasClip);
    }
}
