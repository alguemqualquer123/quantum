// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateImage_h
#define SlateImage_h

#include <string>

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "slate/core/SlateTypes.h"
#include "slate/core/Widget.h"

namespace quantum::slate {

    // A brush on screen: icon, sprite, banner, nine-slice panel background.
    // The brush is always looked up by name so art can be swapped per theme.
    //
    //   page->add<Image>("save")->setBrush("Icons.Save").setTint(Color::white());
    class Image : public Widget {
    public:
        Image();
        explicit Image(std::string id);

        Image& setBrush(const std::string& brushName);
        const std::string& getBrush() const;
        Image& setTint(const Vector4& tint);
        //Draw mode / slice margins of the named brush, overridden per instance.
        Image& setDrawMode(BrushDrawMode mode);

        Vector2 measure(Vector2 available) override;
        void paint(PaintContext& context) override;

    private:
        std::string brushName;
        bool hasTint;
        Vector4 tint;
        bool hasDrawModeOverride;
        BrushDrawMode drawModeOverride;
    };
}

#endif /* SlateImage_h */
