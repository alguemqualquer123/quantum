// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateButton_h
#define SlateButton_h

#include <string>

#include "Export.h"
#include "slate/core/Widget.h"

namespace quantum::slate {

    // Text and/or icon button. All four looks (normal, hovered, pressed,
    // disabled) come from the named style, which is why the click routing is not
    // here yet: with no input phase, the widget renders its normal state and the
    // demo drives the state explicitly to prove the style works.
    //
    //   menu->add<Button>("play")->setText("Play").setStyleName("Menu.Button");
    class Button : public Widget {
    public:
        Button();
        explicit Button(std::string id);

        Button& setText(std::string text);
        const std::string& getText() const;
        const char* getTypeName() const override;
        Button& setIcon(const std::string& brushName);
        //True puts the icon before the text, false after it.
        Button& setIconFirst(bool first);

        //Forced state for tests and previews; getVisualState() still wins for
        //disabled widgets so a forced hover never hides a disabled look.
        Button& setVisualState(WidgetVisualState state);

        WidgetVisualState getVisualState() const override;
        Vector2 measure(Vector2 available) override;
        void paint(PaintContext& context) override;

    private:
        std::string text;
        std::string iconBrush;
        bool iconFirst;
        bool hasForcedState;
        WidgetVisualState forcedState;
    };
}

#endif /* SlateButton_h */
