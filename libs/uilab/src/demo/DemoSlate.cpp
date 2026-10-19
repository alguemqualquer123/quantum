// DemoSlate.cpp - Unreal-editor style Slate page: details, asset browser,
// hierarchy, searchable tree, context menu and command toolbar.
#include "demo/DemoHost.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "core/AppContext.h"
#include "core/Dialogs.h"
#include "core/Notifications.h"
#include "core/Types.h"
#include "panels/AssetLibrary.h"
#include "panels/Panels.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace demo {

namespace {

// Mock actor property values backing the Details panel.
struct SlateDetailsModel {
    float rotation = 15.0f;
    float scale = 1.0f;
    ImVec2 location = ImVec2(120.0f, 40.0f);
    bool visible = true;
    bool castShadows = true;
    int materialSlot = 0;
};
SlateDetailsModel s_details;

// Maps an asset name to a Content Browser asset type using the /Game prefixes.
AssetType GuessAssetType(const std::string& name) {
    const auto starts = [&name](const char* p) {
        return name.rfind(p, 0) == 0;
    };
    if (starts("SK_") || starts("SM_")) return AssetType::Mesh;
    if (starts("M_"))  return AssetType::Material;
    if (starts("T_"))  return AssetType::Texture;
    if (starts("ABP_")) return AssetType::Animation;
    if (starts("BP_"))  return AssetType::Blueprint;
    return AssetType::Widget;
}

icons::Icon IconForCommand(const std::string& name) {
    const std::string lower = ToLower(name);
    if (lower.find("new") != std::string::npos)     return icons::Icon::New;
    if (lower.find("compile") != std::string::npos) return icons::Icon::Refresh;
    if (lower.find("play") != std::string::npos)    return icons::Icon::Play;
    if (lower.find("simulate") != std::string::npos) return icons::Icon::Step;
    if (lower.find("save") != std::string::npos)    return icons::Icon::SaveAll;
    return icons::Icon::Gear;
}

void DrawDetailsSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Details", icons::Icon::Sliders))
        return;

    if (ImGui::BeginChild("##details_card", ImVec2(0.0f, 220.0f), ImGuiChildFlags_Borders)) {
        ImGui::TextDisabled("Actor");
        ImGui::PushStyleColor(ImGuiCol_Text, panels::AccentForCategory(Category::Slate));
        ImGui::TextUnformatted(s.text.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();

        const int count = static_cast<int>(s.items.size());
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            const bool first = i == 0;
            const bool open = ImGui::CollapsingHeader(s.items[static_cast<size_t>(i)].c_str(),
                first ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            if (open) {
                ImGui::Indent();
                if (i == 0) {
                    // Transform category.
                    ImGui::TextUnformatted("Location X");
                    ImGui::SameLine(140.0f);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::DragFloat("##locx", &s_details.location.x, 0.5f))
                        widgets::TrackEdit(ctx, inst, "Location X");
                    ImGui::TextUnformatted("Location Y");
                    ImGui::SameLine(140.0f);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::DragFloat("##locy", &s_details.location.y, 0.5f))
                        widgets::TrackEdit(ctx, inst, "Location Y");
                    ImGui::TextUnformatted("Rotation");
                    ImGui::SameLine(140.0f);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::SliderFloat("##rot", &s_details.rotation, -180.0f, 180.0f, "%.1f"))
                        widgets::TrackEdit(ctx, inst, "Rotation");
                    ImGui::TextUnformatted("Scale");
                    ImGui::SameLine(140.0f);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::SliderFloat("##scale", &s_details.scale, 0.1f, 4.0f, "%.2f"))
                        widgets::TrackEdit(ctx, inst, "Scale");
                } else if (i == 1) {
                    // Static Mesh category.
                    ImGui::TextUnformatted("Visible");
                    ImGui::SameLine(140.0f);
                    if (ImGui::Checkbox("##vis", &s_details.visible)) {
                        widgets::TrackEdit(ctx, inst, "Visible");
                        ctx.Interaction("Details", s_details.visible ? "mesh visible" : "mesh hidden");
                    }
                    ImGui::TextUnformatted("Cast Shadows");
                    ImGui::SameLine(140.0f);
                    if (ImGui::Checkbox("##cast", &s_details.castShadows))
                        widgets::TrackEdit(ctx, inst, "Cast Shadows");
                } else if (i == 2) {
                    // Materials category.
                    ImGui::TextUnformatted("Material Slot");
                    ImGui::SameLine(140.0f);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::Combo("##slot", &s_details.materialSlot, "Default\0Instanced\0Dynamic\0")) {
                        widgets::TrackEdit(ctx, inst, "Material Slot");
                        ctx.LogEvent("Details", "Material slot changed");
                    }
                } else {
                    // Lighting / Tags categories reuse the value slider as a generic float property.
                    ImGui::TextUnformatted("Intensity");
                    ImGui::SameLine(140.0f);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::SliderFloat(("##gen" + std::to_string(i)).c_str(), &s_details.scale, 0.0f, 4.0f, "%.2f"))
                        widgets::TrackEdit(ctx, inst, "Generic value");
                }
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void DrawAssetBrowserSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Content Browser", icons::Icon::Folder))
        return;

    const float areaH = 240.0f;
    if (!ImGui::BeginChild("##browser", ImVec2(0.0f, areaH), ImGuiChildFlags_Borders)) {
        ImGui::EndChild();
        return;
    }

    // Path bar.
    ImGui::AlignTextToFramePadding();
    icons::IconText(icons::Icon::Folder, s.text.c_str(), ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::SameLine();
    ImGui::TextDisabled("(%d items)", static_cast<int>(s.items.size()));
    ImGui::Separator();

    const int count = static_cast<int>(s.items.size());
    const float tile = 92.0f;
    const float gap = 8.0f;
    const float availW = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>((availW + gap) / (tile + gap)));
    const ImVec2 start = ImGui::GetCursorScreenPos();

    for (int i = 0; i < count; ++i) {
        const int col = i % cols;
        const int row = i / cols;
        ImGui::SetCursorScreenPos(ImVec2(start.x + static_cast<float>(col) * (tile + gap),
                                         start.y + static_cast<float>(row) * (tile + gap)));

        const std::string& name = s.items[static_cast<size_t>(i)];
        const AssetType type = GuessAssetType(name);
        const ImVec4 col4 = AssetTypeColor(type);
        const bool selected = s.selectedIndex == i;

        char cardId[32];
        std::snprintf(cardId, sizeof(cardId), "asset_%d", i);
        widgets::BeginCard(cardId, ImVec2(tile, tile),
                           selected ? WithAlpha(panels::AccentForCategory(Category::Slate), 0.28f)
                                    : WithAlpha(col4, 0.12f),
                           6.0f, true);

        const ImVec2 cpos = ImGui::GetWindowPos();
        const ImVec2 csize = ImGui::GetWindowSize();
        const ImVec2 center(cpos.x + csize.x * 0.5f, cpos.y + csize.y * 0.42f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        icons::Draw(dl, icons::ForAssetType(static_cast<int>(type)), center, 30.0f,
                    ImGui::ColorConvertFloat4ToU32(col4), 1.7f);
        const std::string label = widgets::Ellipsize(name, csize.x - 6.0f);
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(cpos.x + (csize.x - ts.x) * 0.5f, cpos.y + csize.y - ts.y - 6.0f),
                    ImGui::GetColorU32(ImGuiCol_Text), label.c_str());

        if (ImGui::Selectable("##tile", selected, 0, csize)) {
            s.selectedIndex = i;
            ctx.SelectWidget(inst.id);
            ctx.Interaction("Asset Browser", "Selected " + name);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s", name.c_str(), AssetTypeName(type));

        widgets::EndCard();
    }

    const int rows = (count + cols - 1) / cols;
    const float gridH = std::max(1, rows) * (tile + gap);
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + gridH + 4.0f));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));   // grows the child so the grid can scroll
    ImGui::EndChild();
}

void DrawHierarchySection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Scene Outliner", icons::Icon::Tree))
        return;

    if (!ImGui::BeginChild("##hier", ImVec2(0.0f, 180.0f), ImGuiChildFlags_Borders)) {
        ImGui::EndChild();
        return;
    }

    const int count = static_cast<int>(s.items.size());
    for (int i = 0; i < count; ++i) {
        const std::string& name = s.items[static_cast<size_t>(i)];
        const bool selected = s.selectedIndex == i;
        ImGui::PushID(i);
        if (widgets::SelectableRow(name.c_str(), selected, icons::Icon::Cube,
                                   widgets::ColorFromIndex(i))) {
            s.selectedIndex = i;
            ctx.SelectWidget(inst.id);
            ctx.Interaction("Hierarchy", "Selected " + name);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void DrawSearchTreeSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Searchable Tree", icons::Icon::Search))
        return;

    ImGui::SetNextItemWidth(-FLT_MIN);
    widgets::SearchField("slate_search", s.text, s.hint.c_str(), ImGui::GetContentRegionAvail().x);

    // Collect hits so the visible count is exact.
    std::vector<int> hits;
    for (int i = 0; i < static_cast<int>(s.items.size()); ++i)
        if (ContainsNoCase(s.items[static_cast<size_t>(i)], s.text))
            hits.push_back(i);

    widgets::TextMuted("%d match%s for \"%s\"", static_cast<int>(hits.size()),
                       static_cast<int>(hits.size()) == 1 ? "" : "es", s.text.empty() ? "*" : s.text.c_str());

    if (!ImGui::BeginChild("##tree", ImVec2(0.0f, 150.0f), ImGuiChildFlags_Borders)) {
        ImGui::EndChild();
        return;
    }
    const bool rootOpen = ImGui::TreeNodeEx("World", ImGuiTreeNodeFlags_DefaultOpen |
                                                       ImGuiTreeNodeFlags_SpanAvailWidth);
    if (rootOpen) {
        for (int idx : hits) {
            const std::string& name = s.items[static_cast<size_t>(idx)];
            const bool selected = s.selectedIndex == idx;
            ImGui::PushID(idx);
            if (widgets::SelectableRow(name.c_str(), selected, icons::Icon::Point,
                                       panels::AccentForCategory(Category::Slate))) {
                s.selectedIndex = idx;
                ctx.SelectWidget(inst.id);
                ctx.Interaction("Search Tree", name);
            }
            ImGui::PopID();
        }
        if (hits.empty())
            widgets::TextMuted("No actors match the filter.");
        ImGui::TreePop();
    }
    ImGui::EndChild();
}

void DrawContextMenuSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Context Menu", icons::Icon::List))
        return;

    widgets::BeginCard("slate_ctx_card", ImVec2(0.0f, 80.0f),
                       WithAlpha(panels::AccentForCategory(Category::Slate), 0.10f), 6.0f, true);
    ImGui::TextDisabled("Right-click this area to open the actor context menu.");

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::Selectable("##ctx_target", false, ImGuiSelectableFlags_AllowOverlap, avail);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("slate_actor_ctx");

    if (ImGui::BeginPopup("slate_actor_ctx")) {
        for (const std::string& item : s.items) {
            if (ImGui::MenuItem(item.c_str())) {
                if (ToLower(item) == "delete") {
                    const std::string actorName = s.text.empty() ? std::string("Actor") : s.text;
                    ctx.dialogs.Confirm("Delete Actor",
                        "Delete '" + actorName + "' from the level? This cannot be undone.", true,
                        [&ctx, actorName](bool ok) {
                            if (!ok)
                                return;
                            ctx.LogConsole(LogLevel::Warning, "Slate", "Deleted " + actorName);
                            ctx.Notify(ToastKind::Info, "Actor deleted", actorName + " removed from the level.");
                        });
                } else {
                    ctx.LogEvent("Slate", "Context: " + item);
                    ctx.Notify(ToastKind::Info, item, "Command '" + item + "' executed.");
                }
            }
        }
        ImGui::EndPopup();
    }
    widgets::EndCard();
}

void DrawToolbarSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Command Toolbar", icons::Icon::Bolt))
        return;

    const float spacing = ImMax(2.0f, s.spacingX);
    const int count = static_cast<int>(s.items.size());
    for (int i = 0; i < count; ++i) {
        const std::string& cmd = s.items[static_cast<size_t>(i)];
        ImGui::PushID(i);
        if (icons::IconButton("cmd", IconForCommand(cmd), cmd.c_str(), ImVec2(34.0f, 30.0f))) {
            ctx.LogEvent("Slate", "Toolbar: " + cmd);
            ctx.Notify(ToastKind::Success, cmd, "'" + cmd + "' started.");
        }
        ImGui::PopID();
        if (i + 1 < count)
            ImGui::SameLine(0.0f, spacing);
    }
}

void DrawSpawnToggle(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    if (!widgets::SectionHeader("Floating Slate Window", icons::Icon::Dock))
        return;

    // state.toggled mirrors the shared show flag so the Inspector stays in sync.
    s.toggled = ctx.showSlateWindow;
    if (widgets::ToggleButton("slate_spawn", s.text.c_str(), &s.toggled, ImVec2(0.0f, 0.0f),
                             s.tooltip.c_str())) {
        ctx.showSlateWindow = s.toggled;
        ctx.Interaction("Slate Window", s.toggled ? "opened floating editor window" : "closed floating editor window");
    }
    ImGui::SameLine();
    widgets::StatusDot(s.toggled ? ctx.theme.Active().success : ctx.theme.Active().textDisabled,
                       s.toggled ? "window open" : "window closed");
}

} // namespace

void DrawSlatePage(AppContext& ctx) {
    const struct Section {
        const char* id;
        void (*fn)(AppContext&, WidgetInstance&);
    } sections[] = {
        { "slate.details.properties",     DrawDetailsSection },
        { "slate.assetbrowser.mini",      DrawAssetBrowserSection },
        { "slate.hierarchy.mini",         DrawHierarchySection },
        { "slate.searchtree.outliner",    DrawSearchTreeSection },
        { "slate.contextmenu.actor",      DrawContextMenuSection },
        { "slate.toolbar.commands",       DrawToolbarSection },
        { "slate.window.spawn",           DrawSpawnToggle },
    };

    for (const Section& sec : sections) {
        WidgetInstance* inst = Bind(ctx, sec.id);
        if (!inst || !inst->state.visible)
            continue;
        sec.fn(ctx, *inst);
        ImGui::Spacing();
    }

    DrawUnboundList(ctx, Category::Slate);
}

void DrawSlateFloatingWindow(AppContext& ctx) {
    WidgetInstance* inst = Bind(ctx, "slate.window.spawn");
    WidgetState* s = inst ? &inst->state : nullptr;

    ImGui::SetNextWindowPos(ImVec2(s ? s->position.x : 360.0f, s ? s->position.y : 200.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(s ? s->size.x : 460.0f, s ? s->size.y : 300.0f),
                             ImGuiCond_FirstUseEver);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!ImGui::Begin("Slate Editor Window", &ctx.showSlateWindow, flags)) {
        ImGui::End();
        return;
    }

    // Fake editor menu bar: the window stays dockable and closable.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Close"))
                ctx.showSlateWindow = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            ImGui::MenuItem("Copy Reference");
            ImGui::MenuItem("Rename");
            ImGui::EndMenu();
        }
        ImGui::TextDisabled("Slate");
        ImGui::EndMenuBar();
    }

    widgets::TextMuted("A dockable window styled like an editor tab (SWidget / SGraphNode).");
    ImGui::Separator();

    if (ImGui::BeginTable("slate_props", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Property");
        ImGui::TableSetupColumn("Value");
        const char* names[4] = { "Title", "Docked", "Pos X", "Size X" };
        for (int i = 0; i < 4; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(names[i]);
            ImGui::TableNextColumn();
            if (!s) {
                ImGui::TextDisabled("-");
                continue;
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            switch (i) {
            case 0:
                if (ImGui::InputText("##title", &s->text))
                    widgets::TrackEdit(ctx, *inst, "Title");
                break;
            case 1: {
                bool docked = true;
                if (ImGui::Checkbox("##docked", &docked))
                    ctx.LogEvent("Slate", docked ? "Re-docked window" : "Undocked window");
                break;
            }
            case 2:
                if (ImGui::DragFloat("##posx", &s->position.x, 1.0f))
                    widgets::TrackEdit(ctx, *inst, "Position");
                break;
            case 3:
                if (ImGui::DragFloat("##sizex", &s->size.x, 1.0f, 200.0f, 2000.0f))
                    widgets::TrackEdit(ctx, *inst, "Size");
                break;
            default:
                break;
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    widgets::TextMuted("Tip: drag this window onto another panel to dock it.");
    ImGui::End();
}

} // namespace demo
} // namespace uilab
