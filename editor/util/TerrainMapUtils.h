// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"

#include "util/TerrainMapRegion.h"

#include <algorithm>
#include <string>
#include <vector>

namespace quantum::editor{

    enum class TerrainMapTarget{
        HeightMap,
        BlendMap,
        DensityMap
    };

    // Names one editable map. "layer" picks the blend map or the foliage layer.
    struct TerrainMapRef{
        TerrainMapTarget target = TerrainMapTarget::HeightMap;
        int layer = 0;

        TerrainMapRef() = default;
        TerrainMapRef(TerrainMapTarget target, int layer = 0): target(target), layer(layer){}

        bool operator==(const TerrainMapRef& other) const{
            return target == other.target && layer == other.layer;
        }
    };

    // Undo payload for a stroke: the map rect it changed, kept instead of a whole map copy.
    // Only applies while the texture still points at this file with this geometry.
    struct TerrainMapPatch{
        std::string path;
        ColorFormat colorFormat = ColorFormat::RGBA;
        int mapWidth = 0;
        int mapHeight = 0;
        int channels = 0;
        TerrainMapRegion region;
        std::vector<unsigned char> beforePixels;
        std::vector<unsigned char> afterPixels;
    };

    // Terrain map storage shared by the terrain editor and its undo commands: both bind, read,
    // persist and refresh the same height, blend and foliage density maps.
    class TerrainMapUtils{

    public:
        // Null when a density map is asked for a foliage layer the terrain no longer has.
        static Texture* findTexture(TerrainComponent& terrain, const TerrainMapRef& ref);
        static std::string getPropertyName(const TerrainMapRef& ref);
        // The list a map lives in, empty when it is not in one. An indexed name alone does
        // not carry a newly created slot to a bundle instance.
        static std::string getContainerPropertyName(const TerrainMapRef& ref);
        static bool hasLoadedData(Texture& texture);
        static bool writeFile(Project* project, const std::string& relativePath, int width, int height, int channels, int bytesPerChannel, const std::vector<unsigned char>& pixels);
        // Re-arms what the engine has to redo after a map was written. A stroke that says which
        // texels it touched narrows the work to them: a heightmap one to the CDLOD ranges over
        // those texels, a density one to the foliage chunks overlapping them. An empty rect is
        // the whole-map case (a new map, a new size, a whole-map undo) and invalidates broadly.
        static void refresh(SceneProject* sceneProject, Entity entity, const TerrainMapRef& ref, const TerrainMapRegion& dirtyRegion = TerrainMapRegion());

        // Copies an inclusive texel rect out of a full map buffer.
        static std::vector<unsigned char> copyRegion(const unsigned char* pixels, int mapWidth, int bytesPerTexel, const TerrainMapRegion& region);

        // A patch shorter than its own rect writes nothing.
        static bool writeRegion(unsigned char* pixels, int mapWidth, int bytesPerTexel, const TerrainMapRegion& region, const std::vector<unsigned char>& regionPixels);
    };

}
