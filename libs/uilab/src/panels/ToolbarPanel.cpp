// ToolbarPanel.cpp - dockable tool strip under the menu bar.
#include "panels/Panels.h"

#include <cstdio>
#include <string>

#include <imgui.h>

#include "core/AppContext.h"
#include "core/Dialogs.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace panels {

namespace {

void ResetAllStates(AppContext& ctx) {
    ctx.dialogs.Confirm("Reset All Components", "Reset all " + std::to_string(ctx.registry.Count()) +
                                                    " components to their default state?", false,
                        [&ctx](bool ok) {
        if (!ok)
            return;
        for (const auto& item : ctx.registry.All())
            item->state = DefaultStateOf(item->kind);
        ctx.undo.Clear();
        ctx.LogConsole(LogLevel::Success, "Toolbar", "All component states were reset.");
        ctx.Notify(ToastKind::Info, "Components reset", "Default state restored.");
    });
    ctx.Interaction("Toolbar", "Reset all components requested");
}

int CountMatching(const AppContext& ctx) {
    if (ctx.demo.searchQuery.empty())
        return static_cast<int>(ctx.registry.Count());
    int n = 0;
    for (const auto& item : ctx.registry.All())
        if (ContainsNoCase(item->displayName, ctx.demo.searchQuery) ||
            ContainsNoCase(item->id, ctx.demo.searchQuery) ||
            ContainsNoCase(KindName(item->kind), ctx.demo.searchQuery))
            ++n;
    return n;
}

// Vertical rule between toolbar groups (public API only, drawn by hand).
void VerticalRule() {
    ImGui::SameLine(0.0f, 8.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x, p.y + h - 2.0f),
                                        ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
}

// The tool strip is a single horizontal row, so every item has to ask for the
// SameLine that ImGui's vertical item flow would not add on its own.
bool s_toolOnLine = false;

bool Tool(const char* id, icons::Icon icon, const char* tooltip, ImVec2 size, bool active = false,
          bool enabled = true) {
    if (s_toolOnLine)
        ImGui::SameLine(0.0f, 4.0f);
    s_toolOnLine = true;
    return icons::IconButton(id, icon, tooltip, size, active, enabled);
}

} // namespace

void DrawToolbar(AppContext& ctx) {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin(kToolbarTitle, nullptr, flags)) {
        ImGui::End();
        return;
    }

    if (ctx.host.toolbar) {
        ctx.host.toolbar(ctx);
        ImGui::End();
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    s_toolOnLine = false;
    const ImVec2 btn(30.0f, 26.0f);

    if (Tool("##new", icons::Icon::New, "New session (reset states)", btn)) {
        ResetAllStates(ctx);
    }
    if (Tool("##open", icons::Icon::Open, "Open project (not supported, explains why)", btn)) {
        ctx.dialogs.Message("Open Project",
                            "This lab generates its sample content at runtime; there is no project file to open. "
                            "Save Session writes the theme, log and layout next to the executable.");
    }
    if (Tool("##save", icons::Icon::Save, "Save session (theme + log + layout)", btn)) {
        std::string error;
        const bool themeOk = ctx.theme.SaveToFile(ctx.settings.themePath, error);
        ctx.requestSaveLayout = true;
        if (themeOk)
            ctx.Notify(ToastKind::Success, "Saved", "Theme + layout written.");
        else
            ctx.Notify(ToastKind::Error, "Save failed", error);
        ctx.Interaction("Toolbar", "Save session");
    }
    VerticalRule();

    if (Tool("##undo", icons::Icon::Undo, "Undo (Ctrl+Z)", btn, false, ctx.undo.CanUndo())) {
        UndoRecord applied;
        if (ctx.undo.Undo(ctx.registry, applied))
            ctx.LogConsole(LogLevel::Info, "History", "Undo " + applied.label);
    }
    if (Tool("##redo", icons::Icon::Redo, "Redo (Ctrl+Y)", btn, false, ctx.undo.CanRedo())) {
        UndoRecord applied;
        if (ctx.undo.Redo(ctx.registry, applied))
            ctx.LogConsole(LogLevel::Info, "History", "Redo " + applied.label);
    }
    VerticalRule();

    if (Tool("##play", icons::Icon::Play, "Play / resume (F5)", btn, ctx.demo.playing)) {
        ctx.demo.playing = true;
        ctx.LogEvent("Toolbar", "Simulation playing");
    }
    if (Tool("##pause", icons::Icon::Pause, "Pause (F6)", btn, !ctx.demo.playing)) {
        ctx.demo.playing = false;
        ctx.LogEvent("Toolbar", "Simulation paused");
    }
    if (Tool("##stop", icons::Icon::Stop, "Stop and rewind (F7)", btn)) {
        ctx.demo.playing = false;
        ctx.demo.time = 0.0f;
        ctx.LogEvent("Toolbar", "Simulation stopped");
    }
    if (Tool("##step", icons::Icon::Step, "Advance one frame", btn)) {
        ctx.demo.time += 1.0f / 60.0f;
        ctx.LogEvent("Toolbar", "Stepped one frame");
    }
    VerticalRule();

    if (Tool("##reset_layout", icons::Icon::Dock, "Reset docking layout", btn)) {
        ctx.requestResetLayout = true;
        ctx.Interaction("Toolbar", "Reset layout");
    }
    if (Tool("##theme", ctx.theme.mode == ThemeMode::Light ? icons::Icon::Moon : icons::Icon::Sun,
                          "Toggle Light / Dark theme (F2)", btn)) {
        const ThemeMode next = ctx.theme.mode == ThemeMode::Dark ? ThemeMode::Light : ThemeMode::Dark;
        ctx.theme.SetMode(next);
        ctx.LogEvent("Theme", std::string("Switched to ") + ThemeModeName(next));
    }
    if (Tool("##theme_editor", icons::Icon::Palette, "Theme editor panel", btn,
                          ctx.settings.showThemeEditor)) {
        ctx.settings.showThemeEditor = !ctx.settings.showThemeEditor;
        ctx.Interaction("Toolbar", ctx.settings.showThemeEditor ? "Opened theme editor" : "Closed theme editor");
    }
    VerticalRule();

    if (Tool("##create", icons::Icon::Plus, "Create component...", btn))
        OpenCreateComponentDialog(ctx);
    if (Tool("##duplicate", icons::Icon::Copy, "Duplicate selected component", btn, false,
                          ctx.SelectedWidget() != nullptr))
        ctx.DuplicateSelectedWidget();
    if (Tool("##reveal", icons::Icon::Star, "Reveal selected on its demo page", btn, false,
                          ctx.SelectedWidget() != nullptr)) {
        if (const WidgetInstance* sel = ctx.SelectedWidget())
            RevealComponent(ctx, sel->id);
    }
    if (Tool("##delete", icons::Icon::Trash, "Delete selected component", btn, false,
                          ctx.SelectedWidget() != nullptr)) {
        if (WidgetInstance* sel = ctx.SelectedWidget())
            RequestDeleteComponent(ctx, *sel);
    }

    // Right aligned search + counter --------------------------------------
    const ImVec4 accent = panels::AccentForCategory(ctx.activeCategory);
    char counter[64];
    std::snprintf(counter, sizeof(counter), "%d hit%s", CountMatching(ctx), CountMatching(ctx) == 1 ? "" : "s");

    const float searchWidth = 240.0f;
    const float counterWidth = ImGui::CalcTextSize(counter).x;
    const float rightWidth = searchWidth + counterWidth + ImGui::GetStyle().ItemSpacing.x * 3.0f;
    const float posX = ImGui::GetWindowWidth() - rightWidth;
    if (posX > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(posX);
        ImGui::SetNextItemWidth(searchWidth);
        widgets::SearchField("##global_search", ctx.demo.searchQuery,
                             "Search components... (filters the tree)", searchWidth);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, accent);
        ImGui::TextUnformatted(counter);
        ImGui::PopStyleColor();
    }

    ImGui::PopStyleVar();
    ImGui::End();
}

} // namespace panels
} // namespace uilab
