// ComponentTreePanel.cpp - left panel listing every component by category/group.
#include "panels/Panels.h"

#include <algorithm>
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

bool MatchesSearch(const WidgetInstance& w, const std::string& query) {
    if (query.empty())
        return true;
    return ContainsNoCase(w.displayName, query) || ContainsNoCase(w.id, query) ||
           ContainsNoCase(KindName(w.kind), query) || ContainsNoCase(w.group, query);
}

void RowContextMenu(AppContext& ctx, WidgetInstance& w) {
    if (!ImGui::BeginPopupContextItem("##row_ctx"))
        return;
    ImGui::TextDisabled("%s", w.id.c_str());
    ImGui::TextDisabled("%s / %s", CategoryShortName(w.category), KindName(w.kind));
    ImGui::Separator();
    if (ImGui::MenuItem("Rename...")) {
        const std::string id = w.id;
        ctx.dialogs.TextInput("Rename Component", "Display name", w.displayName, "nonempty", "",
                              [&ctx, id](bool ok, std::string value) {
            if (!ok)
                return;
            std::string error;
            if (ctx.registry.Rename(id, value, error)) {
                ctx.LogEvent("Component Tree", "Renamed component to " + value);
            } else {
                ctx.LogConsole(LogLevel::Error, "Component Tree", "Rename failed: " + error);
                ctx.Notify(ToastKind::Error, "Rename failed", error);
            }
        });
    }
    if (ImGui::MenuItem("Duplicate")) {
        ctx.SelectWidget(w.id);
        ctx.DuplicateSelectedWidget();
    }
    if (ImGui::MenuItem("Copy Properties")) {
        ctx.SelectWidget(w.id);
        ctx.CopySelectedProperties();
    }
    if (ImGui::MenuItem("Paste Properties", nullptr, false, !ctx.propertyClipboard.empty())) {
        ctx.SelectWidget(w.id);
        ctx.PastePropertiesIntoSelected();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Reveal On Demo Page"))
        RevealComponent(ctx, w.id);
    if (ImGui::MenuItem("Reset Properties"))
        ctx.ResetWidgetState(w);
    if (ImGui::MenuItem(w.state.enabled ? "Disable" : "Enable")) {
        w.state.enabled = !w.state.enabled;
        ctx.LogEvent("Component Tree", w.displayName + (w.state.enabled ? " enabled" : " disabled"));
    }
    if (ImGui::MenuItem(w.state.visible ? "Hide" : "Show")) {
        w.state.visible = !w.state.visible;
        ctx.LogEvent("Component Tree", w.displayName + (w.state.visible ? " shown" : " hidden"));
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete...", widgets::ShortcutText("delete")))
        RequestDeleteComponent(ctx, w);
    ImGui::EndPopup();
}

bool ComponentRow(AppContext& ctx, WidgetInstance& w, float indent) {
    const bool selected = ctx.selection.widgetId == w.id;
    const ImVec4 accent = AccentForCategory(w.category);

    ImGui::PushID(w.id.c_str());
    if (indent > 0.0f)
        ImGui::Indent(indent);

    char label[256];
    std::snprintf(label, sizeof(label), "%s##row", w.displayName.c_str());

    const std::string tooltip = w.id + "\n" + KindName(w.kind) +
                                (w.liveBound ? "\nlive: rendered from the demo page" : "\nnot bound to a demo");
    bool clicked = widgets::SelectableRow(label, selected, IconForKind(w.kind),
                                          w.state.enabled ? accent : ImVec4(0.45f, 0.47f, 0.52f, 1.0f),
                                          tooltip.c_str());
    // Right aligned state markers.
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 34.0f);
    if (!w.state.visible) {
        icons::IconButton("##vis", icons::Icon::EyeOff, "Hidden (click to show)", ImVec2(16.0f, 16.0f), false,
                          true, 0.8f);
        if (ImGui::IsItemClicked()) {
            w.state.visible = true;
            ctx.LogEvent("Component Tree", w.displayName + " shown");
        }
    } else if (!w.state.enabled) {
        icons::IconButton("##dis", icons::Icon::Lock, "Disabled (click to enable)", ImVec2(16.0f, 16.0f), false,
                          true, 0.8f);
        if (ImGui::IsItemClicked()) {
            w.state.enabled = true;
            ctx.LogEvent("Component Tree", w.displayName + " enabled");
        }
    }

    if (clicked) {
        ctx.SelectWidget(w.id);
        ctx.activeCategory = w.category;
        ctx.LogDebug("Component Tree", "Selected " + w.displayName);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        RevealComponent(ctx, w.id);

    RowContextMenu(ctx, w);

    if (indent > 0.0f)
        ImGui::Unindent(indent);
    ImGui::PopID();
    return clicked;
}

} // namespace

void DrawComponentTree(AppContext& ctx) {
    const HostPanel& panel = ctx.host.componentTreePanel;
    if (!ImGui::Begin(panel.titleOr(kComponentTreeTitle), panel.pOpen)) {
        ImGui::End();
        return;
    }

    if (ctx.host.componentTree) {
        ctx.host.componentTree(ctx);
        ImGui::End();
        return;
    }

    // ---- header ----------------------------------------------------------
    widgets::SearchField("##tree_search", ctx.demo.searchQuery, "Filter components...", -1.0f);
    ImGui::Spacing();

    const float btn = 26.0f;
    if (icons::IconButton("##tree_create", icons::Icon::Plus, "Create component...", ImVec2(btn, btn)))
        OpenCreateComponentDialog(ctx);
    ImGui::SameLine();
    if (icons::IconButton("##tree_dup", icons::Icon::Copy, "Duplicate selected", ImVec2(btn, btn), false,
                          ctx.SelectedWidget() != nullptr))
        ctx.DuplicateSelectedWidget();
    ImGui::SameLine();
    if (icons::IconButton("##tree_reset", icons::Icon::Refresh, "Reset selected properties", ImVec2(btn, btn), false,
                          ctx.SelectedWidget() != nullptr)) {
        if (WidgetInstance* sel = ctx.SelectedWidget())
            ctx.ResetWidgetState(*sel);
    }
    ImGui::SameLine();
    if (icons::IconButton("##tree_del", icons::Icon::Trash, "Delete selected component", ImVec2(btn, btn), false,
                          ctx.SelectedWidget() != nullptr)) {
        if (WidgetInstance* sel = ctx.SelectedWidget())
            RequestDeleteComponent(ctx, *sel);
    }

    static bool liveOnly = false;
    ImGui::SameLine();
    ImGui::Checkbox("##live_only", &liveOnly);
    ImGui::SameLine();
    ImGui::TextDisabled("live");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show only components bound to a demo page");

    ImGui::Separator();

    // ---- category tree ---------------------------------------------------
    // Counted up front: collapsed categories render no rows but still match the filter.
    int matched = 0;
    for (const auto& item : ctx.registry.All()) {
        if (!MatchesSearch(*item, ctx.demo.searchQuery))
            continue;
        if (liveOnly && !item->liveBound)
            continue;
        ++matched;
    }

    // The footer is pinned outside this child so it stays visible while the
    // category list scrolls.
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + 8.0f;
    if (ImGui::BeginChild("##tree_rows", ImVec2(0.0f, -footerHeight))) {
        for (int ci = 0; ci < static_cast<int>(Category::Count); ++ci) {
            const Category c = static_cast<Category>(ci);
            const int total = ctx.registry.CountCategory(c);
            if (total == 0)
                continue;

            const bool active = ctx.activeCategory == c;
            const ImVec4 accent = AccentForCategory(c);

            // The count is right-aligned, so shorten the name instead of letting
            // the two collide in a narrow dock. 96px covers the arrow, the count
            // column and the window padding.
            const float nameMax = std::max(40.0f, ImGui::GetWindowWidth() - 96.0f);
            char header[192];
            std::snprintf(header, sizeof(header), "%s##cat%d",
                          widgets::Ellipsize(CategoryName(c), nameMax).c_str(), ci);

            ImGui::PushStyleColor(ImGuiCol_Text, active ? accent : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            const ImGuiTreeNodeFlags catFlags =
                ImGuiTreeNodeFlags_SpanAvailWidth | (active ? ImGuiTreeNodeFlags_Selected : 0) |
                (ctx.demo.searchQuery.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen);
            const bool open = ImGui::TreeNodeEx(header, catFlags);
            ImGui::PopStyleColor();
            if (ImGui::IsItemClicked()) {
                if (ctx.activeCategory != c) {
                    ctx.SelectCategory(c);
                    ctx.LogEvent("Component Tree", std::string("Opened ") + CategoryName(c) + " page");
                }
            }
            ImGui::SameLine(ImGui::GetWindowWidth() - 40.0f);
            ImGui::TextDisabled("%d", total);

            if (open) {
                for (const std::string& group : ctx.registry.GroupsOf(c)) {
                    char groupId[192];
                    std::snprintf(groupId, sizeof(groupId), "%s##grp%d_%s", group.c_str(), ci, group.c_str());
                    const bool groupOpen = ImGui::TreeNodeEx(groupId, ImGuiTreeNodeFlags_SpanAvailWidth |
                                                                          ImGuiTreeNodeFlags_DefaultOpen);
                    if (groupOpen) {
                        for (const auto& item : ctx.registry.All()) {
                            if (item->category != c || item->group != group)
                                continue;
                            if (!MatchesSearch(*item, ctx.demo.searchQuery))
                                continue;
                            if (liveOnly && !item->liveBound)
                                continue;
                            ComponentRow(ctx, *item, 6.0f);
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
        }
    }
    ImGui::EndChild();

    // ---- footer ----------------------------------------------------------
    widgets::TextMutedWrapped("%d matching / %d components | %d live", matched,
                              static_cast<int>(ctx.registry.Count()), ctx.registry.CountLive());

    ImGui::End();
}

} // namespace panels
} // namespace uilab
