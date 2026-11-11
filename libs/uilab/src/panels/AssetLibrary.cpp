#include "panels/AssetLibrary.h"

#include "core/Types.h"

#include <algorithm>

namespace uilab {

const char* AssetTypeName(AssetType t) {
    switch (t) {
    case AssetType::Folder:    return "Folder";
    case AssetType::Blueprint: return "Blueprint";
    case AssetType::Material:  return "Material";
    case AssetType::Texture:   return "Texture";
    case AssetType::Mesh:      return "Static Mesh";
    case AssetType::Animation: return "Animation";
    case AssetType::Sound:     return "Sound";
    case AssetType::Widget:    return "Widget Blueprint";
    case AssetType::DataAsset: return "Data Asset";
    case AssetType::Level:     return "Level";
    default:                   return "?";
    }
}

const char* AssetTypeExtension(AssetType t) {
    switch (t) {
    case AssetType::Folder:    return "";
    case AssetType::Blueprint: return ".uasset (blueprint)";
    case AssetType::Material:  return ".uasset (material)";
    case AssetType::Texture:   return ".uasset (texture)";
    case AssetType::Mesh:      return ".uasset (static mesh)";
    case AssetType::Animation: return ".uasset (anim)";
    case AssetType::Sound:     return ".uasset (sound)";
    case AssetType::Widget:    return ".uasset (widget)";
    case AssetType::DataAsset: return ".uasset (data)";
    case AssetType::Level:     return ".umap";
    default:                   return "";
    }
}

ImVec4 AssetTypeColor(AssetType t) {
    switch (t) {
    case AssetType::Folder:    return ImVec4(0.95f, 0.78f, 0.30f, 1.00f);
    case AssetType::Blueprint: return ImVec4(0.35f, 0.68f, 0.98f, 1.00f);
    case AssetType::Material:  return ImVec4(0.55f, 0.85f, 0.60f, 1.00f);
    case AssetType::Texture:   return ImVec4(0.85f, 0.60f, 0.95f, 1.00f);
    case AssetType::Mesh:      return ImVec4(0.65f, 0.75f, 0.90f, 1.00f);
    case AssetType::Animation: return ImVec4(0.95f, 0.60f, 0.45f, 1.00f);
    case AssetType::Sound:     return ImVec4(0.90f, 0.75f, 0.45f, 1.00f);
    case AssetType::Widget:    return ImVec4(0.45f, 0.85f, 0.90f, 1.00f);
    case AssetType::DataAsset: return ImVec4(0.75f, 0.75f, 0.80f, 1.00f);
    case AssetType::Level:     return ImVec4(0.98f, 0.55f, 0.55f, 1.00f);
    default:                   return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    }
}

namespace {
AssetItem MakeAsset(const char* name, AssetType type, long long sizeBytes, const char* modified,
                    std::initializer_list<const char*> tags = {}) {
    AssetItem a;
    a.name = name;
    a.type = type;
    a.sizeBytes = sizeBytes;
    a.modified = modified;
    for (const char* t : tags)
        a.tags.push_back(t);
    return a;
}

std::string JoinPath(const std::string& parent, const std::string& name) {
    if (parent.empty() || parent == "/")
        return "/" + name;
    return parent + "/" + name;
}
} // namespace

void AssetLibrary::BuildDefault() {
    root_ = MakeAsset("Game", AssetType::Folder, 0, "2026-01-01 09:00");
    selected_ = "/Game/Characters";

    AssetItem characters = MakeAsset("Characters", AssetType::Folder, 0, "2026-02-11 14:22");
    {
        AssetItem knight = MakeAsset("SK_Knight", AssetType::Mesh, 4'812'000, "2026-02-11 14:05",
                                     { "character", "skeletal" });
        characters.children.push_back(knight);
        characters.children.push_back(
            MakeAsset("M_Knight_Body", AssetType::Material, 24'800, "2026-02-11 13:58", { "character" }));
        characters.children.push_back(
            MakeAsset("T_Knight_Diffuse", AssetType::Texture, 1'258'000, "2026-02-10 18:31",
                      { "character", "2k" }));
        characters.children.push_back(
            MakeAsset("ABP_Knight", AssetType::Animation, 96'000, "2026-02-11 14:22", { "character", "anim" }));
    }
    root_.children.push_back(characters);

    AssetItem environment = MakeAsset("Environment", AssetType::Folder, 0, "2026-01-28 10:12");
    {
        environment.children.push_back(
            MakeAsset("SM_Floor", AssetType::Mesh, 820'000, "2026-01-20 09:44", { "modular" }));
        environment.children.push_back(
            MakeAsset("SM_Wall", AssetType::Mesh, 1'120'000, "2026-01-20 09:51", { "modular" }));
        environment.children.push_back(
            MakeAsset("M_Grid", AssetType::Material, 18'400, "2026-01-22 12:02", { "surface" }));
        environment.children.push_back(
            MakeAsset("T_Grid_Albedo", AssetType::Texture, 682'000, "2026-01-22 11:59", { "tileable" }));

        AssetItem props = MakeAsset("Props", AssetType::Folder, 0, "2026-01-25 16:40");
        props.children.push_back(MakeAsset("SM_Crate", AssetType::Mesh, 210'000, "2026-01-25 16:38"));
        props.children.push_back(MakeAsset("SM_Barrel", AssetType::Mesh, 268'000, "2026-01-25 16:40"));
        environment.children.push_back(props);
    }
    root_.children.push_back(environment);

    AssetItem ui = MakeAsset("UI", AssetType::Folder, 0, "2026-02-14 08:15");
    {
        ui.children.push_back(
            MakeAsset("WBP_HUD", AssetType::Widget, 148'000, "2026-02-14 08:15", { "hud", "runtime" }));
        ui.children.push_back(
            MakeAsset("WBP_MainMenu", AssetType::Widget, 132'000, "2026-02-12 17:20", { "menu" }));
        ui.children.push_back(
            MakeAsset("T_UIFrame", AssetType::Texture, 512'000, "2026-02-12 17:05", { "ui", "9slice" }));
        ui.children.push_back(MakeAsset("M_UIGlow", AssetType::Material, 21'000, "2026-02-13 09:12", { "ui" }));
        ui.children.push_back(MakeAsset("MT_Tooltip", AssetType::DataAsset, 4'000, "2026-02-13 09:30", { "ui" }));
    }
    root_.children.push_back(ui);

    AssetItem audio = MakeAsset("Audio", AssetType::Folder, 0, "2026-01-19 20:05");
    {
        audio.children.push_back(MakeAsset("S_Footstep_Grass", AssetType::Sound, 96'000, "2026-01-19 20:05"));
        audio.children.push_back(MakeAsset("S_UIClick", AssetType::Sound, 12'000, "2026-01-19 20:11"));
        audio.children.push_back(MakeAsset("S_BuildComplete", AssetType::Sound, 48'000, "2026-01-19 20:20"));
    }
    root_.children.push_back(audio);

    AssetItem maps = MakeAsset("Maps", AssetType::Folder, 0, "2026-02-01 07:45");
    {
        maps.children.push_back(MakeAsset("L_TrainingGround", AssetType::Level, 2'480'000, "2026-02-01 07:45"));
        maps.children.push_back(MakeAsset("L_Arena", AssetType::Level, 3'120'000, "2026-01-30 19:32"));
    }
    root_.children.push_back(maps);

    root_.children.push_back(
        MakeAsset("M_Master", AssetType::Material, 32'000, "2026-01-15 11:00", { "master" }));
    root_.children.push_back(
        MakeAsset("DA_Settings", AssetType::DataAsset, 6'000, "2026-02-02 10:10", { "settings" }));
    root_.children.push_back(
        MakeAsset("BP_GameMode", AssetType::Blueprint, 86'000, "2026-02-03 15:26", { "gameplay" }));

    RebuildPaths();
}

void AssetLibrary::RebuildPaths() {
    cache_.clear();
    struct Walker {
        AssetLibrary* lib;
        void Visit(AssetItem& item, const std::string& path) {
            lib->cache_.push_back(FlatEntry{ path, item.name, item.type, &item });
            for (AssetItem& child : item.children)
                Visit(child, JoinPath(path, child.name));
        }
    } walker{ this };
    walker.Visit(root_, "/Game");
    dirty = false;
}

AssetItem* AssetLibrary::Find(const std::string& path) {
    if (dirty)
        RebuildPaths();
    for (const FlatEntry& e : cache_) {
        if (e.path == path && e.item)
            return const_cast<AssetItem*>(e.item);
    }
    return nullptr;
}

const AssetItem* AssetLibrary::Find(const std::string& path) const {
    if (dirty)
        const_cast<AssetLibrary*>(this)->RebuildPaths();
    for (const FlatEntry& e : cache_) {
        if (e.path == path)
            return e.item;
    }
    return nullptr;
}

AssetItem* AssetLibrary::CreateFolder(const std::string& parentPath, const std::string& name) {
    AssetItem* parent = Find(parentPath);
    if (!parent || !parent->IsFolder())
        parent = &root_;
    if (Find(JoinPath(parentPath.empty() ? "/Game" : parentPath, name)))
        return nullptr;
    AssetItem folder;
    folder.name = name;
    folder.type = AssetType::Folder;
    folder.modified = "2026-02-15 12:00";
    parent->children.push_back(folder);
    dirty = true;
    RebuildPaths();
    return Find(JoinPath(parentPath.empty() ? "/Game" : parentPath, name));
}

AssetItem* AssetLibrary::Duplicate(const std::string& path) {
    AssetItem* item = Find(path);
    if (!item)
        return nullptr;
    const size_t slash = path.find_last_of('/');
    const std::string parentPath = slash == std::string::npos ? std::string("/Game") : path.substr(0, slash);
    AssetItem* parent = Find(parentPath);
    if (!parent)
        parent = &root_;
    AssetItem copy = *item;
    // Find a free name.
    std::string base = item->name + "_Copy";
    std::string candidate = base;
    int n = 1;
    while (Find(JoinPath(parentPath, candidate)))
        candidate = base + std::to_string(++n);
    copy.name = candidate;
    parent->children.push_back(copy);
    dirty = true;
    RebuildPaths();
    return Find(JoinPath(parentPath, candidate));
}

bool AssetLibrary::Rename(const std::string& path, const std::string& newName, std::string& errorOut) {
    AssetItem* item = Find(path);
    if (!item) {
        errorOut = "Asset not found.";
        return false;
    }
    if (ContainsNoCase(newName, "/") || newName.empty()) {
        errorOut = "Name must not be empty or contain '/'.";
        return false;
    }
    const size_t slash = path.find_last_of('/');
    const std::string parentPath = slash == std::string::npos ? std::string("/Game") : path.substr(0, slash);
    if (Find(JoinPath(parentPath, newName)) && item->name != newName) {
        errorOut = "An asset with that name already exists.";
        return false;
    }
    item->name = newName;
    item->modified = "2026-02-15 12:05";
    dirty = true;
    RebuildPaths();
    errorOut.clear();
    return true;
}

bool AssetLibrary::Remove(const std::string& path) {
    if (path == "/Game")
        return false;
    const size_t slash = path.find_last_of('/');
    const std::string parentPath = slash == std::string::npos ? std::string("/Game") : path.substr(0, slash);
    AssetItem* parent = Find(parentPath);
    if (!parent)
        return false;
    const size_t last = path.find_last_of('/');
    const std::string name = path.substr(last + 1);
    for (auto it = parent->children.begin(); it != parent->children.end(); ++it) {
        if (it->name == name) {
            parent->children.erase(it);
            dirty = true;
            RebuildPaths();
            if (selected_ == path)
                selected_ = parentPath;
            return true;
        }
    }
    return false;
}

void AssetLibrary::CountAssets(int& folders, int& files) const {
    folders = 0;
    files = 0;
    for (const FlatEntry& e : cache_) {
        if (e.type == AssetType::Folder)
            ++folders;
        else
            ++files;
    }
}

std::vector<AssetLibrary::FlatEntry> AssetLibrary::Filter(const std::string& query, AssetType typeFilter,
                                                          bool foldersOnly) const {
    AssetLibrary* self = const_cast<AssetLibrary*>(this);
    if (self->dirty)
        self->RebuildPaths();
    std::vector<FlatEntry> out;
    for (const FlatEntry& e : cache_) {
        if (foldersOnly && e.type != AssetType::Folder)
            continue;
        if (!foldersOnly && typeFilter != AssetType::Count && e.type != typeFilter)
            continue;
        if (!query.empty() && !ContainsNoCase(e.name, query) && !ContainsNoCase(e.path, query))
            continue;
        out.push_back(e);
    }
    return out;
}

} // namespace uilab
