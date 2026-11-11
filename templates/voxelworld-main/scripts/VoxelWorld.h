// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "VoxelConfig.h"
#include "VoxelBlocks.h"
#include "VoxelChunk.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace quantum { class Scene; class Vector3; }

namespace voxel {

// Column climate. Gen writes it, everything else reads it (water freezing, plant species, the
// HUD's biome label). Append only: the id is stored per column in a saved chunk.
enum BiomeId : uint8_t {
    BIOME_OCEAN = 0,
    BIOME_BEACH,
    BIOME_PLAINS,
    BIOME_FOREST,
    BIOME_DESERT,
    BIOME_SAVANNA,
    BIOME_TAIGA,
    BIOME_SNOWY,
    BIOME_MOUNTAINS,
    BIOME_COUNT
};

const char* biomeName(int biome);

// The GPU side of a chunk column. Two entities: one alpha-tested opaque/cutout mesh, one blended
// mesh for water and other translucent blocks. Kept out of Chunk so the storage stays plain data
// a worker thread can own.
struct ChunkRender {
    int64_t opaqueEntity = -1;   // quantum::Entity values are small ints; -1 = none yet
    int64_t liquidEntity = -1;
    bool hasMesh = false;
    uint32_t opaqueTris = 0;
    uint32_t liquidTris = 0;
    uint64_t meshVersion = 0;    // world edit counter this mesh was built at
};

// A request to place a block, kept as a plain value so the netcode, the generator and the player
// all describe the same edit without knowing about each other.
struct BlockEdit {
    int x, y, z;
    uint16_t id;
    uint16_t previous;
};

// The living world: a map of chunk columns, the streaming queues around a focus point, and the
// only sanctioned way to read or write blocks. Generation, light, water, saving and netcode all
// hang off the hooks below, so a module can be added or removed without touching this file.
class VoxelWorld {
public:
    // ---- hooks (a null hook means the module is not compiled in) ----
    // Fill a freshly allocated chunk with terrain. Must be pure: no ECS, no GPU, no throws.
    std::function<void(const class VoxelWorld&, Chunk&, uint64_t seed)> generate;
    // Solve skylight + blocklight for a column after its blocks changed.
    std::function<void(class VoxelWorld&, Chunk&)> relight;
    // Advance flowing liquid once per tick; may write blocks with setBlockQuiet().
    std::function<void(class VoxelWorld&, float dt)> waterTick;
    // Disk cache. load returns false when nothing is stored for that column.
    std::function<bool(class VoxelWorld&, const std::string& dir, int cx, int cz, Chunk&)> loadChunk;
    std::function<bool(class VoxelWorld&, const std::string& dir, const Chunk&)> saveChunk;

    std::string saveDir;         // empty disables disk I/O
    int viewRadius = 4;          // chunk columns kept loaded around the focus
    int keepRadius = 6;          // beyond this a column is dropped (dirty ones are saved first)

    void init(quantum::Scene* scene, uint64_t seed);
    void shutdown();

    // Streams work around the player: load, generate, light, mesh, unload. Cost is bounded per
    // call by the GEN_/MESH_ budgets in VoxelConfig.h.
    void update(float playerX, float playerZ);

    // ---- read path ----
    const Chunk* chunk(int cx, int cz) const;
    Chunk* chunk(int cx, int cz);
    Chunk* chunkAtWorld(int wx, int wz) { return chunk(wx >> 4, wz >> 4); }
    bool loaded(int cx, int cz) const { return chunks.find(chunkKey(cx, cz)) != chunks.end(); }
    size_t loadedCount() const { return chunks.size(); }

    uint16_t get(int wx, int wy, int wz) const;              // BLOCK_AIR outside the loaded set
    int getSky(int wx, int wy, int wz) const;
    int getBlockLight(int wx, int wy, int wz) const;
    uint8_t getBiome(int wx, int wz) const;
    int getTop(int wx, int wz) const;                        // topmost opaque block, -1 if none
    bool solidAt(int wx, int wy, int wz) const;
    bool opaqueAt(int wx, int wy, int wz) const;

    // ---- write path ----
    // Sets a block, dirties the chunk (and its neighbours when the voxel is on an edge), and
    // records the edit for the save/net layers. Returns the id that was there before.
    uint16_t setBlock(int wx, int wy, int wz, uint16_t id);
    // Same, but does not mark the chunk dirty for saving or broadcast. Used by water and by the
    // generator's own cross-column writes.
    uint16_t setBlockQuiet(int wx, int wy, int wz, uint16_t id);
    uint16_t breakBlock(int wx, int wy, int wz);
    bool placeBlock(int wx, int wy, int wz, uint16_t id);   // refuses to replace a non-replaceable

    // Edits since the last flush, for the netcode and the save journal.
    const std::vector<BlockEdit>& pendingEdits() const { return edits; }
    void clearPendingEdits() { edits.clear(); }

    // The mesher reads neighbour blocks through this; it never reaches into another Chunk.
    uint16_t sampleForMesh(int wx, int wy, int wz) const { return get(wx, wy, wz); }

    // Neighbouring columns present? A missing neighbour means the edge stays dirty until it loads.
    bool neighboursReady(int cx, int cz) const;

    void requestRemesh(int cx, int cz);
    void requestRemeshAround(int wx, int wz);

    quantum::Scene* scene() const { return scenePtr; }
    uint64_t seed() const { return worldSeed; }
    uint64_t editCounter() const { return editsMade; }

    // The column the streaming is centred on. A day/night re-bake needs it: it must dirty exactly
    // the chunks the player can see, not every column in the map.
    int focusChunkX() const { return focusCX; }
    int focusChunkZ() const { return focusCZ; }

    // Mesh entities are owned by the world; VoxelMesh fills them and this keeps their lifetime.
    ChunkRender& render(int cx, int cz);
    void destroyRender(int cx, int cz);

    // Diagnostics for the HUD and the headless probe.
    uint32_t chunksGeneratedThisFrame = 0;
    uint32_t chunksMeshedThisFrame = 0;
    double lastGenMs = 0.0;
    double lastMeshMs = 0.0;
    uint64_t totalTriangles() const;

private:
    quantum::Scene* scenePtr = nullptr;
    uint64_t worldSeed = 0;
    std::unordered_map<int64_t, Chunk> chunks;
    std::unordered_map<int64_t, ChunkRender> renders;
    std::vector<std::pair<int, int>> genQueue;     // (cx,cz) sorted by distance, nearest first
    std::vector<std::pair<int, int>> meshQueue;
    int focusCX = 0;
    int focusCZ = 0;
    std::vector<BlockEdit> edits;
    uint64_t editsMade = 0;

    Chunk* ensureEmpty(int cx, int cz);
    void markDirty(int wx, int wy, int wz, uint16_t oldId, uint16_t newId);
    void stream();
    void flushSaves();
    uint64_t frameCounter = 0;
};

// Deterministic world seed helper: mixes the player's numeric seed with the column coordinates so
// a chunk always generates the same way, wherever the player walks to.
uint64_t chunkSeed(uint64_t worldSeed, int cx, int cz);

}  // namespace voxel
