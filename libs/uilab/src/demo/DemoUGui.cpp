// DemoUGui.cpp - Unity-inspector style uGUI page: canvas, RectTransform editor,
// anchor presets, a pivot gizmo, layout group, content fitter, scroll rect and a
// HUD. All edits operate on the selected canvas node and mirror into the bound
// component states.
#include "demo/DemoHost.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "canvas/CanvasRenderer.h"
#include "canvas/CanvasScene.h"
#include "core/AppContext.h"
#include "core/Notifications.h"
#include "core/Types.h"
#include "panels/Panels.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace demo {

namespace {

CanvasView s_ugui_view;

int NearestPreset(const ImVec2& v) {
    int best = static_cast<int>(AnchorPreset::MiddleCenter);
    float bestD = FLT_MAX;
    for (int i = 0; i < static_cast<int>(AnchorPreset::Count); ++i) {
        const ImVec2 a = AnchorVec(static_cast<AnchorPreset>(i));
        const float dx = a.x - v.x;
        const float dy = a.y - v.y;
        const float d = dx * dx + dy * dy;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void ApplyPresetToNode(CanvasNode& n, AnchorPreset p) {
    const ImVec2 v = AnchorVec(p);
    n.anchorMin = v;
    n.anchorMax = v;
    n.pivot = v;
}

void ClampDemo(AppContext& ctx) {
    DemoShared& d = ctx.demo;
    d.health = ImClamp(d.health, 0.0f, 1.0f);
    d.energy = ImClamp(d.energy, 0.0f, 1.0f);
    if (d.health <= 0.0f) {
        d.health = 1.0f;
        ctx.Notify(ToastKind::Warning, "Player Down", "Health hit zero - respawned at full health.");
    }
}

// ---- top toolbar (ugui.canvas.main) ---------------------------------------

void DrawToolbar(AppContext& ctx, WidgetInstance& canvas) {
    WidgetState& s = canvas.state;
    static const char* kModes[3] = { "Screen Space - Overlay", "Screen Space - Camera", "World Space" };
    const int mode = ImClamp(s.selectedIndex, 0, 2);

    ImGui::SetNextItemWidth(240.0f);
    int m = mode;
    if (ImGui::Combo("##rendermode", &m, kModes, 3)) {
        s.selectedIndex = m;
        widgets::TrackEdit(ctx, canvas, "Render mode");
        ctx.Interaction("uGUI Canvas", std::string("Render mode: ") + kModes[m]);
    }
    ImGui::SameLine();
    if (widgets::ToggleButton("rect_tool", "Rect Tool", &s.toggled, ImVec2(0.0f, 0.0f), "Toggle the rect edit tool")) {
        widgets::TrackEdit(ctx, canvas, "Rect tool");
        ctx.Interaction("uGUI Canvas", s.toggled ? "Rect tool on" : "Rect tool off");
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Anchor Tool##ugui", &ctx.settings.drawAnchorMarkers))
        ctx.Interaction("uGUI Canvas", ctx.settings.drawAnchorMarkers ? "Anchors shown" : "Anchors hidden");
    ImGui::SameLine();
    if (ImGui::Checkbox("Bounds##ugui", &ctx.settings.drawBoundsOverlay))
        ctx.Interaction("uGUI Canvas", ctx.settings.drawBoundsOverlay ? "Bounds shown" : "Bounds hidden");
    ImGui::Separator();
}

// ---- left column: canvas ---------------------------------------------------

void DrawCanvasSection(AppContext& ctx, WidgetInstance& canvas) {
    WidgetState& s = canvas.state;
    if (s.size.x > 1.0f && s.size.y > 1.0f)
        s_ugui_view.resolution = ImVec2(s.size.x, s.size.y);

    CanvasRenderOptions options;
    options.showGrid = ctx.settings.showCanvasGrid;
    options.showBounds = ctx.settings.drawBoundsOverlay;
    options.showAnchors = ctx.settings.drawAnchorMarkers;
    options.interactive = true;
    options.dimNonSelected = true;
    options.gridStep = ctx.settings.gridSize;
    options.surfaceId = "ugui_canvas_surface";
    options.background = s.color;

    // Bounded child keeps the note below the canvas visible.
    if (ImGui::BeginChild("##ugui_canvas", ImVec2(0.0f, 420.0f), ImGuiChildFlags_Borders)) {
        const CanvasFrameResult result = DrawCanvas(ctx, s_ugui_view, options);

        if (result.clickedNode >= 0) {
            ctx.selection.sceneNodeId = result.clickedNode;
            if (const CanvasNode* n = ctx.scene.Find(result.clickedNode))
                ctx.LogDebug("uGUI", "Selected " + n->name);
        }
        if (result.dropped && ctx.selection.sceneNodeId >= 0) {
            if (CanvasNode* n = ctx.scene.Find(ctx.selection.sceneNodeId))
                ctx.Interaction("uGUI", n->name + " repositioned");
        }
        if (result.activatedButton >= 0) {
            const CanvasNode* b = ctx.scene.Find(result.activatedButton);
            if (b) {
                const std::string lower = ToLower(b->name + " " + b->text);
                if (lower.find("reward") != std::string::npos || lower.find("claim") != std::string::npos ||
                    lower.find("confirm") != std::string::npos) {
                    ctx.demo.score += 150.0f;
                    ctx.Notify(ToastKind::Success, "Picked Up", "+150 score");
                } else {
                    ctx.demo.health -= ctx.demo.damagePerHit;
                    ctx.Notify(ToastKind::Error, "Damaged", "Lost health");
                }
                ClampDemo(ctx);
                ctx.Interaction("uGUI", "Button " + b->name);
            }
        }
    }
    ImGui::EndChild();

    ImGui::TextDisabled("Nodes: %d | zoom %.2fx", ctx.scene.Count(), static_cast<double>(s_ugui_view.zoom));
}

// ---- right column: inspector sections --------------------------------------

void DrawRectTransformSection(AppContext& ctx, WidgetInstance& inst, CanvasNode* node) {
    if (!widgets::SectionHeader("Rect Transform", icons::Icon::Cube))
        return;

    if (!node) {
        widgets::TextMutedWrapped("Select a node on the canvas to edit its RectTransform.");
        return;
    }

    static const char* kAnchorNames[static_cast<int>(AnchorPreset::Count) + 1] = {};
    static bool namesReady = false;
    if (!namesReady) {
        for (int i = 0; i < static_cast<int>(AnchorPreset::Count); ++i)
            kAnchorNames[i] = AnchorName(static_cast<AnchorPreset>(i));
        namesReady = true;
    }

    char label[128];
    std::snprintf(label, sizeof(label), "%s", node->name.c_str());
    widgets::TextAccent("%s", label);

    int aMin = NearestPreset(node->anchorMin);
    int aMax = NearestPreset(node->anchorMax);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##amin", &aMin, kAnchorNames, static_cast<int>(AnchorPreset::Count))) {
        node->anchorMin = AnchorVec(static_cast<AnchorPreset>(aMin));
        node->anchorMax = node->anchorMin;
        inst.state.anchorMin = aMin;
        inst.state.anchorMax = aMin;
        widgets::TrackEdit(ctx, inst, "Anchor min");
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##amax", &aMax, kAnchorNames, static_cast<int>(AnchorPreset::Count))) {
        node->anchorMax = AnchorVec(static_cast<AnchorPreset>(aMax));
        inst.state.anchorMax = aMax;
        widgets::TrackEdit(ctx, inst, "Anchor max");
    }

    if (ImGui::DragFloat2("##pos", &node->anchoredPosition.x, 0.5f)) {
        inst.state.position = node->anchoredPosition;
        widgets::TrackEdit(ctx, inst, "Anchored position");
    }
    if (ImGui::DragFloat2("##size", &node->sizeDelta.x, 0.5f, 1.0f, 4000.0f)) {
        inst.state.size = node->sizeDelta;
        widgets::TrackEdit(ctx, inst, "Size delta");
    }
    if (ImGui::DragFloat2("##scale", &node->scale.x, 0.01f, 0.05f, 4.0f)) {
        inst.state.scale = node->scale;
        widgets::TrackEdit(ctx, inst, "Scale");
    }
    if (ImGui::SliderFloat("##rot", &node->rotation, -180.0f, 180.0f, "%.1f deg")) {
        inst.state.rotation = node->rotation;
        widgets::TrackEdit(ctx, inst, "Rotation");
    }
    if (ImGui::SliderFloat("##opacity", &node->opacity, 0.0f, 1.0f)) {
        inst.state.opacity = node->opacity;
        widgets::TrackEdit(ctx, inst, "Opacity");
    }
    if (ImGui::DragInt("##z", &node->zOrder)) {
        inst.state.zOrder = node->zOrder;
        widgets::TrackEdit(ctx, inst, "Z order");
    }
    // Keep the mirrored pivot preset in sync with the (possibly gizmo-edited) node.
    inst.state.pivot = NearestPreset(node->pivot);
}

void DrawAnchorsPresetSection(AppContext& ctx, WidgetInstance& inst, CanvasNode* node) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Anchor Presets", icons::Icon::Grid))
        return;

    static const char* kChip[9] = { "TL", "TC", "TR", "ML", "MC", "MR", "BL", "BC", "BR" };
    const ImVec4 accent = panels::AccentForCategory(Category::UGui);
    const int current = ImClamp(s.anchorMin, 0, 8);
    for (int i = 0; i < 9; ++i) {
        const bool clicked = widgets::ChipSelectable(kChip[i], i == current, accent, 30.0f);
        if (clicked && node) {
            ApplyPresetToNode(*node, static_cast<AnchorPreset>(i));
            s.anchorMin = s.anchorMax = s.pivot = i;
            widgets::TrackEdit(ctx, inst, "Anchor preset");
            ctx.Interaction("uGUI Anchors", std::string("Preset ") + kChip[i]);
        }
        if ((i % 3) != 2)
            ImGui::SameLine();
    }
}

void DrawPivotSection(AppContext& ctx, WidgetInstance& inst, CanvasNode* node) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Pivot", icons::Icon::Point))
        return;

    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImMax(90.0f, w * 0.55f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 max(pos.x + w, pos.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 accent = panels::AccentForCategory(Category::UGui);

    dl->AddRectFilled(pos, max, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
    dl->AddRect(pos, max, ImGui::GetColorU32(ImGuiCol_Border), 4.0f);
    for (int i = 1; i < 3; ++i) {
        const float gx = pos.x + w * (static_cast<float>(i) / 3.0f);
        const float gy = pos.y + h * (static_cast<float>(i) / 3.0f);
        dl->AddLine(ImVec2(gx, pos.y), ImVec2(gx, max.y), ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        dl->AddLine(ImVec2(pos.x, gy), ImVec2(max.x, gy), ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
    }
    const ImVec2 pv = node ? node->pivot : AnchorVec(static_cast<AnchorPreset>(s.pivot));
    const ImVec2 pivotPos(pos.x + pv.x * w, pos.y + pv.y * h);
    dl->AddLine(ImVec2(pivotPos.x - 8, pivotPos.y), ImVec2(pivotPos.x + 8, pivotPos.y),
                ImGui::ColorConvertFloat4ToU32(accent), 1.5f);
    dl->AddLine(ImVec2(pivotPos.x, pivotPos.y - 8), ImVec2(pivotPos.x, pivotPos.y + 8),
                ImGui::ColorConvertFloat4ToU32(accent), 1.5f);
    dl->AddCircleFilled(pivotPos, 4.0f, ImGui::ColorConvertFloat4ToU32(accent));

    ImGui::InvisibleButton("##pivotgiz", ImVec2(w, h));
    if (ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (node && ImGui::IsItemClicked()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float nx = ImClamp((m.x - pos.x) / w, 0.0f, 1.0f);
        const float ny = ImClamp((m.y - pos.y) / h, 0.0f, 1.0f);
        node->pivot = ImVec2(nx, ny);
        s.pivot = NearestPreset(node->pivot);
        widgets::TrackEdit(ctx, inst, "Pivot");
        ctx.Interaction("uGUI Pivot", "Pivot set");
    }
}

void DrawLayoutGroupSection(AppContext& ctx, WidgetInstance& inst, CanvasNode* node) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Layout Group", icons::Icon::PanelLeft))
        return;

    if (!node) {
        widgets::TextMutedWrapped("Select a node to add a layout group.");
        return;
    }

    static const char* kModes[4] = { "None", "Horizontal", "Vertical", "Grid" };
    int mode = static_cast<int>(node->layoutMode);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##layoutmode", &mode, kModes, 4)) {
        node->layoutMode = static_cast<LayoutGroupMode>(mode);
        s.selectedIndex = std::max(0, mode - 1);
        widgets::TrackEdit(ctx, inst, "Layout mode");
        ctx.Interaction("uGUI Layout", std::string("Mode ") + kModes[mode]);
    }
    if (ImGui::DragFloat("##spacing", &node->spacing, 0.5f, 0.0f, 200.0f)) {
        s.spacingX = node->spacing;
        widgets::TrackEdit(ctx, inst, "Spacing");
    }
    if (ImGui::DragFloat2("##padding", &node->paddingX, 0.5f, 0.0f, 200.0f)) {
        s.paddingX = node->paddingX;
        s.paddingY = node->paddingY;
        widgets::TrackEdit(ctx, inst, "Padding");
    }
    static const char* kAlign[3] = { "Start", "Center", "End" };
    int align = node->align;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##align", &align, kAlign, 3)) {
        node->align = align;
        s.alignment = align;
        widgets::TrackEdit(ctx, inst, "Align");
    }
    if (ImGui::Checkbox("Force expand##ugui", &node->forceExpand)) {
        s.toggled = node->forceExpand;
        widgets::TrackEdit(ctx, inst, "Force expand");
    }
}

void DrawContentFitterSection(AppContext& ctx, WidgetInstance& inst, CanvasNode* node) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Content Size Fitter", icons::Icon::Sliders))
        return;

    if (!node) {
        widgets::TextMutedWrapped("Select a node to fit its content.");
        return;
    }
    if (ImGui::Checkbox("Fit horizontal##ugui", &node->contentFitterH)) {
        s.toggled = node->contentFitterH;
        widgets::TrackEdit(ctx, inst, "Fit horizontal");
    }
    if (ImGui::Checkbox("Fit vertical##ugui", &node->contentFitterV)) {
        s.checked = node->contentFitterV;
        widgets::TrackEdit(ctx, inst, "Fit vertical");
    }
}

void DrawScrollRectSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Scroll Rect", icons::Icon::List))
        return;

    const int rows = 40;
    const float rowH = 20.0f;
    const float viewH = ImMax(90.0f, s.size.y);
    const float contentH = static_cast<float>(rows) * rowH;
    const float maxScroll = ImMax(0.0f, contentH - viewH);
    s.maxValue = maxScroll;
    s.value = ImClamp(s.value, 0.0f, maxScroll);

    if (ImGui::BeginChild("##scrollviewport", ImVec2(0.0f, viewH), ImGuiChildFlags_Borders)) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - s.value);
        for (int i = 0; i < rows; ++i) {
            char row[64];
            std::snprintf(row, sizeof(row), "Content item %d", i + 1);
            ImGui::TextUnformatted(row);
        }
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##scroll", &s.value, 0.0f, maxScroll, "%.0f px"))
        widgets::TrackEdit(ctx, inst, "Scroll offset");
    if (ImGui::Checkbox("Inertia##ugui", &s.toggled))
        ctx.Interaction("uGUI Scroll", s.toggled ? "Inertia on" : "Inertia off");
}

void DrawHudSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    DemoShared& d = ctx.demo;
    if (!widgets::SectionHeader("HUD Stats", icons::Icon::Heart))
        return;
    if (!s.visible)
        return;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f%%", static_cast<double>(d.health * 100.0f));
    widgets::KeyValueRow("Health", buf);
    widgets::ProgressBarEx("ug_hud_health", d.health, ImVec2(0.0f, 14.0f), nullptr, s.color, false);
    std::snprintf(buf, sizeof(buf), "%.0f%%", static_cast<double>(d.energy * 100.0f));
    widgets::KeyValueRow("Energy", buf);
    widgets::ProgressBarEx("ug_hud_energy", d.energy, ImVec2(0.0f, 14.0f), nullptr, s.color, false);
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(d.score));
    widgets::KeyValueRow("Score", buf);
    std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(d.ammo));
    widgets::KeyValueRow("Ammo", buf);

    if (ImGui::Button("Take Damage")) {
        d.health -= d.damagePerHit;
        ClampDemo(ctx);
        ctx.Interaction("uGUI HUD", "Took damage");
    }
    ImGui::SameLine();
    if (ImGui::Button("Heal")) {
        d.health = std::min(1.0f, d.health + 0.25f);
        ctx.Interaction("uGUI HUD", "Healed");
    }
}

} // namespace

void DrawUGuiPage(AppContext& ctx) {
    WidgetInstance* canvas = Bind(ctx, "ugui.canvas.main");
    WidgetInstance* rectTarget = Bind(ctx, "ugui.recttransform.target");
    WidgetInstance* anchors = Bind(ctx, "ugui.anchors.presets");
    WidgetInstance* pivot = Bind(ctx, "ugui.pivot.control");
    WidgetInstance* layout = Bind(ctx, "ugui.layoutgroup.panel");
    WidgetInstance* fitter = Bind(ctx, "ugui.contentfitter.panel");
    WidgetInstance* scroll = Bind(ctx, "ugui.scrollrect.view");
    WidgetInstance* hud = Bind(ctx, "ugui.hud.stats");

    if (canvas && canvas->state.visible)
        DrawToolbar(ctx, *canvas);

    // Canvas on the left, inspector-style panel on the right.
    ImGui::Columns(2, "ugui_cols", false);

    if (canvas && canvas->state.visible)
        DrawCanvasSection(ctx, *canvas);

    ImGui::NextColumn();

    CanvasNode* node = ctx.scene.Find(ctx.selection.sceneNodeId);
    if (rectTarget && rectTarget->state.visible)
        DrawRectTransformSection(ctx, *rectTarget, node);
    if (anchors && anchors->state.visible)
        DrawAnchorsPresetSection(ctx, *anchors, node);
    if (pivot && pivot->state.visible)
        DrawPivotSection(ctx, *pivot, node);
    if (layout && layout->state.visible)
        DrawLayoutGroupSection(ctx, *layout, node);
    if (fitter && fitter->state.visible)
        DrawContentFitterSection(ctx, *fitter, node);
    if (scroll && scroll->state.visible)
        DrawScrollRectSection(ctx, *scroll);
    if (hud && hud->state.visible)
        DrawHudSection(ctx, *hud);

    ImGui::Columns(1);

    DrawUnboundList(ctx, Category::UGui);
}

} // namespace demo
} // namespace uilab
