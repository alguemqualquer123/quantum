// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"

namespace doriax::editor {

    class Theme {
    public:
        enum class Preset {
            Dark,
            Light,
        };

        struct Colors {
            static ImVec4 ButtonActivated;
            static ImVec4 FileCardBackground;
            static ImVec4 FileCardBackgroundHovered;
            static ImVec4 SubtleText;
            static ImVec4 filenameLabel;
            static ImVec4 ExtEntityButton;
            static ImVec4 ExtEntityButtonHovered;
            static ImVec4 ExtEntityButtonActive;
            static ImVec4 NestedHeader;
            static ImVec4 NestedHeaderHovered;
            static ImVec4 NestedHeaderActive;
            static ImVec4 DisabledGreenText;
            static ImVec4 ChildSceneText;
            // Marks the viewport sub-selection (tiles, instances, occluder points)
            // in Properties lists; same orange as the viewport selection outline.
            static ImVec4 SubSelectionText;
            // Amber warning text, e.g. alert notes; matches OutputWindow's Warning log color.
            static ImVec4 WarningText;
            // Resource Monitor pressure colors: healthy/busy values read green->red
            // without hardcoding RGBA at the call site, so both presets stay legible.
            static ImVec4 SuccessText;
            static ImVec4 DangerText;
            // Downstream/upstream plot series in the Resource Monitor network graphs.
            static ImVec4 NetworkDown;
            static ImVec4 NetworkUp;
        };

        static void apply();
        static void applyDpiScale(float dpiScale);

        // apply() must run again after these for the style to change; the
        // setters only store state so callers can batch changes at boot.
        static void setPreset(Preset preset);
        static Preset getPreset();

        // "#RRGGBB" or "#RRGGBBAA"; invalid values keep the previous accent.
        static bool setAccentColor(const char* hex);
        static const char* getAccentColor();

        static float dpiScale();
        static float dpi(float value);
        static ImVec2 dpi(const ImVec2& value);
    };

}

