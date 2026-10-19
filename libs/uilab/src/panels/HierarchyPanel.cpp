// HierarchyPanel.cpp - outliner for the canvas scene shared by the UMG/uGUI
// demo pages (rect transforms, parenting, z-order).
#include "panels/Panels.h"

#include <cstdio>
#include <functional>
#include <string>

#include <imgui.h>

#include "core/AppContext.h"
#include "core/Dialogs.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace panels {

namespace {

int AddChildNode(AppContext& ctx, int parentId, CanvasNodeType type) {
    CanvasNode node;
    node.type = type;
    node.parent = parentId;
    node.name = std::string(CanvasNodeTypeName(type)) + " " + std::to_string(ctx.scene.Count() + 1);
    node.anchoredPosition = ImVec2(24.0f, 24.0f);
    node.sizeDelta = ImVec2(180.0f, 48.0f);
    switch (type) {
    case CanvasNodeType::Button:      node.text = "Button"; node.color = ImVec4(0.26f, 0.45f, 0.85f, 1.0f); break;
    case CanvasNodeType::Label:       node.text = "Label"; node.color = ImVec4(0, 0, 0, 0); break;
    case CanvasNodeType::ProgressBar: node.value = 0.5f; node.color = ImVec4(0.33f, 0.78f, 0.42f, 1.0f); break;
    case CanvasNodeType::Image:       node.color = ImVec4(0.62f, 0.47f, 0.96f, 0.9f); break;
    case CanvasNodeType::InventorySlot: node.color = ImVec4(0.20f, 0.24f, 0.32f, 1.0f); break;
    case CanvasNodeType::HealthBar:   node.value = 0.8f; node.color = ImVec4(0.85f, 0.30f, 0.30f, 1.0f); break;
    case CanvasNodeType::ScrollView:  node.sizeDelta = ImVec2(320.0f, 180.0f); node.color = ImVec4(0.12f, 0.13f, 0.17f, 1.0f); break;
    default:                          node.color = ImVec4(0.22f, 0.26f, 0.34f, 1.0f); break;
    }
    const int id = ctx.scene.AddNode(node);
    ctx.selection.sceneNodeId = id;
    ctx.LogEvent("Hierarchy", "Added " + node.name);
    return id;
}

void NodeContextMenu(AppContext& ctx, CanvasNode& node) {
    if (!ImGui::BeginPopupContextItem("##node_ctx"))
        return;
    ImGui::TextDisabled("%s  (id %d)", CanvasNodeTypeName(node.type), node.id);
    ImGui::Separator();
    if (ImGui::MenuItem("Rename...")) {
        const int id = node.id;
        ctx.dialogs.TextInput("Rename Node", "Node name", node.name, "nonempty", "",
                              [&ctx, id](bool ok, std::string value) {
            if (!ok)
                return;
            if (CanvasNode* n = ctx.scene.Find(id)) {
                n->name = value;
                ctx.LogEvent("Hierarchy", "Renamed node to " + value);
            }
        });
    }
    if (ImGui::MenuItem("Duplicate")) {
        const int copy = ctx.scene.DuplicateNode(node.id);
        if (copy >= 0) {
            ctx.selection.sceneNodeId = copy;
            ctx.LogEvent("Hierarchy", "Duplicated node " + node.name);
        }
    }
    if (ImGui::MenuItem("Add Button Child"))
        AddChildNode(ctx, node.id, CanvasNodeType::Button);
    if (ImGui::MenuItem("Add Label Child"))
        AddChildNode(ctx, node.id, CanvasNodeType::Label);
    ImGui::Separator();
    if (ImGui::MenuItem("Move Up (z)")) {
        ctx.scene.MoveZOrder(node.id, 1);
        ctx.LogEvent("Hierarchy", node.name + " moved up in z-order");
    }
    if (ImGui::MenuItem("Move Down (z)")) {
        ctx.scene.MoveZOrder(node.id, -1);
        ctx.LogEvent("Hierarchy", node.name + " moved down in z-order");
    }
    if (ImGui::MenuItem("Send To Back"))
        ctx.scene.MoveZOrder(node.id, -100);
    if (ImGui::MenuItem("Bring To Front"))
        ctx.scene.MoveZOrder(node.id, 100);
    ImGui::Separator();
    if (ImGui::MenuItem("Reparent To Root"))
        ctx.scene.SetParent(node.id, ctx.scene.RootId());
    if (ImGui::MenuItem("Delete")) {
        const std::string name = node.name;
        const int id = node.id;
        if (ctx.scene.RemoveNode(id)) {
            if (ctx.selection.sceneNodeId == id)
                ctx.selection.sceneNodeId = -1;
            ctx.LogConsole(LogLevel::Warning, "Hierarchy", "Deleted node: " + name);
        }
    }
    ImGui::EndPopup();
}

} // namespace

void DrawHierarchy(AppContext& ctx) {
    const HostPanel& panel = ctx.host.hierarchyPanel;
    if (!ImGui::Begin(panel.titleOr(kHierarchyTitle), panel.pOpen)) {
        ImGui::End();
        return;
    }

    if (ctx.host.hierarchy) {
        ctx.host.hierarchy(ctx);
        ImGui::End();
        return;
    }

    // ---- toolbar ---------------------------------------------------------
    static int addType = 0;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 150.0f);
    const char* types[] = { "Panel", "Button", "Label", "ProgressBar", "Image", "InventorySlot", "HealthBar", "ScrollView" };
    ImGui::Combo("##add_type", &addType, types, IM_ARRAYSIZE(types));
    ImGui::SameLine();
    if (ImGui::Button("Add Child")) {
        const int parent = ctx.selection.sceneNodeId >= 0 ? ctx.selection.sceneNodeId : ctx.scene.RootId();
        AddChildNode(ctx, parent, static_cast<CanvasNodeType>(addType));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Adds the node as a child of the current selection (or of the root).");

    const bool hasSel = ctx.selection.sceneNodeId >= 0 && ctx.scene.Find(ctx.selection.sceneNodeId) != nullptr;
    ImGui::SameLine();
    if (icons::IconButton("##hier_dup", icons::Icon::Copy, "Duplicate selected node", ImVec2(24, 24), false, hasSel)) {
        ctx.scene.DuplicateNode(ctx.selection.sceneNodeId);
        ctx.LogEvent("Hierarchy", "Duplicated selected node");
    }
    ImGui::SameLine();
    if (icons::IconButton("##hier_up", icons::Icon::ChevronUp, "Move up in z-order", ImVec2(24, 24), false, hasSel)) {
        ctx.scene.MoveZOrder(ctx.selection.sceneNodeId, 1);
        ctx.LogEvent("Hierarchy", "Z-order +1");
    }
    ImGui::SameLine();
    if (icons::IconButton("##hier_down", icons::Icon::ChevronDown, "Move down in z-order", ImVec2(24, 24), false, hasSel)) {
        ctx.scene.MoveZOrder(ctx.selection.sceneNodeId, -1);
        ctx.LogEvent("Hierarchy", "Z-order -1");
    }
    ImGui::SameLine();
    if (icons::IconButton("##hier_del", icons::Icon::Trash, "Delete selected node", ImVec2(24, 24), false, hasSel)) {
        const int id = ctx.selection.sceneNodeId;
        const std::string name = ctx.scene.Find(id) ? ctx.scene.Find(id)->name : std::string("node");
        if (ctx.scene.RemoveNode(id)) {
            ctx.selection.sceneNodeId = -1;
            ctx.LogConsole(LogLevel::Warning, "Hierarchy", "Deleted node: " + name);
        }
    }

    ImGui::Separator();

    // ---- tree ------------------------------------------------------------
    std::function<void(int)> drawNode = [&](int id) {
        CanvasNode* node = ctx.scene.Find(id);
        if (!node)
            return;
        ImGui::PushID(id);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
        if (node->children.empty())
            flags |= ImGuiTreeNodeFlags_Leaf;
        if (ctx.selection.sceneNodeId == id)
            flags |= ImGuiTreeNodeFlags_Selected;

        char label[256];
        std::snprintf(label, sizeof(label), "%s  %s##node", CanvasNodeTypeIcon(node->type), node->name.c_str());
        const bool open = ImGui::TreeNodeEx(label, flags);

        if (ImGui::IsItemClicked()) {
            ctx.selection.sceneNodeId = id;
            ctx.LogDebug("Hierarchy", "Selected node " + node->name);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nz: %d | children: %d | pos (%.0f, %.0f) | size (%.0f, %.0f)",
                              CanvasNodeTypeName(node->type), node->zOrder,
                              static_cast<int>(node->children.size()),
                              static_cast<double>(node->anchoredPosition.x),
                              static_cast<double>(node->anchoredPosition.y),
                              static_cast<double>(node->sizeDelta.x), static_cast<double>(node->sizeDelta.y));
        NodeContextMenu(ctx, *node);

        // Right aligned z-order badge.
        ImGui::SameLine(ImGui::GetWindowWidth() - 44.0f);
        ImGui::TextDisabled("z%d", node->zOrder);

        if (open) {
            const std::vector<int> kids = node->children; // copy: children may change below
            for (int child : kids)
                drawNode(child);
            ImGui::TreePop();
        }
        ImGui::PopID();
    };

    if (ctx.scene.RootId() >= 0)
        drawNode(ctx.scene.RootId());

    if (ctx.scene.RootId() < 0) {
        ImGui::TextDisabled("The scene is empty.");
        if (ImGui::Button("Build HUD sample"))
            ctx.scene.BuildDefaultScene();
    }

    // ---- footer ----------------------------------------------------------
    ImGui::Separator();
    widgets::TextMutedWrapped("%d nodes (%d panels, %d buttons)", ctx.scene.Count(),
                              ctx.scene.CountOfType(CanvasNodeType::Panel),
                              ctx.scene.CountOfType(CanvasNodeType::Button));

    if (const CanvasNode* sel = ctx.scene.Find(ctx.selection.sceneNodeId)) {
        ImGui::Text("Selected: %s", sel->name.c_str());
        ImGui::TextDisabled("rect  %.0f,%.0f  %.0fx%.0f", static_cast<double>(sel->computed.min.x),
                            static_cast<double>(sel->computed.min.y),
                            static_cast<double>(sel->computed.Width()),
                            static_cast<double>(sel->computed.Height()));
    } else {
        widgets::TextMutedWrapped("Nothing selected. Click a node to select it.");
    }
    if (ImGui::Button("Rebuild HUD sample")) {
        ctx.scene.BuildDefaultScene();
        ctx.selection.sceneNodeId = -1;
        ctx.LogConsole(LogLevel::Success, "Hierarchy", "HUD sample scene rebuilt.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Scene")) {
        ctx.dialogs.Confirm("Clear Scene", "Remove every canvas node except the root?", true, [&ctx](bool ok) {
            if (!ok)
                return;
            ctx.scene.Clear();
            ctx.scene.BuildDefaultScene();
            ctx.selection.sceneNodeId = -1;
            ctx.LogConsole(LogLevel::Warning, "Hierarchy", "Scene reset to the default HUD sample.");
        });
    }

    ImGui::End();
}

} // namespace panels
} // namespace uilab
