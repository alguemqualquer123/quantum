// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef TerrainGenerator_h
#define TerrainGenerator_h

#include "Export.h"

#include "util/Noise.h"
#include "util/TerrainMapRegion.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace quantum {

    // Shape of a generated map. Each preset is a set of parameters, not a different algorithm, so a
    // script or an inspector row starts from one and overrides any field it wants.
    enum class TerrainPreset : int {
        CONTINENTS = 0, // wide lowland, a few broad ranges
        ISLAND = 1,     // the field falls off to ocean at the border
        MOUNTAINS = 2,  // ridged relief, high amplitude
        PLAINS = 3,     // gentle rolling, no ridges
        CANYONS = 4     // warped ridges over a flat floor
    };

    // Everything a heightmap is made from. Plain data only, so the same struct can be filled by a
    // plugin schema row, a scene key, a CLI argument list or a script table.
    struct TerrainGenParams {
        uint32_t seed = 1;
        int size = 256; // square, clamped to [TerrainGenerator::MIN_SIZE, MAX_SIZE]
        NoiseType type = NoiseType::SIMPLEX;
        int octaves = 6;
        float lacunarity = 2.0f;
        float gain = 0.5f;
        // Lattice units per texel: the field is sampled at (texel * frequency), so one value works for
        // any map size. 0.012 puts a base octave across roughly 80 texels.
        float frequency = 0.012f;
        float baseHeight = 0.12f;   // where the field's zero lands, in output units
        float amplitude = 0.72f;    // how tall the field reaches, in output units
        float ridgedAmount = 0.0f;  // mix between the smooth sum (0) and the ridged one (1)
        float warpAmplitude = 0.0f; // 0 skips domain warping entirely
        float warpFrequency = 0.4f;
        float reliefCurve = 1.0f;   // >1 flattens valleys, <1 flattens peaks
        float seaLevel = 0.28f;     // normalized; below it the bed is damped, not walled off
        float islandFalloff = 0.0f; // 0 keeps the field, 1 pulls the whole border down to sea level
        int erosionDroplets = 0;    // 0 skips the hydraulic pass
        float erosionHeightScale = 100.0f;
        uint32_t erosionSeed = 1;   // a separate field: re-rolling the map seed must not be the only
                                    // way to change where the rivers cut
        float thermalTalus = 0.0f;  // 0 skips the thermal pass
        float thermalRate = 0.5f;
    };

    // Procedural heightmaps on the engine's own noise: no editor, no ImGui, nothing that throws, so a
    // pooled world-generation task and the main thread can produce the same map. Output is the format
    // a TerrainComponent reads - a 16-bit single-channel PNG, which loads back as ColorFormat::RED16.
    class QUANTUM_API TerrainGenerator {
    public:
        // A map smaller than this cannot carry a lattice, and one larger than this is more texels than
        // a height texture is worth; both are clamped rather than refused, because the size usually
        // comes from an inspector row.
        static const int MIN_SIZE = 8;
        static const int MAX_SIZE = 4096;

        // The side length a request actually produces, after clamping. A caller that writes a texture
        // or a PNG has to use this and not the number it asked for.
        static int clampedSize(int size);

        // The parameter set a preset name means. An unknown preset reads as CONTINENTS.
        static TerrainGenParams preset(TerrainPreset preset);

        // Names are what a manifest, a CLI argument and a schema dropdown all carry.
        static TerrainPreset presetFromName(const std::string& name);
        static const char* presetName(TerrainPreset preset);

        // Fills `heights` with size*size values in [0, 1], row-major from the map's north edge.
        // Deterministic: the same parameters give the same texels on every machine and compiler,
        // which is what makes a seed a save-game field.
        static void generate(const TerrainGenParams& params, std::vector<float>& heights);

        // The same field quantized the way the terrain editor quantizes a brush stroke: [0, 1] mapped
        // to a 16-bit sample, so a generated map and a painted one are stored alike.
        static void encode16(const std::vector<float>& heights, std::vector<unsigned char>& pixels);

        // Generate and write it as a 16-bit grayscale PNG. The path's parent directories must exist.
        static bool generatePng(const TerrainGenParams& params, const std::filesystem::path& path);

        // Where a water plane has to sit for this map, in world units, given the terrain's maxHeight.
        // The inverse of what a heightmap texel means to the shader, kept here so a caller does not
        // have to re-derive it to place a lake.
        static float waterHeight(const TerrainGenParams& params, float maxHeight);
    };
}

#endif /* TerrainGenerator_h */
