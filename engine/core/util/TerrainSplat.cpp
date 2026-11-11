// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "util/TerrainSplat.h"

#include "util/Noise.h"
#include "util/PngWriter.h"

#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

// The blend maps are not tiled textures - the shader samples them with the projected terrain
// coordinates - so the variation field here is ordinary lattice noise rather than the wrapping one
// the material maps need.

namespace quantum {

    namespace {

        // The nine slots, in the order blendMaps[m] channel c weights surfaceLayers[m * 3 + c].
        // Fixed, because a saved scene carries the layer list by position. The terrain has room for
        // nine painted layers and the library ships ten families: moss is what grass becomes in a wet
        // biome, which is FASE 6's call, so the automatic plan leaves it to the base material or to
        // painting.
        const int SLOT_SAND = 0;
        const int SLOT_GRASS = 1;
        const int SLOT_DIRT = 2;
        const int SLOT_GRAVEL = 3;
        const int SLOT_ROCK = 4;
        const int SLOT_CLIFF = 5;
        const int SLOT_SNOW = 6;
        const int SLOT_ICE = 7;
        const int SLOT_VOLCANIC = 8;

        float clamp01(float v) {
            return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }

        float smoothBand(float edge0, float edge1, float v) {
            const float t = clamp01((v - edge0) / std::max(edge1 - edge0, 1e-6f));
            return t * t * (3.0f - 2.0f * t);
        }

        // 1 inside `width` of `centre`, 0 outside - the shape of a band that has to sit between two
        // other materials rather than claim everything above or below a line.
        float bumpAround(float v, float centre, float width) {
            return 1.0f - smoothBand(0.0f, std::max(width, 1e-4f), std::fabs(v - centre));
        }

        // Bilinear read of the height field at uv in [0, 1], clamped at the borders: a blend map
        // resolution has nothing to do with the heightmap's, and wrapping would print the far edge of
        // the map into the near one.
        float sampleHeight(const std::vector<float>& heights, int size, float u, float v) {
            const float fx = std::clamp(u * static_cast<float>(size) - 0.5f, 0.0f, static_cast<float>(size - 1));
            const float fy = std::clamp(v * static_cast<float>(size) - 0.5f, 0.0f, static_cast<float>(size - 1));
            const int x0 = static_cast<int>(std::floor(fx));
            const int y0 = static_cast<int>(std::floor(fy));
            const int x1 = std::min(x0 + 1, size - 1);
            const int y1 = std::min(y0 + 1, size - 1);
            const float tx = fx - static_cast<float>(x0);
            const float ty = fy - static_cast<float>(y0);
            const float h00 = heights[static_cast<size_t>(y0) * size + x0];
            const float h10 = heights[static_cast<size_t>(y0) * size + x1];
            const float h01 = heights[static_cast<size_t>(y1) * size + x0];
            const float h11 = heights[static_cast<size_t>(y1) * size + x1];
            const float a = h00 + (h10 - h00) * tx;
            const float b = h01 + (h11 - h01) * tx;
            return a + (b - a) * ty;
        }

        // 1 - abs(normal.y) straight off the field, in world units: the same quantity the terrain
        // shader uses to move the detail onto the side projections, so a rule set here cannot call a
        // face steep that the renderer draws flat.
        void buildSlopes(const std::vector<float>& heights, int size, float maxHeight, float terrainSize,
                         std::vector<float>& slopes) {
            slopes.assign(static_cast<size_t>(size) * static_cast<size_t>(size), 0.0f);
            const float texel = terrainSize / static_cast<float>(std::max(size - 1, 1));
            const float invStep = texel > 0.0f ? 1.0f / texel : 0.0f;
            for (int y = 0; y < size; ++y) {
                const int ym = std::max(y - 1, 0);
                const int yp = std::min(y + 1, size - 1);
                for (int x = 0; x < size; ++x) {
                    const int xm = std::max(x - 1, 0);
                    const int xp = std::min(x + 1, size - 1);
                    const float dhdx = (heights[static_cast<size_t>(y) * size + xp] -
                                        heights[static_cast<size_t>(y) * size + xm]) * maxHeight * 0.5f * invStep;
                    const float dhdz = (heights[static_cast<size_t>(yp) * size + x] -
                                        heights[static_cast<size_t>(ym) * size + x]) * maxHeight * 0.5f * invStep;
                    const float length = std::sqrt(dhdx * dhdx + dhdz * dhdz);
                    // n = normalize(-dhdx, 1, -dhdz), so n.y = 1 / sqrt(1 + length^2)
                    slopes[static_cast<size_t>(y) * size + x] = 1.0f - 1.0f / std::sqrt(1.0f + length * length);
                }
            }
        }

        float variationField(float u, float v, uint32_t seed, float cells) {
            // [0, 1] from a noise in [-1, 1]; cells keeps the blob size a share of the map rather
            // than an absolute lattice distance, so a 256 and a 2048 map get the same look.
            const float n = Noise::fbm(NoiseType::VALUE, u * cells, v * cells, seed, 4, 2.0f, 0.5f);
            return clamp01(0.5f + 0.5f * n);
        }

        // How well one material suits one texel, before any noise pushes it. Each is a shape in the
        // altitude/slope plane, and they are meant to overlap: the mix at the bottom of the band is
        // where the two compete, not a line.
        float suitability(int slot, float altitude, float slope, const TerrainSplatRules& rules,
                          float dryField, float lavaField) {
            const float sea = rules.seaLevel;
            const float aboveWater = smoothBand(sea, sea + std::max(rules.beachWidth, 0.01f) * 0.8f, altitude);
            const float flat = 1.0f - smoothBand(rules.gravelSlope, rules.rockSlope, slope);

            switch (slot) {
                case SLOT_SAND: {
                    const float d = altitude - sea;
                    // A beach is a band around the waterline, not everything below it: the deep bed
                    // belongs to whatever the terrain material says.
                    const float width = d >= 0.0f ? rules.beachWidth : rules.shoreDepth;
                    const float atShore = 1.0f - smoothBand(width * 0.35f, width * 2.0f, std::fabs(d));
                    return atShore * (1.0f - 0.7f * smoothBand(rules.rockSlope, rules.cliffSlope, slope));
                }
                case SLOT_GRASS:
                    return flat * (1.0f - smoothBand(rules.dirtAltitude * 0.85f, rules.dirtAltitude * 1.3f, altitude)) *
                        aboveWater * (1.0f - 0.5f * lavaField);
                case SLOT_DIRT:
                    // Bare earth where the field says it is dry, and on the shoulders above the lowland -
                    // the two places grass genuinely does not hold.
                    return bumpAround(altitude, rules.dirtAltitude, 0.2f) *
                        (1.0f - smoothBand(rules.rockSlope, rules.cliffSlope, slope)) * aboveWater *
                        smoothBand(0.5f - 0.5f * rules.bareness, 1.0f, dryField);
                case SLOT_GRAVEL:
                    // Talus: the foot of a wall and the shoulder of a slope - steep enough that soil
                    // washes away, not steep enough to be the wall itself.
                    return smoothBand(rules.gravelSlope, rules.rockSlope, slope) *
                        (1.0f - smoothBand(rules.rockSlope * 1.6f, 1.0f, slope)) *
                        (0.4f + 0.6f * smoothBand(rules.dirtAltitude, rules.snowAltitude, altitude)) * aboveWater;
                case SLOT_ROCK:
                    return smoothBand(rules.rockSlope * 0.8f, rules.cliffSlope, slope) * (1.0f - 0.6f * lavaField * aboveWater);
                case SLOT_CLIFF:
                    return smoothBand(rules.cliffSlope * 0.92f, 0.9f, slope);
                case SLOT_SNOW:
                    // Snow holds on a high face but not on a vertical one, or a cliff would vanish
                    // under the coldest colour in the palette.
                    return smoothBand(rules.snowAltitude, rules.snowAltitude + 0.14f, altitude) *
                        (1.0f - 0.5f * smoothBand(rules.cliffSlope, 0.95f, slope)) * aboveWater;
                case SLOT_ICE:
                    return smoothBand(rules.iceAltitude, rules.iceAltitude + 0.1f, altitude) * flat * aboveWater;
                case SLOT_VOLCANIC:
                    // Off is off: a threshold of zero would otherwise read as "the whole map is lava
                    // rock", because the mask is measured from it upward.
                    if (rules.volcanic <= 0.0f) return 0.0f;
                    return smoothBand(rules.volcanic, rules.volcanic + 0.22f, lavaField) * aboveWater;
                default:
                    return 0.0f;
            }
        }

        // The two best-suited materials split the weight, sharpened so a clear winner is not diluted
        // by a runner-up that barely fits. Everything the layers leave unclaimed stays the base.
        void distribute(const float scores[9], float maxWeight, float weights[9]) {
            for (int i = 0; i < 9; ++i) weights[i] = 0.0f;

            int best = -1;
            int second = -1;
            for (int i = 0; i < 9; ++i) {
                if (scores[i] <= 0.02f) continue;
                if (best < 0 || scores[i] > scores[best]) {
                    second = best;
                    best = i;
                } else if (second < 0 || scores[i] > scores[second]) {
                    second = i;
                }
            }
            if (best < 0) return;

            const float cap = std::clamp(maxWeight, 0.0f, 1.0f);
            if (second < 0) {
                weights[best] = cap;
                return;
            }

            // Squared shares: 0.9 against 0.3 gives 0.97 of the weight to the winner, 0.5 against 0.45
            // gives them a near-even split, which is the mix a band edge wants.
            const float a = scores[best] * scores[best];
            const float b = scores[second] * scores[second];
            weights[best] = cap * a / (a + b);
            weights[second] = cap * b / (a + b);
        }

        // One texel of the whole rule set: altitude and slope at (u, v), the nine scores, the split.
        // `slopes` comes from buildSlopes, so the expensive pass over the field happens once.
        void scoresAt(const std::vector<float>& heights, const std::vector<float>& slopes, int size,
                      float u, float v, const TerrainSplatRules& rules, float weights[9]) {
            const float altitude = sampleHeight(heights, size, u, v);
            // Nearest field texel: a blend texel covers several of them at a coarse heightmap, and
            // averaging slopes there would round away the very cliffs the band keys on.
            const int sx = std::clamp(static_cast<int>(u * static_cast<float>(size)), 0, size - 1);
            const int sy = std::clamp(static_cast<int>(v * static_cast<float>(size)), 0, size - 1);
            const float slope = slopes[static_cast<size_t>(sy) * size + static_cast<size_t>(sx)];

            const float cells = 18.0f;
            const float dryField = variationField(u, v, rules.seed + 11u, cells * 0.6f);
            const float lavaField = variationField(u, v, rules.seed + 17u, cells * 0.35f);

            float scores[9];
            for (int slot = 0; slot < 9; ++slot) {
                float s = suitability(slot, altitude, slope, rules, dryField, lavaField);
                if (rules.variation > 0.0f) {
                    // Every material gets its own push, or the whole map would move as one and the
                    // bands would still be lines.
                    s *= 1.0f + rules.variation * (2.0f * variationField(u, v, rules.seed + 101u * slot, cells) - 1.0f);
                }
                scores[slot] = clamp01(s);
            }
            distribute(scores, rules.maxWeight, weights);
        }

    } // namespace

    int TerrainSplat::clampedResolution(int resolution) {
        return std::clamp(resolution, MIN_RESOLUTION, MAX_RESOLUTION);
    }

    std::vector<TerrainMaterialKind> TerrainSplat::layerPlan() {
        // The slot table, spelled as materials so a caller can build the layer list that matches it.
        return {
            TerrainMaterialKind::SAND,     // 0
            TerrainMaterialKind::GRASS,    // 1
            TerrainMaterialKind::DIRT,     // 2
            TerrainMaterialKind::GRAVEL,   // 3
            TerrainMaterialKind::ROCK,     // 4
            TerrainMaterialKind::CLIFF,    // 5
            TerrainMaterialKind::SNOW,     // 6
            TerrainMaterialKind::ICE,      // 7
            TerrainMaterialKind::VOLCANIC  // 8
        };
    }

    void TerrainSplat::bake(const std::vector<float>& heights, int size, float maxHeight, float terrainSize,
                            const TerrainSplatRules& rules, Maps& out) {
        out.resolution = 0;
        for (int m = 0; m < 3; ++m) out.blend[m].clear();
        if (size <= 0 || heights.size() < static_cast<size_t>(size) * static_cast<size_t>(size)) return;

        const int side = clampedResolution(rules.resolution);
        std::vector<float> slopes;
        buildSlopes(heights, size, std::max(maxHeight, 0.001f), std::max(terrainSize, 0.001f), slopes);

        for (int y = 0; y < side; ++y) {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(side);
            for (int x = 0; x < side; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(side);

                float weights[9];
                scoresAt(heights, slopes, size, u, v, rules, weights);

                for (int slot = 0; slot < 9; ++slot) {
                    if (weights[slot] <= 0.0f) continue;
                    std::vector<unsigned char>& map = out.blend[slot / 3];
                    if (map.empty()) {
                        map.assign(static_cast<size_t>(side) * static_cast<size_t>(side) * 3, 0);
                    }
                    const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(side) + static_cast<size_t>(x)) * 3;
                    map[o + static_cast<size_t>(slot % 3)] =
                        static_cast<unsigned char>(std::lround(weights[slot] * 255.0f));
                }
            }
        }

        // An untouched map still has to exist on disk: a missing blend map is a component with fewer
        // maps than the layer plan assumes.
        for (int m = 0; m < 3; ++m) {
            if (out.blend[m].empty()) {
                out.blend[m].assign(static_cast<size_t>(side) * static_cast<size_t>(side) * 3, 0);
            }
        }
        out.resolution = side;
    }

    bool TerrainSplat::bakeFromFile(const std::filesystem::path& heightmapPath, float maxHeight,
                                    float terrainSize, const TerrainSplatRules& rules, Maps& out,
                                    std::string& error) {
        error.clear();
        std::ifstream file(heightmapPath, std::ios::binary);
        if (!file) {
            error = "cannot read " + heightmapPath.string();
            return false;
        }
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        if (bytes.empty()) {
            error = "empty file " + heightmapPath.string();
            return false;
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_us* pixels = stbi_load_16_from_memory(reinterpret_cast<unsigned char*>(bytes.data()),
                                                   static_cast<int>(bytes.size()), &width, &height,
                                                   &channels, 1);
        if (!pixels || width <= 0 || height != width) {
            if (pixels) stbi_image_free(pixels);
            error = "not a square 16-bit heightmap: " + heightmapPath.string();
            return false;
        }

        // 16-bit samples, big-endian in the file, back to the normalized field the generator made.
        std::vector<float> heights(static_cast<size_t>(width) * static_cast<size_t>(width));
        for (size_t i = 0; i < heights.size(); ++i) {
            heights[i] = static_cast<float>(pixels[i]) / 65535.0f;
        }
        stbi_image_free(pixels);

        bake(heights, width, maxHeight, terrainSize, rules, out);
        return true;
    }

    bool TerrainSplat::write(const Maps& maps, const std::filesystem::path& directory,
                             const std::string& pathBase, Files& files, std::string& error) {
        error.clear();
        if (maps.resolution <= 0) {
            error = "no blend maps to write";
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(directory, ec);

        const size_t expected = static_cast<size_t>(maps.resolution) * static_cast<size_t>(maps.resolution) * 3;
        for (int m = 0; m < 3; ++m) {
            if (maps.blend[m].size() != expected) {
                error = "blend map " + std::to_string(m) + " is the wrong size";
                return false;
            }
            const std::string fileName = "blend_" + std::to_string(m) + ".png";
            const std::filesystem::path file = directory / fileName;
            if (!writeRgb8Png(file, maps.resolution, maps.resolution, maps.blend[m].data(), maps.blend[m].size())) {
                error = "cannot write " + file.string();
                return false;
            }
            files.blend[m] = pathBase.empty() ? file.generic_string() : pathBase + "/" + fileName;
        }
        return true;
    }

    bool TerrainSplat::bakeAndWrite(const std::vector<float>& heights, int size, float maxHeight,
                                    float terrainSize, const TerrainSplatRules& rules,
                                    const std::filesystem::path& directory, const std::string& pathBase,
                                    Files& files, std::string& error) {
        Maps maps;
        bake(heights, size, maxHeight, terrainSize, rules, maps);
        return write(maps, directory, pathBase, files, error);
    }

    void TerrainSplat::weightsAt(const std::vector<float>& heights, int size, float maxHeight,
                                 float terrainSize, const TerrainSplatRules& rules, int x, int y,
                                 float weights[9]) {
        for (int i = 0; i < 9; ++i) weights[i] = 0.0f;
        if (size <= 0 || x < 0 || y < 0 || x >= size || y >= size) return;
        if (heights.size() < static_cast<size_t>(size) * static_cast<size_t>(size)) return;

        std::vector<float> slopes;
        buildSlopes(heights, size, std::max(maxHeight, 0.001f), std::max(terrainSize, 0.001f), slopes);

        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        scoresAt(heights, slopes, size, u, v, rules, weights);
    }
}
