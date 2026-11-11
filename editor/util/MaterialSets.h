// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "texture/Material.h"

#include <filesystem>
#include <string>
#include <vector>

namespace quantum::editor {

    // Which surface map a texture file is, judged only by the naming conventions the
    // free texture libraries converged on (diff/nor_gl/rough/ao/arm plus a
    // resolution suffix). Nothing in a filename guarantees the channel layout, so a
    // wrong guess shows up as a wrong preview and is fixed here, not in a manifest.
    enum class MaterialMapKind {
        BaseColor,
        Normal,
        NormalDX,
        Roughness,
        Metallic,
        MetallicRoughness,
        Occlusion,
        PackedAO,      // arm/orm: the same image in the occlusion and the metallic-roughness slot
        Emissive,
        Height,
        Unknown
    };

    // One surface, described by however many of its maps happen to be on disk. A set
    // with only a diffuse image is still a set: a lone ground texture deserves a
    // sphere too.
    struct MaterialSet {
        std::string name;
        std::filesystem::path directory;

        std::filesystem::path baseColor;
        std::filesystem::path normal;
        std::filesystem::path normalDX;
        std::filesystem::path roughness;
        std::filesystem::path metallic;
        std::filesystem::path metallicRoughness;
        std::filesystem::path occlusion;
        std::filesystem::path packedAO;
        std::filesystem::path emissive;
        std::filesystem::path height;

        // Every file that joined the set, in the order they were found, so the
        // detail view can list the maps even when two of them claim one slot.
        std::vector<std::filesystem::path> files;

        int mapCount() const;
        std::string kindLabel(const std::filesystem::path& file) const;
    };

    class MaterialSets {
    public:
        // Unknown when the filename carries no map convention at all.
        static MaterialMapKind classify(const std::string& filename);

        // The shared part of the filename: "rock_ground_02_diff_1k" -> "rock_ground_02".
        static std::string stemOf(const std::string& filename);

        static std::vector<MaterialSet> group(const std::vector<std::filesystem::path>& files);

        // Absolute paths, because the preview scene is not the user's scene and must
        // not depend on which project is open.
        static Material buildMaterial(const MaterialSet& set);

        // The material library the engine itself ships: <engine>/assets/materials. The
        // folder is not part of a project, so a scene can only use one of these sets
        // once installSet has copied it under the project's assets root.
        // Empty when the engine folder is neither next to the executable nor in a
        // checkout that has not built yet.
        static std::filesystem::path engineMaterialsRoot();

        // Every set the engine ships, sorted by folder then name so a CLI listing is
        // stable across runs and platforms.
        static std::vector<MaterialSet> engineSets();

        // The first shipped set whose name matches, ignoring case and separators, so
        // "ground_grass", "Ground Grass" and "grass" all find one set when they are
        // unambiguous. Returns a set with an empty directory when nothing matches.
        static MaterialSet findEngineSet(const std::string& query);

        // Copy every map of a set into <assetsRoot>/materials/<its folder>. Files the
        // project already has are left alone - a project may have edited its own copy.
        // *installed receives the paths as a scene must store them, relative to the
        // assets root. Fails only when there is nothing to copy or the copy was denied.
        static bool installSet(const MaterialSet& set, const std::filesystem::path& assetsRoot,
                               std::vector<std::string>* installed, std::string* error);
    };

}
