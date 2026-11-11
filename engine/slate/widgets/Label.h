// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateLabel_h
#define SlateLabel_h

#include <string>

#include "Export.h"
#include "math/Vector2.h"
#include "slate/core/TextRenderer.h"
#include "slate/core/Widget.h"

namespace quantum::slate {

    // Non-interactive text. Font, size, colour, alignment, wrap and overflow all
    // come from the style, so a Label is themeable without touching call sites.
    //
    //   page->add<Label>("score")->setText("Score: 1200");
    class Label : public Widget {
    public:
        Label();
        explicit Label(std::string id);

        Label& setText(std::string text);
        const std::string& getText() const;
        const char* getTypeName() const override;

        //Per-instance overrides of the style; prefer a named style when the
        //look is shared, since overrides are the reason a theme stops working.
        Label& setWrap(bool wrap);
        Label& setTextAlign(TextAlign align);

        Vector2 measure(Vector2 available) override;
        void paint(PaintContext& context) override;

    private:
        //Style font, resolved at paint time through TextRenderer's cache.
        const TextRenderer* resolveFont() const;

        std::string text;

        bool hasWrapOverride;
        bool wrapValue;
        bool hasAlignOverride;
        TextAlign alignValue;
    };
}

#endif /* SlateLabel_h */
