// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateProgressBar_h
#define SlateProgressBar_h

#include <string>

#include "Export.h"
#include "slate/core/Widget.h"

namespace quantum::slate {

    // Read-only percentage bar: a track brush and a fill brush clipped to the
    // value. Used for health, ammo, encumbrance and load screens.
    //
    //   hud->add<ProgressBar>("hp")->setRange(0.0f, 100.0f).setValue(72.0f);
    class ProgressBar : public Widget {
    public:
        ProgressBar();
        explicit ProgressBar(std::string id);

        ProgressBar& setMinValue(float value);
        ProgressBar& setMaxValue(float value);
        ProgressBar& setRange(float minValue, float maxValue);
        ProgressBar& setValue(float value);
        float getValue() const;
        //0..1 fraction of the track the value covers.
        float getFraction() const;
        //Optional "{0}"-style caption drawn over the bar from the style text.
        ProgressBar& setTextFormat(const std::string& format);

        Vector2 measure(Vector2 available) override;
        void paint(PaintContext& context) override;

    private:
        float minValue;
        float maxValue;
        float value;
        std::string format;
    };
}

#endif /* SlateProgressBar_h */
