// AssetLibrary.h - in-memory asset tree backing the Content Browser panels.
// Nothing is read from disk: the "Content" folder is generated so the lab is
// self contained, but every operation (rename/delete/duplicate/create folder)
// really mutates the tree.
#pragma once

#include <string>
#include <vector>

#include <imgui.h>

namespace uilab {

enum class AssetType : int {
    Folder = 0, Blueprint, Material, Texture, Mesh, Animation, Sound, Widget, DataAsset, Level, Count
};

const char* AssetTypeName(AssetType t);
const char* AssetTypeExtension(AssetType t);
ImVec4 AssetTypeColor(AssetType t);

struct AssetItem {
    std::string name;
    AssetType type = AssetType::Folder;
    std::vector<AssetItem> children;
    long long sizeBytes = 0;
    std::string modified = "2026-01-01 09:00";
    std::vector<std::string> tags;

    bool IsFolder() const { return type == AssetType::Folder; }
    size_t ChildCount() const { return children.size(); }
};

class AssetLibrary {
public:
    AssetLibrary() { BuildDefault(); }

    void BuildDefault();
    AssetItem& Root() { return root_; }
    const AssetItem& Root() const { return root_; }

    // Path based lookup ("/Game/Characters/SK_Knight").
    AssetItem* Find(const std::string& path);
    const AssetItem* Find(const std::string& path) const;

    // Mutating operations used by the context menus.
    AssetItem* CreateFolder(const std::string& parentPath, const std::string& name);
    AssetItem* Duplicate(const std::string& path);
    bool Rename(const std::string& path, const std::string& newName, std::string& errorOut);
    bool Remove(const std::string& path);

    const std::string& SelectedPath() const { return selected_; }
    void SetSelected(const std::string& path) { selected_ = path; }

    // Recursive count of folders/files.
    void CountAssets(int& folders, int& files) const;

    // Flat list of assets matching a filter (used by the search field and grid view).
    struct FlatEntry {
        std::string path;
        std::string name;
        AssetType type;
        const AssetItem* item;
    };
    std::vector<FlatEntry> Filter(const std::string& query, AssetType typeFilter, bool foldersOnly) const;

    // Cached path of each item, refreshed by RebuildPaths().
    const std::vector<FlatEntry>& AllCached() const { return cache_; }
    void RebuildPaths();

    bool dirty = true;

private:
    AssetItem root_;
    std::string selected_ = "/Game/Characters";
    std::vector<FlatEntry> cache_;
};

} // namespace uilab
