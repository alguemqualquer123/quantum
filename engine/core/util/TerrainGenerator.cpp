// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "TerrainGenerator.h"

#include "util/PngWriter.h"
#include "util/TerrainErosion.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

using namespace quantum;

namespace {

    // The relief field (the ridged sum) reads from this offset seed, so mixing it with the smooth sum
    // blends two different lattices instead of the same one shaped twice.
    const uint32_t RELIEF_SEED = 0x165667B1u;

    // Below the water line the bed keeps its shape but loses most of its range: a hard floor at sea
    // level makes a bathtub, and a lake needs a bottom that a boat can run aground on.
    const float SUBSEA_DAMPING = 0.25f;

    const float INVERSE_SQRT2 = 0.7071067811865476f;

    inline float clampUnit(float value) {
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }

    inline float sanitize(float value, float low, float high, float fallback) {
        if (value != value) return fallback;
        if (value < low) return low;
        if (value > high) return high;
        return value;
    }

    inline std::string lower(const std::string& text) {
        std::string out = text;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return out;
    }

    // 0 at the centre of the map, 1 at a corner: the distance a border mask is read from.
    inline float radialDistance(int x, int y, int size) {
        const float span = static_cast<float>(size - 1);
        const float u = (2.0f * static_cast<float>(x)) / span - 1.0f;
        const float v = (2.0f * static_cast<float>(y)) / span - 1.0f;
        return std::sqrt(u * u + v * v) * INVERSE_SQRT2;
    }

    // The border only starts to pull at three quarters of the way out, so an island keeps a round
    // interior instead of becoming a lens.
    inline float borderMask(float radius) {
        const float t = clampUnit((radius - 0.55f) / 0.45f);
        return t * t * (3.0f - 2.0f * t);
    }

    inline void applyErosion(std::vector<float>& heights, int size, const TerrainGenParams& params) {
        const TerrainMapRegion whole{0, 0, size - 1, size - 1};
        const auto everywhere = [](int, int) { return 1.0f; };

        if (params.erosionDroplets > 0) {
            const float heightScale = sanitize(params.erosionHeightScale, 1.0f, 4000.0f, 100.0f);
            TerrainErosion::hydraulic(heights, size, size, whole, heightScale,
                params.erosionDroplets, params.erosionSeed, everywhere);
        }
        if (params.thermalTalus > 0.0f) {
            const float heightScale = sanitize(params.erosionHeightScale, 1.0f, 4000.0f, 100.0f);
            TerrainErosion::thermal(heights, size, size, whole, heightScale,
                sanitize(params.thermalTalus, 0.0001f, 0.5f, 0.02f),
                sanitize(params.thermalRate, 0.0f, 1.0f, 0.5f), everywhere);
        }
    }
}

int TerrainGenerator::clampedSize(int size) {
    return std::clamp(size, MIN_SIZE, MAX_SIZE);
}

TerrainGenParams TerrainGenerator::preset(TerrainPreset preset) {
    TerrainGenParams params;

    switch (preset) {
        case TerrainPreset::ISLAND:
            params.type = NoiseType::SIMPLEX;
            params.octaves = 6;
            params.frequency = 0.010f;
            params.baseHeight = 0.14f;
            params.amplitude = 0.78f;
            params.ridgedAmount = 0.35f;
            params.warpAmplitude = 1.2f;
            params.warpFrequency = 0.35f;
            params.reliefCurve = 1.2f;
            params.seaLevel = 0.34f;
            params.islandFalloff = 1.0f;
            params.erosionDroplets = 6000;
            params.thermalTalus = 0.02f;
            break;
        case TerrainPreset::MOUNTAINS:
            params.type = NoiseType::SIMPLEX;
            params.octaves = 7;
            params.frequency = 0.016f;
            params.baseHeight = 0.05f;
            params.amplitude = 0.85f;
            params.ridgedAmount = 0.85f;
            params.warpAmplitude = 0.6f;
            params.warpFrequency = 0.5f;
            params.reliefCurve = 1.25f;
            params.seaLevel = 0.18f;
            params.erosionDroplets = 8000;
            params.thermalTalus = 0.03f;
            break;
        case TerrainPreset::PLAINS:
            params.type = NoiseType::PERLIN;
            params.octaves = 4;
            params.frequency = 0.010f;
            params.baseHeight = 0.30f;
            params.amplitude = 0.35f;
            params.reliefCurve = 1.4f;
            params.seaLevel = 0.16f;
            params.erosionDroplets = 1500;
            break;
        case TerrainPreset::CANYONS:
            params.type = NoiseType::SIMPLEX;
            params.octaves = 6;
            params.frequency = 0.014f;
            params.baseHeight = 0.10f;
            params.amplitude = 0.80f;
            params.ridgedAmount = 0.60f;
            params.warpAmplitude = 2.5f;
            params.warpFrequency = 0.25f;
            params.reliefCurve = 1.6f;
            params.seaLevel = 0.22f;
            params.erosionDroplets = 9000;
            params.thermalTalus = 0.015f;
            break;
        case TerrainPreset::CONTINENTS:
        default:
            params.type = NoiseType::SIMPLEX;
            params.octaves = 6;
            params.frequency = 0.008f;
            params.baseHeight = 0.15f;
            params.amplitude = 0.75f;
            params.ridgedAmount = 0.20f;
            params.warpAmplitude = 0.8f;
            params.warpFrequency = 0.35f;
            params.reliefCurve = 1.1f;
            params.seaLevel = 0.30f;
            params.erosionDroplets = 4000;
            params.thermalTalus = 0.02f;
            break;
    }

    return params;
}

TerrainPreset TerrainGenerator::presetFromName(const std::string& name) {
    const std::string key = lower(name);
    if (key == "island") return TerrainPreset::ISLAND;
    if (key == "mountains") return TerrainPreset::MOUNTAINS;
    if (key == "plains") return TerrainPreset::PLAINS;
    if (key == "canyons") return TerrainPreset::CANYONS;
    return TerrainPreset::CONTINENTS;
}

const char* TerrainGenerator::presetName(TerrainPreset preset) {
    switch (preset) {
        case TerrainPreset::ISLAND: return "island";
        case TerrainPreset::MOUNTAINS: return "mountains";
        case TerrainPreset::PLAINS: return "plains";
        case TerrainPreset::CANYONS: return "canyons";
        case TerrainPreset::CONTINENTS:
        default: return "continents";
    }
}

void TerrainGenerator::generate(const TerrainGenParams& params, std::vector<float>& heights) {
    const int size = clampedSize(params.size);
    const size_t texels = static_cast<size_t>(size) * static_cast<size_t>(size);

    heights.assign(texels, 0.0f);

    const float frequency = sanitize(params.frequency, 0.00002f, 1.0f, 0.012f);
    const float baseHeight = clampUnit(params.baseHeight);
    const float amplitude = sanitize(params.amplitude, 0.0f, 1.0f, 0.72f);
    const float ridgedAmount = clampUnit(params.ridgedAmount);
    const float reliefCurve = sanitize(params.reliefCurve, 0.25f, 4.0f, 1.0f);
    const float seaLevel = clampUnit(params.seaLevel);
    const float islandFalloff = clampUnit(params.islandFalloff);
    const bool warp = params.warpAmplitude > 0.0f;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float nx = static_cast<float>(x) * frequency;
            float ny = static_cast<float>(y) * frequency;
            if (warp) {
                Noise::warp2(nx, ny, params.seed, params.warpAmplitude, params.warpFrequency);
            }

            float field = Noise::fbm(params.type, nx, ny, params.seed, params.octaves,
                params.lacunarity, params.gain) * 0.5f + 0.5f;

            if (ridgedAmount > 0.0f) {
                const float ridge = Noise::ridged(params.type, nx, ny, params.seed ^ RELIEF_SEED,
                    params.octaves, params.lacunarity, params.gain);
                field += (ridge - field) * ridgedAmount;
            }

            // The relief curve is a power on the normalized field: above 1 it compresses the low
            // ground into plains and leaves the crests alone, below 1 does the opposite.
            field = std::pow(clampUnit(field), reliefCurve);

            float height = clampUnit(baseHeight + field * amplitude);

            if (height < seaLevel) {
                height = seaLevel - (seaLevel - height) * SUBSEA_DAMPING;
            }

            // The border mask pulls toward the map floor, not toward sea level: an island needs open
            // water around it, and a shelf exactly at the water line reads as a swimming pool.
            if (islandFalloff > 0.0f) {
                const float mask = borderMask(radialDistance(x, y, size)) * islandFalloff;
                height += (0.0f - height) * mask;
            }

            heights[static_cast<size_t>(y) * size + x] = clampUnit(height);
        }
    }

    applyErosion(heights, size, params);
}

void TerrainGenerator::encode16(const std::vector<float>& heights, std::vector<unsigned char>& pixels) {
    pixels.resize(heights.size() * 2u);
    for (size_t i = 0; i < heights.size(); i++) {
        const unsigned int quantized = static_cast<unsigned int>(std::lround(clampUnit(heights[i]) * 65535.0f));
        pixels[i * 2u] = static_cast<unsigned char>(quantized & 0xFF);
        pixels[i * 2u + 1u] = static_cast<unsigned char>((quantized >> 8) & 0xFF);
    }
}

bool TerrainGenerator::generatePng(const TerrainGenParams& params, const std::filesystem::path& path) {
    std::vector<float> heights;
    generate(params, heights);

    std::vector<unsigned char> pixels;
    encode16(heights, pixels);
    const int size = clampedSize(params.size);
    return writeGray16Png(path, size, size, pixels.data(), pixels.size());
}

float TerrainGenerator::waterHeight(const TerrainGenParams& params, float maxHeight) {
    // The vertex shader lifts a texel by heightMap.r * maxHeight above the entity's own plane, so the
    // water line for a map is the normalized sea level times the same scale.
    return clampUnit(params.seaLevel) * maxHeight;
}
