// MenuBarPanel.cpp - the application menu bar (File / Edit / View / Components /
// Theme / Help) drawn into the main viewport.
#include "panels/Panels.h"

#include <cstdio>

#include <imgui.h>

#include "core/AppContext.h"
#include "core/Dialogs.h"
#include "core/UndoStack.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace panels {

namespace {

void DoUndo(AppContext& ctx) {
    UndoRecord applied;
    if (ctx.undo.Undo(ctx.registry, applied)) {
        ctx.LogConsole(LogLevel::Info, "History",
                       "Undo " + applied.label + " on " + applied.widgetName);
        ctx.Notify(ToastKind::Info, "Undo", applied.label + " - " + applied.widgetName, 2.5f);
    } else {
        ctx.LogConsole(LogLevel::Warning, "History", "Nothing to undo.");
    }
}

void DoRedo(AppContext& ctx) {
    UndoRecord applied;
    if (ctx.undo.Redo(ctx.registry, applied)) {
        ctx.LogConsole(LogLevel::Info, "History",
                       "Redo " + applied.label + " on " + applied.widgetName);
        ctx.Notify(ToastKind::Info, "Redo", applied.label + " - " + applied.widgetName, 2.5f);
    } else {
        ctx.LogConsole(LogLevel::Warning, "History", "Nothing to redo.");
    }
}

void SaveSession(AppContext& ctx) {
    std::string error;
    int saved = 0;
    if (ctx.theme.SaveToFile(ctx.settings.themePath, error))
        ++saved;
    else
        ctx.LogConsole(LogLevel::Error, "Save", "Theme: " + error);

    if (ctx.log.ExportToFile(ctx.settings.logExportPath, false, error))
        ++saved;
    else
        ctx.LogConsole(LogLevel::Error, "Save", "Log: " + error);

    ctx.requestSaveLayout = true;
    ctx.LogConsole(LogLevel::Success, "Save",
                   "Saved " + std::to_string(saved) + "/2 files + layout (" + ctx.settings.layoutIniPath + ")");
    ctx.Notify(ToastKind::Success, "Session saved", "Theme, log and layout were written to disk.");
}

void MenuFile(AppContext& ctx) {
    if (!ImGui::BeginMenu("File"))
        return;
    if (ImGui::MenuItem("New Session", widgets::ShortcutText("new"))) {
        ctx.dialogs.Confirm("New Session", "Reset every component to its default state and clear the history?", false,
                            [&ctx](bool ok) {
            if (!ok)
                return;
            for (const auto& item : ctx.registry.All())
                item->state = DefaultStateOf(item->kind);
            ctx.undo.Clear();
            ctx.log.ClearAll();
            ctx.scene.BuildDefaultScene();
            ctx.LogConsole(LogLevel::Success, "Session", "New session started, components reset.");
            ctx.Notify(ToastKind::Info, "New session", "All components were reset to defaults.");
        });
    }
    if (ImGui::MenuItem("Open Project...", widgets::ShortcutText("open")))
        ctx.dialogs.Message("Open Project",
                            "Universal UI Components Lab is self contained: the sample content is generated in "
                            "memory, so there is no project file to open. Use File > Save Session to write the "
                            "theme, the log and the docking layout next to the executable.");
    ImGui::Separator();
    if (ImGui::MenuItem("Save Session", widgets::ShortcutText("save")))
        SaveSession(ctx);
    if (ImGui::MenuItem("Save Layout Only", widgets::ShortcutText("save_layout"))) {
        ctx.requestSaveLayout = true;
        ctx.LogConsole(LogLevel::Info, "Layout", "Layout save requested.");
    }
    if (ImGui::MenuItem("Load Layout", nullptr)) {
        ctx.requestLoadLayout = true;
        ctx.LogConsole(LogLevel::Info, "Layout", "Layout load requested.");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Export Log...", nullptr)) {
        std::string error;
        if (ctx.log.ExportToFile(ctx.settings.logExportPath, true, error)) {
            ctx.LogConsole(LogLevel::Success, "Console", "Exported (filtered) log to " + ctx.settings.logExportPath);
            ctx.Notify(ToastKind::Success, "Log exported", ctx.settings.logExportPath);
        } else {
            ctx.LogConsole(LogLevel::Error, "Console", "Export failed: " + error);
            ctx.Notify(ToastKind::Error, "Export failed", error);
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Exit", "Alt+F4")) {
        ctx.dialogs.Confirm("Exit", "Close Universal UI Components Lab?", true, [&ctx](bool ok) {
            if (ok)
                ctx.requestExit = true;
        });
    }
    ImGui::EndMenu();
}

void MenuEdit(AppContext& ctx) {
    if (!ImGui::BeginMenu("Edit"))
        return;
    const std::string undoLabel = ctx.undo.CanUndo() ? "Undo " + ctx.undo.UndoLabel() : std::string("Undo");
    const std::string redoLabel = ctx.undo.CanRedo() ? "Redo " + ctx.undo.RedoLabel() : std::string("Redo");
    if (ImGui::MenuItem(undoLabel.c_str(), widgets::ShortcutText("undo"), false, ctx.undo.CanUndo()))
        DoUndo(ctx);
    if (ImGui::MenuItem(redoLabel.c_str(), widgets::ShortcutText("redo"), false, ctx.undo.CanRedo()))
        DoRedo(ctx);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy Properties", widgets::ShortcutText("copy"), false, ctx.SelectedWidget() != nullptr))
        ctx.CopySelectedProperties();
    if (ImGui::MenuItem("Paste Properties", widgets::ShortcutText("paste"), false, !ctx.propertyClipboard.empty()))
        ctx.PastePropertiesIntoSelected();
    if (ImGui::MenuItem("Duplicate Component", widgets::ShortcutText("duplicate"), false,
                        ctx.SelectedWidget() != nullptr))
        ctx.DuplicateSelectedWidget();
    ImGui::Separator();
    if (ImGui::MenuItem("Reset Properties", nullptr, false, ctx.SelectedWidget() != nullptr)) {
        if (WidgetInstance* sel = ctx.SelectedWidget()) {
            ctx.ResetWidgetState(*sel);
            ctx.Notify(ToastKind::Info, "Properties reset", sel->displayName);
        }
    }
    if (ImGui::MenuItem("Delete Component", widgets::ShortcutText("delete"), false, ctx.SelectedWidget() != nullptr)) {
        if (WidgetInstance* sel = ctx.SelectedWidget())
            RequestDeleteComponent(ctx, *sel);
    }
    ImGui::Separator();
    ImGui::MenuItem("Log every widget edit", nullptr, &ctx.settings.logAllWidgetEdits);
    ImGui::MenuItem("Log interactions", nullptr, &ctx.settings.logInteractions);
    ImGui::EndMenu();
}

void MenuView(AppContext& ctx) {
    if (!ImGui::BeginMenu("View"))
        return;
    ImGui::MenuItem(kComponentTreeTitle, nullptr, &ctx.settings.showComponentTree);
    ImGui::MenuItem(kHierarchyTitle, nullptr, &ctx.settings.showHierarchy);
    ImGui::MenuItem(kInspectorTitle, nullptr, &ctx.settings.showInspector);
    ImGui::MenuItem(kConsoleTitle, nullptr, &ctx.settings.showConsole);
    ImGui::MenuItem(kAssetBrowserTitle, nullptr, &ctx.settings.showAssetBrowser);
    ImGui::MenuItem(kThemeEditorTitle, nullptr, &ctx.settings.showThemeEditor);
    ImGui::MenuItem(kToolbarTitle, nullptr, &ctx.settings.showToolbar);
    ImGui::MenuItem(kStatusBarTitle, nullptr, &ctx.settings.showStatusBar);
    ImGui::Separator();
    if (ImGui::MenuItem("Reset Layout", widgets::ShortcutText("reset_layout")))
        ctx.requestResetLayout = true;
    if (ImGui::MenuItem("Close Floating Windows"))
        ctx.requestCloseAllFloatingWindows = true;
    ImGui::Separator();
    ImGui::MenuItem("Canvas grid", nullptr, &ctx.settings.showCanvasGrid);
    ImGui::MenuItem("Canvas bounds overlay", nullptr, &ctx.settings.drawBoundsOverlay);
    ImGui::MenuItem("Anchor markers", nullptr, &ctx.settings.drawAnchorMarkers);
    ImGui::MenuItem("Snap to grid", nullptr, &ctx.settings.snapToGrid);
    ImGui::Separator();
    ImGui::MenuItem("Dear ImGui Demo", nullptr, &ctx.settings.showImGuiDemo);
    ImGui::MenuItem("Dear ImGui Metrics", nullptr, &ctx.settings.showImGuiMetrics);
    ImGui::MenuItem("Dear ImGui Style Editor", nullptr, &ctx.settings.showImGuiStyleEditor);
    ImGui::EndMenu();
}

void MenuComponents(AppContext& ctx) {
    if (!ImGui::BeginMenu("Components"))
        return;
    for (int i = 0; i < static_cast<int>(Category::Count); ++i) {
        const Category c = static_cast<Category>(i);
        char label[128];
        std::snprintf(label, sizeof(label), "%s  (%d)", CategoryName(c), ctx.registry.CountCategory(c));
        if (ImGui::MenuItem(label, nullptr, ctx.activeCategory == c)) {
            ctx.SelectCategory(c);
            ctx.LogEvent("Menu", std::string("Switched to ") + CategoryName(c));
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Create Component..."))
        OpenCreateComponentDialog(ctx);
    if (ImGui::MenuItem("Duplicate Selected", nullptr, false, ctx.SelectedWidget() != nullptr))
        ctx.DuplicateSelectedWidget();
    if (ImGui::MenuItem("Reveal Selected", nullptr, false, ctx.SelectedWidget() != nullptr)) {
        if (const WidgetInstance* sel = ctx.SelectedWidget())
            RevealComponent(ctx, sel->id);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("HUD sample scene (reset)", nullptr)) {
        ctx.scene.BuildDefaultScene();
        ctx.selection.sceneNodeId = -1;
        ctx.LogConsole(LogLevel::Success, "Scene", "Canvas scene rebuilt with the default HUD sample.");
    }
    ImGui::EndMenu();
}

void MenuTheme(AppContext& ctx) {
    if (!ImGui::BeginMenu("Theme"))
        return;
    if (ImGui::MenuItem("Dark", nullptr, ctx.theme.mode == ThemeMode::Dark))
        ctx.theme.SetMode(ThemeMode::Dark);
    if (ImGui::MenuItem("Light", nullptr, ctx.theme.mode == ThemeMode::Light))
        ctx.theme.SetMode(ThemeMode::Light);
    if (ImGui::MenuItem("Custom", nullptr, ctx.theme.mode == ThemeMode::Custom))
        ctx.theme.SetMode(ThemeMode::Custom);
    ImGui::Separator();
    ImGui::MenuItem("Theme Editor", nullptr, &ctx.settings.showThemeEditor);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy Dark into Custom")) {
        ctx.theme.ResetCustom();
        ctx.theme.SetMode(ThemeMode::Custom);
        ctx.LogConsole(LogLevel::Success, "Theme", "Custom palette reset from the dark theme.");
    }
    if (ImGui::MenuItem("Reset Style Metrics")) {
        ctx.theme.ResetStyleToDefault();
        ctx.LogConsole(LogLevel::Info, "Theme", "Style metrics restored to defaults.");
    }
    ImGui::MenuItem("Compact mode", nullptr, &ctx.settings.compactMode);
    ImGui::Separator();
    if (ImGui::MenuItem("Save Theme")) {
        std::string error;
        if (ctx.theme.SaveToFile(ctx.settings.themePath, error))
            ctx.Notify(ToastKind::Success, "Theme saved", ctx.settings.themePath);
        else
            ctx.Notify(ToastKind::Error, "Theme save failed", error);
    }
    if (ImGui::MenuItem("Load Theme")) {
        std::string error;
        if (ctx.theme.LoadFromFile(ctx.settings.themePath, error))
            ctx.Notify(ToastKind::Success, "Theme loaded", ctx.settings.themePath);
        else
            ctx.Notify(ToastKind::Warning, "Theme load failed", error);
    }
    ImGui::EndMenu();
}

void MenuHelp(AppContext& ctx) {
    if (!ImGui::BeginMenu("Help"))
        return;
    if (ImGui::MenuItem("About")) {
        ctx.settings.showAbout = true;
        ctx.dialogs.Message(
            std::string(ctx.appName) + " " + ctx.appVersion,
            "A self-contained laboratory of UI components and patterns inspired by Unreal Slate/UMG, Unity UI "
            "Toolkit, IMGUI and uGUI.\n\n"
            "Built with C++17, Dear ImGui (docking) and GLFW. All icons and textures are generated at runtime; "
            "no engine code or assets are used.\n\n"
            "Pages: Basic widgets, Layout, Slate, UMG, UI Toolkit, Immediate mode and uGUI.");
    }
    if (ImGui::MenuItem("Controls & Shortcuts", nullptr))
        ctx.dialogs.Message("Controls",
                            "Mouse: click to interact, right-click rows for context menus, drag on canvases to move "
                            "widgets, middle-drag to pan, wheel to zoom/scroll.\n\n"
                            "Shortcuts: Ctrl+Z / Ctrl+Y history, Ctrl+C / Ctrl+V properties, Ctrl+D duplicate, "
                            "Ctrl+L reset layout, F5/F6/F7 play controls, F2 cycle theme.");
    ImGui::Separator();
    ImGui::MenuItem("Dear ImGui Demo", nullptr, &ctx.settings.showImGuiDemo);
    ImGui::MenuItem("Dear ImGui Metrics", nullptr, &ctx.settings.showImGuiMetrics);
    ImGui::EndMenu();
}

} // namespace

void DrawMenuBar(AppContext& ctx) {
    if (!ImGui::BeginMainMenuBar())
        return;

    // An embedding host owns the menu items; the bar itself stays here.
    if (ctx.host.menuBar) {
        ctx.host.menuBar(ctx);
        ImGui::EndMainMenuBar();
        return;
    }

    icons::IconText(icons::Icon::Widget, "  UI Components Lab  ", ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::Separator();
    MenuFile(ctx);
    MenuEdit(ctx);
    MenuView(ctx);
    MenuComponents(ctx);
    MenuTheme(ctx);
    MenuHelp(ctx);

    // Right hand side: quick theme switch + fps.
    char right[160];
    std::snprintf(right, sizeof(right), "%s | %.0f fps | %d components",
                  ctx.theme.ActiveName(), static_cast<double>(ctx.stats.currentFps),
                  static_cast<int>(ctx.registry.Count()));
    const float width = ImGui::CalcTextSize(right).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    const float posX = ImGui::GetWindowWidth() - width;
    if (posX > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(posX);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(right);
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemClicked()) {
        const ThemeMode next = ctx.theme.mode == ThemeMode::Dark ? ThemeMode::Light : ThemeMode::Dark;
        ctx.theme.SetMode(next);
        ctx.LogEvent("Theme", std::string("Switched to ") + ThemeModeName(next));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Click to toggle Dark / Light. Use the Theme menu for the custom palette.");

    ImGui::EndMainMenuBar();
}

} // namespace panels
} // namespace uilab
