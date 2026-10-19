// Panels.h - entry points of the dockable editor panels.
//
// Every panel is a plain function taking the shared AppContext; the Application
// owns the windows/docking and decides when each panel is visible.
#pragma once

#include <string>

#include <imgui.h>

#include "core/Types.h"
#include "ui/Icons.h"

namespace uilab {

class AppContext;
struct WidgetInstance;

namespace panels {

// Window titles - also used by the docking layout, so they live in one place.
constexpr const char* kToolbarTitle = "Toolbar";
constexpr const char* kStatusBarTitle = "Status Bar";
constexpr const char* kComponentTreeTitle = "Component Tree";
constexpr const char* kHierarchyTitle = "Hierarchy";
constexpr const char* kInspectorTitle = "Inspector";
constexpr const char* kConsoleTitle = "Console";
constexpr const char* kAssetBrowserTitle = "Content Browser";
constexpr const char* kThemeEditorTitle = "Theme Editor";
constexpr const char* kDemoTitle = "Demo";

void DrawMenuBar(AppContext& ctx);        // main viewport menu bar
void DrawToolbar(AppContext& ctx);        // dockable tool strip
void DrawComponentTree(AppContext& ctx);  // left: components by category
void DrawHierarchy(AppContext& ctx);      // left: canvas scene outliner
void DrawInspector(AppContext& ctx);      // right: property editor
void DrawConsole(AppContext& ctx);        // bottom: console / events / debug
void DrawAssetBrowser(AppContext& ctx);   // bottom left: content browser
void DrawStatusBar(AppContext& ctx);      // thin bottom strip
void DrawThemeEditor(AppContext& ctx);    // palette + metrics editor

// Shared helpers ------------------------------------------------------------

// Icon + accent colour used for a widget kind (component tree / inspector).
icons::Icon IconForKind(WidgetKind kind);
ImVec4 AccentForCategory(Category c);

// Opens the "create component" modal and registers the result in the registry.
void OpenCreateComponentDialog(AppContext& ctx);
// Confirms and deletes a component (used by the tree + inspector).
void RequestDeleteComponent(AppContext& ctx, WidgetInstance& inst);
// Switches the demo page to the category of a component and selects it.
void RevealComponent(AppContext& ctx, const std::string& id);

} // namespace panels
} // namespace uilab
