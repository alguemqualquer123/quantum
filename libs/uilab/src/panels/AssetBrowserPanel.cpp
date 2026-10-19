// AssetBrowserPanel.cpp - content browser dock: tree/grid views over the in-memory asset library.
#include "panels/Panels.h"

#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/AppContext.h"
#include "core/Dialogs.h"
#include "panels/AssetLibrary.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace panels {

namespace {

// Panel-local view state. A file-scope static keeps the search text out of the
// shared demo state (the global search box belongs to the toolbar/tree).
std::string s_query;
bool s_gridView = false;

// Library mutations raised from a row context menu are deferred until the list
// has finished drawing, so the tree vectors we are iterating stay valid.
std::string s_duplicatePath;

std::string ParentOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
        return std::string("/Game");
    return path.substr(0, slash);
}

std::string FormatSize(long long bytes) {
    static const char* kUnits[] = { "B", "KB", "MB", "GB", "TB" };
    double v = static_cast<double>(bytes);
    if (v <= 0.0)
        return "0 B";
    int unit = 0;
    while (v >= 1024.0 && unit < 4) {
        v /= 1024.0;
        ++unit;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s" : "%.1f %s", v, kUnits[unit]);
    return std::string(buf);
}

void OpenAsset(AppContext& ctx, const std::string& path, const AssetItem& item) {
    if (item.IsFolder())
        return; // folders open through the tree arrow instead
    ctx.assets.SetSelected(path);
    ctx.LogEvent("ContentBrowser", "Opened " + item.name + " (" + std::string(AssetTypeName(item.type)) + ")");
    ctx.Notify(ToastKind::Info, "Asset opened", path);
}

void DuplicateAsset(AppContext& ctx, const std::string& path) {
    AssetItem* made = ctx.assets.Duplicate(path);
    if (!made) {
        ctx.LogConsole(LogLevel::Error, "ContentBrowser", "Duplicate failed for " + path);
        ctx.Notify(ToastKind::Error, "Duplicate failed", path);
        return;
    }
    const std::string newPath = ParentOf(path) + "/" + made->name;
    ctx.assets.SetSelected(newPath);
    ctx.LogEvent("ContentBrowser", "Duplicated " + path + " as " + newPath);
    ctx.Notify(ToastKind::Success, "Asset duplicated", newPath);
}

void RequestRename(AppContext& ctx, const std::string& path, const std::string& currentName) {
    ctx.dialogs.TextInput("Rename Asset", "Name", currentName, "nonempty", "", [&ctx, path](bool ok, std::string value) {
        if (!ok)
            return;
        std::string error;
        if (ctx.assets.Rename(path, value, error)) {
            ctx.assets.SetSelected(ParentOf(path) + "/" + value);
            ctx.LogEvent("ContentBrowser", "Renamed " + path + " to " + value);
            ctx.Notify(ToastKind::Success, "Asset renamed", value);
        } else {
            ctx.LogConsole(LogLevel::Error, "ContentBrowser", "Rename failed: " + error);
            ctx.Notify(ToastKind::Error, "Rename failed", error);
        }
    });
}

void RequestDelete(AppContext& ctx, const std::string& path) {
    ctx.dialogs.Confirm("Delete Asset", "Delete \"" + path + "\"? Folders are removed together with their content.",
                        true, [&ctx, path](bool ok) {
        if (!ok)
            return;
        if (ctx.assets.Remove(path)) {
            ctx.LogConsole(LogLevel::Warning, "ContentBrowser", "Deleted " + path);
            ctx.Notify(ToastKind::Warning, "Asset deleted", path);
        } else {
            ctx.LogConsole(LogLevel::Error, "ContentBrowser", "Delete failed for " + path);
            ctx.Notify(ToastKind::Error, "Delete failed", path);
        }
    });
}

void RequestNewFolder(AppContext& ctx, const std::string& parentPath) {
    ctx.dialogs.TextInput("New Folder", "Folder name", "NewFolder", "nonempty", "",
                          [&ctx, parentPath](bool ok, std::string name) {
        if (!ok)
            return;
        AssetItem* made = ctx.assets.CreateFolder(parentPath, name);
        if (made) {
            const std::string path = parentPath + "/" + name;
            ctx.assets.SetSelected(path);
            ctx.LogConsole(LogLevel::Success, "ContentBrowser", "Created folder " + path);
            ctx.Notify(ToastKind::Success, "Folder created", path);
        } else {
            const std::string error = "\"" + name + "\" already exists in " + parentPath;
            ctx.LogConsole(LogLevel::Error, "ContentBrowser", "Folder creation failed: " + error);
            ctx.Notify(ToastKind::Error, "Folder creation failed", error);
        }
    });
}

void RowContextMenu(AppContext& ctx, const std::string& path, const AssetItem& item) {
    if (!ImGui::BeginPopupContextItem("##asset_ctx"))
        return;
    ImGui::TextDisabled("%s", path.c_str());
    ImGui::TextDisabled("%s", AssetTypeName(item.type));
    ImGui::Separator();
    if (ImGui::MenuItem("New Folder..."))
        RequestNewFolder(ctx, item.IsFolder() ? path : ParentOf(path));
    if (ImGui::MenuItem("Duplicate"))
        s_duplicatePath = path;
    if (ImGui::MenuItem("Rename..."))
        RequestRename(ctx, path, item.name);
    ImGui::Separator();
    if (ImGui::MenuItem("Delete..."))
        RequestDelete(ctx, path);
    ImGui::EndPopup();
}

bool SubtreeMatches(const AssetItem& node, const std::string& path, const std::string& query) {
    if (query.empty())
        return true;
    if (ContainsNoCase(node.name, query) || ContainsNoCase(path, query))
        return true;
    for (const AssetItem& child : node.children) {
        if (SubtreeMatches(child, path + "/" + child.name, query))
            return true;
    }
    return false;
}

void DrawTreeNode(AppContext& ctx, const AssetItem& node, const std::string& path) {
    if (!SubtreeMatches(node, path, s_query))
        return;

    const bool selected = ctx.assets.SelectedPath() == path;
    ImGui::PushID(path.c_str());

    const std::string label = node.name + "##node";
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | (selected ? ImGuiTreeNodeFlags_Selected : 0);
    if (!s_query.empty())
        flags |= ImGuiTreeNodeFlags_DefaultOpen;

    ImGui::PushStyleColor(ImGuiCol_Text, AssetTypeColor(node.type));
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    ImGui::PopStyleColor();

    if (ImGui::IsItemClicked())
        ctx.assets.SetSelected(path);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        OpenAsset(ctx, path, node);
    RowContextMenu(ctx, path, node);

    if (open) {
        for (const AssetItem& child : node.children)
            DrawTreeNode(ctx, child, path + "/" + child.name);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void DrawGrid(AppContext& ctx, const std::vector<AssetLibrary::FlatEntry>& entries) {
    if (entries.empty()) {
        ImGui::TextDisabled("No assets match \"%s\".", s_query.c_str());
        return;
    }

    const int columns = 4;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float cardW =
        (ImGui::GetContentRegionAvail().x - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float cardH = 104.0f;
    const ThemePalette& palette = ctx.theme.Active();

    for (size_t i = 0; i < entries.size(); ++i) {
        const AssetLibrary::FlatEntry& entry = entries[i];
        const AssetItem& item = *entry.item;
        if (i % static_cast<size_t>(columns) != 0)
            ImGui::SameLine(0.0f, spacing);

        ImGui::PushID(entry.path.c_str());
        widgets::BeginCard("##card", ImVec2(cardW, cardH), WithAlpha(palette.frameBg, 0.65f));
        const ImVec2 inner = ImGui::GetContentRegionAvail();
        const ImVec2 cardPos = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##hit", inner);
        const bool hovered = ImGui::IsItemHovered();
        if (clicked)
            ctx.assets.SetSelected(entry.path);
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            OpenAsset(ctx, entry.path, item);
        RowContextMenu(ctx, entry.path, item);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec4 typeCol = AssetTypeColor(item.type);
        const ImU32 typeCol32 = ImGui::ColorConvertFloat4ToU32(typeCol);
        dl->AddRectFilled(cardPos, ImVec2(cardPos.x + inner.x, cardPos.y + 3.0f), typeCol32, 1.5f);
        icons::Draw(dl, icons::ForAssetType(static_cast<int>(item.type)),
                    ImVec2(cardPos.x + inner.x * 0.5f, cardPos.y + 30.0f), 30.0f, typeCol32, 1.5f);

        const std::string name = widgets::Ellipsize(entry.name, inner.x - 4.0f);
        const ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
        dl->AddText(ImVec2(cardPos.x + (inner.x - nameSize.x) * 0.5f, cardPos.y + 50.0f),
                    ImGui::GetColorU32(ImGuiCol_Text), name.c_str());

        const std::string subtitle =
            widgets::Ellipsize(std::string(AssetTypeName(item.type)) + " - " + FormatSize(item.sizeBytes),
                               inner.x - 4.0f);
        const ImVec2 subSize = ImGui::CalcTextSize(subtitle.c_str());
        dl->AddText(ImVec2(cardPos.x + (inner.x - subSize.x) * 0.5f, cardPos.y + 68.0f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), subtitle.c_str());

        if (ctx.assets.SelectedPath() == entry.path || hovered) {
            const ImVec2 outerMin = ImVec2(cardPos.x - 10.0f, cardPos.y - 10.0f);
            const ImVec2 outerMax = ImVec2(outerMin.x + cardW, outerMin.y + cardH);
            dl->AddRect(outerMin, outerMax, ImGui::GetColorU32(palette.selection), 6.0f, 0, hovered ? 2.0f : 1.5f);
        }
        widgets::EndCard();
        ImGui::PopID();
    }
}

} // namespace

void DrawAssetBrowser(AppContext& ctx) {
    if (!ImGui::Begin(kAssetBrowserTitle)) {
        ImGui::End();
        return;
    }

    if (ctx.host.assetBrowser) {
        ctx.host.assetBrowser(ctx);
        ImGui::End();
        return;
    }

    if (ctx.assets.dirty)
        ctx.assets.RebuildPaths();

    // Copy: mutating operations reassign the library's selection string.
    const std::string selected = ctx.assets.SelectedPath();
    AssetItem* selItem = ctx.assets.Find(selected);
    const bool canAct = selItem != nullptr && selected != "/Game";

    // ---- toolbar ----------------------------------------------------------
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    const float btn = 26.0f;

    icons::IconText(icons::Icon::Folder, selected.c_str(), ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::SameLine();
    const float searchWidth = 220.0f;
    ImGui::SetNextItemWidth(searchWidth);
    widgets::SearchField("##cb_search", s_query, "Search assets...", searchWidth);
    ImGui::SameLine();
    if (icons::IconButton("##view_tree", icons::Icon::Tree, "Tree view", ImVec2(btn, btn), !s_gridView))
        s_gridView = false;
    ImGui::SameLine();
    if (icons::IconButton("##view_grid", icons::Icon::Grid, "Grid view", ImVec2(btn, btn), s_gridView))
        s_gridView = true;

    ImGui::NewLine();
    if (icons::IconButton("##new_folder", icons::Icon::Plus, "New folder", ImVec2(btn, btn))) {
        RequestNewFolder(ctx, (selItem && selItem->IsFolder()) ? selected : ParentOf(selected));
    }
    ImGui::SameLine();
    if (icons::IconButton("##cb_dup", icons::Icon::Copy, "Duplicate selected", ImVec2(btn, btn), false, canAct))
        DuplicateAsset(ctx, selected);
    ImGui::SameLine();
    if (icons::IconButton("##cb_rename", icons::Icon::File, "Rename selected", ImVec2(btn, btn), false, canAct))
        RequestRename(ctx, selected, selItem->name);
    ImGui::SameLine();
    if (icons::IconButton("##cb_delete", icons::Icon::Trash, "Delete selected", ImVec2(btn, btn), false, canAct))
        RequestDelete(ctx, selected);
    ImGui::SameLine();
    // Re-looked-up (not selItem): the duplicate branch above may have moved the tree.
    if (const AssetItem* shown = ctx.assets.Find(ctx.assets.SelectedPath()))
        ImGui::TextDisabled("| %s", AssetTypeName(shown->type));
    ImGui::PopStyleVar();
    ImGui::Separator();

    // ---- body -------------------------------------------------------------
    const std::vector<AssetLibrary::FlatEntry> matches = ctx.assets.Filter(s_query, AssetType::Count, false);
    if (s_gridView) {
        DrawGrid(ctx, matches);
    } else {
        if (!SubtreeMatches(ctx.assets.Root(), "/Game", s_query))
            ImGui::TextDisabled("No assets match \"%s\".", s_query.c_str());
        else
            DrawTreeNode(ctx, ctx.assets.Root(), "/Game");
    }

    if (!s_duplicatePath.empty()) {
        const std::string path = s_duplicatePath;
        s_duplicatePath.clear();
        DuplicateAsset(ctx, path);
    }

    // ---- footer -----------------------------------------------------------
    ImGui::Separator();
    int folders = 0;
    int files = 0;
    ctx.assets.CountAssets(folders, files);
    ImGui::TextDisabled("%d folder(s) / %d asset(s) | %d match%s", folders, files, static_cast<int>(matches.size()),
                        matches.size() == 1 ? "" : "es");

    if (const AssetItem* item = ctx.assets.Find(ctx.assets.SelectedPath())) {
        std::string tags;
        for (const std::string& tag : item->tags) {
            if (!tags.empty())
                tags += ", ";
            tags += tag;
        }
        widgets::TextMuted("%s | %s | modified %s | tags: %s", item->name.c_str(),
                           FormatSize(item->sizeBytes).c_str(), item->modified.c_str(),
                           tags.empty() ? "none" : tags.c_str());
    } else {
        widgets::TextMuted("Nothing selected.");
    }

    ImGui::End();
}

} // namespace panels
} // namespace uilab
