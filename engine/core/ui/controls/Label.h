// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef Label_h
#define Label_h

#include <string>

#include "Export.h"
#include "ui/core/VisualElement.h"

namespace doriax::ui {

    class FontAtlas;

    // Single-line text element. Multiline, wrapping and rich text arrive with
    // the text layout phase.
    class DORIAX_API Label : public VisualElement {
    public:
        Label();
        explicit Label(std::string text);

        void setText(std::string text);
        const std::string& getText() const;

        //empty path means the engine built-in faces
        void setFont(std::string fontPath);
        const std::string& getFont() const;

        void setFontSize(unsigned int fontSize);
        unsigned int getFontSize() const;

        void setTextColor(const Vector4& color);
        const Vector4& getTextColor() const;

        void paint(PaintContext& context, const Rect& rect) override;

        //content sizing: label boxes are measured from the shaped text
        bool usesMeasureFunc() const override;
        Vector2 measureIntrinsicSize(float availableWidth, bool widthAtMost,
                                     float availableHeight, bool heightAtMost) override;

    private:
        FontAtlas* resolveAtlas();

        std::string text;
        std::string fontPath;
        unsigned int fontSize;
        Vector4 textColor;

        //cached atlas; the font atlas cache outlives every element
        FontAtlas* atlas;

        //measure cache: shaping is expensive and the layout pass can ask the
        //same label for its size several times before a repaint
        bool measured;
        Vector2 measuredSize;
    };
}

#endif /* Label_h */
