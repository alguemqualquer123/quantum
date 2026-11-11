// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateSlider_h
#define SlateSlider_h

#include <functional>
#include <string>

#include "Export.h"
#include "slate/core/Widget.h"

namespace quantum::slate {

    // A horizontal, keyboard-accessible scalar control. The style maps to the
    // track background, filled range border colour and thumb text colour.
    class Slider : public Widget {
    public:
        Slider();
        explicit Slider(std::string id);

        Slider& setRange(float minValue, float maxValue);
        Slider& setValue(float value);
        Slider& setStep(float step);
        Slider& setOnChanged(std::function<void(float)> callback);
        float getMinValue() const;
        float getMaxValue() const;
        float getValue() const;
        float getStep() const;

        const char* getTypeName() const override;
        Vector2 measure(Vector2 available) override;
        void paint(PaintContext& context) override;
        bool onPointerDown(SlatePointerEvent& event) override;
        bool onPointerMove(SlatePointerEvent& event) override;
        bool onPointerUp(SlatePointerEvent& event) override;
        bool onKeyDown(SlateKeyEvent& event) override;

    private:
        void setValueFromPointer(const Vector2& position);
        float normalizedValue() const;
        float clampValue(float value) const;
        void notifyIfChanged(float oldValue);

        float minValue = 0.0f;
        float maxValue = 1.0f;
        float value = 0.0f;
        float step = 0.01f;
        bool dragging = false;
        std::function<void(float)> onChanged;
    };

}

#endif /* SlateSlider_h */
