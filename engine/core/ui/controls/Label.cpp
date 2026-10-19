// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "ui/controls/Label.h"

#include <utility>

#include "ui/render/FontAtlas.h"
#include "ui/render/PaintList.h"

using namespace doriax;
using namespace doriax::ui;

Label::Label() :
    fontSize(18),
    textColor(1.0f, 1.0f, 1.0f, 1.0f),
    atlas(nullptr),
    measured(false),
    measuredSize(0.0f, 0.0f) {
}

Label::Label(std::string text) : Label() {
    this->text = std::move(text);
}

void Label::setText(std::string text) {
    if (this->text == text)
        return;

    this->text = std::move(text);
    measured = false;
    markDirty(DirtyFlags::Paint | DirtyFlags::Layout);
}

const std::string& Label::getText() const {
    return text;
}

void Label::setFont(std::string fontPath) {
    if (this->fontPath == fontPath)
        return;

    this->fontPath = std::move(fontPath);
    atlas = nullptr;
    measured = false;
    markDirty(DirtyFlags::Paint | DirtyFlags::Layout);
}

const std::string& Label::getFont() const {
    return fontPath;
}

void Label::setFontSize(unsigned int fontSize) {
    if (this->fontSize == fontSize)
        return;

    this->fontSize = fontSize;
    atlas = nullptr;
    measured = false;
    markDirty(DirtyFlags::Paint | DirtyFlags::Layout);
}

unsigned int Label::getFontSize() const {
    return fontSize;
}

void Label::setTextColor(const Vector4& color) {
    textColor = color;
    markDirty(DirtyFlags::Paint);
}

const Vector4& Label::getTextColor() const {
    return textColor;
}

FontAtlas* Label::resolveAtlas() {
    if (atlas == nullptr)
        atlas = FontAtlas::get(fontPath, fontSize);

    return atlas;
}

bool Label::usesMeasureFunc() const {
    return true;
}

Vector2 Label::measureIntrinsicSize(float availableWidth, bool widthAtMost,
                                    float availableHeight, bool heightAtMost) {
    //single line for now: the constraints are ignored (no wrapping yet)
    (void)availableWidth;
    (void)widthAtMost;
    (void)availableHeight;
    (void)heightAtMost;

    if (!measured) {
        FontAtlas* fontAtlas = resolveAtlas();
        measuredSize = fontAtlas != nullptr ? fontAtlas->measure(text) : Vector2(0.0f, 0.0f);
        measured = true;
    }

    return measuredSize;
}

void Label::paint(PaintContext& context, const Rect& rect) {
    VisualElement::paint(context, rect);

    if (text.empty() || context.list == nullptr || !isVisible())
        return;

    FontAtlas* fontAtlas = resolveAtlas();
    if (fontAtlas == nullptr)
        return;

    const Vector4 color = applyOpacity(textColor, context);
    if (color.w <= 0.0f)
        return;

    //STBText anchors the glyphs on the first baseline; a Label anchors the text
    //on the top-left of its box, so the origin drops by the font ascent
    const float ascent = fontAtlas->getAscent();
    fontAtlas->layout(text, Vector2(rect.getX(), rect.getY() + ascent), color, context.clip, context.hasClip, *context.list);
}
