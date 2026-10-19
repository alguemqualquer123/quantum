// DemoUMG.cpp - Unreal UMG designer page: scene hierarchy, an interactive canvas
// renderer, overlay/HUD, widget container, anchors, z-order, transform, progress
// and an interface preview. Inspector edits drive every section live.
#include "demo/DemoHost.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

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

CanvasView s_view;

void ClampAndRespawn(AppContext& ctx) {
    DemoShared& d = ctx.demo;
    d.health = std::max(0.0f, std::min(1.0f, d.health));
    d.energy = std::max(0.0f, std::min(1.0f, d.energy));
    d.ammo = std::max(0.0f, d.ammo);
    d.score = std::max(0.0f, d.score);
    if (d.health <= 0.0f) {
        d.health = 1.0f;
        d.wave += 1;
        ctx.Notify(ToastKind::Warning, "Player Down", "Health reached zero - respawned at full health.");
        ctx.LogConsole(LogLevel::Warning, "UMG", "Respawned player, wave now " + std::to_string(d.wave));
    }
}

// The free-transform node the Widget Transform section drives.
CanvasNode* FindRewardCard(AppContext& ctx) {
    for (CanvasNode& n : ctx.scene.Nodes())
        if (n.name == "RewardCard")
            return &n;
    return nullptr;
}

CanvasNode* ResolveTarget(AppContext& ctx) {
    if (CanvasNode* n = ctx.scene.Find(ctx.selection.sceneNodeId))
        return n;
    return FindRewardCard(ctx);
}

void ApplyPresetToNode(CanvasNode& n, AnchorPreset p) {
    const ImVec2 v = AnchorVec(p);
    n.anchorMin = v;
    n.anchorMax = v;
    n.pivot = v;
}

void SyncTransformToNode(const WidgetState& s, CanvasNode* node) {
    if (!node)
        return;
    node->anchoredPosition = s.position;
    node->sizeDelta = s.size;
    node->scale = s.scale;
    node->rotation = s.rotation;
    node->opacity = s.opacity;
    node->zOrder = s.zOrder;
}

// ---- left column sections --------------------------------------------------

void DrawSceneTree(AppContext& ctx, CanvasNode& node) {
    ImGui::PushID(node.id);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
    if (node.children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (ctx.selection.sceneNodeId == node.id)
        flags |= ImGuiTreeNodeFlags_Selected;

    char label[256];
    std::snprintf(label, sizeof(label), "%s %s##tnode", CanvasNodeTypeIcon(node.type), node.name.c_str());
    const bool open = ImGui::TreeNodeEx(label, flags);
    if (ImGui::IsItemClicked()) {
        ctx.selection.sceneNodeId = node.id;
        ctx.LogDebug("UMG", "Selected node " + node.name);
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 40.0f);
    ImGui::TextDisabled("z%d", node.zOrder);

    if (open) {
        const std::vector<int> kids = node.children;
        for (int child : kids)
            if (CanvasNode* n = ctx.scene.Find(child))
                DrawSceneTree(ctx, *n);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void DrawTreeSection(AppContext& ctx, WidgetInstance& canvas) {
    if (!widgets::SectionHeader("Widget Hierarchy", icons::Icon::Tree))
        return;
    (void)canvas;
    if (ImGui::BeginChild("##umg_tree", ImVec2(0.0f, 240.0f), ImGuiChildFlags_Borders)) {
        if (const CanvasNode* root = ctx.scene.Find(ctx.scene.RootId()))
            DrawSceneTree(ctx, *const_cast<CanvasNode*>(root));
        else
            widgets::TextMuted("Empty scene.");
    }
    ImGui::EndChild();
}

void DrawAnchorsSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Anchors & Alignment", icons::Icon::Link))
        return;

    const char* names[static_cast<int>(AnchorPreset::Count) + 1] = {};
    for (int i = 0; i < static_cast<int>(AnchorPreset::Count); ++i)
        names[i] = AnchorName(static_cast<AnchorPreset>(i));

    CanvasNode* target = ResolveTarget(ctx);
    int preset = s.anchorMin;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##anchor_preset", &preset, names, static_cast<int>(AnchorPreset::Count))) {
        s.anchorMin = s.anchorMax = s.pivot = preset;
        if (target) {
            ApplyPresetToNode(*target, static_cast<AnchorPreset>(preset));
            ctx.Interaction("UMG Anchors", "Anchored '" + target->name + "' to " + names[preset]);
        }
    }
    if (target) {
        ImGui::TextDisabled("target: %s", target->name.c_str());
        widgets::KeyValueRow("anchor min",
            (std::to_string(static_cast<int>(target->anchorMin.x * 100)) + "," +
             std::to_string(static_cast<int>(target->anchorMin.y * 100))).c_str());
    } else {
        widgets::TextMuted("Select a node or keep the reward card to apply anchors.");
    }
}

void DrawZOrderSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Z-Order", icons::Icon::Layers))
        return;

    CanvasNode* target = ResolveTarget(ctx);
    if (target)
        s.intValue = target->zOrder;

    if (icons::IconButton("z_minus", icons::Icon::Minus, "Send backward", ImVec2(26.0f, 24.0f), false, target != nullptr)) {
        ctx.scene.MoveZOrder(target->id, -1);
        ctx.LogEvent("UMG", target->name + " z-order -1");
    }
    ImGui::SameLine();
    if (icons::IconButton("z_plus", icons::Icon::Plus, "Bring forward", ImVec2(26.0f, 24.0f), false, target != nullptr)) {
        ctx.scene.MoveZOrder(target->id, 1);
        ctx.LogEvent("UMG", target->name + " z-order +1");
    }
    ImGui::SameLine();
    const int before = s.intValue;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##z", &s.intValue, s.intMin, s.intMax)) {
        if (target) {
            ctx.scene.MoveZOrder(target->id, s.intValue - before);
            s.intValue = target->zOrder;
            ctx.Interaction("UMG", "Set z-order " + std::to_string(s.intValue));
        }
    }
    if (target)
        ImGui::TextDisabled("target: %s", target->name.c_str());
}

void DrawTransformSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Widget Transform", icons::Icon::Cube))
        return;

    // The canvas re-reads these fields before drawing (see DrawUmgPage).
    ImGui::TextDisabled("%s", s.text.c_str());
    if (ImGui::DragFloat2("##pos", &s.position.x, 0.5f))
        widgets::TrackEdit(ctx, inst, "Position");
    if (ImGui::DragFloat2("##size", &s.size.x, 0.5f, 1.0f, 4000.0f))
        widgets::TrackEdit(ctx, inst, "Size");
    if (ImGui::DragFloat2("##scale", &s.scale.x, 0.01f, 0.05f, 4.0f))
        widgets::TrackEdit(ctx, inst, "Scale");
    if (ImGui::SliderFloat("##rot", &s.rotation, -180.0f, 180.0f, "%.1f deg"))
        widgets::TrackEdit(ctx, inst, "Rotation");
    if (ImGui::SliderFloat("##alpha", &s.opacity, 0.0f, 1.0f))
        widgets::TrackEdit(ctx, inst, "Opacity");
    if (ImGui::DragInt("##z", &s.zOrder))
        widgets::TrackEdit(ctx, inst, "Z order");
}

// ---- right column sections -------------------------------------------------

void DrawCanvasRow(AppContext& ctx, WidgetInstance& canvas) {
    DemoShared& d = ctx.demo;
    const ImVec4 accent = panels::AccentForCategory(Category::UMG);

    if (canvas.state.size.x > 1.0f && canvas.state.size.y > 1.0f) {
        s_view.resolution = ImVec2(canvas.state.size.x, canvas.state.size.y);
    }

    // Control row: zoom / fit / grid / snap.
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("##zoom", &s_view.zoom, 0.2f, 3.0f, "%.2fx"))
        s_view.autoFit = false;
    if (ImGui::IsItemDeactivatedAfterEdit())
        ctx.settings.canvasZoom = s_view.zoom;
    ImGui::SameLine();
    if (ImGui::Checkbox("auto-fit##umg", &s_view.autoFit))
        ctx.Interaction("UMG", s_view.autoFit ? "Auto-fit canvas" : "Manual zoom");
    ImGui::SameLine();
    if (ImGui::Button("Fit")) {
        s_view.autoFit = true;
        ctx.LogEvent("UMG", "Canvas auto-fit");
    }

    CanvasRenderOptions options;
    options.showGrid = ctx.settings.showCanvasGrid;
    options.showBounds = ctx.settings.drawBoundsOverlay;
    options.showAnchors = ctx.settings.drawAnchorMarkers;
    options.interactive = true;
    options.gridStep = ctx.settings.gridSize;
    options.surfaceId = "umg_canvas_surface";
    options.background = canvas.state.color;
    options.transformWidgetId = "umg.transform.widget";

    CanvasNode* transformNode = FindRewardCard(ctx);
    WidgetInstance* transformInst = Bind(ctx, "umg.transform.widget");
    if (transformInst && transformInst->state.visible)
        SyncTransformToNode(transformInst->state, transformNode);

    // Fixed-height child so the canvas does not push the rows below it off-screen.
    if (!ImGui::BeginChild("##umg_canvas", ImVec2(0.0f, 360.0f), ImGuiChildFlags_Borders)) {
        ImGui::EndChild();
        return;
    }

    const CanvasFrameResult result = DrawCanvas(ctx, s_view, options);

    if (result.clickedNode >= 0) {
        ctx.selection.sceneNodeId = result.clickedNode;
        if (const CanvasNode* n = ctx.scene.Find(result.clickedNode))
            ctx.LogDebug("UMG", "Selected " + n->name);
    }
    if (result.dropped && transformNode && ctx.selection.sceneNodeId == transformNode->id) {
        // Read the drag result back into the Widget Transform component.
        if (WidgetInstance* tw = Bind(ctx, "umg.transform.widget")) {
            tw->state.position = transformNode->anchoredPosition;
            widgets::TrackEdit(ctx, *tw, "Position");
        }
    }
    if (result.activatedButton >= 0) {
        const CanvasNode* b = ctx.scene.Find(result.activatedButton);
        if (b) {
            const std::string lower = ToLower(b->name + " " + b->text);
            if (lower.find("reward") != std::string::npos || lower.find("claim") != std::string::npos ||
                lower.find("confirm") != std::string::npos) {
                d.score += 250.0f;
                ctx.Notify(ToastKind::Success, "Reward Claimed", "+250 score");
            } else {
                d.health -= d.damagePerHit;
                ctx.Notify(ToastKind::Warning, "Button Pressed", "-" + FormatFloat(d.damagePerHit, "%.2f") + " health");
            }
            ClampAndRespawn(ctx);
            ctx.Interaction("UMG", "Activated button " + b->name);
        }
    }

    // Overlay HUD painted on top of the canvas surface.
    if (WidgetInstance* overlay = Bind(ctx, "umg.overlay.hud")) {
        if (overlay->state.visible && d.hudVisible) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2& mn = result.surfaceMin;
            const ImVec2& mx = result.surfaceMax;
            const float pad = overlay->state.spacingY;
            const ImVec2 boxMin(mn.x + pad, mn.y + pad);
            const ImVec2 boxMax(std::min(mx.x - pad, boxMin.x + 220.0f * d.hudScale),
                                boxMin.y + 34.0f * d.hudScale);
            dl->AddRectFilled(boxMin, boxMax, ImGui::ColorConvertFloat4ToU32(overlay->state.color), 6.0f);
            dl->AddRect(boxMin, boxMax, ImGui::ColorConvertFloat4ToU32(accent), 6.0f, 0, 1.0f);
            char hud[96];
            std::snprintf(hud, sizeof(hud), "SCORE %d   WAVE %d", static_cast<int>(d.score), d.wave);
            dl->AddText(ImVec2(boxMin.x + 10.0f, boxMin.y + 8.0f), ImGui::GetColorU32(ImGuiCol_Text), hud);
        }
    }
    ImGui::EndChild();
}

void DrawActionRow(AppContext& ctx, WidgetInstance& progress) {
    DemoShared& d = ctx.demo;
    if (ImGui::Button("Take Damage")) {
        d.health -= d.damagePerHit;
        ClampAndRespawn(ctx);
        ctx.Interaction("UMG", "Took damage");
        ctx.Notify(ToastKind::Error, "Damage", "-" + FormatFloat(d.damagePerHit, "%.2f") + " health");
    }
    ImGui::SameLine();
    if (ImGui::Button("Pick Up")) {
        d.score += 100.0f;
        d.ammo += 8.0f;
        d.energy = std::min(1.0f, d.energy + 0.05f);
        ctx.Interaction("UMG", "Picked up loot");
        ctx.Notify(ToastKind::Success, "Picked Up", "+100 score, +8 ammo");
    }

    ImGui::Spacing();
    const float fraction = std::max(0.0f, std::min(1.0f, d.score / 3000.0f));
    const std::string overlay = progress.state.text + "  " + std::to_string(static_cast<int>(d.score));
    widgets::ProgressBarEx("xp", fraction, ImVec2(0.0f, 20.0f), overlay.c_str(), progress.state.color, false);
}

void DrawHudSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    DemoShared& d = ctx.demo;
    if (!widgets::SectionHeader("Player HUD", icons::Icon::Heart))
        return;
    if (!s.visible)
        return;

    const float scale = std::max(0.5f, std::min(1.5f, d.hudScale));
    char buf[32];
    widgets::StatCard("Health", (std::snprintf(buf, sizeof(buf), "%.0f%%", static_cast<double>(d.health * 100.0f)), buf),
                      "", s.color, ImVec2(96.0f * scale, 46.0f * scale));
    ImGui::SameLine();
    widgets::StatCard("Energy", (std::snprintf(buf, sizeof(buf), "%.0f%%", static_cast<double>(d.energy * 100.0f)), buf),
                      "", s.color, ImVec2(96.0f * scale, 46.0f * scale));
    ImGui::SameLine();
    widgets::StatCard("Ammo", (std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(d.ammo)), buf), "",
                      s.color, ImVec2(80.0f * scale, 46.0f * scale));
    ImGui::SameLine();
    widgets::StatCard("Wave", (std::snprintf(buf, sizeof(buf), "%d", d.wave), buf), "", s.color,
                      ImVec2(80.0f * scale, 46.0f * scale));

    widgets::ProgressBarEx("hud_health", d.health, ImVec2(0.0f, 16.0f * scale), "Health", s.color, false);
    widgets::ProgressBarEx("hud_energy", d.energy, ImVec2(0.0f, 16.0f * scale), "Energy", s.color, false);

    if (ImGui::Checkbox("HUD visible##umg", &d.hudVisible))
        ctx.Interaction("UMG", d.hudVisible ? "HUD shown" : "HUD hidden");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##hudscale", &d.hudScale, 0.5f, 1.5f, "%.2fx"))
        widgets::TrackEdit(ctx, inst, "HUD scale");
}

void DrawContainerSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Widget Container", icons::Icon::Grid))
        return;
    if (!s.visible)
        return;

    if (!ImGui::BeginChild("##container", ImVec2(0.0f, 150.0f), ImGuiChildFlags_Borders)) {
        ImGui::EndChild();
        return;
    }
    static const icons::Icon kIcons[5] = { icons::Icon::Heart, icons::Icon::Bolt, icons::Icon::Shield,
                                           icons::Icon::Coin, icons::Icon::Backpack };
    const int count = static_cast<int>(ctx.demo.inventory.size());
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        const bool selected = ctx.demo.inventorySelected == i;
        widgets::BeginCard("slot", ImVec2(96.0f, 96.0f),
                           selected ? WithAlpha(s.color, 0.95f) : WithAlpha(s.color, 0.55f), 6.0f, true);
        const ImVec2 cpos = ImGui::GetWindowPos();
        const ImVec2 csize = ImGui::GetWindowSize();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const std::string& itemName = ctx.demo.inventory[static_cast<size_t>(i)];
        icons::Draw(dl, kIcons[i % 5], ImVec2(cpos.x + csize.x * 0.5f, cpos.y + csize.y * 0.42f), 28.0f,
                    ImGui::GetColorU32(ImGuiCol_Text), 1.6f);
        const std::string label = widgets::Ellipsize(itemName, csize.x - 6.0f);
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(cpos.x + (csize.x - ts.x) * 0.5f, cpos.y + csize.y * 0.68f),
                    ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
        const int n = i < static_cast<int>(ctx.demo.inventoryCounts.size())
                          ? ctx.demo.inventoryCounts[static_cast<size_t>(i)]
                          : 1;
        const std::string countStr = "x" + std::to_string(n);
        const ImVec2 cs2 = ImGui::CalcTextSize(countStr.c_str());
        dl->AddText(ImVec2(cpos.x + csize.x - cs2.x - 6.0f, cpos.y + 4.0f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), countStr.c_str());
        if (ImGui::Selectable("##slot_sel", selected, 0, csize)) {
            ctx.demo.inventorySelected = i;
            ctx.SelectWidget(inst.id);
            ctx.Interaction("UMG", "Selected inventory " + itemName);
        }
        widgets::EndCard();
        if (i + 1 < count)
            ImGui::SameLine(0.0f, ImMax(4.0f, s.spacingX));
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void DrawPreviewSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    DemoShared& d = ctx.demo;
    if (!widgets::SectionHeader("Interface Preview", icons::Icon::Camera))
        return;
    if (!s.visible)
        return;

    const float aspect = 16.0f / 9.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImMax(120.0f, width / aspect);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 max(origin.x + width, origin.y + height);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (ctx.textures && ctx.textures->Preview().Valid()) {
        dl->AddImage(ctx.textures->Preview().Ref(), origin, max);
    } else {
        // Procedural sky/ground gradient so the preview works headless.
        const ImU32 sky = ImGui::ColorConvertFloat4ToU32(ImVec4(0.10f, 0.20f, 0.35f, 1.0f));
        const ImU32 horizon = ImGui::ColorConvertFloat4ToU32(ImVec4(0.55f, 0.35f, 0.25f, 1.0f));
        const ImU32 ground = ImGui::ColorConvertFloat4ToU32(ImVec4(0.08f, 0.12f, 0.10f, 1.0f));
        const float mid = origin.y + height * 0.6f;
        dl->AddRectFilledMultiColor(origin, ImVec2(max.x, mid), sky, sky, horizon, horizon);
        dl->AddRectFilledMultiColor(ImVec2(origin.x, mid), max, ground, ground, ground, ground);
    }

    // Animated element uses the shared page clock (pauses with F6).
    const float t = PageClock(ctx, 0.25f);
    const float sunX = origin.x + t * width;
    const float sunY = origin.y + height * 0.3f;
    dl->AddCircleFilled(ImVec2(sunX, sunY), 12.0f, ImGui::ColorConvertFloat4ToU32(s.color));

    dl->AddRect(origin, max, ImGui::GetColorU32(ImGuiCol_Border), 4.0f, 0, 1.0f);
    dl->AddText(ImVec2(origin.x + 8.0f, origin.y + 6.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                d.playing ? "PLAYING" : "PAUSED");
    ImGui::Dummy(ImVec2(width, height));

    if (icons::IconButton("p_play", icons::Icon::Play, "Play", ImVec2(30.0f, 26.0f), d.playing)) {
        d.playing = true;
        ctx.Interaction("UMG", "Preview play");
    }
    ImGui::SameLine();
    if (icons::IconButton("p_pause", icons::Icon::Pause, "Pause", ImVec2(30.0f, 26.0f), !d.playing)) {
        d.playing = false;
        ctx.Interaction("UMG", "Preview pause");
    }
    ImGui::SameLine();
    if (icons::IconButton("p_stop", icons::Icon::Stop, "Stop", ImVec2(30.0f, 26.0f))) {
        d.playing = false;
        d.time = 0.0f;
        ctx.Interaction("UMG", "Preview stop");
    }
}

} // namespace

void DrawUmgPage(AppContext& ctx) {
    WidgetInstance* canvas = Bind(ctx, "umg.canvas.main");
    if (!canvas || !canvas->state.visible)
        canvas = nullptr;

    WidgetInstance* anchors = Bind(ctx, "umg.anchors.controls");
    WidgetInstance* zorder = Bind(ctx, "umg.zorder.controls");
    WidgetInstance* transform = Bind(ctx, "umg.transform.widget");
    WidgetInstance* progress = Bind(ctx, "umg.progress.bar");
    WidgetInstance* hud = Bind(ctx, "umg.hud.status");
    WidgetInstance* container = Bind(ctx, "umg.container.panel");
    WidgetInstance* preview = Bind(ctx, "umg.preview.screen");

    // Two-column design layout: authoring tree/controls on the left, live canvas
    // and gameplay on the right.
    ImGui::Columns(2, "umg_cols", false);

    // ---- left column ----
    if (canvas)
        DrawTreeSection(ctx, *canvas);
    if (anchors && anchors->state.visible)
        DrawAnchorsSection(ctx, *anchors);
    if (zorder && zorder->state.visible)
        DrawZOrderSection(ctx, *zorder);
    if (transform && transform->state.visible)
        DrawTransformSection(ctx, *transform);

    ImGui::NextColumn();

    // ---- right column ----
    if (canvas)
        DrawCanvasRow(ctx, *canvas);
    if (progress && progress->state.visible)
        DrawActionRow(ctx, *progress);
    if (hud && hud->state.visible)
        DrawHudSection(ctx, *hud);
    if (container && container->state.visible)
        DrawContainerSection(ctx, *container);
    if (preview && preview->state.visible)
        DrawPreviewSection(ctx, *preview);

    ImGui::Columns(1);

    DrawUnboundList(ctx, Category::UMG);
}

} // namespace demo
} // namespace uilab
