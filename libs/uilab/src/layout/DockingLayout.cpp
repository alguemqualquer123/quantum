// DockingLayout.cpp - main dockspace host window and the default node tree.
#include "layout/DockingLayout.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/AppContext.h"
#include "core/Serialize.h"
#include "panels/Panels.h"

namespace uilab {
namespace layout {

namespace {

const ImGuiID kDockSpaceId = 0x11000001u;   // stable: the ini file keys nodes by id

struct DockedPanel {
    const char* title;
    ImGuiID node;
};

bool s_ready = false;
std::vector<DockedPanel> s_panels;

void Dock(const char* title, ImGuiID node) {
    ImGui::DockBuilderDockWindow(title, node);
    s_panels.push_back({title, node});
}

// Builds the editor tree inside the dockspace root node.
void BuildTree() {
    s_panels.clear();

    ImGuiID top = 0, body = 0;
    ImGui::DockBuilderSplitNode(kDockSpaceId, ImGuiDir_Up, 0.055f, &top, &body);

    ImGuiID bottom = 0, middle = 0;
    ImGui::DockBuilderSplitNode(body, ImGuiDir_Down, 0.26f, &bottom, &middle);

    ImGuiID left = 0, middleRight = 0;
    ImGui::DockBuilderSplitNode(middle, ImGuiDir_Left, 0.17f, &left, &middleRight);

    ImGuiID right = 0, center = 0;
    ImGui::DockBuilderSplitNode(middleRight, ImGuiDir_Right, 0.24f, &right, &center);

    // Left column: component tree above the scene outliner.
    ImGuiID tree = 0, hierarchy = 0;
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.55f, &tree, &hierarchy);

    // Right column: inspector above the theme editor.
    ImGuiID inspector = 0, theme = 0;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.66f, &inspector, &theme);

    // Bottom: content browser beside the console.
    ImGuiID browser = 0, console = 0;
    ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.42f, &browser, &console);

    // The tool strip is a single-window node: no tab bar, it reads as a toolbar.
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(top))
        node->LocalFlags = ImGuiDockNodeFlags_AutoHideTabBar | ImGuiDockNodeFlags_NoUndocking;

    Dock(panels::kToolbarTitle, top);
    Dock(panels::kComponentTreeTitle, tree);
    Dock(panels::kHierarchyTitle, hierarchy);
    Dock(panels::kInspectorTitle, inspector);
    Dock(panels::kThemeEditorTitle, theme);
    Dock(panels::kAssetBrowserTitle, browser);
    Dock(panels::kConsoleTitle, console);
    Dock(panels::kDemoTitle, center);

    ImGui::DockBuilderFinish(kDockSpaceId);
}

} // namespace

bool IsLayoutReady() { return s_ready; }

ImGuiID DockSpaceId() { return kDockSpaceId; }

void BeginFrame(AppContext& ctx) {
    if (ctx.requestResetLayout) {
        ctx.requestResetLayout = false;
        if (ctx.installDefaultDockTree)
            ApplyDefaultLayout(ctx);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // The status bar is a screen-fixed strip, so the dockspace stops short of it.
    ImVec2 size = viewport->WorkSize;
    if (ctx.settings.showStatusBar)
        size.y -= ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    const ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;
    ImGui::Begin("##uilab_dockspace_host", nullptr, hostFlags);

    const ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode;
    ImGui::DockSpace(kDockSpaceId, ImVec2(0.0f, 0.0f), dockFlags);
    ImGui::End();

    ImGui::PopStyleVar(3);

    if (ctx.installDefaultDockTree && !s_ready)
        ApplyDefaultLayout(ctx);
}

void ApplyDefaultLayout(AppContext& ctx) {
    ImGui::DockBuilderRemoveNode(kDockSpaceId);
    const ImGuiID created = ImGui::DockBuilderAddNode(kDockSpaceId, ImGuiDockNodeFlags_DockSpace);
    (void)created;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 size = viewport->WorkSize;
    if (ctx.settings.showStatusBar)
        size.y -= ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
    ImGui::DockBuilderSetNodePos(kDockSpaceId, viewport->WorkPos);
    ImGui::DockBuilderSetNodeSize(kDockSpaceId, size);

    BuildTree();
    s_ready = true;

    ctx.LogConsole(LogLevel::Info, "Layout", "Default docking layout applied.");
}

bool SaveLayout(AppContext& ctx, std::string& errorOut) {
    size_t length = 0;
    const char* data = ImGui::SaveIniSettingsToMemory(&length);
    if (!data || length == 0) {
        errorOut = "ImGui returned an empty layout snapshot.";
        return false;
    }
    const std::string text(data, length);
    if (!WriteTextFile(ctx.settings.layoutIniPath, text, errorOut))
        return false;
    ctx.LogDebug("Layout", "Saved layout to " + ctx.settings.layoutIniPath);
    return true;
}

bool LoadLayout(AppContext& ctx, std::string& errorOut) {
    std::string text;
    if (!ReadTextFile(ctx.settings.layoutIniPath, text, errorOut))
        return false;
    ImGui::LoadIniSettingsFromMemory(text.data(), text.size());
    s_ready = true;   // the restored node tree replaces the built-in default
    ctx.LogConsole(LogLevel::Info, "Layout", "Loaded layout from " + ctx.settings.layoutIniPath);
    errorOut.clear();
    return true;
}

void CloseAllFloatingWindows(AppContext& ctx) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    int reattached = 0;
    for (ImGuiWindow* window : g.Windows) {
        if (window->DockId != 0 || window->WriteAccessed || !window->Appearing && window->HiddenFramesCanSkipItems > 0)
            continue;
        for (const DockedPanel& panel : s_panels) {
            if (std::strcmp(window->Name, panel.title) == 0 && window->DockNode == nullptr) {
                ImGui::SetWindowDock(window, panel.node, ImGuiCond_Always);
                ++reattached;
                break;
            }
        }
    }

    ctx.showSlateWindow = false;
    ctx.showImmediateWindow = false;
    ctx.showDebugOverlayWindow = false;
    ctx.showUiToolkitInspectorWindow = false;
    ctx.showSplitterDemoWindow = false;

    ctx.LogConsole(LogLevel::Info, "Layout",
                   "Floating windows closed (" + std::to_string(reattached) + " panels re-docked).");
}

} // namespace layout
} // namespace uilab
