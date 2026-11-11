// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelGen.h"

#include "VoxelBlocks.h"
#include "VoxelChunk.h"
#include "VoxelConfig.h"
#include "VoxelWorld.h"

#include "Noise.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace voxel {
namespace gen {

using quantum::Noise;
using quantum::NoiseType;

// One salt per field. quantum::Noise hashes (coordinate, seed), so reusing a seed would put two
// fields on the same lattice and the second would read as a scaled copy of the first.
enum Salt : uint32_t {
    kSaltCont = 1, kSaltHills, kSaltDetail, kSaltRidge, kSaltRidge2, kSaltTemp, kSaltHumid,
    kSaltCaveA, kSaltCaveB, kSaltCaveC, kSaltBedrock, kSaltPatch, kSaltStrata,
    kSaltTree, kSaltSpecies, kSaltShape, kSaltPlant, kSaltPool, kSaltGlow,
    kSaltReject, kSaltVein, kSaltCoal, kSaltIron, kSaltGold, kSaltLapis, kSaltRedstone, kSaltDiamond, kSaltEmerald
};

// 64-bit world seed to the 32-bit seed the noise takes. Integer-only, like the engine's own hash,
// so the same seed yields the same continent on MSVC, MinGW and clang.
inline uint32_t seedOf(uint64_t worldSeed, uint32_t salt) {
    uint64_t h = worldSeed * 0x9E3779B97F4A7C15ull + (uint64_t)salt * 0xFF51AFD7ED558CCDull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return (uint32_t)(h & 0xFFFFFFFFu);
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int iabs(int v) { return v < 0 ? -v : v; }

// Signed division that rounds towards minus infinity: the ore lattice is addressed by world
// coordinates, and a cell must mean the same thing on both sides of zero.
inline int floorDiv(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

// --------------------------------------------------------------------------- elevation, climate

// quantum::Noise::fbm divides by the sum of its octave weights, so a 4-octave Perlin sum never
// leaves about +-0.42 and one Perlin octave about +-0.9 of its documented +-1. Each field gets a
// gain that puts it back on a +-1 scale, which is what lets the amplitudes below mean what they say.
const float kGainCont = 6.6f;        // fbm(PERLIN, 4 octaves)
const float kGainHills = 2.8f;       // fbm(SIMPLEX, 3 octaves)
const float kGainDetail = 3.2f;      // perlin2
const float kSeaMean = 52.0f;        // the flat of the land, before hills and crests
const float kContAmp = 46.0f;        // how far continentalness alone swings the ground
const float kHillAmp = 9.0f;
const float kDetailAmp = 2.0f;
const float kCrestAmp = 62.0f;

// One low-frequency fBm decides land versus sea; hills, ridges, detail and a climate pair fill it
// out. Eight base samples per column, which is what keeps the vertical fill a table lookup.
struct Field {
    int h;          // surface top block y, -1 until the column has been sampled
    uint8_t biome;
};

inline Field sampleField(uint64_t ws, int wx, int wz) {
    const float fx = (float)wx;
    const float fz = (float)wz;

    // Continents at ~1/870 blocks: an ocean basin wide enough to read as one.
    const float cont = Noise::fbm(NoiseType::PERLIN, fx * 0.00115f, fz * 0.00115f,
        seedOf(ws, kSaltCont), 4, 2.0f, 0.5f);
    const float c01 = 0.5f + 0.5f * clampf(cont * kGainCont, -1.0f, 1.0f);

    const float hills = Noise::fbm(NoiseType::SIMPLEX, fx * 0.0071f, fz * 0.0071f,
        seedOf(ws, kSaltHills), 3, 2.0f, 0.5f);
    const float detail = Noise::perlin2(fx * 0.031f, fz * 0.031f, seedOf(ws, kSaltDetail));

    // Ridged noise: the ridge line sits where the field crosses zero, so 1-|n| keeps the crests and
    // drops the plateaus - squared, so only where both octaves agree does the ground get tall.
    const float r1 = Noise::simplex2(fx * 0.0036f, fz * 0.0036f, seedOf(ws, kSaltRidge));
    const float r2 = Noise::simplex2(fx * 0.0112f + 51.0f, fz * 0.0112f - 37.0f, seedOf(ws, kSaltRidge2));
    float crest = 1.0f - std::fabs(r1);
    crest *= 0.62f + 0.38f * (1.0f - std::fabs(r2));
    crest *= crest;

    const float temp = Noise::perlin2(fx * 0.00142f, fz * 0.00142f, seedOf(ws, kSaltTemp));
    const float humid = Noise::perlin2(fx * 0.0024f + 311.0f, fz * 0.0024f - 173.0f, seedOf(ws, kSaltHumid));

    // Sea level is a band of continentalness rather than a threshold, so the shore is a smooth
    // surface and never a step. The spread puts 28% of the world under water, the p50 of the land
    // at ~52 and crest peaks at ~120 of the 128 deep world.
    float e = kSeaMean + (c01 - 0.5f) * kContAmp
        + clampf(hills * kGainHills, -1.0f, 1.0f) * kHillAmp
        + clampf(detail * kGainDetail, -1.0f, 1.0f) * kDetailAmp;
    const float mask = clampf((c01 - 0.52f) * 4.2f, 0.0f, 1.0f);
    const float mount = crest * mask * mask * kCrestAmp;
    e += mount;

    Field f;
    f.h = clampi((int)std::lroundf(e), 2, WORLD_HEIGHT - 2);

    // Biome from elevation first (water is water), then from the climate pair. The height that
    // terrainHeight() reports and the biome that lands in Chunk::biome both come from this single
    // sample, so the two public entry points can never disagree about a column. The thresholds are
    // the quartiles of each field, which is what makes the eight biomes come out at a share worth
    // seeing rather than one dominant one.
    uint8_t b;
    if (f.h <= SEA_LEVEL - 2) b = BIOME_OCEAN;
    else if (f.h <= SEA_LEVEL + 1) b = BIOME_BEACH;
    else if (mount > 22.0f || f.h >= 90) b = BIOME_MOUNTAINS;
    else if (temp < -0.24f) b = (humid > -0.11f) ? BIOME_TAIGA : BIOME_SNOWY;
    else if (temp > 0.20f) b = (humid < -0.11f) ? BIOME_DESERT : BIOME_SAVANNA;
    else b = (humid > 0.06f) ? BIOME_FOREST : BIOME_PLAINS;
    f.biome = b;
    return f;
}

// --------------------------------------------------------------------------- column cache

// A chunk needs its own 16x16 columns plus a halo of neighbour columns for the trees that grow in
// another chunk and lean into this one. Columns are sampled lazily into a fixed grid: nothing is
// allocated in the generation loop, and a halo column that fails the cheap tree roll is never
// sampled at all.
const int kHalo = 4;
const int kGrid = CHUNK_SX + 2 * kHalo;

struct ColumnCache {
    int ox, oz;                       // world coordinate of grid cell (0, 0)
    uint64_t seed;
    mutable Field cols[kGrid * kGrid];

    ColumnCache(uint64_t worldSeed, int originX, int originZ) : ox(originX), oz(originZ), seed(worldSeed) {
        for (int i = 0; i < kGrid * kGrid; i++) cols[i].h = -1;
    }

    const Field& at(int wx, int wz) const {
        const int ix = clampi(wx - ox, 0, kGrid - 1);
        const int iz = clampi(wz - oz, 0, kGrid - 1);
        Field& c = cols[iz * kGrid + ix];
        if (c.h < 0) c = sampleField(seed, wx, wz);
        return c;
    }
};

// --------------------------------------------------------------------------- strata

// Two named bands under the surface block: enough for desert sand over sandstone, and for a sea
// floor of sand or clay over dirt, without special-casing the fill loop per biome.
struct Strata {
    uint16_t top;
    uint16_t a; int aDepth;
    uint16_t b; int bDepth;
};

Strata strataOf(uint64_t ws, const Field& f, int wx, int wz) {
    const uint32_t sPatch = seedOf(ws, kSaltPatch);
    const uint32_t sStrata = seedOf(ws, kSaltStrata);
    const float r = Noise::hash2(wx, wz, sPatch);
    const float d = Noise::hash2(wx + 7919, wz - 104729, sStrata);

    Strata s;
    s.top = BLOCK_GRASS;
    s.a = BLOCK_DIRT; s.aDepth = 3;
    s.b = BLOCK_STONE; s.bDepth = 2;

    switch (f.biome) {
        case BIOME_OCEAN: {
            const int depth = SEA_LEVEL - f.h;
            if (depth <= 4) {
                s.top = (r < 0.62f) ? BLOCK_SAND : BLOCK_GRAVEL; s.a = BLOCK_SAND; s.aDepth = 3;
            }
            else if (depth <= 9) {
                s.top = (r < 0.45f) ? BLOCK_SAND : ((r < 0.75f) ? BLOCK_GRAVEL : BLOCK_DIRT);
                s.a = BLOCK_DIRT; s.aDepth = 3;
            }
            else {
                s.top = (r < 0.30f) ? BLOCK_CLAY : ((r < 0.60f) ? BLOCK_GRAVEL : BLOCK_SAND);
                s.a = (r < 0.42f) ? BLOCK_CLAY : BLOCK_DIRT;
                s.aDepth = 3 + (int)(d * 2.0f);
            }
            s.b = BLOCK_STONE; s.bDepth = 3;
            break;
        }
        case BIOME_BEACH:
            s.top = BLOCK_SAND; s.a = BLOCK_SAND; s.aDepth = 3 + (int)(d * 2.0f);
            s.b = BLOCK_DIRT; s.bDepth = 2;
            break;
        case BIOME_DESERT:
            s.top = BLOCK_SAND; s.a = BLOCK_SAND; s.aDepth = 3 + (int)(d * 2.0f);
            s.b = BLOCK_SANDSTONE; s.bDepth = 3 + (int)(d * 3.0f);
            break;
        case BIOME_SNOWY:
            s.top = (r < 0.06f) ? BLOCK_PACKED_ICE : BLOCK_SNOW;
            s.a = BLOCK_DIRT; s.aDepth = 3; s.b = (r < 0.20f) ? BLOCK_GRAVEL : BLOCK_STONE;
            break;
        case BIOME_MOUNTAINS:
            if (f.h >= 88) {
                s.top = BLOCK_SNOW; s.a = BLOCK_STONE; s.aDepth = 2; s.b = BLOCK_COBBLESTONE; s.bDepth = 3;
            }
            else if (f.h >= 70) {
                s.top = (r < 0.30f) ? BLOCK_GRASS : ((r < 0.55f) ? BLOCK_STONE : ((r < 0.66f) ? BLOCK_GRAVEL : BLOCK_STONE));
                s.a = BLOCK_STONE; s.aDepth = 3;
                s.b = (r < 0.30f) ? BLOCK_DIRT : BLOCK_COBBLESTONE; s.bDepth = 2;
            }
            else {
                s.top = (r < 0.55f) ? BLOCK_GRASS : ((r < 0.74f) ? BLOCK_STONE : BLOCK_GRAVEL);
                s.a = BLOCK_DIRT; s.aDepth = 3; s.b = BLOCK_STONE;
            }
            break;
        case BIOME_TAIGA:
            s.top = (r < 0.07f) ? BLOCK_MOSSY_COBBLESTONE : BLOCK_GRASS;
            s.a = BLOCK_DIRT; s.aDepth = 3 + (int)(d * 2.0f); s.b = BLOCK_STONE;
            break;
        case BIOME_SAVANNA:
            s.top = (r < 0.14f) ? BLOCK_DIRT : BLOCK_GRASS;
            s.a = BLOCK_DIRT; s.aDepth = 3; s.b = BLOCK_STONE;
            break;
        case BIOME_FOREST:
            s.top = (r < 0.05f) ? BLOCK_MOSSY_COBBLESTONE : BLOCK_GRASS;
            s.a = BLOCK_DIRT; s.aDepth = 3 + (int)(d * 2.0f); s.b = BLOCK_STONE;
            break;
        case BIOME_PLAINS:
        default:
            break;
    }
    return s;
}

// --------------------------------------------------------------------------- caves

// The carved slice stops here; below the bedrock of the world nothing is hollow.
const int kCaveCeiling = 47;

// Two thin sheets of 3D noise: rock gives way only where both are near zero, which reads as
// connected tunnels rather than the isolated bubbles a single threshold carves. The second sheet is
// sampled only for the fraction of voxels the first one lets through.
inline bool caveOpen(int wx, int y, int wz, uint32_t sa, uint32_t sb, uint32_t sc) {
    const float fx = (float)wx * 0.0295f;
    const float fy = (float)y * 0.0410f;
    const float fz = (float)wz * 0.0295f;

    const float a = Noise::simplex3(fx, fy, fz, sa);
    if (std::fabs(a) < 0.115f) {
        const float b = Noise::simplex3(fx * 0.9f + 300.0f, fy * 1.17f - 90.0f, fz * 0.9f + 140.0f, sb);
        if (std::fabs(b) < 0.142f) return true;
    }
    // A few wide rooms down deep, where the tunnel network alone would only read as scratch.
    if (y <= 24) {
        const float c = Noise::value3((float)wx * 0.0181f, (float)y * 0.0263f, (float)wz * 0.0181f, sc);
        if (c > 0.855f) return true;
    }
    return false;
}

// --------------------------------------------------------------------------- ores

struct OreBand {
    uint16_t type;
    int yMax;
    int cell;
    float chance;
    float fill;
    Salt salt;
};

// Rare bands first: a voxel that qualifies for diamond and for coal keeps the diamond.
static const OreBand kOres[] = {
    { BLOCK_DIAMOND_ORE,   16, 2, 0.011f, 0.55f, kSaltDiamond },
    { BLOCK_EMERALD_ORE,   40, 2, 0.006f, 0.60f, kSaltEmerald },
    { BLOCK_GOLD_ORE,      44, 2, 0.013f, 0.60f, kSaltGold },
    { BLOCK_LAPIS_ORE,     30, 3, 0.017f, 0.62f, kSaltLapis },
    { BLOCK_REDSTONE_ORE,  34, 3, 0.021f, 0.62f, kSaltRedstone },
    { BLOCK_IRON_ORE,      66, 3, 0.033f, 0.58f, kSaltIron },
    { BLOCK_COAL_ORE,     102, 4, 0.043f, 0.52f, kSaltCoal },
};
const int kOreCount = (int)(sizeof(kOres) / sizeof(kOres[0]));

// A vein is a coarse world-space cell that either is or is not mineralised, then a per-voxel roll
// that eats into it, so ore comes out as a cluster a player can mine out instead of salt scattered
// one block at a time. Cells are addressed by world coordinate, so a vein straddling a chunk border
// is placed identically by both chunks.
uint16_t veinOf(uint64_t ws, int wx, int y, int wz, uint8_t biome, uint32_t sReject) {
    // Most stone is not ore, so one white-noise roll skips the band loop entirely for it.
    if (Noise::hash3(wx, y, wz, sReject) > 0.30f) return BLOCK_AIR;

    const uint32_t sVein = seedOf(ws, kSaltVein);
    for (int i = 0; i < kOreCount; i++) {
        const OreBand& band = kOres[i];
        if (y > band.yMax) continue;
        if (band.type == BLOCK_EMERALD_ORE && biome != BIOME_MOUNTAINS) continue;
        const uint32_t sb = sVein + (uint32_t)band.salt * 7919u;
        const int bx = floorDiv(wx, band.cell);
        const int by = floorDiv(y, band.cell);
        const int bz = floorDiv(wz, band.cell);
        if (Noise::hash3(bx, by, bz, sb) < band.chance) {
            if (Noise::hash3(wx, y, wz, sb + 1u) < band.fill) return band.type;
        }
    }
    return BLOCK_AIR;
}

// --------------------------------------------------------------------------- stamps

// Stamps one voxel of a decoration that may have been planted in a neighbouring chunk.
inline void stamp(Chunk& ch, int originX, int originZ, int wx, int ly, int wz, uint16_t id) {
    const int lx = wx - originX;
    const int lz = wz - originZ;
    if ((unsigned)lx >= (unsigned)CHUNK_SX || (unsigned)lz >= (unsigned)CHUNK_SZ) return;
    if (ly < 0 || ly >= WORLD_HEIGHT) return;                 // a tall spruce just stops at the roof
    if (!isReplaceable(ch.typeAt(lx, ly, lz))) return;        // never bury terrain
    ch.setRaw(lx, ly, lz, id);
}

inline float treeChance(uint8_t biome, int h) {
    switch (biome) {
        case BIOME_FOREST: return 0.052f;
        case BIOME_TAIGA: return 0.048f;
        case BIOME_PLAINS: return 0.009f;
        case BIOME_SAVANNA: return 0.016f;
        case BIOME_SNOWY: return 0.015f;
        case BIOME_MOUNTAINS: return (h < 84) ? 0.010f : 0.0f;   // nothing above the snow line
        default: return 0.0f;
    }
}

const float kMaxTreeChance = 0.052f;   // the densest biome, used as the cheap pre-gate

// Canopy corners are cut with a hash keyed on the base column plus the offset, so a chunk stamping
// a tree from a neighbour's column removes exactly the corners the owning chunk does.
inline bool cornerCut(uint32_t sShape, int wx, int ly, int wz, int dx, int dz) {
    return Noise::hash3(wx * 4 + dx, ly, wz * 4 + dz, sShape) < 0.42f;
}

void stampTree(Chunk& ch, uint64_t ws, int originX, int originZ, int wx, int wz, const Field& f) {
    const uint32_t sSpecies = seedOf(ws, kSaltSpecies);
    const uint32_t sShape = seedOf(ws, kSaltShape);
    const float sp = Noise::hash2(wx * 3 + 1, wz * 3 - 1, sSpecies);

    int kind;                       // 0 oak, 1 birch, 2 spruce
    if (f.biome == BIOME_TAIGA || f.biome == BIOME_SNOWY) kind = (sp < 0.87f) ? 2 : 1;
    else if (f.biome == BIOME_FOREST) kind = (sp < 0.78f) ? 0 : 1;
    else if (f.biome == BIOME_PLAINS) kind = (sp < 0.62f) ? 0 : 1;
    else if (f.biome == BIOME_MOUNTAINS) kind = 1;
    else kind = 0;

    const int roll = (int)(Noise::hash2(wx * 5 + 3, wz * 5 - 11, sShape) * 16.0f);
    uint16_t log, leaf;
    int trunkH;
    if (kind == 2) { trunkH = 7 + roll % 7; log = BLOCK_LOG_SPRUCE; leaf = BLOCK_LEAVES_OAK; }
    else if (kind == 1) { trunkH = 5 + roll % 4; log = BLOCK_LOG_BIRCH; leaf = BLOCK_LEAVES_BIRCH; }
    else { trunkH = 4 + roll % 4; log = BLOCK_LOG_OAK; leaf = BLOCK_LEAVES_OAK; }

    const int base = f.h + 1;
    const int top = base + trunkH - 1;

    if (kind == 2) {
        // Cone, widest at the skirt, ending in a single tip leaf. The block table has no spruce
        // foliage, so conifers wear the darker oak leaves.
        for (int ly = base + 2; ly <= top + 1; ly++) {
            const int k = (top + 1) - ly;
            const int r = (k <= 0) ? 0 : ((k <= 3) ? 1 : ((k <= 6) ? 2 : 3));
            for (int dx = -r; dx <= r; dx++) {
                for (int dz = -r; dz <= r; dz++) {
                    if (dx == 0 && dz == 0) continue;
                    if (r >= 2 && iabs(dx) == r && iabs(dz) == r && cornerCut(sShape, wx, ly, wz, dx, dz)) continue;
                    stamp(ch, originX, originZ, wx + dx, ly, wz + dz, leaf);
                }
            }
        }
    }
    else {
        const int low = (kind == 1) ? top - 2 : top - 2;
        const int high = (kind == 1) ? top + 2 : top + 1;
        for (int ly = low; ly <= high; ly++) {
            const int r = (ly <= top - 1) ? 2 : 1;
            for (int dx = -r; dx <= r; dx++) {
                for (int dz = -r; dz <= r; dz++) {
                    if (dx == 0 && dz == 0 && ly != high) continue;
                    if (r == 2 && iabs(dx) == 2 && iabs(dz) == 2 && cornerCut(sShape, wx, ly, wz, dx, dz)) continue;
                    stamp(ch, originX, originZ, wx + dx, ly, wz + dz, leaf);
                }
            }
        }
    }

    // Trunk last: logs are not replaceable, so the trunk outlives the canopy it grows through.
    for (int ly = base; ly <= top; ly++) stamp(ch, originX, originZ, wx, ly, wz, log);
}

void stampCactus(Chunk& ch, uint32_t sShape, int originX, int originZ, int wx, int wz, const Field& f) {
    const int n = 2 + (int)(Noise::hash2(wx * 7 - 5, wz * 7 + 9, sShape) * 3.0f);
    for (int i = 0; i < n; i++) stamp(ch, originX, originZ, wx, f.h + 1 + i, wz, BLOCK_CACTUS);
}

// --------------------------------------------------------------------------- chunk fill

void fillColumn(Chunk& ch, uint64_t ws, const ColumnCache& cache, int lx, int lz, int wx, int wz,
    uint32_t sa, uint32_t sb, uint32_t sc, uint32_t sBed, uint32_t sPool, uint32_t sGlow, uint32_t sReject) {
    const Field& f = cache.at(wx, wz);
    const int h = f.h;
    ch.biome[lz * CHUNK_SX + lx] = f.biome;

    const Strata s = strataOf(ws, f, wx, wz);

    // Caves fill a fixed slice of the world, y 5 up to at most y 47, just under the skin of the
    // lowest terrain: the deep half of a 120 block mountain is never seen and paying for it would
    // blow the per-frame generation budget. Keeping the slice anchored at the bottom is also what
    // guarantees the lava layer at the floor of the world is always reachable.
    int caveBot = 5;                       // nothing is carved through the bedrock floor
    int caveTop = h - 5;
    if (caveTop > kCaveCeiling) caveTop = kCaveCeiling;

    const uint16_t waterFull = makeState(BLOCK_WATER, 15);
    const uint16_t lavaFull = makeState(BLOCK_LAVA, 15);

    for (int y = 0; y <= h; y++) {
        uint16_t id;
        if (y <= 1) id = BLOCK_BEDROCK;
        else if (y == 2 && Noise::hash3(wx, y, wz, sBed) < 0.5f) id = BLOCK_BEDROCK;
        else if (y == h) id = s.top;
        else if (y >= h - s.aDepth) id = s.a;
        else if (y >= h - s.aDepth - s.bDepth) id = s.b;
        else if (y <= 13) id = (Noise::hash3(wx, y, wz, sBed + 11u) < 0.07f) ? BLOCK_MOSSY_COBBLESTONE : BLOCK_COBBLESTONE;
        else id = BLOCK_STONE;

        if (y >= caveBot && y <= caveTop && caveOpen(wx, y, wz, sa, sb, sc)) {
            if (y <= 6 && Noise::hash3(wx, y, wz, sPool) < 0.7f) {
                id = lavaFull;                        // the floor of the world is lava, not air
            }
            else if (y >= 11 && y <= 26 && Noise::hash3(wx, y + 7, wz, sPool) < 0.045f) {
                id = waterFull;                       // a pool left on a cave floor
            }
            else {
                id = BLOCK_AIR;
                // Light the tunnels from their own floor: caves are the one place the skylight
                // solver cannot reach, and an emissive block under an opening is seen the same way
                // from both sides of a border, so it never depends on which chunk wrote it.
                if (y >= 7 && y <= 24 && Noise::hash3(wx, y - 1, wz, sGlow) < 0.055f) {
                    const uint16_t under = ch.typeAt(lx, y - 1, lz);
                    if (under == BLOCK_STONE || under == BLOCK_COBBLESTONE || under == BLOCK_MOSSY_COBBLESTONE) {
                        ch.setRaw(lx, y - 1, lz, BLOCK_GLOWSTONE);
                    }
                }
            }
        }

        if (id == BLOCK_STONE || id == BLOCK_COBBLESTONE || id == BLOCK_MOSSY_COBBLESTONE) {
            const uint16_t ore = veinOf(ws, wx, y, wz, f.biome, sReject);
            if (ore != BLOCK_AIR) id = ore;
        }

        ch.setRaw(lx, y, lz, id);
    }

    // Anything whose ground sits below sea level is flooded up to the surface - and the floor it
    // stands on drops with it. A column one block under the waterline otherwise makes a puddle whose
    // surface is a hundredth of a block below the beach beside it: the sea is then a film the eye
    // cannot separate from the sand it lies in. The basin deepens by two blocks for every block the
    // land sits under the waterline, which is what gives a coast a surface worth looking at. Only
    // cells under the waterline are touched, so no hill above the beach moves and the seed still
    // describes the same land.
    if (h < SEA_LEVEL) {
        int bed = h - 2 * (SEA_LEVEL - h);
        if (bed < 3) bed = 3;
        for (int y = SEA_LEVEL; y > bed; y--) ch.setRaw(lx, y, lz, waterFull);
        ch.setRaw(lx, bed, lz, s.top);
        if (f.biome == BIOME_SNOWY) {
            ch.setRaw(lx, SEA_LEVEL, lz, BLOCK_ICE);
            if (h <= SEA_LEVEL - 6) ch.setRaw(lx, SEA_LEVEL - 1, lz, BLOCK_PACKED_ICE);
        }
    }
}

void decorate(Chunk& ch, uint64_t ws, const ColumnCache& cache, int originX, int originZ,
    uint32_t sTree, uint32_t sPlant, uint32_t sShape) {
    // Trees are planted from the world column they grow on, which may sit in a neighbouring chunk.
    // Rule: never read another Chunk - take the base height from the pure query and stamp only the
    // voxels that land inside this one. Both chunks walk the same halo over the same pure field, so
    // the two halves of a border tree agree without the chunks ever talking to each other.
    for (int dz = -kHalo; dz < CHUNK_SZ + kHalo; dz++) {
        const int wz = originZ + dz;
        for (int dx = -kHalo; dx < CHUNK_SX + kHalo; dx++) {
            const int wx = originX + dx;
            const float gate = Noise::hash2(wx, wz, sTree);
            if (gate >= kMaxTreeChance) continue;               // cheap reject, no field sampled
            const Field& f = cache.at(wx, wz);
            if (gate >= treeChance(f.biome, f.h)) continue;
            if (f.h < SEA_LEVEL + 2 || f.h > WORLD_HEIGHT - 18) continue;
            stampTree(ch, ws, originX, originZ, wx, wz, f);
        }
    }

    for (int lz = 0; lz < CHUNK_SZ; lz++) {
        const int wz = originZ + lz;
        for (int lx = 0; lx < CHUNK_SX; lx++) {
            const int wx = originX + lx;
            const Field& f = cache.at(wx, wz);
            const uint16_t surf = ch.typeAt(lx, f.h, lz);
            const float roll = Noise::hash2(wx + 313, wz - 977, sPlant);
            // Nothing grows with its head under water. The voxel above a flooded column is water and
            // water is replaceable, so without this the ocean floor and the wet edge of a beach get
            // dry-land shrubs.
            if (ch.typeAt(lx, f.h + 1, lz) == BLOCK_WATER) continue;

            if (surf == BLOCK_GRASS) {
                uint16_t plant = BLOCK_AIR;
                if (roll < 0.012f) plant = BLOCK_POPPY;
                else if (roll < 0.024f) plant = BLOCK_DANDELION;
                else if (roll < ((f.biome == BIOME_FOREST) ? 0.085f : 0.130f)) plant = BLOCK_TALL_GRASS;
                if (plant != BLOCK_AIR) stamp(ch, originX, originZ, wx, f.h + 1, wz, plant);
            }
            else if (surf == BLOCK_SAND || surf == BLOCK_DIRT || surf == BLOCK_GRAVEL) {
                if (f.biome == BIOME_DESERT) {
                    if (roll < 0.012f) stamp(ch, originX, originZ, wx, f.h + 1, wz, BLOCK_DEAD_BUSH);
                    else if (roll < 0.026f) stampCactus(ch, sShape, originX, originZ, wx, wz, f);
                }
                else if (roll < 0.008f) stamp(ch, originX, originZ, wx, f.h + 1, wz, BLOCK_DEAD_BUSH);
            }
            else if (surf == BLOCK_SNOW && roll < 0.004f) {
                stamp(ch, originX, originZ, wx, f.h + 1, wz, BLOCK_DEAD_BUSH);
            }
        }
    }
}

void generateChunk(const VoxelWorld& world, Chunk& chunk, uint64_t chunkSeed) {
    // The hook's seed is per chunk. Deliberately unused: every field here must be a pure function of
    // the world seed and the world coordinate, or a column shared by two chunks would disagree with
    // itself and a border tree would tear.
    (void)chunkSeed;
    const uint64_t ws = world.seed();

    chunk.allocate();                     // one zeroed set of correctly sized vectors, up front

    const int originX = chunk.cx * CHUNK_SX - kHalo;
    const int originZ = chunk.cz * CHUNK_SZ - kHalo;
    ColumnCache cache(ws, originX, originZ);

    const uint32_t sa = seedOf(ws, kSaltCaveA);
    const uint32_t sb = seedOf(ws, kSaltCaveB);
    const uint32_t sc = seedOf(ws, kSaltCaveC);
    const uint32_t sBed = seedOf(ws, kSaltBedrock);
    const uint32_t sPool = seedOf(ws, kSaltPool);
    const uint32_t sGlow = seedOf(ws, kSaltGlow);
    const uint32_t sTree = seedOf(ws, kSaltTree);
    const uint32_t sPlant = seedOf(ws, kSaltPlant);
    const uint32_t sShape = seedOf(ws, kSaltShape);
    const uint32_t sReject = seedOf(ws, kSaltReject);

    for (int lz = 0; lz < CHUNK_SZ; lz++) {
        for (int lx = 0; lx < CHUNK_SX; lx++) {
            fillColumn(chunk, ws, cache, lx, lz, chunk.cx * CHUNK_SX + lx, chunk.cz * CHUNK_SZ + lz,
                sa, sb, sc, sBed, sPool, sGlow, sReject);
        }
    }

    decorate(chunk, ws, cache, chunk.cx * CHUNK_SX, chunk.cz * CHUNK_SZ, sTree, sPlant, sShape);

    // Chunk::height is the topmost opaque block, the same definition the light solver and the
    // mesher use, so recomputing it after decoration is what keeps the three of them in agreement.
    for (int lz = 0; lz < CHUNK_SZ; lz++) {
        for (int lx = 0; lx < CHUNK_SX; lx++) chunk.recomputeColumnTop(lx, lz);
    }

    chunk.generated = true;
    chunk.dirtyLight = true;              // chunk.light belongs to the lighting module
    chunk.dirtyMesh = true;
}

} // namespace gen
} // namespace voxel

// --------------------------------------------------------------------------- public surface

void installVoxelGen(voxel::VoxelWorld& world) {
    world.generate = [](const voxel::VoxelWorld& w, voxel::Chunk& chunk, uint64_t seed) {
        voxel::gen::generateChunk(w, chunk, seed);
    };
}

int terrainHeight(uint64_t worldSeed, int worldX, int worldZ) {
    return voxel::gen::sampleField(worldSeed, worldX, worldZ).h;
}

uint8_t biomeAt(uint64_t worldSeed, int worldX, int worldZ) {
    return voxel::gen::sampleField(worldSeed, worldX, worldZ).biome;
}

namespace {

// What the eight columns around one candidate say about it: how many go under, and which way the
// water lies. The direction is the average of the submerged offsets, so a spot on a headland looks
// out to sea instead of at the single puddle behind it; a candidate with no water ring has no
// direction to offer, and the lowest column around it is the next best answer to point at.
struct WaterRing {
    int submerged;
    float dirX;
    float dirZ;
};

WaterRing waterRing(uint64_t ws, int x, int z) {
    WaterRing ring = { 0, 0.0f, 0.0f };
    int lowest = 1 << 30, lowDx = 0, lowDz = -1;
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dz == 0) continue;
            const int h = terrainHeight(ws, x + dx, z + dz);
            if (h < lowest) { lowest = h; lowDx = dx; lowDz = dz; }
            if (h < voxel::SEA_LEVEL) {
                ring.submerged++;
                ring.dirX += (float)dx;
                ring.dirZ += (float)dz;
            }
        }
    }
    if (ring.submerged == 0 || (ring.dirX == 0.0f && ring.dirZ == 0.0f)) {
        ring.dirX = (float)lowDx;
        ring.dirZ = (float)lowDz;
    }
    return ring;
}

// How far the flooded ground keeps going from a candidate, in the direction its own water ring
// points, and how deep it gets on the way. Five of eight neighbours under water is what a coast
// looks like from one column, and a hemmed trough passes that test exactly as well as an ocean -
// the run is what separates them, because the trough stops at its own lip one block out. Stops at
// the first dry column, so a puddle behind a sand wall reports the puddle and not the sea behind it.
int shoreRun(uint64_t ws, int x, int z, float dirX, float dirZ, int maxSteps, int& deepestOut) {
    deepestOut = 0;
    const float len = std::sqrt(dirX * dirX + dirZ * dirZ);
    if (len <= 0.0f) return 0;
    const float ux = dirX / len, uz = dirZ / len;
    int run = 0, deepest = 0;
    for (int s = 1; s <= maxSteps; s++) {
        const int sx = x + (int)std::lroundf(ux * (float)s);
        const int sz = z + (int)std::lroundf(uz * (float)s);
        const int h = terrainHeight(ws, sx, sz);
        if (h >= voxel::SEA_LEVEL) break;
        run++;
        const int depth = voxel::SEA_LEVEL - h;
        if (depth > deepest) deepest = depth;
    }
    deepestOut = deepest;
    return run;
}

}  // namespace

bool findShoreColumn(uint64_t worldSeed, int startX, int startZ, int maxRadius,
                     int& outX, int& outZ, float& towardX, float& towardZ) {
    // Three blocks of freeboard is the whole band: higher than that the camera is looking at a
    // hillside, and the generator only floods a column below SEA_LEVEL, so a candidate has to sit at
    // or just over the waterline to be dry ground beside water. Five of the eight neighbours under
    // water is a coast; four is a pond.
    const int kMaxFreeboard = 3;
    const int kMinWaterRing = 5;
    const int kRunSteps = 24;
    // Twelve blocks of continuous flooded ground in front of the head is the sea: past the horizon
    // of a first-person camera, more run than that buys nothing on screen, so the search stops.
    const int kOpenRun = 12;
    // The run is worth ten thousand and everything else together cannot reach three hundred, so the
    // score is already sorted by how far the water goes: a plateau at exactly SEA_LEVEL ringed by
    // one-deep water scores 119 at best and never displaces a coast with one flooded column in front
    // of it (9 100). No separate tier is needed - only no run at all in the whole radius answers pond.

    int bestX = startX, bestZ = startZ;
    float bestDirX = 0.0f, bestDirZ = 0.0f;
    long long bestScore = -1;
    bool found = false;
    int bestRun = 0;

    // One cell of the height field, and whether it beats what the search already holds. The water
    // ring is the entry test and the run is the ranking: how far the flooded ground keeps going in
    // the direction the head is turned is the only term that measures what the eye actually gets.
    // Freeboard is the next cost - a body standing three blocks over the waterline looks at the far
    // shore instead of the horizon - and the ring size only separates two columns that both open
    // onto the same sea. Ties are refused, so within a ring the first cell of a score wins and the
    // answer depends on the seed alone - never on the order the caller asks.
    auto consider = [&](int cx, int cz) {
        const int h = terrainHeight(worldSeed, cx, cz);
        if (h < voxel::SEA_LEVEL || h > voxel::SEA_LEVEL + kMaxFreeboard) return;
        const WaterRing ring = waterRing(worldSeed, cx, cz);
        if (ring.submerged < kMinWaterRing) return;
        int deepest = 0;
        const int run = shoreRun(worldSeed, cx, cz, ring.dirX, ring.dirZ, kRunSteps, deepest);
        const long long freeboard = (long long)(h - voxel::SEA_LEVEL);
        const long long score = (long long)run * 10000 - freeboard * 300
                              + (long long)ring.submerged * 10 + std::min<long long>(deepest, 39);
        if (found && score <= bestScore) return;
        bestScore = score;
        found = true;
        bestRun = run;
        bestX = cx; bestZ = cz;
        bestDirX = ring.dirX; bestDirZ = ring.dirZ;
    };

    // The start itself first: a run that already began on the coast must not be moved by a search
    // that would happily find a wider beach two rings out.
    consider(startX, startZ);
    // Then Chebyshev rings four blocks apart, until one of them holds an open coast. A trough walled
    // by a hill passes the entry test at the first ring and would be the answer every time, so the
    // walk keeps going past it; the best score so far still wins if the whole radius has nothing
    // better. Walking a ring instead of a disc keeps the cost on the perimeter: 8r columns at r.
    for (int r = 4; r <= maxRadius && bestRun < kOpenRun; r += 4) {
        for (int d = -r; d <= r; d++) {
            consider(startX + d, startZ - r);
            consider(startX + d, startZ + r);
        }
        for (int d = -r + 1; d <= r - 1; d++) {
            consider(startX - r, startZ + d);
            consider(startX + r, startZ + d);
        }
    }

    if (!found) return false;
    outX = bestX; outZ = bestZ;
    towardX = bestDirX; towardZ = bestDirZ;
    return true;
}
