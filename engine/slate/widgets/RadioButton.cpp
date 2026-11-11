// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "slate/widgets/RadioButton.h"

#include "Input.h"
#include "slate/core/TextRenderer.h"

#include <algorithm>
#include <utility>

namespace quantum::slate {
namespace {

const std::string kDefaultFontPath;

void clampSize(float& width, float& height, const Vector2& minSize, const Vector2& maxSize) {
    if (maxSize.x > 0.0f) width = std::min(width, maxSize.x);
    if (maxSize.y > 0.0f) height = std::min(height, maxSize.y);
    width = std::max(width, minSize.x);
    height = std::max(height, minSize.y);
}

}

RadioButton::RadioButton() : RadioButton(std::string()) {}

RadioButton::RadioButton(std::string radioId) : Widget(std::move(radioId)) {
    setFocusable(true);
}

const char* RadioButton::getTypeName() const { return "RadioButton"; }

RadioButton& RadioButton::setText(std::string newText) {
    if (text == newText) return *this;
    text = std::move(newText);
    markSubtreeDirty(WidgetDirty::Layout | WidgetDirty::Paint);
    return *this;
}

const std::string& RadioButton::getText() const { return text; }

RadioButton& RadioButton::setGroup(std::string newGroup) {
    if (group == newGroup) return *this;
    group = std::move(newGroup);
    return *this;
}

const std::string& RadioButton::getGroup() const { return group; }

RadioButton& RadioButton::setChecked(bool newChecked) {
    if (checked == newChecked) return *this;
    checked = newChecked;
    markDirty(WidgetDirty::Paint);
    return *this;
}

bool RadioButton::isChecked() const { return checked; }

RadioButton& RadioButton::setOnChanged(std::function<void(bool)> callback) {
    onChanged = std::move(callback);
    return *this;
}

const TextRenderer* RadioButton::resolveFont() const {
    const WidgetStyle& style = getStyle();
    const std::string& path = style.fontFamily.empty() ? kDefaultFontPath : style.fontFamily;
    const TextRenderer* font = TextRenderer::get(path, style.fontSize);
    return font != nullptr && font->isValid() ? font : nullptr;
}

Vector2 RadioButton::measure(Vector2 available) {
    const WidgetStyle& style = getStyle();
    const TextRenderer* font = resolveFont();
    const float indicator = font != nullptr ? static_cast<float>(style.fontSize) : 16.0f;
    const Vector2 textSize = font != nullptr && !text.empty()
        ? font->measureWrapped(text, -1.0f, false) : Vector2(0.0f, 0.0f);
    const float gap = textSize.x > 0.0f ? std::max(4.0f, indicator * 0.35f) : 0.0f;
    float width = style.padding.left + style.padding.right + indicator + gap + textSize.x;
    float height = style.padding.top + style.padding.bottom + std::max(indicator, textSize.y);
    clampSize(width, height, style.minSize, style.maxSize);
    clampSize(width, height, slot.minSize, slot.maxSize);
    if (available.x >= 0.0f) width = std::min(width, available.x);
    if (available.y >= 0.0f) height = std::min(height, available.y);
    desiredSize = Vector2(width, height);
    return desiredSize;
}

void RadioButton::paint(PaintContext& context) {
    Widget::paint(context);
    if (!visible || context.list == nullptr) return;

    const WidgetStyle& style = getStyle();
    const Rect inner = applyPaintTransform(contentRect(layoutRect), context);
    if (inner.getWidth() <= 0.0f || inner.getHeight() <= 0.0f) return;
    const TextRenderer* font = resolveFont();
    const float indicatorSize = font != nullptr ? static_cast<float>(style.fontSize) : 16.0f;
    const float indicatorY = inner.getY() + std::max(0.0f, (inner.getHeight() - indicatorSize) * 0.5f);
    const Rect indicator(inner.getX(), indicatorY, indicatorSize, indicatorSize);
    const Vector4 color = checked
        ? applyOpacity(style.textColor, context)
        : applyOpacity(style.borderColor.w > 0.0f ? style.borderColor : style.textColor, context);
    context.list->addRect(indicator, color, indicatorSize * 0.5f, context.clip, context.hasClip);
    if (!checked && style.borderWidth > 0.0f) {
        context.list->addBorder(indicator, applyOpacity(style.borderColor, context),
            style.borderWidth, indicatorSize * 0.5f, context.clip, context.hasClip);
    }

    if (font == nullptr || text.empty()) return;
    const float gap = std::max(4.0f, indicatorSize * 0.35f);
    const float textX = inner.getX() + indicatorSize + gap;
    const Vector2 textSize = font->measureWrapped(text, inner.getWidth() - indicatorSize - gap, false);
    const float textY = inner.getY() + std::max(0.0f, (inner.getHeight() - textSize.y) * 0.5f);
    font->layout(text, Vector2(textX, textY), applyOpacity(style.textColor, context),
        std::max(0.0f, inner.getWidth() - indicatorSize - gap), false,
        style.textAlign, style.overflow, context.clip, context.hasClip, *context.list);
}

void RadioButton::select() {
    if (checked) return;
    if (!group.empty() && getParent() != nullptr) {
        for (const auto& child : getParent()->getChildren()) {
            auto* sibling = dynamic_cast<RadioButton*>(child.get());
            if (sibling != nullptr && sibling != this && sibling->getGroup() == group)
                sibling->setChecked(false);
        }
    }
    checked = true;
    markDirty(WidgetDirty::Paint);
    if (onChanged) onChanged(true);
}

bool RadioButton::onPointerUp(SlatePointerEvent& event) {
    if (event.button != D_MOUSE_BUTTON_LEFT || !layoutRect.contains(event.position)) return false;
    select();
    return true;
}

bool RadioButton::onKeyDown(SlateKeyEvent& event) {
    if (event.repeat || (event.key != D_KEY_SPACE && event.key != D_KEY_ENTER)) return false;
    select();
    return true;
}

}
