// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "slate/widgets/Label.h"

#include <algorithm>
#include <utility>

#include "slate/core/TextRenderer.h"

namespace doriax::slate {

    namespace {
        //The engine has no font file path to fall back on: the default face is the
        //Roboto subset compiled into the binary (engine/core/util/DefaultFont.h),
        //which STBText loads when the path handed to it is empty
        //(*STBText.cpp:120*), and TextRenderer::get() forwards exactly that empty
        //path to FontPool (TextRenderer.cpp:158-161). So the "default font" of the
        //module is the empty string.
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
    }

    Label::Label() : Label(std::string()) {
    }

    Label::Label(std::string labelId) : Widget(std::move(labelId)),
        hasWrapOverride(false),
        wrapValue(false),
        hasAlignOverride(false),
        alignValue(TextAlign::Near) {
    }

    Label& Label::setText(std::string newText) {
        if (text == newText) {
            return *this;
        }
        text = std::move(newText);
        //the content changed size, so the whole flow has to be re-solved
        markSubtreeDirty(WidgetDirty::Layout | WidgetDirty::Paint);
        return *this;
    }

    const std::string& Label::getText() const {
        return text;
    }

    Label& Label::setWrap(bool wrap) {
        if (hasWrapOverride && wrapValue == wrap) {
            return *this;
        }
        hasWrapOverride = true;
        wrapValue = wrap;
        markSubtreeDirty(WidgetDirty::Layout | WidgetDirty::Paint);
        return *this;
    }

    Label& Label::setTextAlign(TextAlign align) {
        if (hasAlignOverride && alignValue == align) {
            return *this;
        }
        hasAlignOverride = true;
        alignValue = align;
        markDirty(WidgetDirty::Paint);
        return *this;
    }

    const TextRenderer* Label::resolveFont() const {
        //Font and size come from the style only: a label never hard-codes a face,
        //otherwise a theme could not retype a whole HUD.
        const WidgetStyle& style = getStyle();
        const std::string& path = style.fontFamily.empty() ? kDefaultFontPath : style.fontFamily;
        const TextRenderer* font = TextRenderer::get(path, style.fontSize);
        return font != nullptr && font->isValid() ? font : nullptr;
    }

    Vector2 Label::measure(Vector2 available) {
        const WidgetStyle& style = getStyle();
        const float padX = style.padding.left + style.padding.right;
        const float padY = style.padding.top + style.padding.bottom;

        float width = padX;
        float height = padY;

        const TextRenderer* font = resolveFont();
        if (font != nullptr && !text.empty()) {
            const bool multiline = hasWrapOverride ? wrapValue : style.wrap;
            //a negative available is the Widget::measure spelling of unbounded
            const float wrapWidth = available.x >= 0.0f
                ? std::max(0.0f, available.x - padX) : -1.0f;
            const Vector2 size = font->measureWrapped(text, wrapWidth, multiline);
            width += size.x;
            height += size.y;
        }

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

    void Label::paint(PaintContext& context) {
        //style background/border first, then the glyphs on top
        Widget::paint(context);

        if (context.list == nullptr || !visible || text.empty()) {
            return;
        }

        const TextRenderer* font = resolveFont();
        if (font == nullptr) {
            return;
        }

        const WidgetStyle& style = getStyle();
        const bool multiline = hasWrapOverride ? wrapValue : style.wrap;
        const TextAlign align = hasAlignOverride ? alignValue : style.textAlign;

        const Rect content = applyPaintTransform(contentRect(layoutRect), context);
        if (content.getWidth() <= 0.0f || content.getHeight() <= 0.0f) {
            return;
        }

        //layout() aligns the lines inside maxWidth, so the horizontal placement is
        //the style's job; only the vertical centring is added here (UMG labels sit
        //in the middle of their box rather than on its first baseline).
        const Vector2 block = font->measureWrapped(text, content.getWidth(), multiline);
        const float top = content.getY() + std::max(0.0f, (content.getHeight() - block.y) * 0.5f);

        font->layout(text, Vector2(content.getX(), top), applyOpacity(style.textColor, context),
            content.getWidth(), multiline, align, style.overflow,
            context.clip, context.hasClip, *context.list);
    }
}
