// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "Theme.h"

#include "imgui_internal.h"

#include <cstring>
#include <string>

namespace {

ImGuiStyle g_baseStyle;
bool g_hasBaseStyle = false;
float g_appliedDpiScale = 0.0f;

doriax::editor::Theme::Preset g_preset = doriax::editor::Theme::Preset::Dark;
std::string g_accentHex = "#3E5C82";

struct Tokens {
    ImVec4 text;
    ImVec4 textDisabled;
    ImVec4 bgDeep;     // behind frames: inputs, docks emptiness
    ImVec4 bgBase;     // window/child/title surface
    ImVec4 bgSurface;  // headers, tabs, popups, table headers
    ImVec4 bgElevated; // hovered/active surfaces
    ImVec4 border;
    ImVec4 accent;
    ImVec4 accentBright;
    ImVec4 button;
    ImVec4 buttonHovered;
    ImVec4 buttonActive;
    ImVec4 scrollbar;
    ImVec4 plot;
    ImVec4 selection;
    ImVec4 warning;
};

Tokens darkTokens() {
    Tokens t;
    t.text = ImVec4(0.82f, 0.80f, 0.79f, 1.00f);
    t.textDisabled = ImVec4(0.47f, 0.44f, 0.42f, 1.00f);
    t.bgDeep = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    t.bgBase = ImVec4(0.15f, 0.15f, 0.16f, 1.00f);
    t.bgSurface = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    t.bgElevated = ImVec4(0.26f, 0.26f, 0.29f, 1.00f);
    t.border = ImVec4(0.07f, 0.07f, 0.08f, 1.00f);
    return t;
}

Tokens lightTokens() {
    Tokens t;
    t.text = ImVec4(0.13f, 0.13f, 0.14f, 1.00f);
    t.textDisabled = ImVec4(0.48f, 0.48f, 0.50f, 1.00f);
    t.bgDeep = ImVec4(0.86f, 0.86f, 0.88f, 1.00f);
    t.bgBase = ImVec4(0.94f, 0.94f, 0.95f, 1.00f);
    t.bgSurface = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
    t.bgElevated = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
    t.border = ImVec4(0.72f, 0.72f, 0.75f, 1.00f);
    return t;
}

bool parseHexComponent(const std::string& hex, size_t pos, float& out) {
    if (pos + 1 >= hex.size()) {
        return false;
    }
    int value = 0;
    for (size_t i = pos; i < pos + 2; ++i) {
        const char c = hex[i];
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return false;
        }
        value = value * 16 + digit;
    }
    out = value / 255.0f;
    return true;
}

bool parseAccent(ImVec4& out) {
    const std::string& hex = g_accentHex;
    if (hex.size() != 7 && hex.size() != 9) {
        return false;
    }
    if (hex[0] != '#') {
        return false;
    }
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
    if (!parseHexComponent(hex, 1, r)
        || !parseHexComponent(hex, 3, g)
        || !parseHexComponent(hex, 5, b)) {
        return false;
    }
    if (hex.size() == 9 && !parseHexComponent(hex, 7, a)) {
        return false;
    }
    out = ImVec4(r, g, b, a);
    return true;
}

}

ImVec4 doriax::editor::Theme::Colors::ButtonActivated;
ImVec4 doriax::editor::Theme::Colors::FileCardBackground;
ImVec4 doriax::editor::Theme::Colors::FileCardBackgroundHovered;
ImVec4 doriax::editor::Theme::Colors::SubtleText;
ImVec4 doriax::editor::Theme::Colors::filenameLabel;
ImVec4 doriax::editor::Theme::Colors::ExtEntityButton;
ImVec4 doriax::editor::Theme::Colors::ExtEntityButtonHovered;
ImVec4 doriax::editor::Theme::Colors::ExtEntityButtonActive;
ImVec4 doriax::editor::Theme::Colors::NestedHeader;
ImVec4 doriax::editor::Theme::Colors::NestedHeaderHovered;
ImVec4 doriax::editor::Theme::Colors::NestedHeaderActive;
ImVec4 doriax::editor::Theme::Colors::DisabledGreenText;
ImVec4 doriax::editor::Theme::Colors::ChildSceneText;
ImVec4 doriax::editor::Theme::Colors::SubSelectionText;
ImVec4 doriax::editor::Theme::Colors::WarningText;

void doriax::editor::Theme::setPreset(Preset preset) {
    g_preset = preset;
}

doriax::editor::Theme::Preset doriax::editor::Theme::getPreset() {
    return g_preset;
}

bool doriax::editor::Theme::setAccentColor(const char* hex) {
    if (!hex) {
        return false;
    }
    const std::string candidate(hex);
    ImVec4 probe;
    const std::string previous = g_accentHex;
    g_accentHex = candidate;
    if (!parseAccent(probe)) {
        g_accentHex = previous;
        return false;
    }
    return true;
}

const char* doriax::editor::Theme::getAccentColor() {
    return g_accentHex.c_str();
}

void doriax::editor::Theme::apply() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    Tokens t = (g_preset == Preset::Dark) ? darkTokens() : lightTokens();
    if (!parseAccent(t.accent)) {
        t.accent = ImVec4(0.24f, 0.36f, 0.51f, 1.00f);
    }
    const ImVec4 black(0.0f, 0.0f, 0.0f, 1.00f);
    const ImVec4 white(1.0f, 1.0f, 1.0f, 1.00f);
    const bool dark = g_preset == Preset::Dark;
    t.accentBright = ImLerp(t.accent, dark ? white : black, 0.25f);
    t.button = ImLerp(t.bgSurface, t.accent, dark ? 0.55f : 0.35f);
    t.buttonHovered = ImLerp(t.button, dark ? white : black, 0.12f);
    t.buttonActive = ImLerp(t.button, dark ? white : black, -0.12f);
    t.scrollbar = ImLerp(t.bgElevated, t.text, 0.25f);
    t.plot = ImLerp(t.text, t.bgSurface, 0.35f);
    t.selection = ImVec4(t.accent.x, t.accent.y, t.accent.z, dark ? 0.45f : 0.30f);
    t.warning = ImVec4(1.00f, dark ? 0.70f : 0.45f, 0.20f, 1.00f);

    colors[ImGuiCol_Text]                       = t.text;
    colors[ImGuiCol_TextDisabled]               = t.textDisabled;
    colors[ImGuiCol_WindowBg]                   = t.bgBase;
    colors[ImGuiCol_ChildBg]                    = t.bgBase;
    colors[ImGuiCol_PopupBg]                    = t.bgSurface;
    colors[ImGuiCol_Border]                     = t.border;
    colors[ImGuiCol_BorderShadow]               = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]                    = t.bgDeep;
    colors[ImGuiCol_FrameBgHovered]             = ImLerp(t.bgDeep, t.bgElevated, 0.6f);
    colors[ImGuiCol_FrameBgActive]              = t.bgElevated;
    colors[ImGuiCol_TitleBg]                    = t.bgBase;
    colors[ImGuiCol_TitleBgActive]              = t.bgSurface;
    colors[ImGuiCol_TitleBgCollapsed]           = t.bgBase;
    colors[ImGuiCol_MenuBarBg]                  = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_ScrollbarBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_ScrollbarGrab]              = t.scrollbar;
    colors[ImGuiCol_ScrollbarGrabHovered]       = ImLerp(t.scrollbar, t.text, 0.25f);
    colors[ImGuiCol_ScrollbarGrabActive]        = ImLerp(t.scrollbar, t.text, 0.45f);
    colors[ImGuiCol_CheckMark]                  = t.accentBright;
    colors[ImGuiCol_SliderGrab]                 = t.accent;
    colors[ImGuiCol_SliderGrabActive]           = t.accentBright;
    colors[ImGuiCol_Button]                     = t.button;
    colors[ImGuiCol_ButtonHovered]              = t.buttonHovered;
    colors[ImGuiCol_ButtonActive]               = t.buttonActive;
    colors[ImGuiCol_Header]                     = ImLerp(t.bgSurface, t.accent, 0.25f);
    colors[ImGuiCol_HeaderHovered]              = ImLerp(t.bgSurface, t.accent, 0.45f);
    colors[ImGuiCol_HeaderActive]               = ImLerp(t.bgSurface, t.accent, 0.60f);
    colors[ImGuiCol_Separator]                  = ImLerp(t.border, t.text, 0.15f);
    colors[ImGuiCol_SeparatorHovered]           = t.accent;
    colors[ImGuiCol_SeparatorActive]            = t.accentBright;
    colors[ImGuiCol_ResizeGrip]                 = ImLerp(t.border, t.text, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]          = t.accent;
    colors[ImGuiCol_ResizeGripActive]           = t.accentBright;
    colors[ImGuiCol_TabHovered]                 = ImLerp(t.bgSurface, t.accent, 0.35f);
    colors[ImGuiCol_Tab]                        = t.bgSurface;
    colors[ImGuiCol_TabSelected]                = ImLerp(t.bgSurface, t.accent, 0.30f);
    colors[ImGuiCol_TabSelectedOverline]        = t.accent;
    colors[ImGuiCol_TabDimmed]                  = t.bgSurface;
    colors[ImGuiCol_TabDimmedSelected]          = ImLerp(t.bgSurface, t.accent, 0.15f);
    colors[ImGuiCol_TabDimmedSelectedOverline]  = ImLerp(t.accent, t.text, 0.20f);
    colors[ImGuiCol_DockingPreview]             = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.78f);
    colors[ImGuiCol_DockingEmptyBg]             = t.bgDeep;
    colors[ImGuiCol_PlotLines]                  = t.plot;
    colors[ImGuiCol_PlotLinesHovered]           = t.warning;
    colors[ImGuiCol_PlotHistogram]              = ImLerp(t.accent, t.bgSurface, 0.25f);
    colors[ImGuiCol_PlotHistogramHovered]       = t.accentBright;
    colors[ImGuiCol_TableHeaderBg]              = t.bgSurface;
    colors[ImGuiCol_TableBorderStrong]          = ImLerp(t.border, t.text, 0.20f);
    colors[ImGuiCol_TableBorderLight]           = ImLerp(t.border, t.text, 0.08f);
    colors[ImGuiCol_TableRowBg]                 = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]              = ImVec4(dark ? 1.00f : 0.00f, dark ? 1.00f : 0.00f, dark ? 1.00f : 0.00f, 0.04f);
    colors[ImGuiCol_TextLink]                   = t.accentBright;
    colors[ImGuiCol_TextSelectedBg]             = t.selection;
    colors[ImGuiCol_DragDropTarget]             = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight]               = t.accentBright;
    colors[ImGuiCol_NavWindowingHighlight]      = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]          = dark ? ImVec4(0.00f, 0.00f, 0.00f, 0.40f) : ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]           = dark ? ImVec4(0.00f, 0.00f, 0.00f, 0.50f) : ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
    colors[ImGuiCol_CheckboxSelectedBg]         = colors[ImGuiCol_FrameBg];

    Colors::ButtonActivated = ImLerp(colors[ImGuiCol_Button], colors[ImGuiCol_NavHighlight], 0.4f);
    Colors::FileCardBackground = ImLerp(colors[ImGuiCol_WindowBg], colors[ImGuiCol_FrameBg], 0.5f);
    Colors::FileCardBackgroundHovered = ImLerp(colors[ImGuiCol_WindowBg], colors[ImGuiCol_HeaderHovered], 0.25f);
    Colors::SubtleText = ImLerp(colors[ImGuiCol_Text], colors[ImGuiCol_NavHighlight], 0.4f);
    Colors::filenameLabel = dark ? ImVec4(50.0f/255.0f, 50.0f/255.0f, 50.0f/255.0f, 1.0f)
                                 : ImVec4(120.0f/255.0f, 120.0f/255.0f, 125.0f/255.0f, 1.0f);
    Colors::ExtEntityButton = ImVec4(colors[ImGuiCol_Button].x * 0.85f, colors[ImGuiCol_Button].y * 0.9f, colors[ImGuiCol_Button].z * 1.15f, colors[ImGuiCol_Button].w);
    Colors::ExtEntityButtonHovered = ImVec4(colors[ImGuiCol_ButtonHovered].x * 0.85f, colors[ImGuiCol_ButtonHovered].y * 0.9f, colors[ImGuiCol_ButtonHovered].z * 1.15f, colors[ImGuiCol_ButtonHovered].w);
    Colors::ExtEntityButtonActive = ImVec4(colors[ImGuiCol_ButtonActive].x * 0.85f, colors[ImGuiCol_ButtonActive].y * 0.9f, colors[ImGuiCol_ButtonActive].z * 1.15f, colors[ImGuiCol_ButtonActive].w);
    Colors::NestedHeader = ImLerp(t.bgSurface, t.accent, 0.12f);
    Colors::NestedHeaderHovered = ImLerp(t.bgElevated, t.accent, 0.18f);
    Colors::NestedHeaderActive = ImLerp(t.bgElevated, t.accent, 0.30f);
    Colors::DisabledGreenText = ImLerp(colors[ImGuiCol_TextDisabled],
                                       ImVec4(0.38f, 0.72f, 0.46f, 1.00f),
                                       0.58f);
    Colors::ChildSceneText = dark ? ImVec4(0.55f, 0.80f, 0.85f, 1.0f)
                                  : ImVec4(0.15f, 0.45f, 0.52f, 1.0f);
    Colors::SubSelectionText = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    Colors::WarningText = ImVec4(1.00f, 0.85f, 0.40f, 1.00f);

    // main
    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(10, 4);
    style.ItemSpacing = ImVec2(8, 8);
    style.ItemInnerSpacing = ImVec2(8, 8);
    style.TouchExtraPadding = ImVec2(0, 0);
    style.IndentSpacing = 16;
    style.ScrollbarSize = 12;

    // borders
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.TabBorderSize = 0;
    style.TabBarBorderSize = 1;
    style.TabBarOverlineSize = 2;

    // rounding
    style.WindowRounding = 4;
    style.ChildRounding = 2;
    style.FrameRounding = 4;
    style.PopupRounding = 5;
    style.ScrollbarRounding = 6;
    style.GrabRounding = 4;
    style.TabRounding = 4;

    // tables
    style.CellPadding = ImVec2(8, 4);
    style.TableAngledHeadersAngle = 0.61; //35 deg
    style.TableAngledHeadersTextAlign = ImVec2(0.50, 0.00);

    // widgets
    style.WindowTitleAlign = ImVec2(0.00, 0.50);
    style.WindowMenuButtonPosition = ImGuiDir::ImGuiDir_Right;
    style.ColorButtonPosition = ImGuiDir::ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.50, 0.50);
    style.SelectableTextAlign = ImVec2(0.00, 0.00);
    style.SeparatorTextBorderSize = 1;
    style.SeparatorTextAlign = ImVec2(0.00, 0.50);
    style.SeparatorTextPadding = ImVec2(16, 0);
    style.LogSliderDeadzone = 4;

    // docking
    style.DockingSeparatorSize = 6;

    g_baseStyle = style;
    g_hasBaseStyle = true;
    g_appliedDpiScale = 0.0f;
}

void doriax::editor::Theme::applyDpiScale(float dpiScale) {
    if (!g_hasBaseStyle) {
        return;
    }
    if (dpiScale < 0.5f || dpiScale > 8.0f) {
        dpiScale = 1.0f;
    }
    if (g_appliedDpiScale == dpiScale) {
        return;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    const float fontScaleDpi = style.FontScaleDpi;
    style = g_baseStyle;
    style.ScaleAllSizes(dpiScale);
    // ConfigDpiScaleFonts overwrites FontScaleDpi per window; keep the current
    // value so this reset does not briefly unscale fonts.
    style.FontScaleDpi = fontScaleDpi;
    g_appliedDpiScale = dpiScale;
}

float doriax::editor::Theme::dpiScale() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx && ctx->CurrentWindow && ctx->CurrentWindow->Viewport && ctx->CurrentWindow->Viewport->DpiScale > 0.0f) {
        return ctx->CurrentWindow->Viewport->DpiScale;
    }
    return (g_appliedDpiScale > 0.0f) ? g_appliedDpiScale : 1.0f;
}

float doriax::editor::Theme::dpi(float value) {
    return value * dpiScale();
}

ImVec2 doriax::editor::Theme::dpi(const ImVec2& value) {
    const float scale = dpiScale();
    return ImVec2(value.x * scale, value.y * scale);
}
