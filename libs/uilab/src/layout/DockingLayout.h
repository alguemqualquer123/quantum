// DockingLayout.h - main dockspace + default node tree.
//
// The layout is built with ImGui's DockBuilder once, then persisted in the imgui
// ini file. Panels are keyed by the titles declared in Panels.h.
#pragma once

#include <string>

#include <imgui.h>

namespace uilab {

class AppContext;

namespace layout {

// The id of the dockspace BeginFrame() creates. The ini file keys its nodes by it,
// and a host that builds its own node tree needs it for the DockBuilder calls.
ImGuiID DockSpaceId();

// Creates the dockspace over the work area (below the menu bar, above the
// status bar) and installs the default node tree the first time it is needed or
// after ctx.requestResetLayout. Call once per frame before the panels.
void BeginFrame(AppContext& ctx);

// True when the default node tree has been installed in this run.
bool IsLayoutReady();

// Forces the default node tree (drops the current docking state).
void ApplyDefaultLayout(AppContext& ctx);

// Ini persistence (the same file ImGui uses, pointed at ctx.settings.layoutIniPath).
bool SaveLayout(AppContext& ctx, std::string& errorOut);
bool LoadLayout(AppContext& ctx, std::string& errorOut);

// Detaches every floating window back into the dockspace tree.
void CloseAllFloatingWindows(AppContext& ctx);

} // namespace layout
} // namespace uilab
