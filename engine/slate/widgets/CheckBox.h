// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateCheckBox_h
#define SlateCheckBox_h

#include <functional>
#include <string>

#include "Export.h"
#include "slate/core/TextRenderer.h"
#include "slate/core/Widget.h"

namespace quantum::slate {

    // A focusable, themeable two-state control. The callback is invoked only by
    // user interaction; setChecked is intentionally silent for data binding.
    class CheckBox : public Widget {
    public:
        CheckBox();
        explicit CheckBox(std::string id);

        CheckBox& setText(std::string text);
        const std::string& getText() const;
        CheckBox& setChecked(bool checked);
        bool isChecked() const;
        CheckBox& setOnChanged(std::function<void(bool)> callback);

        const char* getTypeName() const override;
        Vector2 measure(Vector2 available) override;
        void paint(PaintContext& context) override;
        bool onPointerUp(SlatePointerEvent& event) override;
        bool onKeyDown(SlateKeyEvent& event) override;

    private:
        void toggle();
        const TextRenderer* resolveFont() const;

        std::string text;
        bool checked = false;
        std::function<void(bool)> onChanged;
    };

}

#endif /* SlateCheckBox_h */
