// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "TerrainCommands.h"

#include "Stream.h"

#include "util/PngWriter.h"
#include "util/TerrainGenerator.h"
#include "util/TerrainMaterialLibrary.h"
#include "util/TerrainSplat.h"

#include "stb_image.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace quantum;

namespace quantum::editor::cli {

namespace {

    void usage(const char* executableName) {
        std::cout
            << "usage: " << (executableName ? executableName : "quantum-editor-cmd")
            << " terrain <presets | generate | info MAP.png | materials | splat>\n"
            << "\n"
            << "  presets                 every preset and the parameters it means\n"
            << "  generate                write a 16-bit grayscale heightmap PNG\n"
            << "  info MAP.png            decode a heightmap and report its range\n"
            << "  materials               write the procedural terrain material set (5 maps each)\n"
            << "  splat                   bake the three blend maps a heightmap implies\n"
            << "\n"
            << "generate options (a preset sets the defaults, then any flag overrides it):\n"
            << "  --out PATH              where to write (required)\n"
            << "  --preset NAME           continents | island | mountains | plains | canyons\n"
            << "  --size N                texels per side, 8..4096 (256)\n"
            << "  --seed N                the whole map comes out of this one number\n"
            << "  --noise NAME            perlin | simplex | value\n"
            << "  --octaves N   --lacunarity F   --gain F\n"
            << "  --frequency F           lattice units per texel (0.012)\n"
            << "  --base F   --amplitude F   --curve F\n"
            << "  --ridged F              0 smooth sum, 1 all ridges\n"
            << "  --warp F   --warp-frequency F   domain warping, in lattice units\n"
            << "  --sea-level F   --island F      border mask, 1 pulls the edge under\n"
            << "  --droplets N   --erosion-scale F   --erosion-seed N\n"
            << "  --talus F    --thermal-rate F\n"
            << "\n"
            << "materials options:\n"
            << "  --out DIR               where to write (required, unless --plan)\n"
            << "  --plan                  list the materials and their recipes, write nothing\n"
            << "  --kinds a,b,c           grass,dirt,sand,rock,cliff,snow,ice,gravel,moss,volcanic\n"
            << "  --resolution N          map side, 16..2048 rounded to a power of two (512)\n"
            << "  --seed N                same seed, same bytes on every machine\n"
            << "  --path-base PREFIX      what the .material files reference instead of a name\n"
            << "  --material-files 1      also write a .material YAML per material\n"
            << "\n"
            << "splat options:\n"
            << "  --map HEIGHTMAP.png     the 16-bit map to read altitude and slope from (required)\n"
            << "  --out DIR               where blend_0/1/2.png go (required, unless --plan)\n"
            << "  --plan                  print the default rules and the nine-slot plan\n"
            << "  --resolution N          blend map side, 8..2048 (512); the heightmap's size is free\n"
            << "  --max-height F          the TerrainComponent's maxHeight (100)\n"
            << "  --terrain-size F        the TerrainComponent's terrainSize (200)\n"
            << "  --sea-level --beach-width --shore-depth --bareness --volcanic --variation\n"
            << "  --gravel-slope --rock-slope --cliff-slope   band edges, slope = 1 - abs(normal.y)\n"
            << "  --dirt-altitude --snow-altitude --ice-altitude   band edges, normalized altitude\n"
            << "  --max-weight F          what the layers may claim; the base material keeps the rest\n"
            << "  --seed N --path-base PREFIX\n"
            << "\n"
            << "The map is the format the terrain editor writes, so a generated heightmap can be\n"
            << "painted over afterwards. Nothing here needs a project or a window.\n";
    }

    struct Options {
        bool wantsHelp = false;
        std::string verb;
        std::string operand;
        std::string out;
        TerrainGenParams params;
        // The materials and splat verbs read these instead of `params`: a texture side, a rule set,
        // and the component numbers a slope is measured against.
        TerrainSplatRules splat;
        int resolution = TerrainMaterialLibrary::DEFAULT_SIZE;
        std::vector<std::string> kinds;
        std::string pathBase;
        std::string map;
        float maxHeight = 100.0f;
        float terrainSize = 200.0f;
        bool wantMaterialFiles = false;
        bool printPlan = false;
    };

    bool readInt(const char* text, int& out) {
        if (!text) return false;
        char* end = nullptr;
        const long value = std::strtol(text, &end, 10);
        if (end == text || *end != '\0') return false;
        out = static_cast<int>(value);
        return true;
    }

    bool readFloat(const char* text, float& out) {
        if (!text) return false;
        char* end = nullptr;
        const double value = std::strtod(text, &end);
        if (end == text || *end != '\0') return false;
        out = static_cast<float>(value);
        return true;
    }

    bool readUint(const char* text, uint32_t& out) {
        if (!text) return false;
        char* end = nullptr;
        const unsigned long value = std::strtoul(text, &end, 10);
        if (end == text || *end != '\0') return false;
        out = static_cast<uint32_t>(value);
        return true;
    }

    // "--key value" and "--key=value" both work: a shell history and a CI script disagree about
    // which is easier to quote, and the engine's other verbs accept both.
    bool takeValue(int& index, int argc, char** argv, const char* flag, std::string& value) {
        const std::string argument(argv[index]);
        const std::string prefix = std::string(flag) + "=";
        if (argument.rfind(prefix, 0) == 0) {
            value = argument.substr(prefix.size());
            return true;
        }
        if (argument == flag) {
            if (index + 1 >= argc) return false;
            value = argv[++index];
            return true;
        }
        return false;
    }

    NoiseType noiseFromName(const std::string& name) {
        if (name == "perlin") return NoiseType::PERLIN;
        if (name == "value") return NoiseType::VALUE;
        return NoiseType::SIMPLEX;
    }

    const char* noiseName(NoiseType type) {
        switch (type) {
            case NoiseType::PERLIN: return "perlin";
            case NoiseType::VALUE: return "value";
            case NoiseType::SIMPLEX: return "simplex";
        }
        return "simplex";
    }

    bool parseInto(Options& options, int argc, char** argv) {
        for (int i = 1; i < argc; i++) {
            const std::string argument(argv[i]);
            std::string value;

            if (argument == "--help" || argument == "-h") {
                options.wantsHelp = true;
                continue;
            }
            if (argument.rfind("--", 0) != 0) {
                if (options.verb.empty()) options.verb = argument;
                else options.operand = argument;
                continue;
            }

            if (takeValue(i, argc, argv, "--out", value)) options.out = value;
            else if (takeValue(i, argc, argv, "--preset", value)) {
                options.params = TerrainGenerator::preset(TerrainGenerator::presetFromName(value));
            }
            else if (takeValue(i, argc, argv, "--size", value)) {
                if (!readInt(value.c_str(), options.params.size)) return false;
            }
            else if (takeValue(i, argc, argv, "--seed", value)) {
                if (!readUint(value.c_str(), options.params.seed)) return false;
                // One seed for the whole command: the field and the noise that breaks up its bands
                // come from the same number, so a saved CLI line reproduces the map exactly.
                options.splat.seed = options.params.seed;
            }
            else if (takeValue(i, argc, argv, "--noise", value)) {
                options.params.type = noiseFromName(value);
            }
            else if (takeValue(i, argc, argv, "--octaves", value)) {
                if (!readInt(value.c_str(), options.params.octaves)) return false;
            }
            else if (takeValue(i, argc, argv, "--lacunarity", value)) {
                if (!readFloat(value.c_str(), options.params.lacunarity)) return false;
            }
            else if (takeValue(i, argc, argv, "--gain", value)) {
                if (!readFloat(value.c_str(), options.params.gain)) return false;
            }
            else if (takeValue(i, argc, argv, "--frequency", value)) {
                if (!readFloat(value.c_str(), options.params.frequency)) return false;
            }
            else if (takeValue(i, argc, argv, "--base", value)) {
                if (!readFloat(value.c_str(), options.params.baseHeight)) return false;
            }
            else if (takeValue(i, argc, argv, "--amplitude", value)) {
                if (!readFloat(value.c_str(), options.params.amplitude)) return false;
            }
            else if (takeValue(i, argc, argv, "--curve", value)) {
                if (!readFloat(value.c_str(), options.params.reliefCurve)) return false;
            }
            else if (takeValue(i, argc, argv, "--ridged", value)) {
                if (!readFloat(value.c_str(), options.params.ridgedAmount)) return false;
            }
            else if (takeValue(i, argc, argv, "--warp", value)) {
                if (!readFloat(value.c_str(), options.params.warpAmplitude)) return false;
            }
            else if (takeValue(i, argc, argv, "--warp-frequency", value)) {
                if (!readFloat(value.c_str(), options.params.warpFrequency)) return false;
            }
            else if (takeValue(i, argc, argv, "--sea-level", value)) {
                // One waterline for both halves of a map: the generator damps the bed below it and the
                // splat rules call the same line beach.
                if (!readFloat(value.c_str(), options.params.seaLevel)) return false;
                options.splat.seaLevel = options.params.seaLevel;
            }
            else if (takeValue(i, argc, argv, "--island", value)) {
                if (!readFloat(value.c_str(), options.params.islandFalloff)) return false;
            }
            else if (takeValue(i, argc, argv, "--droplets", value)) {
                if (!readInt(value.c_str(), options.params.erosionDroplets)) return false;
            }
            else if (takeValue(i, argc, argv, "--erosion-scale", value)) {
                if (!readFloat(value.c_str(), options.params.erosionHeightScale)) return false;
            }
            else if (takeValue(i, argc, argv, "--erosion-seed", value)) {
                if (!readUint(value.c_str(), options.params.erosionSeed)) return false;
            }
            else if (takeValue(i, argc, argv, "--talus", value)) {
                if (!readFloat(value.c_str(), options.params.thermalTalus)) return false;
            }
            else if (takeValue(i, argc, argv, "--thermal-rate", value)) {
                if (!readFloat(value.c_str(), options.params.thermalRate)) return false;
            }
            else if (takeValue(i, argc, argv, "--resolution", value)) {
                if (!readInt(value.c_str(), options.resolution)) return false;
                options.splat.resolution = options.resolution;
            }
            else if (takeValue(i, argc, argv, "--kinds", value)) {
                // Comma separated, because a shell loop over ten arguments is worse than one field.
                options.kinds.clear();
                std::stringstream split(value);
                std::string item;
                while (std::getline(split, item, ',')) {
                    if (!item.empty()) options.kinds.push_back(item);
                }
            }
            else if (takeValue(i, argc, argv, "--path-base", value)) {
                options.pathBase = value;
            }
            else if (takeValue(i, argc, argv, "--map", value)) {
                options.map = value;
            }
            else if (takeValue(i, argc, argv, "--material-files", value)) {
                if (value == "1" || value == "true") options.wantMaterialFiles = true;
                else if (value == "0" || value == "false") options.wantMaterialFiles = false;
                else return false;
            }
            else if (takeValue(i, argc, argv, "--max-height", value)) {
                if (!readFloat(value.c_str(), options.maxHeight)) return false;
            }
            else if (takeValue(i, argc, argv, "--terrain-size", value)) {
                if (!readFloat(value.c_str(), options.terrainSize)) return false;
            }
            else if (takeValue(i, argc, argv, "--beach-width", value)) {
                if (!readFloat(value.c_str(), options.splat.beachWidth)) return false;
            }
            else if (takeValue(i, argc, argv, "--shore-depth", value)) {
                if (!readFloat(value.c_str(), options.splat.shoreDepth)) return false;
            }
            else if (takeValue(i, argc, argv, "--gravel-slope", value)) {
                if (!readFloat(value.c_str(), options.splat.gravelSlope)) return false;
            }
            else if (takeValue(i, argc, argv, "--rock-slope", value)) {
                if (!readFloat(value.c_str(), options.splat.rockSlope)) return false;
            }
            else if (takeValue(i, argc, argv, "--cliff-slope", value)) {
                if (!readFloat(value.c_str(), options.splat.cliffSlope)) return false;
            }
            else if (takeValue(i, argc, argv, "--dirt-altitude", value)) {
                if (!readFloat(value.c_str(), options.splat.dirtAltitude)) return false;
            }
            else if (takeValue(i, argc, argv, "--snow-altitude", value)) {
                if (!readFloat(value.c_str(), options.splat.snowAltitude)) return false;
            }
            else if (takeValue(i, argc, argv, "--ice-altitude", value)) {
                if (!readFloat(value.c_str(), options.splat.iceAltitude)) return false;
            }
            else if (takeValue(i, argc, argv, "--bareness", value)) {
                if (!readFloat(value.c_str(), options.splat.bareness)) return false;
            }
            else if (takeValue(i, argc, argv, "--volcanic", value)) {
                if (!readFloat(value.c_str(), options.splat.volcanic)) return false;
            }
            else if (takeValue(i, argc, argv, "--variation", value)) {
                if (!readFloat(value.c_str(), options.splat.variation)) return false;
            }
            else if (takeValue(i, argc, argv, "--max-weight", value)) {
                if (!readFloat(value.c_str(), options.splat.maxWeight)) return false;
            }
            else if (argument == "--plan") {
                options.printPlan = true;
            }
            else {
                std::cerr << "terrain: unknown or incomplete argument: " << argument << "\n";
                return false;
            }
        }
        return true;
    }

    std::string fixed(float value, int precision = 3) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(precision) << value;
        return out.str();
    }

    void printParams(const TerrainGenParams& params) {
        std::cout
            << "  size " << params.size << "  seed " << params.seed << "  noise " << noiseName(params.type) << "\n"
            << "  octaves " << params.octaves << "  lacunarity " << fixed(params.lacunarity, 2)
            << "  gain " << fixed(params.gain, 2) << "  frequency " << fixed(params.frequency) << "\n"
            << "  base " << fixed(params.baseHeight) << "  amplitude " << fixed(params.amplitude)
            << "  curve " << fixed(params.reliefCurve) << "\n"
            << "  ridged " << fixed(params.ridgedAmount) << "  warp " << fixed(params.warpAmplitude)
            << " @ " << fixed(params.warpFrequency) << "\n"
            << "  sea level " << fixed(params.seaLevel) << "  island " << fixed(params.islandFalloff) << "\n"
            << "  droplets " << params.erosionDroplets << "  erosion scale "
            << fixed(params.erosionHeightScale, 1) << "  erosion seed " << params.erosionSeed << "\n"
            << "  talus " << fixed(params.thermalTalus) << "  thermal rate " << fixed(params.thermalRate) << "\n";
    }

    struct Stats {
        float min = 0.0f;
        float max = 0.0f;
        float mean = 0.0f;
        float belowSea = 0.0f;
        size_t texels = 0;
    };

    Stats measure(const std::vector<float>& heights, float seaLevel) {
        Stats stats;
        if (heights.empty()) return stats;

        stats.min = heights.front();
        stats.max = stats.min;
        double total = 0.0;
        size_t below = 0;
        for (const float h : heights) {
            stats.min = std::min(stats.min, h);
            stats.max = std::max(stats.max, h);
            total += h;
            if (h < seaLevel) below++;
        }
        stats.texels = heights.size();
        stats.mean = static_cast<float>(total / static_cast<double>(heights.size()));
        stats.belowSea = static_cast<float>(below) / static_cast<float>(heights.size());
        return stats;
    }

    void printStats(const Stats& stats, float seaLevel) {
        std::cout << "  min " << fixed(stats.min) << "  mean " << fixed(stats.mean)
                  << "  max " << fixed(stats.max) << "\n";
        std::cout << "  texels " << stats.texels << "  below " << fixed(seaLevel) << " water line: "
                  << static_cast<int>(stats.belowSea * 100.0f) << "%\n";
    }

    int runPresets() {
        const TerrainPreset names[] = {
            TerrainPreset::CONTINENTS, TerrainPreset::ISLAND, TerrainPreset::MOUNTAINS,
            TerrainPreset::PLAINS, TerrainPreset::CANYONS
        };
        for (const TerrainPreset preset : names) {
            std::cout << TerrainGenerator::presetName(preset) << ":\n";
            printParams(TerrainGenerator::preset(preset));
        }
        return 0;
    }

    int runGenerate(const Options& options) {
        if (options.out.empty()) {
            std::cerr << "terrain generate: --out PATH is required\n";
            return 1;
        }

        const TerrainGenParams& params = options.params;
        std::vector<float> heights;
        TerrainGenerator::generate(params, heights);

        std::vector<unsigned char> pixels;
        TerrainGenerator::encode16(heights, pixels);

        const std::filesystem::path path(options.out);
        std::error_code ec;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }

        if (!writeGray16Png(path, TerrainGenerator::clampedSize(params.size), TerrainGenerator::clampedSize(params.size), pixels.data(), pixels.size())) {
            std::cerr << "terrain generate: cannot write " << options.out << "\n";
            return 1;
        }

        std::cout << "terrain generate: wrote " << options.out << "\n";
        printParams(params);
        printStats(measure(heights, params.seaLevel), params.seaLevel);
        return 0;
    }

    int runInfo(const std::string& pathText, float seaLevel) {
        if (pathText.empty()) {
            std::cerr << "terrain info: a heightmap path is required\n";
            return 1;
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned short* loaded = stbi_load_16(pathText.c_str(), &width, &height, &channels, 1);
        if (!loaded) {
            std::cerr << "terrain info: cannot decode " << pathText << ": " << stbi_failure_reason() << "\n";
            return 1;
        }

        std::vector<float> heights(static_cast<size_t>(width) * static_cast<size_t>(height));
        for (size_t i = 0; i < heights.size(); i++) {
            heights[i] = static_cast<float>(loaded[i]) / 65535.0f;
        }
        stbi_image_free(loaded);

        std::cout << "terrain info: " << pathText << " is " << width << "x" << height
                  << " with " << channels << " channel(s)\n";
        printStats(measure(heights, seaLevel), seaLevel);
        return 0;
    }

    void printMaterialRoster() {
        std::cout << "materials (each is a tileable recipe over quantum::Noise, seed-reproducible):\n";
        for (const TerrainMaterialKind kind : TerrainMaterialLibrary::allKinds()) {
            const TerrainMaterialSpec& spec = TerrainMaterialLibrary::spec(kind);
            std::cout << "  " << TerrainMaterialLibrary::materialName(kind)
                      << "  rough " << fixed(spec.roughness, 2) << "  cells " << spec.baseCells
                      << "  octaves " << spec.octaves << "  pebble " << fixed(spec.pebbleAmount, 2)
                      << "  crack " << fixed(spec.crackAmount, 2) << "  bands " << fixed(spec.bandCount, 2)
                      << "  speckle " << fixed(spec.speckleAmount, 2) << "  relief " << fixed(spec.relief, 2)
                      << "\n";
        }
    }

    int runMaterials(const Options& options) {
        if (options.printPlan) {
            printMaterialRoster();
            return 0;
        }
        if (options.out.empty()) {
            std::cerr << "terrain materials: --out DIR is required (or --plan to list)\n";
            return 1;
        }

        std::vector<TerrainMaterialKind> kinds;
        for (const std::string& name : options.kinds) {
            TerrainMaterialKind kind;
            if (!TerrainMaterialLibrary::tryMaterialFromName(name, kind)) {
                std::cerr << "terrain materials: unknown material: " << name << "\n";
                printMaterialRoster();
                return 1;
            }
            kinds.push_back(kind);
        }
        if (kinds.empty()) kinds = TerrainMaterialLibrary::allKinds();

        const int size = TerrainMaterialLibrary::clampedSize(options.resolution);
        const std::filesystem::path directory(options.out);

        std::vector<TerrainMaterialLibrary::Entry> entries;
        std::string error;
        if (!TerrainMaterialLibrary::writeLibrary(kinds, options.params.seed, size, directory,
                                                  options.pathBase, entries, error)) {
            std::cerr << "terrain materials: " << error << "\n";
            return 1;
        }

        std::cout << "terrain materials: wrote " << entries.size() << " material(s), "
                  << size << "x" << size << ", seed " << options.params.seed << " into "
                  << directory.string() << "\n";
        for (const TerrainMaterialLibrary::Entry& entry : entries) {
            const std::string name(TerrainMaterialLibrary::materialName(entry.kind));
            std::cout << "  " << name << ": " << entry.files.baseColor << "  " << entry.files.normal
                      << "  " << entry.files.metallicRoughness << "  " << entry.files.occlusion
                      << "  " << entry.files.height << "\n";
            if (!options.wantMaterialFiles) continue;

            // The .material half is the editor's own format, so the keys come from Stream rather
            // than a second list here. embedTextureData is off: the maps are on disk by path.
            const std::filesystem::path materialPath = directory / (name + ".material");
            std::ofstream file(materialPath, std::ios::binary);
            if (!file) {
                std::cerr << "terrain materials: cannot write " << materialPath.string() << "\n";
                return 1;
            }
            file << YAML::Dump(Stream::encodeMaterial(entry.material, false));
            std::cout << "    " << materialPath.string() << "\n";
        }
        return 0;
    }

    void printSplatRules(const TerrainSplatRules& rules) {
        std::cout
            << "  resolution " << rules.resolution << "  seed " << rules.seed << "\n"
            << "  sea level " << fixed(rules.seaLevel) << "  beach " << fixed(rules.beachWidth)
            << "  shore " << fixed(rules.shoreDepth) << "\n"
            << "  gravel slope " << fixed(rules.gravelSlope) << "  rock slope " << fixed(rules.rockSlope)
            << "  cliff slope " << fixed(rules.cliffSlope) << "\n"
            << "  dirt " << fixed(rules.dirtAltitude) << "  snow " << fixed(rules.snowAltitude)
            << "  ice " << fixed(rules.iceAltitude) << "\n"
            << "  bareness " << fixed(rules.bareness) << "  volcanic " << fixed(rules.volcanic)
            << "  variation " << fixed(rules.variation) << "  max weight " << fixed(rules.maxWeight) << "\n";
    }

    void printLayerPlan() {
        std::cout << "  slot -> material (blendMaps[m] channel c weights surfaceLayers[m * 3 + c]):\n";
        const std::vector<TerrainMaterialKind> plan = TerrainSplat::layerPlan();
        for (size_t i = 0; i < plan.size(); i++) {
            std::cout << "  " << i << "  " << TerrainMaterialLibrary::materialName(plan[i]) << "\n";
        }
    }

    // The shader hands 1 - (r + g + b) to the terrain's own base material, so the mean channel sum
    // says how much of the map the rules claimed - the number to watch when a band looks wrong.
    void printCoverage(const TerrainSplat::Maps& maps) {
        for (int m = 0; m < 3; m++) {
            const size_t texels = maps.blend[m].size() / 3;
            if (texels == 0) continue;
            double total = 0.0;
            for (size_t t = 0; t < texels; t++) {
                total += (static_cast<double>(maps.blend[m][t * 3 + 0])
                        + static_cast<double>(maps.blend[m][t * 3 + 1])
                        + static_cast<double>(maps.blend[m][t * 3 + 2])) / 255.0;
            }
            std::cout << "  blend " << m << " claimed " << fixed(static_cast<float>(total / static_cast<double>(texels)))
                      << " per texel\n";
        }
    }

    int runSplat(const Options& options) {
        if (options.printPlan) {
            printSplatRules(options.splat);
            printLayerPlan();
            return 0;
        }
        if (options.map.empty() || options.out.empty()) {
            std::cerr << "terrain splat: --map HEIGHTMAP.png and --out DIR are required (or --plan)\n";
            return 1;
        }

        TerrainSplatRules rules = options.splat;
        rules.resolution = TerrainSplat::clampedResolution(rules.resolution);
        // A slope is a world-space quantity, so the component's numbers have to be the ones the
        // scene uses or every band lands on the wrong terrain.
        if (options.maxHeight <= 0.0f || options.terrainSize <= 0.0f) {
            std::cerr << "terrain splat: --max-height and --terrain-size must be positive\n";
            return 1;
        }

        TerrainSplat::Maps maps;
        std::string error;
        if (!TerrainSplat::bakeFromFile(std::filesystem::path(options.map), options.maxHeight,
                                        options.terrainSize, rules, maps, error)) {
            std::cerr << "terrain splat: " << error << "\n";
            return 1;
        }

        const std::filesystem::path directory(options.out);
        TerrainSplat::Files files;
        if (!TerrainSplat::write(maps, directory, options.pathBase, files, error)) {
            std::cerr << "terrain splat: " << error << "\n";
            return 1;
        }

        std::cout << "terrain splat: wrote 3 blend maps of " << maps.resolution << "x" << maps.resolution
                  << " from " << options.map << " into " << directory.string() << "\n";
        std::cout << "  " << files.blend[0] << "  " << files.blend[1] << "  " << files.blend[2] << "\n";
        printSplatRules(rules);
        printCoverage(maps);
        return 0;
    }
}

int runTerrainCommand(int argc, char** argv, const char* executableName) {
    Options options;
    if (!parseInto(options, argc, argv)) {
        usage(executableName);
        return 1;
    }

    if (options.wantsHelp || options.verb.empty()) {
        usage(executableName);
        return options.wantsHelp ? 0 : 1;
    }

    if (options.verb == "presets") return runPresets();
    if (options.verb == "generate") return runGenerate(options);
    if (options.verb == "info") return runInfo(options.operand, options.params.seaLevel);
    if (options.verb == "materials") return runMaterials(options);
    if (options.verb == "splat") return runSplat(options);

    std::cerr << "terrain: unknown verb: " << options.verb << "\n";
    usage(executableName);
    return 1;
}

}
