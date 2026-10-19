// ThemeEditorPanel.cpp - palette / style metric editor with actions and a live preview card.
#include "panels/Panels.h"

#include <cstdio>
#include <string>

#include <imgui.h>

#include "core/AppContext.h"
#include "core/Serialize.h"
#include "themes/Theme.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace panels {

namespace {

// Panel-local state: slot filter text and preview demo values.
std::string s_colorQuery;
bool s_previewCheckbox = true;
float s_previewProgress = 0.62f;

// The colour slots always edit ctx.theme.custom, so switch to Custom on the first edit.
void EnsureCustomMode(AppContext& ctx) {
    if (ctx.theme.mode == ThemeMode::Custom)
        return;
    ctx.theme.SetMode(ThemeMode::Custom);
    ctx.LogEvent("Theme Editor", "Palette switched to Custom (a colour slot was edited)");
}

void StyleEdited(AppContext& ctx, const char* name) {
    ctx.theme.MarkDirty();
    ctx.LogDebug("Theme Editor", std::string("Adjusted style: ") + name);
}

void DrawModeSection(AppContext& ctx) {
    if (!widgets::SectionHeader("Mode", icons::Icon::Sun))
        return;

    for (int i = 0; i < static_cast<int>(ThemeMode::Count); ++i) {
        if (i > 0)
            ImGui::SameLine();
        const ThemeMode m = static_cast<ThemeMode>(i);
        if (ImGui::RadioButton(ThemeModeName(m), static_cast<int>(ctx.theme.mode) == i) && ctx.theme.mode != m) {
            ctx.theme.SetMode(m);
            ctx.LogEvent("Theme Editor", std::string("Switched to ") + ThemeModeName(m) + " theme");
            ctx.Notify(ToastKind::Info, "Theme mode", ThemeModeName(m));
        }
    }
    ImGui::TextDisabled("Active: %s | applied %d time(s)", ctx.theme.ActiveName(), ctx.theme.ApplyCount());
}

void DrawPaletteSection(AppContext& ctx) {
    if (!widgets::SectionHeader("Colours", icons::Icon::Palette))
        return;

    if (ctx.theme.mode != ThemeMode::Custom)
        widgets::TextMuted("Slots edit the Custom palette; the first edit switches to Custom automatically.");
    widgets::SearchField("##slot_filter", s_colorQuery, "Filter colour slots...", 240.0f);

    int slotCount = 0;
    const ThemeManager::ColorSlot* slots = ThemeManager::ColorSlots(slotCount);

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;
    if (ImGui::BeginTable("##color_slots", 2, flags, ImVec2(0.0f, 300.0f))) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (int i = 0; i < slotCount; ++i) {
            const ThemeManager::ColorSlot& slot = slots[i];
            if (!s_colorQuery.empty() && !ContainsNoCase(slot.name, s_colorQuery))
                continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(i);
            ImGui::TextUnformatted(slot.name);
            ImGui::TableSetColumnIndex(1);
            ImVec4 col = ctx.theme.custom.*(slot.member);
            if (ImGui::ColorEdit4("##color", &col.x,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                                      ImGuiColorEditFlags_AlphaBar)) {
                EnsureCustomMode(ctx);
                ctx.theme.custom.*(slot.member) = col;
                ctx.theme.MarkDirty();
                ctx.LogDebug("Theme Editor", std::string("Adjusted colour: ") + slot.name);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    ctx.Notify(ToastKind::Info, "Custom palette updated", slot.name);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DrawMetricsSection(AppContext& ctx) {
    if (!widgets::SectionHeader("Style metrics", icons::Icon::Sliders))
        return;

    ThemeStyle& st = ctx.theme.style;

    struct DragRow {
        const char* name;
        float* value;
        float speed;
        float vMin;
        float vMax;
        const char* fmt;
    };
    const DragRow rows[] = {
        { "Window rounding", &st.windowRounding, 0.2f, 0.0f, 24.0f, "%.1f" },
        { "Frame rounding", &st.frameRounding, 0.2f, 0.0f, 24.0f, "%.1f" },
        { "Grab rounding", &st.grabRounding, 0.2f, 0.0f, 24.0f, "%.1f" },
        { "Tab rounding", &st.tabRounding, 0.2f, 0.0f, 24.0f, "%.1f" },
        { "Scrollbar size", &st.scrollbarSize, 0.2f, 6.0f, 32.0f, "%.1f" },
        { "Indent spacing", &st.indentSpacing, 0.4f, 4.0f, 48.0f, "%.1f" },
        { "Disabled alpha", &st.disabledAlpha, 0.01f, 0.0f, 1.0f, "%.2f" },
        { "Density scale", &st.densityScale, 0.01f, 0.6f, 1.6f, "%.2f" },
    };
    // The row labels are unique strings, so they can double as the widget ids.
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        const DragRow& row = rows[i];
        if (ImGui::DragFloat(row.name, row.value, row.speed, row.vMin, row.vMax, row.fmt))
            StyleEdited(ctx, row.name);
        if (ImGui::IsItemDeactivatedAfterEdit())
            ctx.Notify(ToastKind::Info, "Style updated", row.name);
    }

    struct DragPair {
        const char* name;
        float* value;
    };
    const DragPair pairs[] = {
        { "Item spacing", &st.itemSpacing.x },     { "Frame padding", &st.framePadding.x },
        { "Window padding", &st.windowPadding.x }, { "Cell padding", &st.cellPadding.x },
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
        const DragPair& pair = pairs[i];
        if (ImGui::DragFloat2(pair.name, pair.value, 0.2f))
            StyleEdited(ctx, pair.name);
        if (ImGui::IsItemDeactivatedAfterEdit())
            ctx.Notify(ToastKind::Info, "Style updated", pair.name);
    }

    if (ImGui::DragFloat("Font size##fs", &st.fontSizeBase, 0.1f, 9.0f, 32.0f, "%.1f")) {
        // Apply the scale right away (relative to the default base size) so the
        // edit is visible even before the theme manager re-applies everything.
        const float defaultBase = ThemeStyle().fontSizeBase;
        ApplyFontScale(st.fontSizeBase / defaultBase);
        StyleEdited(ctx, "Font size");
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        ctx.Notify(ToastKind::Info, "Style updated", "Font size");
}

void DrawActionsSection(AppContext& ctx) {
    if (!widgets::SectionHeader("Actions", icons::Icon::Gear))
        return;

    widgets::TextMuted("Theme file: %s", ctx.settings.themePath.c_str());
    if (ImGui::Button("Reset Style")) {
        ctx.theme.ResetStyleToDefault();
        ctx.theme.MarkDirty();
        ctx.LogConsole(LogLevel::Info, "Theme Editor", "Style metrics reset to defaults.");
        ctx.Notify(ToastKind::Info, "Style reset", "All metrics are back to their defaults.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Custom Palette")) {
        ctx.theme.ResetCustom();
        ctx.theme.MarkDirty();
        ctx.LogConsole(LogLevel::Info, "Theme Editor", "Custom palette reset (copied from the dark palette).");
        ctx.Notify(ToastKind::Info, "Palette reset", "The custom palette now matches Dark.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Dark -> Custom")) {
        ctx.theme.custom = ctx.theme.darkPalette;
        ctx.theme.SetMode(ThemeMode::Custom);
        ctx.theme.MarkDirty();
        ctx.LogEvent("Theme Editor", "Copied the dark palette into the custom palette");
        ctx.Notify(ToastKind::Success, "Palette copied", "Dark -> Custom.");
    }

    if (ImGui::Button("Save To File")) {
        std::string error;
        EnsureParentDirectory(ctx.settings.themePath, error);
        if (ctx.theme.SaveToFile(ctx.settings.themePath, error)) {
            ctx.LogConsole(LogLevel::Success, "Theme Editor", "Saved theme to " + ctx.settings.themePath);
            ctx.Notify(ToastKind::Success, "Theme saved", ctx.settings.themePath);
        } else {
            ctx.LogConsole(LogLevel::Error, "Theme Editor", "Theme save failed: " + error);
            ctx.Notify(ToastKind::Error, "Theme save failed", error);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load From File")) {
        std::string error;
        if (ctx.theme.LoadFromFile(ctx.settings.themePath, error)) {
            ctx.theme.MarkDirty();
            ctx.LogConsole(LogLevel::Success, "Theme Editor", "Loaded theme from " + ctx.settings.themePath);
            ctx.Notify(ToastKind::Success, "Theme loaded", ctx.settings.themePath);
        } else {
            ctx.LogConsole(LogLevel::Error, "Theme Editor", "Theme load failed: " + error);
            ctx.Notify(ToastKind::Error, "Theme load failed", error);
        }
    }
}

void DrawPreviewSection(AppContext& ctx) {
    if (!widgets::SectionHeader("Live preview", icons::Icon::Eye))
        return;

    const ThemePalette& p = ctx.theme.Active();
    widgets::BeginCard("theme_preview", ImVec2(0.0f, 150.0f), p.childBg);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, p.textDisabled);
    ImGui::PushStyleColor(ImGuiCol_Button, p.button);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.buttonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.buttonActive);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, p.frameBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, p.frameHover);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, p.accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, p.accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, p.accentActive);
    ImGui::PushStyleColor(ImGuiCol_Header, p.header);
    ImGui::PushStyleColor(ImGuiCol_Separator, p.separator);

    ImGui::TextUnformatted("The quick brown fox jumps over the lazy dog.");
    ImGui::Button("Button");
    ImGui::SameLine();
    ImGui::Checkbox("Checkbox", &s_previewCheckbox);
    ImGui::SameLine();
    ImGui::ColorButton("##chip", p.accent, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                       ImVec2(22.0f, 22.0f));
    ImGui::SliderFloat("##preview_value", &s_previewProgress, 0.0f, 1.0f, "%.0%%");
    widgets::ProgressBarEx("##preview_bar", s_previewProgress, ImVec2(0.0f, 0.0f), nullptr, p.progressBar);

    ImGui::PopStyleColor(12);
    widgets::EndCard();
}

} // namespace

void DrawThemeEditor(AppContext& ctx) {
    if (!ImGui::Begin(kThemeEditorTitle)) {
        ImGui::End();
        return;
    }

    DrawModeSection(ctx);
    DrawPaletteSection(ctx);
    DrawMetricsSection(ctx);
    DrawActionsSection(ctx);
    DrawPreviewSection(ctx);

    ImGui::End();
}

} // namespace panels
} // namespace uilab
