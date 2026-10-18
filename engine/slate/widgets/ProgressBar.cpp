// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "slate/widgets/ProgressBar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "slate/core/TextRenderer.h"

namespace doriax::slate {

    namespace {
        //See Label.cpp: an empty path selects the Roboto face compiled into the
        //engine (engine/core/util/DefaultFont.h), the module's default font.
        const std::string kDefaultFontPath;

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

        Rect intersectRect(const Rect& a, const Rect& b) {
            const float left = std::max(a.getX(), b.getX());
            const float top = std::max(a.getY(), b.getY());
            const float right = std::min(a.getX() + a.getWidth(), b.getX() + b.getWidth());
            const float bottom = std::min(a.getY() + a.getHeight(), b.getY() + b.getHeight());
            if (right <= left || bottom <= top) {
                return Rect(0.0f, 0.0f, 0.0f, 0.0f);
            }
            return Rect(left, top, right - left, bottom - top);
        }

        //"1 234.5" is what a HUD wants for a value that may or may not be whole;
        //no locale-dependent decimal separator and no allocation.
        std::string formatValue(float value) {
            char buffer[32];
            if (std::fabs(value - std::round(value)) < 0.0001f) {
                std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(std::lround(value)));
            } else {
                std::snprintf(buffer, sizeof(buffer), "%.1f", static_cast<double>(value));
            }
            return std::string(buffer);
        }

        std::string applyFormat(const std::string& pattern, float value) {
            const std::string token = "{0}";
            std::string out;
            out.reserve(pattern.size() + 8);

            size_t cursor = 0;
            while (cursor < pattern.size()) {
                const size_t found = pattern.find(token, cursor);
                if (found == std::string::npos) {
                    out.append(pattern, cursor, pattern.size() - cursor);
                    break;
                }
                out.append(pattern, cursor, found - cursor);
                out.append(formatValue(value));
                cursor = found + token.size();
            }
            return out;
        }
    }

    ProgressBar::ProgressBar() : ProgressBar(std::string()) {
    }

    ProgressBar::ProgressBar(std::string barId) : Widget(std::move(barId)),
        minValue(0.0f),
        maxValue(1.0f),
        value(0.0f) {
    }

    ProgressBar& ProgressBar::setMinValue(float newValue) {
        minValue = newValue;
        markDirty(WidgetDirty::Paint);
        return *this;
    }

    ProgressBar& ProgressBar::setMaxValue(float newValue) {
        maxValue = newValue;
        markDirty(WidgetDirty::Paint);
        return *this;
    }

    ProgressBar& ProgressBar::setRange(float newMinValue, float newMaxValue) {
        minValue = newMinValue;
        maxValue = newMaxValue;
        markDirty(WidgetDirty::Paint);
        return *this;
    }

    ProgressBar& ProgressBar::setValue(float newValue) {
        if (value == newValue) {
            return *this;
        }
        value = newValue;
        //only the fill length and the caption change: no re-layout
        markDirty(WidgetDirty::Paint);
        return *this;
    }

    float ProgressBar::getValue() const {
        return value;
    }

    float ProgressBar::getFraction() const {
        const float span = maxValue - minValue;
        if (!(span > 0.0f)) {
            return 0.0f;
        }
        return std::max(0.0f, std::min(1.0f, (value - minValue) / span));
    }

    ProgressBar& ProgressBar::setTextFormat(const std::string& format) {
        if (this->format == format) {
            return *this;
        }
        this->format = format;
        //the caption can make the bar taller when the theme gives it no min height
        markSubtreeDirty(WidgetDirty::Layout | WidgetDirty::Paint);
        return *this;
    }

    Vector2 ProgressBar::measure(Vector2 available) {
        const WidgetStyle& style = getStyle();

        //a bar claims no intrinsic length: the theme's min size (or the slot) sizes
        //it, the same way Slate's progress widgets need an explicit extent
        float width = std::max(style.minSize.x, slot.minSize.x);
        float height = std::max(style.minSize.y, slot.minSize.y);

        if (!format.empty()) {
            const std::string& path = style.fontFamily.empty() ? kDefaultFontPath : style.fontFamily;
            const TextRenderer* font = TextRenderer::get(path, style.fontSize);
            if (font != nullptr && font->isValid()) {
                height = std::max(height, font->getLineHeight());
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

    void ProgressBar::paint(PaintContext& context) {
        if (!visible || context.list == nullptr) {
            return;
        }

        const WidgetStyle& style = getStyle();
        const Rect rect = applyPaintTransform(layoutRect, context);
        if (rect.getWidth() <= 0.0f || rect.getHeight() <= 0.0f) {
            return;
        }

        //WidgetStyle has no "fill" attribute, so the bar maps the style's two slots
        //onto its two parts: background* is the track, border* the fill. The border
        //stroke is skipped on purpose - drawing it would ring the bar in the fill
        //colour and read as a full one at any value.
        paintBackground(context, rect);

        const float fraction = getFraction();
        if (fraction > 0.0f) {
            const Rect inner = applyPaintTransform(contentRect(layoutRect), context);
            if (inner.getWidth() > 0.0f && inner.getHeight() > 0.0f) {
                const float fillWidth = inner.getWidth() * fraction;
                const Rect fill(inner.getX(), inner.getY(), fillWidth, inner.getHeight());

                //the fill is clipped to the track's inner box so a rounded track
                //never bleeds at either end
                const Rect clip = context.hasClip ? intersectRect(context.clip, inner) : inner;

                StyleSet* set = StyleSet::get();
                const SlateBrush* fillBrush = !style.borderBrush.empty() && set != nullptr
                    ? set->findBrush(style.borderBrush) : nullptr;

                if (fillBrush != nullptr && !fillBrush->isEmpty()) {
                    Vector4 tint = style.borderColor.w > 0.0f
                        ? style.borderColor : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
                    context.list->addImage(fill, *fillBrush, applyOpacity(tint, context),
                        clip, true);
                } else if (style.borderColor.w > 0.0f) {
                    //a square cap while the track is rounded would poke out of it, so
                    //the radius only applies once the fill covers the whole track
                    const float radius = fraction >= 1.0f ? style.borderRadius : 0.0f;
                    context.list->addRect(fill, applyOpacity(style.borderColor, context),
                        radius, clip, true);
                }
            }
        }

        if (format.empty()) {
            return;
        }

        const std::string& path = style.fontFamily.empty() ? kDefaultFontPath : style.fontFamily;
        const TextRenderer* font = TextRenderer::get(path, style.fontSize);
        if (font == nullptr || !font->isValid()) {
            return;
        }

        const std::string caption = applyFormat(format, value);
        const Rect inner = applyPaintTransform(contentRect(layoutRect), context);
        if (inner.getWidth() <= 0.0f || inner.getHeight() <= 0.0f) {
            return;
        }

        //layout() centres the line inside maxWidth, which is what a caption over a
        //bar wants; the vertical offset only centres the block.
        const Vector2 block = font->measureWrapped(caption, inner.getWidth(), false);
        const float top = inner.getY() + std::max(0.0f, (inner.getHeight() - block.y) * 0.5f);
        const TextAlign align = style.textAlign;

        font->layout(caption, Vector2(inner.getX(), top), applyOpacity(style.textColor, context),
            inner.getWidth(), false, align, style.overflow,
            context.clip, context.hasClip, *context.list);
    }
}
