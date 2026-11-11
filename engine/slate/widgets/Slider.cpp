// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "slate/widgets/Slider.h"

#include "Input.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace quantum::slate {

namespace {

void clampSize(float& width, float& height, const Vector2& minSize, const Vector2& maxSize) {
    if (maxSize.x > 0.0f) width = std::min(width, maxSize.x);
    if (maxSize.y > 0.0f) height = std::min(height, maxSize.y);
    width = std::max(width, minSize.x);
    height = std::max(height, minSize.y);
}

}

Slider::Slider() : Slider(std::string()) {}

Slider::Slider(std::string sliderId) : Widget(std::move(sliderId)) {
    setFocusable(true);
}

const char* Slider::getTypeName() const { return "Slider"; }

Slider& Slider::setRange(float newMinValue, float newMaxValue) {
    minValue = std::min(newMinValue, newMaxValue);
    maxValue = std::max(newMinValue, newMaxValue);
    const float oldValue = value;
    value = clampValue(value);
    markDirty(WidgetDirty::Paint);
    notifyIfChanged(oldValue);
    return *this;
}

Slider& Slider::setValue(float newValue) {
    const float nextValue = clampValue(newValue);
    if (std::fabs(nextValue - value) < 0.000001f) return *this;
    const float oldValue = value;
    value = nextValue;
    markDirty(WidgetDirty::Paint);
    notifyIfChanged(oldValue);
    return *this;
}

Slider& Slider::setStep(float newStep) {
    step = std::max(0.0f, newStep);
    return *this;
}

Slider& Slider::setOnChanged(std::function<void(float)> callback) {
    onChanged = std::move(callback);
    return *this;
}

float Slider::getMinValue() const { return minValue; }
float Slider::getMaxValue() const { return maxValue; }
float Slider::getValue() const { return value; }
float Slider::getStep() const { return step; }

Vector2 Slider::measure(Vector2 available) {
    const WidgetStyle& style = getStyle();
    float width = std::max(160.0f, style.minSize.x);
    float height = std::max(24.0f, style.minSize.y);
    width += style.padding.left + style.padding.right;
    height += style.padding.top + style.padding.bottom;
    clampSize(width, height, style.minSize, style.maxSize);
    clampSize(width, height, slot.minSize, slot.maxSize);
    if (available.x >= 0.0f) width = std::min(width, available.x);
    if (available.y >= 0.0f) height = std::min(height, available.y);
    desiredSize = Vector2(width, height);
    return desiredSize;
}

void Slider::paint(PaintContext& context) {
    Widget::paint(context);
    if (!visible || context.list == nullptr) return;

    const WidgetStyle& style = getStyle();
    const Rect inner = applyPaintTransform(contentRect(layoutRect), context);
    if (inner.getWidth() <= 0.0f || inner.getHeight() <= 0.0f) return;

    const float trackHeight = std::min(8.0f, std::max(4.0f, inner.getHeight() * 0.25f));
    const float trackY = inner.getY() + (inner.getHeight() - trackHeight) * 0.5f;
    const Rect track(inner.getX(), trackY, inner.getWidth(), trackHeight);
    const Vector4 trackColor = style.backgroundColor.w > 0.0f
        ? style.backgroundColor : Vector4(0.2f, 0.2f, 0.2f, 1.0f);
    context.list->addRect(track, applyOpacity(trackColor, context), style.borderRadius,
        context.clip, context.hasClip);

    const float fraction = normalizedValue();
    const Vector4 fillColor = style.borderColor.w > 0.0f
        ? style.borderColor : style.textColor;
    if (fraction > 0.0f) {
        context.list->addRect(Rect(track.getX(), track.getY(), track.getWidth() * fraction,
            track.getHeight()), applyOpacity(fillColor, context), style.borderRadius,
            context.clip, context.hasClip);
    }

    const float thumbSize = std::max(trackHeight * 2.0f, 12.0f);
    const float thumbX = track.getX() + track.getWidth() * fraction - thumbSize * 0.5f;
    const Rect thumb(thumbX, inner.getY() + (inner.getHeight() - thumbSize) * 0.5f,
        thumbSize, thumbSize);
    const Vector4 thumbColor = style.textColor.w > 0.0f
        ? style.textColor : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    context.list->addRect(thumb, applyOpacity(thumbColor, context), thumbSize * 0.5f,
        context.clip, context.hasClip);
}

bool Slider::onPointerDown(SlatePointerEvent& event) {
    if (event.button != D_MOUSE_BUTTON_LEFT || !layoutRect.contains(event.position)) return false;
    dragging = true;
    setValueFromPointer(event.position);
    return true;
}

bool Slider::onPointerMove(SlatePointerEvent& event) {
    if (!dragging) return false;
    setValueFromPointer(event.position);
    return true;
}

bool Slider::onPointerUp(SlatePointerEvent& event) {
    if (event.button != D_MOUSE_BUTTON_LEFT || !dragging) return false;
    setValueFromPointer(event.position);
    dragging = false;
    return true;
}

bool Slider::onKeyDown(SlateKeyEvent& event) {
    if (event.repeat && step <= 0.0f) return false;
    const float delta = step > 0.0f ? step : (maxValue - minValue) * 0.01f;
    if (event.key == D_KEY_LEFT || event.key == D_KEY_DOWN) {
        setValue(value - delta);
        return true;
    }
    if (event.key == D_KEY_RIGHT || event.key == D_KEY_UP) {
        setValue(value + delta);
        return true;
    }
    if (event.key == D_KEY_HOME) {
        setValue(minValue);
        return true;
    }
    if (event.key == D_KEY_END) {
        setValue(maxValue);
        return true;
    }
    return false;
}

void Slider::setValueFromPointer(const Vector2& position) {
    const Rect inner = contentRect(layoutRect);
    if (inner.getWidth() <= 0.0f) return;
    const float fraction = std::max(0.0f, std::min(1.0f,
        (position.x - inner.getX()) / inner.getWidth()));
    setValue(minValue + (maxValue - minValue) * fraction);
}

float Slider::normalizedValue() const {
    const float span = maxValue - minValue;
    return span > 0.0f ? std::max(0.0f, std::min(1.0f, (value - minValue) / span)) : 0.0f;
}

float Slider::clampValue(float newValue) const {
    const float span = maxValue - minValue;
    float clamped = std::max(minValue, std::min(maxValue, newValue));
    if (step > 0.0f && span > 0.0f) {
        clamped = minValue + std::round((clamped - minValue) / step) * step;
        clamped = std::max(minValue, std::min(maxValue, clamped));
    }
    return clamped;
}

void Slider::notifyIfChanged(float oldValue) {
    if (onChanged && std::fabs(oldValue - value) >= 0.000001f) onChanged(value);
}

}
