// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef TerrainSplat_h
#define TerrainSplat_h

#include "Export.h"

#include "util/TerrainMaterialLibrary.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace quantum {

    // How a bare heightmap decides what covers itself. Every threshold is in the same units the
    // terrain already uses: altitude is the heightmap's own [0, 1] value, slope is 1 - abs(normal.y)
    // of the surface built from it - which is the very quantity the shader's triplanar mix reads, so
    // a band drawn here and a cliff rendered there agree.
    struct TerrainSplatRules {
        uint32_t seed = 1;
        int resolution = 512;      // the blend map's side; the heightmap's own size is unrelated
        float seaLevel = 0.28f;    // normalized, same meaning as TerrainGenParams::seaLevel
        float beachWidth = 0.06f;  // how far above sea level sand still holds
        float shoreDepth = 0.10f;  // how far below it the seabed stays sand instead of the base
        float gravelSlope = 0.16f; // the talus band: too steep for grass, not steep enough for rock
        float rockSlope = 0.34f;
        float cliffSlope = 0.60f;
        float dirtAltitude = 0.42f;  // grass gives up above this
        float snowAltitude = 0.70f;
        float iceAltitude = 0.86f;   // and only where it is flat enough to be a cap
        float bareness = 0.45f;      // how much of the lowland reads bare earth instead of grass
        float volcanic = 0.0f;       // 0 keeps lava rock out; >0 is the mask threshold that lets it in
        float variation = 0.15f;     // how far the noise pushes every threshold, so no band is a line
        float maxWeight = 0.9f;      // what the layers may claim; the base material keeps the rest
    };

    // The automatic half of the splatting system. It does not replace painting: it writes the same
    // three RGB blend maps the paint tool paints into and the same shader samples, from the field a
    // generator produced, so a generated island arrives covered and a user then paints over it.
    //
    // Slot order is fixed and is the contract between these maps and the terrain's layer list:
    // blendMaps[m] channel c weights surfaceLayers[m * 3 + c], and the plan below says which
    // material each of those nine slots carries.
    class QUANTUM_API TerrainSplat {
    public:
        static const int MIN_RESOLUTION = 8;
        static const int MAX_RESOLUTION = 2048;
        static int clampedResolution(int resolution);

        // The nine slots, in the order the blend maps and the layer array read them.
        static std::vector<TerrainMaterialKind> layerPlan();

        // Three RGB8 maps of resolution*resolution*3 bytes each, in blendMaps order. `heights` is a
        // size*size field in [0, 1] row-major from the map's north edge - the same buffer
        // TerrainGenerator::generate fills. `maxHeight` and `terrainSize` are the component's, and
        // they set the world scale a slope is measured against: a 200-unit map and a 2000-unit one
        // with the same normalized field are not equally steep.
        struct Maps {
            int resolution = 0;
            std::vector<unsigned char> blend[3];
        };
        static void bake(const std::vector<float>& heights, int size, float maxHeight, float terrainSize,
                         const TerrainSplatRules& rules, Maps& out);

        // The same, from a 16-bit grayscale heightmap PNG on disk - the path a CLI or a plugin takes
        // when the map already exists and only its materials are missing. False with a message when
        // the file cannot be read.
        static bool bakeFromFile(const std::filesystem::path& heightmapPath, float maxHeight,
                                 float terrainSize, const TerrainSplatRules& rules, Maps& out,
                                 std::string& error);

        // Writes the maps as blend_0.png, blend_1.png, blend_2.png in `directory` (created if
        // missing) and returns the paths the component's blendMaps fields get, each prefixed by
        // `pathBase` when one is given.
        struct Files {
            std::string blend[3];
        };
        static bool write(const Maps& maps, const std::filesystem::path& directory,
                          const std::string& pathBase, Files& files, std::string& error);

        // Convenience for a caller that has the field and wants everything: bake and write. `heights`
        // is taken by reference because a caller that also wants a preview keeps the field alive.
        static bool bakeAndWrite(const std::vector<float>& heights, int size, float maxHeight,
                                 float terrainSize, const TerrainSplatRules& rules,
                                 const std::filesystem::path& directory, const std::string& pathBase,
                                 Files& files, std::string& error);

        // The weight one texel gives each of the nine slots, for a caller that wants to preview or
        // test a rule without writing anything. Sums to at most maxWeight.
        static void weightsAt(const std::vector<float>& heights, int size, float maxHeight, float terrainSize,
                              const TerrainSplatRules& rules, int x, int y, float weights[9]);
    };
}

#endif /* TerrainSplat_h */
