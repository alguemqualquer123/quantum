// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "util/TerrainMaterialLibrary.h"

#include "util/Noise.h"
#include "util/PngWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace quantum {

    namespace {

        const uint32_t kGolden = 0x9E3779B9u;

        float mix(float a, float b, float t) {
            return a + (b - a) * t;
        }

        float clamp01(float v) {
            return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }

        float smoothstep01(float edge0, float edge1, float v) {
            const float t = clamp01((v - edge0) / std::max(edge1 - edge0, 1e-6f));
            return t * t * (3.0f - 2.0f * t);
        }

        // Positive modulo: a texel left of the lattice origin has to read the wrapped cell, not a
        // negative one, or the tile's edges stop matching.
        int wrapCell(int index, int cells) {
            const int m = index % cells;
            return m < 0 ? m + cells : m;
        }

        // Value noise on a lattice that wraps at `cells`. This is what makes every map here
        // tileable: the texture repeats at uv 1.0, and a field sampled on a lattice that is an
        // integer number of cells per tile has no seam to hide.
        float tileValue(float u, float v, int cells, uint32_t seed) {
            if (cells < 1) cells = 1;
            const float x = u * static_cast<float>(cells);
            const float y = v * static_cast<float>(cells);
            const int ix = static_cast<int>(std::floor(x));
            const int iy = static_cast<int>(std::floor(y));
            const float sx = smoothstep01(0.0f, 1.0f, x - static_cast<float>(ix));
            const float sy = smoothstep01(0.0f, 1.0f, y - static_cast<float>(iy));

            const float v00 = Noise::hash2(wrapCell(ix, cells), wrapCell(iy, cells), seed);
            const float v10 = Noise::hash2(wrapCell(ix + 1, cells), wrapCell(iy, cells), seed);
            const float v01 = Noise::hash2(wrapCell(ix, cells), wrapCell(iy + 1, cells), seed);
            const float v11 = Noise::hash2(wrapCell(ix + 1, cells), wrapCell(iy + 1, cells), seed);

            return mix(mix(v00, v10, sx), mix(v01, v11, sx), sy);
        }

        // The fractal sum of the wrapping lattice. Each octave doubles an integer cell count, so the
        // whole sum still closes on itself at the tile edge.
        float tileFbm(float u, float v, int baseCells, int octaves, float gain, uint32_t seed) {
            const int count = std::clamp(octaves, 1, Noise::MAX_OCTAVES);
            const float g = std::clamp(gain, 0.0f, 1.0f);
            float sum = 0.0f;
            float weight = 0.0f;
            float w = 1.0f;
            int cells = std::max(baseCells, 1);
            for (int o = 0; o < count; ++o) {
                sum += w * tileValue(u, v, cells, seed + static_cast<uint32_t>(o) * 1013u);
                weight += w;
                w *= g;
                cells *= 2;
            }
            return weight > 0.0f ? sum / weight : 0.5f;
        }

        // Sharp crests where the field crosses zero - the fracture and fissure shape. [0, 1].
        float tileRidged(float u, float v, int baseCells, int octaves, float gain, uint32_t seed) {
            const int count = std::clamp(octaves, 1, Noise::MAX_OCTAVES);
            const float g = std::clamp(gain, 0.0f, 1.0f);
            float sum = 0.0f;
            float weight = 0.0f;
            float w = 1.0f;
            int cells = std::max(baseCells, 1);
            for (int o = 0; o < count; ++o) {
                const float n = tileValue(u, v, cells, seed + static_cast<uint32_t>(o) * 1013u) * 2.0f - 1.0f;
                const float ridge = 1.0f - std::fabs(n);
                sum += w * ridge * ridge;
                weight += w;
                w *= g;
                cells *= 2;
            }
            return weight > 0.0f ? sum / weight : 0.5f;
        }

        // Jittered voronoi on the wrapping lattice. `f1` is the distance to the nearest point (a
        // pebble falls where it is small), `edge` the gap to the second (a crack line is where it
        // goes to zero). Positions come from the unwrapped cell index while the jitter comes from
        // the wrapped one, so a neighbour across the tile border sits on the far side and still
        // measures the distance it should.
        void tileVoronoi(float u, float v, int cells, uint32_t seed, float& f1, float& edge) {
            if (cells < 1) cells = 1;
            const float x = u * static_cast<float>(cells);
            const float y = v * static_cast<float>(cells);
            const int cx = static_cast<int>(std::floor(x));
            const int cy = static_cast<int>(std::floor(y));

            float nearest = 1e9f;
            float second = 1e9f;
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    const int gx = wrapCell(cx + ox, cells);
                    const int gy = wrapCell(cy + oy, cells);
                    const float px = static_cast<float>(cx + ox) + Noise::hash2(gx, gy, seed);
                    const float py = static_cast<float>(cy + oy) + Noise::hash2(gx, gy, seed + kGolden);
                    const float dx = px - x;
                    const float dy = py - y;
                    const float d = dx * dx + dy * dy;
                    if (d < nearest) {
                        second = nearest;
                        nearest = d;
                    } else if (d < second) {
                        second = d;
                    }
                }
            }

            f1 = std::sqrt(nearest);
            edge = second < 1e8f ? std::sqrt(second) - std::sqrt(nearest) : 1.0f;
        }

        // Everything one texel of one material is made of, before any byte is written.
        struct TexelField {
            float height;    // [0, 1] around a half, the relief the normal and the blend read
            float albedoMix; // [0, 1] between the spec's two colours
            float roughness; // [0, 1]
            float occlusion; // [0, 1]
        };

        TexelField sampleField(const TerrainMaterialSpec& spec, float u, float v, uint32_t seed) {
            TexelField out;

            const float base = tileFbm(u, v, spec.baseCells, spec.octaves, spec.gain, seed);

            float pebble = 0.0f;
            float crack = 0.0f;
            if (spec.pebbleAmount > 0.0f || spec.crackAmount > 0.0f) {
                float f1 = 0.0f;
                float edge = 0.0f;
                tileVoronoi(u, v, std::max(spec.baseCells * 2, 2), seed + 977u, f1, edge);
                pebble = clamp01(1.0f - f1 * 1.7f);
                // The width of a fracture band scales with how much of it the material wants.
                const float width = 0.05f + 0.25f * spec.crackAmount;
                crack = 1.0f - smoothstep01(0.0f, width, edge);
            }
            if (spec.crackAmount > 0.0f) {
                // A second, larger fracture set, so the pattern never reads as one cell grid.
                const float fissure = 1.0f - tileRidged(u, v, std::max(spec.baseCells / 2, 1), 3, spec.gain, seed + 313u);
                crack = std::max(crack, fissure * spec.crackAmount);
            }

            float band = 0.5f;
            if (spec.bandCount > 0.0f) {
                // An integer band count keeps the stripe closed at the tile edge; the warp rides on a
                // wrapping field, so bending the strata does not open a seam either.
                const int bands = std::max(static_cast<int>(std::lround(spec.bandCount)), 1);
                const float warped = v * static_cast<float>(bands) +
                    spec.bandWarp * (tileFbm(u, v, spec.baseCells, 3, spec.gain, seed + 41u) - 0.5f) * 2.0f;
                band = 0.5f + 0.5f * std::sin(warped * 6.2831853f);
            }

            float speckle = 0.0f;
            if (spec.speckleAmount > 0.0f) {
                speckle = smoothstep01(0.90f, 0.995f, tileValue(u, v, 64, seed + 7919u)) * spec.speckleAmount;
            }

            // The relief is the structure, not the colour: pebbles stand, cracks and fissures sink.
            float shape = base;
            shape = mix(shape, pebble, spec.pebbleAmount);
            shape = mix(shape, band, spec.bandCount > 0.0f ? 0.35f : 0.0f);
            shape -= crack * (0.35f + 0.4f * spec.crackAmount);
            out.height = clamp01(0.5f + (shape - 0.5f) * spec.heightAmplitude * spec.relief + speckle * 0.25f);

            float tint = base * 0.55f + band * (spec.bandCount > 0.0f ? 0.3f : 0.0f) +
                pebble * spec.pebbleAmount * 0.45f + speckle;
            tint = clamp01(0.5f + (tint - 0.5f) * spec.colorContrast);
            // A fracture is dirtier and darker than the surface around it.
            out.albedoMix = clamp01(tint * (1.0f - crack * 0.55f));

            out.roughness = clamp01(spec.roughness + (base - 0.5f) * 2.0f * spec.roughnessVariation +
                crack * 0.12f - pebble * 0.05f);

            out.occlusion = clamp01(1.0f - crack * 0.75f - (1.0f - base) * 0.3f - (1.0f - out.height) * 0.2f);

            return out;
        }

        unsigned char toByte(float v) {
            return static_cast<unsigned char>(std::lround(clamp01(v) * 255.0f));
        }

        const TerrainMaterialSpec& specAt(TerrainMaterialKind kind) {
            static const TerrainMaterialSpec kSpecs[static_cast<int>(TerrainMaterialKind::COUNT)] = {
                // name, lowColor, highColor, roughness, roughVar, metallic,
                // baseCells, octaves, gain, pebble, crack, bands, bandWarp, speckle, relief,
                // heightAmplitude, normalStrength, colorContrast
                {
                    "grass", {0.085f, 0.16f, 0.055f}, {0.30f, 0.44f, 0.13f},
                    0.86f, 0.10f, 0.0f,
                    6, 6, 0.55f, 0.0f, 0.0f, 0.0f, 0.0f, 0.05f, 1.0f,
                    0.30f, 0.8f, 1.15f
                },
                {
                    "dirt", {0.15f, 0.10f, 0.06f}, {0.40f, 0.28f, 0.17f},
                    0.92f, 0.08f, 0.0f,
                    5, 6, 0.52f, 0.45f, 0.20f, 0.0f, 0.0f, 0.0f, 1.0f,
                    0.34f, 0.9f, 1.0f
                },
                {
                    "sand", {0.62f, 0.52f, 0.35f}, {0.86f, 0.78f, 0.58f},
                    0.76f, 0.06f, 0.0f,
                    8, 4, 0.45f, 0.30f, 0.0f, 9.0f, 0.9f, 0.0f, 0.7f,
                    0.20f, 0.6f, 0.8f
                },
                {
                    "rock", {0.21f, 0.21f, 0.22f}, {0.54f, 0.53f, 0.50f},
                    0.82f, 0.10f, 0.0f,
                    4, 6, 0.55f, 0.25f, 0.55f, 0.0f, 0.0f, 0.0f, 1.2f,
                    0.40f, 1.1f, 1.1f
                },
                {
                    "cliff", {0.12f, 0.12f, 0.13f}, {0.42f, 0.41f, 0.38f},
                    0.78f, 0.12f, 0.0f,
                    4, 5, 0.5f, 0.15f, 0.75f, 6.0f, 0.35f, 0.0f, 1.5f,
                    0.52f, 1.35f, 1.25f
                },
                {
                    "snow", {0.80f, 0.83f, 0.90f}, {0.98f, 0.99f, 1.00f},
                    0.62f, 0.14f, 0.0f,
                    6, 5, 0.5f, 0.20f, 0.0f, 0.0f, 0.0f, 0.55f, 0.6f,
                    0.18f, 0.45f, 0.6f
                },
                {
                    "ice", {0.55f, 0.70f, 0.80f}, {0.88f, 0.95f, 1.00f},
                    0.26f, 0.10f, 0.0f,
                    3, 5, 0.55f, 0.0f, 0.65f, 0.0f, 0.0f, 0.25f, 1.3f,
                    0.34f, 0.8f, 0.9f
                },
                {
                    "gravel", {0.24f, 0.23f, 0.22f}, {0.62f, 0.60f, 0.56f},
                    0.86f, 0.08f, 0.0f,
                    3, 5, 0.5f, 0.90f, 0.65f, 0.0f, 0.0f, 0.05f, 1.4f,
                    0.55f, 1.2f, 1.3f
                },
                {
                    "moss", {0.11f, 0.21f, 0.05f}, {0.48f, 0.58f, 0.16f},
                    0.95f, 0.05f, 0.0f,
                    5, 6, 0.6f, 0.35f, 0.0f, 0.0f, 0.0f, 0.08f, 1.1f,
                    0.28f, 0.7f, 1.2f
                },
                {
                    "volcanic", {0.030f, 0.026f, 0.025f}, {0.21f, 0.17f, 0.15f},
                    0.90f, 0.06f, 0.0f,
                    4, 6, 0.55f, 0.70f, 0.85f, 0.0f, 0.0f, 0.0f, 1.3f,
                    0.48f, 1.25f, 1.2f
                },
            };

            const int index = std::clamp(static_cast<int>(kind), 0, static_cast<int>(TerrainMaterialKind::COUNT) - 1);
            return kSpecs[index];
        }

        // The file name of one map of one material. One place, so the paths written into the
        // material and the paths written to disk cannot disagree.
        std::string mapFileName(const TerrainMaterialSpec& spec, const char* suffix) {
            return std::string(spec.name) + "_" + suffix + ".png";
        }

    } // namespace

    int TerrainMaterialLibrary::clampedSize(int size) {
        // A power of two, because the detail array and every mip chain in the engine want one, and
        // the nearest rather than the next up: a 300 request is much closer to a 256 map than to a
        // 512 one, and asking for 3000 texels must not quietly become 4096 worth of work.
        if (size <= MIN_SIZE) return MIN_SIZE;
        if (size >= MAX_SIZE) return MAX_SIZE;

        int down = MIN_SIZE;
        while (down * 2 <= size) down *= 2;
        const int up = down * 2;
        // The geometric midpoint, so the choice does not depend on where in the range the ask lands.
        // 64-bit because size*size at the top of the range is 16 million, and a cap that ever moves
        // would turn that into an overflow rather than a wrong answer.
        const long long ask = static_cast<long long>(size) * size;
        const long long midpoint = static_cast<long long>(down) * up;
        return std::clamp(ask >= midpoint ? up : down, MIN_SIZE, MAX_SIZE);
    }

    const char* TerrainMaterialLibrary::materialName(TerrainMaterialKind kind) {
        return specAt(kind).name;
    }

    TerrainMaterialKind TerrainMaterialLibrary::materialFromName(const std::string& name) {
        TerrainMaterialKind kind = TerrainMaterialKind::GRASS;
        if (!tryMaterialFromName(name, kind)) {
            kind = TerrainMaterialKind::GRASS;
        }
        return kind;
    }

    bool TerrainMaterialLibrary::tryMaterialFromName(const std::string& name, TerrainMaterialKind& out) {
        for (int i = 0; i < static_cast<int>(TerrainMaterialKind::COUNT); ++i) {
            if (name == specAt(static_cast<TerrainMaterialKind>(i)).name) {
                out = static_cast<TerrainMaterialKind>(i);
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> TerrainMaterialLibrary::materialNames() {
        std::vector<std::string> names;
        names.reserve(static_cast<size_t>(TerrainMaterialKind::COUNT));
        for (int i = 0; i < static_cast<int>(TerrainMaterialKind::COUNT); ++i) {
            names.push_back(specAt(static_cast<TerrainMaterialKind>(i)).name);
        }
        return names;
    }

    std::vector<TerrainMaterialKind> TerrainMaterialLibrary::allKinds() {
        std::vector<TerrainMaterialKind> kinds;
        kinds.reserve(static_cast<size_t>(TerrainMaterialKind::COUNT));
        for (int i = 0; i < static_cast<int>(TerrainMaterialKind::COUNT); ++i) {
            kinds.push_back(static_cast<TerrainMaterialKind>(i));
        }
        return kinds;
    }

    const TerrainMaterialSpec& TerrainMaterialLibrary::spec(TerrainMaterialKind kind) {
        return specAt(kind);
    }

    void TerrainMaterialLibrary::build(TerrainMaterialKind kind, uint32_t seed, int size, Maps& out) {
        const TerrainMaterialSpec& spec = specAt(kind);
        const int side = clampedSize(size);
        out.size = side;

        // The field is evaluated once per texel into float buffers; the bytes - and especially the
        // normal - are derived from those, so a bump never disagrees with the height the blend uses.
        std::vector<float> height(static_cast<size_t>(side) * static_cast<size_t>(side));
        std::vector<float> albedo(height.size());
        std::vector<float> rough(height.size());
        std::vector<float> occlusion(height.size());

        const float inv = 1.0f / static_cast<float>(side);
        for (int y = 0; y < side; ++y) {
            // Texel centers, so the field samples symmetrically about the tile edge.
            const float v = (static_cast<float>(y) + 0.5f) * inv;
            for (int x = 0; x < side; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) * inv;
                const TexelField f = sampleField(spec, u, v, seed);
                const size_t i = static_cast<size_t>(y) * static_cast<size_t>(side) + static_cast<size_t>(x);
                height[i] = f.height;
                albedo[i] = f.albedoMix;
                rough[i] = f.roughness;
                occlusion[i] = f.occlusion;
            }
        }

        out.baseColor.resize(albedo.size() * 3);
        out.normal.resize(albedo.size() * 3);
        out.metallicRoughness.resize(albedo.size() * 3);
        out.occlusion.resize(albedo.size());
        out.height.resize(albedo.size());

        // Slope to bump. The derivative is taken in UV space (a texel step is 1/size), so a map
        // rebuilt at another resolution shades the same hill the same way, and the wrapped
        // neighbours are what keep that derivative honest across a tile edge.
        const float slope = spec.normalStrength * spec.relief * 0.35f;

        for (int y = 0; y < side; ++y) {
            const int up = wrapCell(y - 1, side);
            const int down = wrapCell(y + 1, side);
            for (int x = 0; x < side; ++x) {
                const size_t i = static_cast<size_t>(y) * static_cast<size_t>(side) + static_cast<size_t>(x);
                const int left = wrapCell(x - 1, side);
                const int right = wrapCell(x + 1, side);

                const float halfTexel = 0.5f * static_cast<float>(side);
                const float dhdx = (height[static_cast<size_t>(y) * static_cast<size_t>(side) + static_cast<size_t>(right)] -
                                    height[static_cast<size_t>(y) * static_cast<size_t>(side) + static_cast<size_t>(left)]) *
                                   halfTexel * slope;
                const float dhdy = (height[static_cast<size_t>(down) * static_cast<size_t>(side) + static_cast<size_t>(x)] -
                                    height[static_cast<size_t>(up) * static_cast<size_t>(side) + static_cast<size_t>(x)]) *
                                   halfTexel * slope;

                // Tangent-space normal, Z up, encoded the way every normal map in this engine is.
                const float nx = -dhdx;
                const float ny = -dhdy;
                const float nz = 1.0f;
                const float invLen = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);

                const size_t o3 = i * 3;
                const float t = albedo[i];
                out.baseColor[o3 + 0] = toByte(mix(spec.lowColor[0], spec.highColor[0], t));
                out.baseColor[o3 + 1] = toByte(mix(spec.lowColor[1], spec.highColor[1], t));
                out.baseColor[o3 + 2] = toByte(mix(spec.lowColor[2], spec.highColor[2], t));

                out.normal[o3 + 0] = toByte(0.5f + 0.5f * nx * invLen);
                out.normal[o3 + 1] = toByte(0.5f + 0.5f * ny * invLen);
                out.normal[o3 + 2] = toByte(0.5f + 0.5f * nz * invLen);

                // glTF packing: the channel a roughness map is read from is green, metal is blue.
                out.metallicRoughness[o3 + 0] = 0;
                out.metallicRoughness[o3 + 1] = toByte(rough[i]);
                out.metallicRoughness[o3 + 2] = toByte(spec.metallic);

                out.occlusion[i] = toByte(occlusion[i]);
                out.height[i] = toByte(height[i]);
            }
        }
    }

    bool TerrainMaterialLibrary::writeMaps(TerrainMaterialKind kind, uint32_t seed, int size,
                                           const std::filesystem::path& directory, const std::string& pathBase,
                                           Files& files, Material& material, std::string& error) {
        const TerrainMaterialSpec& spec = specAt(kind);
        error.clear();

        std::error_code ec;
        std::filesystem::create_directories(directory, ec);

        Maps maps;
        build(kind, seed, size, maps);
        const size_t bytes = static_cast<size_t>(maps.size) * static_cast<size_t>(maps.size);

        struct Slot {
            const char* suffix;
            const std::vector<unsigned char>* pixels;
            int channels;
            std::string* destination;
        };
        const Slot slots[] = {
            {"basecolor", &maps.baseColor, 3, &files.baseColor},
            {"normal", &maps.normal, 3, &files.normal},
            {"metallicroughness", &maps.metallicRoughness, 3, &files.metallicRoughness},
            {"occlusion", &maps.occlusion, 1, &files.occlusion},
            {"height", &maps.height, 1, &files.height},
        };

        for (const Slot& slot : slots) {
            const std::filesystem::path file = directory / mapFileName(spec, slot.suffix);
            const bool written = slot.channels == 3
                ? writeRgb8Png(file, maps.size, maps.size, slot.pixels->data(), slot.pixels->size())
                : writeGray8Png(file, maps.size, maps.size, slot.pixels->data(), slot.pixels->size());
            if (!written) {
                error = std::string("cannot write ") + file.string();
                return false;
            }
            slot.destination->assign(pathBase.empty() ? file.generic_string() : pathBase + "/" + mapFileName(spec, slot.suffix));
        }

        material = Material();
        material.name = spec.name;
        // Authored colours are the sRGB bytes the map already carries, so the factor stays white: a
        // tint here would double the palette the texture spells.
        material.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        material.roughnessFactor = 1.0f;
        material.metallicFactor = 1.0f;
        material.baseColorTexture.setPath(files.baseColor);
        material.normalTexture.setPath(files.normal);
        material.metallicRoughnessTexture.setPath(files.metallicRoughness);
        material.occlusionTexture.setPath(files.occlusion);
        material.heightTexture.setPath(files.height);
        return true;
    }

    bool TerrainMaterialLibrary::writeLibrary(const std::vector<TerrainMaterialKind>& kinds, uint32_t seed,
                                              int size, const std::filesystem::path& directory,
                                              const std::string& pathBase, std::vector<Entry>& entries,
                                              std::string& error) {
        entries.clear();
        if (kinds.empty()) {
            error = "no material requested";
            return false;
        }

        for (size_t i = 0; i < kinds.size(); ++i) {
            Entry entry;
            entry.kind = kinds[i];
            // The offset is the material's own ordinal, not its position in the request: a script that
            // asks for {sand, rock} and a CLI that asks for the whole set must get the same bytes for
            // sand, or a seed stops being a save-game field.
            if (!writeMaps(kinds[i], seed + static_cast<uint32_t>(kinds[i]), size, directory, pathBase,
                           entry.files, entry.material, error)) {
                entries.clear();
                return false;
            }
            entries.push_back(std::move(entry));
        }
        return true;
    }

    TerrainSurfaceLayer TerrainMaterialLibrary::layerFor(const Material& material,
                                                          const TerrainMaterialSpec& spec, float uvScale) {
        TerrainSurfaceLayer layer = terrainLayerFromMaterial(material, TerrainSurfaceLayer());
        // The bridge above carries every map; these two are the material's own numbers, which no
        // Material field exists for.
        layer.normalStrength = spec.normalStrength;
        const float s = uvScale > 0.0f ? uvScale : 1.0f;
        layer.uvScale = Vector2(s, s);
        return layer;
    }
}
