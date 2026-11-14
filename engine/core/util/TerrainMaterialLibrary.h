// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef TerrainMaterialLibrary_h
#define TerrainMaterialLibrary_h

#include "Export.h"

#include "component/TerrainComponent.h"
#include "texture/Material.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace quantum {

    // The surface families a generated world is made of. Names, not ordinals, travel through
    // manifests, CLI arguments and saved scenes, so an entry added in the middle cannot re-point
    // a stored name at a different material - but appending is still what keeps a written
    // `heightmap.yaml` readable by an older editor.
    enum class TerrainMaterialKind : int {
        GRASS = 0,
        DIRT = 1,
        SAND = 2,
        ROCK = 3,
        CLIFF = 4,
        SNOW = 5,
        ICE = 6,
        GRAVEL = 7,
        MOSS = 8,
        VOLCANIC = 9,
        COUNT = 10
    };

    // What one material is made of: a palette and the structural recipe that splits it. Plain data,
    // so a plugin schema or a scene key can override any single field.
    struct TerrainMaterialSpec {
        const char* name;

        // Albedo endpoints, authored as the sRGB bytes the texture stores. The field mixes between
        // them, so a material is two colours and a structure, never a photograph.
        float lowColor[3];
        float highColor[3];

        float roughness = 0.8f;          // the value the map centres on
        float roughnessVariation = 0.1f; // how far the field pushes it, in both directions
        float metallic = 0.0f;           // 0 for every ground material; a shader multiplies this in

        // Structure. `baseCells` is the tileable lattice (integer cells per tile edge), so the map
        // tiles with no seam whatever the octave count does to it.
        int baseCells = 4;
        int octaves = 5;
        float gain = 0.5f;
        float pebbleAmount = 0.0f;  // jittered voronoi grain: gravel, sand, basalt bubbles
        float crackAmount = 0.0f;   // ridged / voronoi-edge fractures: rock, cliff, volcanic
        float bandCount = 0.0f;     // integer strata or dune frequency across v; 0 = none
        float bandWarp = 0.0f;      // how far the bands bend, in lattice units
        float speckleAmount = 0.0f; // sparse bright points: snow and ice sparkle
        float relief = 1.0f;        // scales the height field, so the normal and the blend agree

        float heightAmplitude = 0.25f; // share of the map's range the relief occupies
        float normalStrength = 1.0f;   // stored on the layer, not baked twice into the map
        float colorContrast = 1.0f;    // >1 pushes the palette toward its endpoints
    };

    // Procedural terrain materials, built from quantum::Noise's own lattice so the same seed gives
    // the same bytes on MSVC, MinGW and any machine - which is what makes a material set a
    // save-game field rather than an asset someone has to keep. Nothing here downloads or reads a
    // third-party file: the licence of every byte is this repository's.
    //
    // The output is the set of maps the terrain layer and the `.material` format already read:
    // an sRGB base colour, a tangent-space normal, a glTF metallicRoughness (G=rough, B=metal),
    // an occlusion and a blending height. A caller that wants a `.material` file on disk takes the
    // returned `Material` to `editor::Stream::encodeMaterial` - the engine cannot write YAML for a
    // material, and duplicating the key list here would be a drift waiting to happen.
    class QUANTUM_API TerrainMaterialLibrary {
    public:
        // 16 is the smallest tile that still carries four octaves of a 4-cell lattice; 2048 is
        // MAX_TERRAIN_DETAIL_SIZE, the cap the detail array already enforces on a source map.
        static const int MIN_SIZE = 16;
        static const int MAX_SIZE = 2048;
        static const int DEFAULT_SIZE = 512;

        // The side length a request actually produces (a texture size is always a power of two here,
        // so a caller gets the nearest one inside the range rather than the number it asked for).
        static int clampedSize(int size);

        static const char* materialName(TerrainMaterialKind kind);
        static TerrainMaterialKind materialFromName(const std::string& name);
        // False on an unknown name, so a CLI can report it instead of silently drawing grass.
        static bool tryMaterialFromName(const std::string& name, TerrainMaterialKind& out);
        static std::vector<std::string> materialNames();
        static const TerrainMaterialSpec& spec(TerrainMaterialKind kind);

        // The five maps for one material, in memory. `size` is clamped by clampedSize().
        struct Maps {
            int size = 0;
            std::vector<unsigned char> baseColor;          // RGB8, sRGB
            std::vector<unsigned char> normal;             // RGB8, tangent space
            std::vector<unsigned char> metallicRoughness;  // RGB8: G roughness, B metallic
            std::vector<unsigned char> occlusion;          // R8
            std::vector<unsigned char> height;             // R8, the blending height
        };
        static void build(TerrainMaterialKind kind, uint32_t seed, int size, Maps& out);

        // Paths as written into the material, relative to whatever root the caller passes.
        struct Files {
            std::string baseColor;
            std::string normal;
            std::string metallicRoughness;
            std::string occlusion;
            std::string height;
        };

        // Builds the maps and writes them into `directory` as `<name>_<map>.png`, then fills
        // `material` with the paths each texture gets - `pathBase` prefixed, so the same call works
        // for a project-relative asset path and for a plain directory. Directories are created.
        // Returns false with a message in `error` on the first file that does not land.
        static bool writeMaps(TerrainMaterialKind kind, uint32_t seed, int size,
                              const std::filesystem::path& directory, const std::string& pathBase,
                              Files& files, Material& material, std::string& error);

        // The whole set, or the kinds a caller picked. `layers` receives one ready terrain surface
        // layer per material, in the order of `kinds`, so a World Creator pass can hand them to the
        // component without walking the maps again.
        struct Entry {
            TerrainMaterialKind kind;
            Files files;
            Material material;
        };
        static bool writeLibrary(const std::vector<TerrainMaterialKind>& kinds, uint32_t seed, int size,
                                 const std::filesystem::path& directory, const std::string& pathBase,
                                 std::vector<Entry>& entries, std::string& error);

        // Every kind the library knows, in name order - what a dropdown and a CLI list both want.
        static std::vector<TerrainMaterialKind> allKinds();

        // The layer a generated material becomes on a terrain: the PBR path on, with the height map
        // wired to the blend and the spec's own tiling. `detailTiles` is the terrain's detail
        // tiling, so uvScale only carries the material's own multiplier.
        static TerrainSurfaceLayer layerFor(const Material& material, const TerrainMaterialSpec& spec,
                                            float uvScale);
    };
}

#endif /* TerrainMaterialLibrary_h */
