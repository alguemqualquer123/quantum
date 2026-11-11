// Theme.h - theme manager: Dark / Light / Custom
//
// A theme is a colour palette plus a set of style metrics. "Custom" starts as a
// copy of the dark palette and can be edited by the Theme Editor panel and
// persisted to a text file.
#pragma once

#include <string>

#include <imgui.h>

namespace uilab {

enum class ThemeMode : int { Dark = 0, Light, Custom, Count };

const char* ThemeModeName(ThemeMode m);

// Dark palette: near-black, neutral grey like the Unreal Editor (no blue tint).
struct ThemePalette {
    ImVec4 accent = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    ImVec4 accentHover = ImVec4(0.34f, 0.66f, 1.00f, 1.00f);
    ImVec4 accentActive = ImVec4(0.20f, 0.50f, 0.88f, 1.00f);
    ImVec4 windowBg = ImVec4(0.043f, 0.043f, 0.047f, 1.00f);
    ImVec4 childBg = ImVec4(0.055f, 0.055f, 0.059f, 1.00f);
    ImVec4 popupBg = ImVec4(0.075f, 0.075f, 0.082f, 0.98f);
    ImVec4 border = ImVec4(0.145f, 0.145f, 0.155f, 1.00f);
    ImVec4 separator = ImVec4(0.130f, 0.130f, 0.140f, 1.00f);
    ImVec4 text = ImVec4(0.850f, 0.850f, 0.870f, 1.00f);
    ImVec4 textDisabled = ImVec4(0.420f, 0.420f, 0.440f, 1.00f);
    ImVec4 header = ImVec4(0.115f, 0.115f, 0.125f, 1.00f);
    ImVec4 headerHover = ImVec4(0.165f, 0.165f, 0.180f, 1.00f);
    ImVec4 headerActive = ImVec4(0.205f, 0.230f, 0.290f, 1.00f);
    ImVec4 tab = ImVec4(0.062f, 0.062f, 0.068f, 1.00f);
    ImVec4 tabHover = ImVec4(0.155f, 0.155f, 0.170f, 1.00f);
    ImVec4 tabActive = ImVec4(0.115f, 0.130f, 0.165f, 1.00f);
    ImVec4 frameBg = ImVec4(0.090f, 0.090f, 0.098f, 1.00f);
    ImVec4 frameHover = ImVec4(0.145f, 0.145f, 0.160f, 1.00f);
    ImVec4 frameActive = ImVec4(0.185f, 0.205f, 0.255f, 1.00f);
    ImVec4 button = ImVec4(0.120f, 0.120f, 0.130f, 1.00f);
    ImVec4 buttonHover = ImVec4(0.175f, 0.195f, 0.245f, 1.00f);
    ImVec4 buttonActive = ImVec4(0.215f, 0.250f, 0.330f, 1.00f);
    ImVec4 scrollbar = ImVec4(0.155f, 0.155f, 0.165f, 1.00f);
    ImVec4 plotLines = ImVec4(0.60f, 0.68f, 0.82f, 1.00f);
    ImVec4 plotHistogram = ImVec4(0.35f, 0.66f, 0.98f, 1.00f);
    ImVec4 success = ImVec4(0.33f, 0.78f, 0.42f, 1.00f);
    ImVec4 warning = ImVec4(0.95f, 0.72f, 0.20f, 1.00f);
    ImVec4 error = ImVec4(0.92f, 0.36f, 0.32f, 1.00f);
    ImVec4 info = ImVec4(0.41f, 0.72f, 0.98f, 1.00f);
    ImVec4 gridLine = ImVec4(0.200f, 0.200f, 0.220f, 0.55f);
    ImVec4 canvasBg = ImVec4(0.025f, 0.025f, 0.028f, 1.00f);
    ImVec4 selection = ImVec4(0.95f, 0.62f, 0.24f, 1.00f);
    ImVec4 progressBar = ImVec4(0.33f, 0.78f, 0.42f, 1.00f);
};

struct ThemeStyle {
    float windowRounding = 6.0f;
    float childRounding = 5.0f;
    float frameRounding = 4.0f;
    float popupRounding = 5.0f;
    float grabRounding = 3.0f;
    float tabRounding = 4.0f;
    float scrollbarRounding = 8.0f;
    float frameBorderSize = 1.0f;
    float windowBorderSize = 1.0f;
    float popupBorderSize = 1.0f;
    ImVec2 windowPadding = ImVec2(10.0f, 10.0f);
    ImVec2 framePadding = ImVec2(9.0f, 5.0f);
    ImVec2 itemSpacing = ImVec2(8.0f, 6.0f);
    ImVec2 itemInnerSpacing = ImVec2(6.0f, 5.0f);
    ImVec2 cellPadding = ImVec2(6.0f, 4.0f);
    float indentSpacing = 18.0f;
    float scrollbarSize = 13.0f;
    float grabMinSize = 10.0f;
    float tabBarBorderSize = 1.0f;
    float tabsRoundingScale = 1.0f;
    float disabledAlpha = 0.55f;
    ImVec2 windowTitleAlign = ImVec2(0.0f, 0.5f);
    ImVec2 buttonTextAlign = ImVec2(0.5f, 0.5f);
    ImVec2 selectableTextAlign = ImVec2(0.0f, 0.5f);
    float fontSizeBase = 15.0f;
    float densityScale = 1.0f;   // multiplies paddings/spacings (compact vs comfortable)
};

class ThemeManager {
public:
    ThemeManager();

    ThemeMode mode = ThemeMode::Dark;
    ThemePalette custom;          // editable palette (used by ThemeMode::Custom)
    ThemePalette darkPalette;
    ThemePalette lightPalette;
    ThemeStyle style;

    void Apply();                  // writes the active theme into ImGuiStyle/font atlas
    void MarkDirty() { dirty_ = true; }
    bool IsDirty() const { return dirty_; }
    void SetMode(ThemeMode m);

    const ThemePalette& Active() const;
    const char* ActiveName() const { return ThemeModeName(mode); }

    void ResetCustom();                       // copy the dark palette
    void ResetStyleToDefault();
    void LoadBuiltIn(ThemeMode mode);

    std::string Serialize() const;
    bool Deserialize(const std::string& text, std::string& errorOut);
    bool SaveToFile(const std::string& path, std::string& errorOut) const;
    bool LoadFromFile(const std::string& path, std::string& errorOut);

    static const char* DefaultPath() { return "assets/themes/uicomp.ui-theme"; }

    // Editor helpers used by the Theme Editor panel to enumerate colours.
    struct ColorSlot {
        const char* name;
        ImVec4 ThemePalette::* member;
    };
    static const ColorSlot* ColorSlots(int& countOut);

    int ApplyCount() const { return applyCount_; }

private:
    bool dirty_ = true;
    int applyCount_ = 0;
};

// Applies only the font size (used by the font scale slider without touching colours).
void ApplyFontScale(float scale);

} // namespace uilab
