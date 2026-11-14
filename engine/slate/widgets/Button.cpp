// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "slate/widgets/Button.h"

#include <algorithm>
#include <utility>

#include "slate/core/TextRenderer.h"

namespace quantum::slate {

    const char* Button::getTypeName() const { return "Button"; }

    namespace {
        //See Label.cpp: the engine's default face is the Roboto subset compiled into
        //the binary, selected by handing STBText an empty path.
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

        struct ButtonContent {
            const TextRenderer* font = nullptr;
            Vector2 textSize = Vector2(0.0f, 0.0f);
            Vector2 iconSize = Vector2(0.0f, 0.0f);
            float gap = 0.0f;
        };

        //Shared by measure() and paint() so the two can never disagree about how
        //wide the button wants to be. `iconBrush` is passed in because the frozen
        //Button.h exposes no getter for it.
        ButtonContent contentOf(const std::string& label, const std::string& iconBrush,
                                const WidgetStyle& style) {
            ButtonContent out;

            const std::string& path = style.fontFamily.empty() ? kDefaultFontPath : style.fontFamily;
            const TextRenderer* font = TextRenderer::get(path, style.fontSize);
            if (font != nullptr && font->isValid()) {
                out.font = font;
                if (!label.empty()) {
                    out.textSize = font->measureWrapped(label, -1.0f, false);
                }
            }

            const float square = static_cast<float>(style.fontSize);
            if (!iconBrush.empty()) {
                StyleSet* set = StyleSet::get();
                //findBrush, not getBrush: measuring must not hit the disk
                const SlateBrush* brush = set != nullptr ? set->findBrush(iconBrush) : nullptr;
                out.iconSize = brush != nullptr && !brush->isEmpty()
                    ? brush->size : Vector2(square, square);
                if (out.iconSize.x <= 0.0f) {
                    out.iconSize.x = square;
                }
                if (out.iconSize.y <= 0.0f) {
                    out.iconSize.y = square;
                }
            }

            //icon/label spacing scales with the font so a retyped theme restyles it
            out.gap = out.iconSize.x > 0.0f && out.textSize.x > 0.0f
                ? std::max(4.0f, square * 0.35f) : 0.0f;
            return out;
        }
    }

    Button::Button() : Button(std::string()) {
    }

    Button::Button(std::string buttonId) : Widget(std::move(buttonId)),
        iconFirst(true),
        hasForcedState(false),
        forcedState(WidgetVisualState::Normal) {
    }

    Button& Button::setText(std::string newText) {
        if (text == newText) {
            return *this;
        }
        text = std::move(newText);
        markSubtreeDirty(WidgetDirty::Layout | WidgetDirty::Paint);
        return *this;
    }

    const std::string& Button::getText() const {
        return text;
    }

    Button& Button::setIcon(const std::string& brushName) {
        if (iconBrush == brushName) {
            return *this;
        }
        iconBrush = brushName;
        markSubtreeDirty(WidgetDirty::Layout | WidgetDirty::Paint);
        return *this;
    }

    Button& Button::setIconFirst(bool first) {
        if (iconFirst == first) {
            return *this;
        }
        iconFirst = first;
        markDirty(WidgetDirty::Paint);
        return *this;
    }

    Button& Button::setVisualState(WidgetVisualState state) {
        if (hasForcedState && forcedState == state) {
            return *this;
        }
        hasForcedState = true;
        forcedState = state;
        //a different state resolves a different style, which can carry different
        //padding and colours
        markSubtreeDirty(WidgetDirty::Layout);
        markDirty(WidgetDirty::Style | WidgetDirty::Paint);
        return *this;
    }

    WidgetVisualState Button::getVisualState() const {
        //disabled always wins: a forced hover must never hide the disabled look
        if (!enabled) {
            return WidgetVisualState::Disabled;
        }
        return hasForcedState ? forcedState : Widget::getVisualState();
    }

    Vector2 Button::measure(Vector2 available) {
        const WidgetStyle& style = getStyle();
        const ButtonContent content = contentOf(text, iconBrush, style);

        const float padX = style.padding.left + style.padding.right;
        const float padY = style.padding.top + style.padding.bottom;

        float width = padX + content.iconSize.x + content.textSize.x + content.gap;
        float height = padY + std::max(content.iconSize.y,
            std::max(content.textSize.y, content.font != nullptr ? content.font->getLineHeight() : 0.0f));

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

    void Button::paint(PaintContext& context) {
        //background and border come from the style resolved for the current visual
        //state, which is why the demo can prove a theme by only restating it
        Widget::paint(context);

        if (context.list == nullptr || !visible) {
            return;
        }

        const WidgetStyle& style = getStyle();
        const ButtonContent content = contentOf(text, iconBrush, style);
        if (content.iconSize.x <= 0.0f && content.textSize.x <= 0.0f) {
            return;
        }

        const Rect inner = applyPaintTransform(contentRect(layoutRect), context);
        if (inner.getWidth() <= 0.0f || inner.getHeight() <= 0.0f) {
            return;
        }

        const float total = content.iconSize.x + content.textSize.x + content.gap;
        //the whole group is centred in the content box (UMG buttons centre their
        //label), so a squeezed button crops symmetrically
        float x = inner.getX() + std::max(0.0f, (inner.getWidth() - total) * 0.5f);
        const float groupHeight = std::max(content.iconSize.y, content.textSize.y);
        const float y = inner.getY() + std::max(0.0f, (inner.getHeight() - groupHeight) * 0.5f);

        //the icon inherits the label colour so one token restyles both
        const Vector4 tint = applyOpacity(style.textColor, context);
        //maxWidth is the measured label width: the group placement above already
        //is the alignment, so layout() must not realign inside it
        const float maxWidth = content.textSize.x;

        if (!iconFirst && content.font != nullptr && content.textSize.x > 0.0f) {
            const float top = y + std::max(0.0f, (groupHeight - content.textSize.y) * 0.5f);
            content.font->layout(text, Vector2(x, top), tint, maxWidth, false,
                style.textAlign, style.overflow, context.clip, context.hasClip, *context.list);
            x += content.textSize.x + content.gap;
        }

        if (content.iconSize.x > 0.0f) {
            StyleSet* set = StyleSet::get();
            if (set != nullptr) {
                //getBrush: lazy texture upload on first paint
                const SlateBrush& brush = set->getBrush(iconBrush);
                if (!brush.isEmpty()) {
                    const Rect iconRect(x,
                        y + std::max(0.0f, (groupHeight - content.iconSize.y) * 0.5f),
                        content.iconSize.x, content.iconSize.y);
                    context.list->addImage(iconRect, brush, tint, context.clip, context.hasClip);
                }
            }
            x += content.iconSize.x + content.gap;
        }

        if (iconFirst && content.font != nullptr && content.textSize.x > 0.0f) {
            const float top = y + std::max(0.0f, (groupHeight - content.textSize.y) * 0.5f);
            content.font->layout(text, Vector2(x, top), tint, maxWidth, false,
                style.textAlign, style.overflow, context.clip, context.hasClip, *context.list);
        }
    }
}
