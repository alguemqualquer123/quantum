// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateRadioButton_h
#define SlateRadioButton_h

#include <functional>
#include <string>

#include "slate/core/TextRenderer.h"
#include "slate/core/Widget.h"

namespace quantum::slate {

    // A mutually-exclusive option. Siblings with the same non-empty group are
    // cleared when the user selects this control.
    class RadioButton : public Widget {
    public:
        RadioButton();
        explicit RadioButton(std::string id);

        RadioButton& setText(std::string text);
        const std::string& getText() const;
        RadioButton& setGroup(std::string group);
        const std::string& getGroup() const;
        RadioButton& setChecked(bool checked);
        bool isChecked() const;
        RadioButton& setOnChanged(std::function<void(bool)> callback);

        const char* getTypeName() const override;
        Vector2 measure(Vector2 available) override;
        void paint(PaintContext& context) override;
        bool onPointerUp(SlatePointerEvent& event) override;
        bool onKeyDown(SlateKeyEvent& event) override;

    private:
        void select();
        const TextRenderer* resolveFont() const;

        std::string text;
        std::string group;
        bool checked = false;
        std::function<void(bool)> onChanged;
    };

}

#endif /* SlateRadioButton_h */
