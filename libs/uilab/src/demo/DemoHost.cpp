// DemoHost.cpp - the dockable "Demo" window: a header, a per-category tab bar and
// the live-binding helpers shared by every page.
//
// The host never owns widget data: it only routes the active category to the
// matching page and exposes Bind()/Render() so the pages can render *from* the
// registry's WidgetState. The per-frame "bound id" set drives DrawUnboundList(),
// which surfaces components no page rendered this frame (e.g. user-created ones).
#include "demo/DemoHost.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "core/AppContext.h"
#include "core/Types.h"
#include "panels/Panels.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace demo {

namespace {

// Ids rendered by a page this frame, refreshed whenever the frame index changes.
std::unordered_set<std::string>& BoundIds() {
    static std::unordered_set<std::string> s;
    return s;
}
std::unordered_set<std::string>& WarnedIds() {
    static std::unordered_set<std::string> s;
    return s;
}
int s_boundFrame = -1;

void RefreshFrame(const AppContext& ctx) {
    if (ctx.frameIndex == s_boundFrame)
        return;
    s_boundFrame = ctx.frameIndex;
    BoundIds().clear();
}

void DispatchPage(AppContext& ctx, Category c) {
    switch (c) {
    case Category::Basic:     DrawBasicPage(ctx); break;
    case Category::Layout:    DrawLayoutPage(ctx); break;
    case Category::Slate:     DrawSlatePage(ctx); break;
    case Category::UMG:       DrawUmgPage(ctx); break;
    case Category::UIToolkit: DrawUIToolkitPage(ctx); break;
    case Category::Immediate: DrawImmediatePage(ctx); break;
    case Category::UGui:      DrawUGuiPage(ctx); break;
    case Category::Count:     break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Page metadata
// ---------------------------------------------------------------------------
const char* PageName(Category c) {
    switch (c) {
    case Category::Basic:     return "Basic Widgets";
    case Category::Layout:    return "Layout & Boxes";
    case Category::Slate:     return "Unreal Slate";
    case Category::UMG:       return "UMG Widgets";
    case Category::UIToolkit: return "UI Toolkit";
    case Category::Immediate: return "Immediate Mode";
    case Category::UGui:      return "Unity uGUI";
    case Category::Count:     break;
    }
    return "Components";
}

const char* PageBlurb(Category c) {
    switch (c) {
    case Category::Basic:
        return "A gallery of the standard widgets every toolkit ships: buttons, values, "
               "inputs, collections, bars and feedback controls, all driven by the Inspector.";
    case Category::Layout:
        return "Structural containers - boxes, grids, stacks, splitters and docking - "
               "resized and re-anchored straight from the component state.";
    case Category::Slate:
        return "Editor-style panels in the Unreal Slate idiom: details, outliners, "
               "asset browsers and command toolbars.";
    case Category::UMG:
        return "Retainer-style game UI: canvas panels, overlays, widgets and HUD built from "
               "shared health/energy/score values.";
    case Category::UIToolkit:
        return "The UXML/USS model: element hierarchy, a live mini style sheet, classes and "
               "flex containers.";
    case Category::Immediate:
        return "Unity IMGUI: per-frame conditional widgets, dynamic controls and real-time "
               "diagnostics.";
    case Category::UGui:
        return "Canvas / RectTransform style UI: anchored layout groups, content fitters, "
               "scroll rects and a HUD.";
    case Category::Count:
        break;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Live binding
// ---------------------------------------------------------------------------
WidgetInstance* Bind(AppContext& ctx, const char* id) {
    RefreshFrame(ctx);
    WidgetInstance* w = ctx.registry.Find(id);
    if (!w) {
        if (WarnedIds().insert(id).second)
            ctx.LogConsole(LogLevel::Warning, "Demo",
                           std::string("A demo page bound an unknown component: ") + id);
        return nullptr;
    }
    BoundIds().insert(w->id);
    return w;
}

bool Render(AppContext& ctx, const char* id, const WidgetInstance** out) {
    WidgetInstance* w = Bind(ctx, id);
    if (!w)
        return false;
    if (out)
        *out = w;
    return w->state.visible;
}

bool RenderSelectable(AppContext& ctx, const char* id, const char* label, bool* clickedOut) {
    const WidgetInstance* w = nullptr;
    if (!Render(ctx, id, &w)) {
        if (clickedOut)
            *clickedOut = false;
        return false;
    }
    if (label == nullptr || *label == '\0')
        label = w->displayName.c_str();
    const bool selected = ctx.selection.widgetId == w->id;
    const bool clicked = widgets::SelectableRow(label, selected, panels::IconForKind(w->kind),
                                                panels::AccentForCategory(w->category));
    if (clicked)
        ctx.SelectWidget(w->id);
    if (clickedOut)
        *clickedOut = clicked;
    return clicked;
}

void DrawUnboundList(AppContext& ctx, Category category) {
    RefreshFrame(ctx);
    const std::unordered_set<std::string>& bound = BoundIds();

    std::vector<const WidgetInstance*> unbound;
    for (const auto& item : ctx.registry.All()) {
        if (item->category != category)
            continue;
        if (bound.find(item->id) != bound.end())
            continue;
        unbound.push_back(item.get());
    }
    if (unbound.empty())
        return;

    if (!widgets::SectionHeader("Unbound components (inspector only)", icons::Icon::Widget, false))
        return;
    for (const WidgetInstance* w : unbound) {
        const bool selected = ctx.selection.widgetId == w->id;
        ImGui::PushID(w->id.c_str());
        if (widgets::SelectableRow(w->displayName.c_str(), selected, panels::IconForKind(w->kind),
                                   panels::AccentForCategory(w->category))) {
            ctx.SelectWidget(w->id);
        }
        widgets::TextMuted("%s - not rendered by this page, edit it in the Inspector", KindName(w->kind));
        ImGui::PopID();
    }
}

float PageClock(const AppContext& ctx, float cyclesPerSecond) {
    const float t = ctx.demo.time * cyclesPerSecond;
    return t - std::floor(t);
}

// ---------------------------------------------------------------------------
// Host window
// ---------------------------------------------------------------------------
void DrawHost(AppContext& ctx) {
    if (!ImGui::Begin(panels::kDemoTitle)) {
        ImGui::End();
        return;
    }

    const Category cat = ctx.activeCategory;
    const ImVec4 accent = panels::AccentForCategory(cat);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextUnformatted(PageName(cat));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    widgets::Badge("live", accent);
    const char* hint = "every widget here reads its state from the Inspector";
    const float hintW = ImGui::CalcTextSize(hint).x;
    const float hintX = ImGui::GetWindowWidth() - hintW - ImGui::GetStyle().WindowPadding.x;
    if (hintX > ImGui::GetCursorPosX() + ImGui::GetStyle().ItemSpacing.x) {
        ImGui::SameLine(hintX);
        widgets::TextMuted("%s", hint);
    }

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(PageBlurb(cat));
    ImGui::PopTextWrapPos();
    widgets::TextMuted("Live-bound components: %d   |   registry total: %d", ctx.registry.CountLive(),
                       static_cast<int>(ctx.registry.Count()));
    ImGui::Separator();

    // Tab bar drives (and follows) ctx.activeCategory. Content is drawn below the
    // bar so a single tab is never rendered twice when the selection switches.
    static Category s_lastShown = Category::Count;
    const bool forceSync = (s_lastShown != ctx.activeCategory);
    Category target = ctx.activeCategory;

    if (ImGui::BeginTabBar("##demo_categories", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int ci = 0; ci < static_cast<int>(Category::Count); ++ci) {
            const Category c = static_cast<Category>(ci);
            char tabLabel[96];
            std::snprintf(tabLabel, sizeof(tabLabel), "%s##c%d", PageName(c), ci);
            ImGuiTabItemFlags flags = 0;
            if (forceSync && c == ctx.activeCategory)
                flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem(tabLabel, nullptr, flags)) {
                if (!forceSync)
                    target = c;
                ImGui::EndTabItem();
            }
        }
        if (target != ctx.activeCategory)
            ctx.SelectCategory(target);
        s_lastShown = ctx.activeCategory;
        ImGui::EndTabBar();
    }

    if (ImGui::BeginChild("##page_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        DispatchPage(ctx, ctx.activeCategory);
        ImGui::EndChild();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Floating windows
// ---------------------------------------------------------------------------
void DrawFloatingWindows(AppContext& ctx) {
    if (ctx.showSlateWindow)
        DrawSlateFloatingWindow(ctx);
    if (ctx.showImmediateWindow)
        DrawImmediateFloatingWindow(ctx);
    if (ctx.showDebugOverlayWindow)
        DrawDebugOverlayWindow(ctx);
    if (ctx.showUiToolkitInspectorWindow)
        DrawUiToolkitInspectorWindow(ctx);
    if (ctx.showSplitterDemoWindow)
        DrawSplitterFloatingWindow(ctx);

    // The dockable window is spawned from the layout.dockable.window component's
    // "Toggled" flag; DrawDockableFloatingWindow writes the close button back.
    if (WidgetInstance* dockable = ctx.registry.Find("layout.dockable.window")) {
        if (dockable->state.toggled)
            DrawDockableFloatingWindow(ctx);
    }
}

} // namespace demo
} // namespace uilab
