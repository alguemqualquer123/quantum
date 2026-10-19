// DemoHost.h - the "Demo" window and the seven category pages.
//
// The host owns the tab bar and the page header; each page renders *from* the
// WidgetState of the live-bound components in the registry, which is what makes
// Inspector edits appear in the preview immediately.
#pragma once

#include <string>

#include <imgui.h>

#include "core/Types.h"

namespace uilab {

class AppContext;
struct WidgetInstance;

namespace demo {

// ---- host -----------------------------------------------------------------
// Draws the dockable "Demo" window (title bar content, tab bar, page body).
void DrawHost(AppContext& ctx);
// Draws the floating windows requested by the pages/toolbar (one call per frame,
// outside of any other window).
void DrawFloatingWindows(AppContext& ctx);

// ---- pages ----------------------------------------------------------------
void DrawBasicPage(AppContext& ctx);
void DrawLayoutPage(AppContext& ctx);
void DrawSlatePage(AppContext& ctx);
void DrawUmgPage(AppContext& ctx);
void DrawUIToolkitPage(AppContext& ctx);
void DrawImmediatePage(AppContext& ctx);
void DrawUGuiPage(AppContext& ctx);

// Page metadata (labels shown by the tab bar and the status bar).
const char* PageName(Category c);
const char* PageBlurb(Category c);

// ---- floating windows -----------------------------------------------------
// Each is implemented by the page that owns its content and started/stopped by
// DrawFloatingWindows() through the ctx.show* flags. All take the ctx window's
// coordinates as their own; they are drawn outside of the host window.
void DrawSlateFloatingWindow(AppContext& ctx);        // DemoSlate.cpp
void DrawImmediateFloatingWindow(AppContext& ctx);    // DemoImmediate.cpp
void DrawDebugOverlayWindow(AppContext& ctx);         // DemoImmediate.cpp
void DrawUiToolkitInspectorWindow(AppContext& ctx);   // DemoUIToolkit.cpp
void DrawSplitterFloatingWindow(AppContext& ctx);     // DemoLayout.cpp
void DrawDockableFloatingWindow(AppContext& ctx);     // DemoLayout.cpp

// ---- live binding ---------------------------------------------------------
// Looks a registered component up by id and caches a warning in the log the
// first time an id is missing, so a renamed/deleted component never crashes a
// page. Returns nullptr when the component does not exist.
WidgetInstance* Bind(AppContext& ctx, const char* id);

// Convenience wrappers: Bind() + the "visible/enabled" flags of the state.
// Render() returns false when the page must skip the component entirely.
bool Render(AppContext& ctx, const char* id, const WidgetInstance** out = nullptr);
// Same as Render() and additionally selects the component on click.
bool RenderSelectable(AppContext& ctx, const char* id, const char* label, bool* clickedOut = nullptr);

// Draws the standard "unbound component" card so pages can list components of
// their category that no demo section renders explicitly.
void DrawUnboundList(AppContext& ctx, Category category);

// Animation clock helper: 0..1 sawtooth driven by ctx.demo (pauses with F6).
float PageClock(const AppContext& ctx, float cyclesPerSecond = 0.25f);

} // namespace demo
} // namespace uilab
