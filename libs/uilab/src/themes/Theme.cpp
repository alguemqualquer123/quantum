#include "themes/Theme.h"

#include "core/Serialize.h"
#include "core/Types.h"

#include <algorithm>
#include <imgui.h>

namespace uilab {

const char* ThemeModeName(ThemeMode m) {
    switch (m) {
    case ThemeMode::Dark:   return "Dark";
    case ThemeMode::Light:  return "Light";
    case ThemeMode::Custom: return "Custom";
    default:                return "?";
    }
}

namespace {
const ThemeManager::ColorSlot kSlots[] = {
    { "Accent", &ThemePalette::accent },
    { "Accent Hovered", &ThemePalette::accentHover },
    { "Accent Active", &ThemePalette::accentActive },
    { "Window Background", &ThemePalette::windowBg },
    { "Child Background", &ThemePalette::childBg },
    { "Popup Background", &ThemePalette::popupBg },
    { "Border", &ThemePalette::border },
    { "Separator", &ThemePalette::separator },
    { "Text", &ThemePalette::text },
    { "Text Disabled", &ThemePalette::textDisabled },
    { "Header", &ThemePalette::header },
    { "Header Hovered", &ThemePalette::headerHover },
    { "Header Active", &ThemePalette::headerActive },
    { "Tab", &ThemePalette::tab },
    { "Tab Hovered", &ThemePalette::tabHover },
    { "Tab Active", &ThemePalette::tabActive },
    { "Frame Background", &ThemePalette::frameBg },
    { "Frame Hovered", &ThemePalette::frameHover },
    { "Frame Active", &ThemePalette::frameActive },
    { "Button", &ThemePalette::button },
    { "Button Hovered", &ThemePalette::buttonHover },
    { "Button Active", &ThemePalette::buttonActive },
    { "Scrollbar", &ThemePalette::scrollbar },
    { "Plot Lines", &ThemePalette::plotLines },
    { "Plot Histogram", &ThemePalette::plotHistogram },
    { "Success", &ThemePalette::success },
    { "Warning", &ThemePalette::warning },
    { "Error", &ThemePalette::error },
    { "Info", &ThemePalette::info },
    { "Grid Line", &ThemePalette::gridLine },
    { "Canvas Background", &ThemePalette::canvasBg },
    { "Selection", &ThemePalette::selection },
    { "Progress Bar", &ThemePalette::progressBar },
};

// One palette per mode. The custom palette starts as a copy of the dark one.
ThemePalette MakeDarkPalette() { return ThemePalette(); }

ThemePalette MakeLightPalette() {
    ThemePalette p;
    p.accent = ImVec4(0.13f, 0.43f, 0.85f, 1.00f);
    p.accentHover = ImVec4(0.18f, 0.50f, 0.92f, 1.00f);
    p.accentActive = ImVec4(0.10f, 0.36f, 0.74f, 1.00f);
    p.windowBg = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
    p.childBg = ImVec4(0.965f, 0.970f, 0.980f, 1.00f);
    p.popupBg = ImVec4(0.99f, 0.99f, 1.00f, 0.99f);
    p.border = ImVec4(0.76f, 0.78f, 0.82f, 1.00f);
    p.separator = ImVec4(0.78f, 0.80f, 0.84f, 1.00f);
    p.text = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    p.textDisabled = ImVec4(0.48f, 0.50f, 0.54f, 1.00f);
    p.header = ImVec4(0.82f, 0.85f, 0.90f, 1.00f);
    p.headerHover = ImVec4(0.76f, 0.82f, 0.92f, 1.00f);
    p.headerActive = ImVec4(0.68f, 0.76f, 0.90f, 1.00f);
    p.tab = ImVec4(0.87f, 0.89f, 0.92f, 1.00f);
    p.tabHover = ImVec4(0.80f, 0.85f, 0.92f, 1.00f);
    p.tabActive = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    p.frameBg = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    p.frameHover = ImVec4(0.96f, 0.97f, 0.99f, 1.00f);
    p.frameActive = ImVec4(0.92f, 0.94f, 0.98f, 1.00f);
    p.button = ImVec4(0.86f, 0.88f, 0.92f, 1.00f);
    p.buttonHover = ImVec4(0.78f, 0.84f, 0.94f, 1.00f);
    p.buttonActive = ImVec4(0.68f, 0.78f, 0.94f, 1.00f);
    p.scrollbar = ImVec4(0.72f, 0.75f, 0.80f, 1.00f);
    p.plotLines = ImVec4(0.32f, 0.40f, 0.55f, 1.00f);
    p.plotHistogram = ImVec4(0.16f, 0.47f, 0.85f, 1.00f);
    p.success = ImVec4(0.13f, 0.60f, 0.24f, 1.00f);
    p.warning = ImVec4(0.78f, 0.52f, 0.05f, 1.00f);
    p.error = ImVec4(0.78f, 0.20f, 0.16f, 1.00f);
    p.info = ImVec4(0.12f, 0.45f, 0.78f, 1.00f);
    p.gridLine = ImVec4(0.60f, 0.64f, 0.70f, 0.55f);
    p.canvasBg = ImVec4(0.85f, 0.87f, 0.90f, 1.00f);
    p.selection = ImVec4(0.95f, 0.55f, 0.10f, 1.00f);
    p.progressBar = ImVec4(0.13f, 0.60f, 0.24f, 1.00f);
    return p;
}

} // namespace

ThemeManager::ThemeManager() {
    darkPalette = MakeDarkPalette();
    lightPalette = MakeLightPalette();
    custom = darkPalette;
    dirty_ = true;
}

const ThemePalette& ThemeManager::Active() const {
    switch (mode) {
    case ThemeMode::Light:  return lightPalette;
    case ThemeMode::Custom: return custom;
    case ThemeMode::Dark:
    default:                return darkPalette;
    }
}

void ThemeManager::SetMode(ThemeMode m) {
    if (mode == m)
        return;
    mode = m;
    dirty_ = true;
}

void ThemeManager::ResetCustom() {
    custom = darkPalette;
    dirty_ = true;
}

void ThemeManager::ResetStyleToDefault() {
    style = ThemeStyle();
    // Keep the current font size, it is a user preference rather than a metric.
    dirty_ = true;
}

void ThemeManager::LoadBuiltIn(ThemeMode m) {
    if (m == ThemeMode::Light)
        lightPalette = MakeLightPalette();
    else if (m == ThemeMode::Dark)
        darkPalette = MakeDarkPalette();
    else
        ResetCustom();
    dirty_ = true;
}

void ThemeManager::Apply() {
    ImGuiStyle& s = ImGui::GetStyle();
    const ThemePalette& p = Active();

    // ---- metrics -------------------------------------------------------
    const float d = std::clamp(style.densityScale, 0.6f, 1.6f);
    s.WindowRounding = style.windowRounding;
    s.ChildRounding = style.childRounding;
    s.FrameRounding = style.frameRounding;
    s.PopupRounding = style.popupRounding;
    s.GrabRounding = style.grabRounding;
    s.TabRounding = style.tabRounding;
    s.ScrollbarRounding = style.scrollbarRounding;
    s.FrameBorderSize = style.frameBorderSize;
    s.WindowBorderSize = style.windowBorderSize;
    s.PopupBorderSize = style.popupBorderSize;
    s.WindowPadding = ImVec2(style.windowPadding.x * d, style.windowPadding.y * d);
    s.FramePadding = ImVec2(style.framePadding.x * d, style.framePadding.y * d);
    s.ItemSpacing = ImVec2(style.itemSpacing.x * d, style.itemSpacing.y * d);
    s.ItemInnerSpacing = ImVec2(style.itemInnerSpacing.x * d, style.itemInnerSpacing.y * d);
    s.CellPadding = ImVec2(style.cellPadding.x * d, style.cellPadding.y * d);
    s.IndentSpacing = style.indentSpacing * d;
    s.ScrollbarSize = style.scrollbarSize * d;
    s.GrabMinSize = style.grabMinSize * d;
    s.TabBorderSize = style.tabBarBorderSize;
    s.TabBarBorderSize = style.tabBarBorderSize;
    s.TabBarOverlineSize = 2.0f;
    s.DisabledAlpha = style.disabledAlpha;
    s.WindowTitleAlign = style.windowTitleAlign;
    s.ButtonTextAlign = style.buttonTextAlign;
    s.SelectableTextAlign = style.selectableTextAlign;
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextPadding = ImVec2(20.0f, 3.0f);
    s.AntiAliasedLines = true;
    s.AntiAliasedLinesUseTex = true;
    s.AntiAliasedFill = true;
    s.FontSizeBase = style.fontSizeBase;

    // ---- colours -------------------------------------------------------
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.textDisabled;
    c[ImGuiCol_WindowBg] = p.windowBg;
    c[ImGuiCol_ChildBg] = p.childBg;
    c[ImGuiCol_PopupBg] = p.popupBg;
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = p.frameBg;
    c[ImGuiCol_FrameBgHovered] = p.frameHover;
    c[ImGuiCol_FrameBgActive] = p.frameActive;
    c[ImGuiCol_TitleBg] = p.header;
    c[ImGuiCol_TitleBgActive] = p.headerActive;
    c[ImGuiCol_TitleBgCollapsed] = p.header;
    c[ImGuiCol_MenuBarBg] = p.childBg;
    c[ImGuiCol_ScrollbarBg] = p.frameBg;
    c[ImGuiCol_ScrollbarGrab] = p.scrollbar;
    c[ImGuiCol_ScrollbarGrabHovered] = p.frameHover;
    c[ImGuiCol_ScrollbarGrabActive] = p.accent;
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = p.accentActive;
    c[ImGuiCol_Button] = p.button;
    c[ImGuiCol_ButtonHovered] = p.buttonHover;
    c[ImGuiCol_ButtonActive] = p.buttonActive;
    c[ImGuiCol_Header] = p.header;
    c[ImGuiCol_HeaderHovered] = p.headerHover;
    c[ImGuiCol_HeaderActive] = p.headerActive;
    c[ImGuiCol_Separator] = p.separator;
    c[ImGuiCol_SeparatorHovered] = p.accentHover;
    c[ImGuiCol_SeparatorActive] = p.accent;
    c[ImGuiCol_ResizeGrip] = p.scrollbar;
    c[ImGuiCol_ResizeGripHovered] = p.accentHover;
    c[ImGuiCol_ResizeGripActive] = p.accent;
    c[ImGuiCol_Tab] = p.tab;
    c[ImGuiCol_TabHovered] = p.tabHover;
    c[ImGuiCol_TabSelected] = p.tabActive;
    c[ImGuiCol_TabSelectedOverline] = p.accent;
    c[ImGuiCol_TabDimmed] = p.tab;
    c[ImGuiCol_TabDimmedSelected] = p.header;
    c[ImGuiCol_TabDimmedSelectedOverline] = p.border;
    c[ImGuiCol_DockingPreview] = WithAlpha(p.accent, 0.55f);
    c[ImGuiCol_DockingEmptyBg] = p.canvasBg;
    c[ImGuiCol_PlotLines] = p.plotLines;
    c[ImGuiCol_PlotLinesHovered] = p.accentHover;
    c[ImGuiCol_PlotHistogram] = p.plotHistogram;
    c[ImGuiCol_PlotHistogramHovered] = p.accentHover;
    c[ImGuiCol_TableHeaderBg] = p.header;
    c[ImGuiCol_TableBorderStrong] = p.border;
    c[ImGuiCol_TableBorderLight] = WithAlpha(p.border, 0.5f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = WithAlpha(p.text, 0.03f);
    c[ImGuiCol_TextLink] = p.accent;
    c[ImGuiCol_TextSelectedBg] = WithAlpha(p.accent, 0.38f);
    c[ImGuiCol_TreeLines] = p.separator;
    c[ImGuiCol_DragDropTarget] = p.selection;
    c[ImGuiCol_DragDropTargetBg] = WithAlpha(p.selection, 0.15f);
    c[ImGuiCol_UnsavedMarker] = p.warning;
    c[ImGuiCol_NavCursor] = p.accent;
    c[ImGuiCol_NavWindowingHighlight] = p.accentHover;
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
    c[ImGuiCol_CheckboxSelectedBg] = p.accent;
    c[ImGuiCol_InputTextCursor] = p.accent;

    ++applyCount_;
    dirty_ = false;
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------
std::string ThemeManager::Serialize() const {
    KeyValueDoc doc;
    doc.Set("theme.mode", ThemeModeName(mode));
    doc.Set("theme.style.density", std::to_string(style.densityScale));
    doc.SetFloat("theme.style.fontSizeBase", style.fontSizeBase);
    doc.SetFloat("theme.style.windowRounding", style.windowRounding);
    doc.SetFloat("theme.style.childRounding", style.childRounding);
    doc.SetFloat("theme.style.frameRounding", style.frameRounding);
    doc.SetFloat("theme.style.popupRounding", style.popupRounding);
    doc.SetFloat("theme.style.grabRounding", style.grabRounding);
    doc.SetFloat("theme.style.tabRounding", style.tabRounding);
    doc.SetFloat("theme.style.scrollbarRounding", style.scrollbarRounding);
    doc.SetVec2("theme.style.framePadding", style.framePadding);
    doc.SetVec2("theme.style.itemSpacing", style.itemSpacing);
    doc.SetVec2("theme.style.windowPadding", style.windowPadding);
    doc.SetFloat("theme.style.disabledAlpha", style.disabledAlpha);

    int count = 0;
    const ColorSlot* slots = ColorSlots(count);
    for (int i = 0; i < count; ++i)
        doc.SetVec4(std::string("color.") + slots[i].name, custom.*(slots[i].member));

    return doc.ToText("Universal UI Components Lab - theme file");
}

bool ThemeManager::Deserialize(const std::string& text, std::string& errorOut) {
    KeyValueDoc doc;
    if (!doc.Parse(text, errorOut))
        return false;

    const std::string modeName = doc.Get("theme.mode", "Custom");
    if (modeName == "Dark")
        mode = ThemeMode::Dark;
    else if (modeName == "Light")
        mode = ThemeMode::Light;
    else
        mode = ThemeMode::Custom;

    style.densityScale = doc.GetFloat("theme.style.density", style.densityScale);
    style.fontSizeBase = doc.GetFloat("theme.style.fontSizeBase", style.fontSizeBase);
    style.windowRounding = doc.GetFloat("theme.style.windowRounding", style.windowRounding);
    style.childRounding = doc.GetFloat("theme.style.childRounding", style.childRounding);
    style.frameRounding = doc.GetFloat("theme.style.frameRounding", style.frameRounding);
    style.popupRounding = doc.GetFloat("theme.style.popupRounding", style.popupRounding);
    style.grabRounding = doc.GetFloat("theme.style.grabRounding", style.grabRounding);
    style.tabRounding = doc.GetFloat("theme.style.tabRounding", style.tabRounding);
    style.scrollbarRounding = doc.GetFloat("theme.style.scrollbarRounding", style.scrollbarRounding);
    style.framePadding = doc.GetVec2("theme.style.framePadding", style.framePadding);
    style.itemSpacing = doc.GetVec2("theme.style.itemSpacing", style.itemSpacing);
    style.windowPadding = doc.GetVec2("theme.style.windowPadding", style.windowPadding);
    style.disabledAlpha = doc.GetFloat("theme.style.disabledAlpha", style.disabledAlpha);

    ThemePalette& target = (mode == ThemeMode::Light) ? lightPalette
                        : (mode == ThemeMode::Dark) ? darkPalette
                                                    : custom;

    int count = 0;
    const ColorSlot* slots = ColorSlots(count);
    for (int i = 0; i < count; ++i) {
        const std::string key = std::string("color.") + slots[i].name;
        if (doc.Has(key))
            target.*(slots[i].member) = doc.GetVec4(key, target.*(slots[i].member));
    }

    style.densityScale = std::clamp(style.densityScale, 0.6f, 1.6f);
    style.fontSizeBase = std::clamp(style.fontSizeBase, 9.0f, 32.0f);
    if (mode == ThemeMode::Custom && target.windowBg.w <= 0.0f)
        custom = darkPalette;

    dirty_ = true;
    errorOut.clear();
    return true;
}

bool ThemeManager::SaveToFile(const std::string& path, std::string& errorOut) const {
    return WriteTextFile(path, Serialize(), errorOut);
}

bool ThemeManager::LoadFromFile(const std::string& path, std::string& errorOut) {
    std::string text;
    if (!ReadTextFile(path, text, errorOut))
        return false;
    return Deserialize(text, errorOut);
}

const ThemeManager::ColorSlot* ThemeManager::ColorSlots(int& countOut) {
    countOut = static_cast<int>(sizeof(kSlots) / sizeof(kSlots[0]));
    return kSlots;
}

void ApplyFontScale(float scale) {
    ImGui::GetStyle().FontScaleMain = std::clamp(scale, 0.75f, 2.0f);
}

} // namespace uilab
