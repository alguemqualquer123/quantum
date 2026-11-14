// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "Generator.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <future>
#include <vector>

struct VoxelRayHit {
    bool hit = false;
    float distance = 0.0f;
    int blockX = 0;
    int blockY = 0;
    int blockZ = 0;
    int normalX = 0;
    int normalY = 0;
    int normalZ = 0;
    uint8_t block = 0;
};

class World {
public:
    explicit World(uint32_t seed = 0x51F15EEDu);
    ~World();

    // Obtém (ou cria) um chunk na posição de chunk (cx, cz)
    Chunk& getChunk(int cx, int cz);
    bool setBlock(int worldX, int worldY, int worldZ, uint8_t block);
    void markChunkDirty(int chunkX, int chunkZ);

    void update(int centerChunkX = 0, int centerChunkZ = 0, int radius = 1);
    // Schedules deterministic chunk generation on worker futures and commits
    // completed chunks only when this method is called by the owner thread.
    // maxJobs bounds new work per call; the world remains usable while loads run.
    size_t updateAsync(int centerChunkX = 0, int centerChunkZ = 0, int radius = 1,
        size_t maxJobs = 2);
    size_t pendingChunkCount() const { return pendingLoads.size(); }
    void rebuildLighting();
    uint8_t getBlock(int worldX, int worldY, int worldZ) const;
    VoxelRayHit raycast(float originX, float originY, float originZ,
        float directionX, float directionY, float directionZ, float maxDistance) const;
    size_t chunkCount() const { return chunks.size(); }
    const Chunk* findChunk(int cx, int cz) const;
    std::vector<std::pair<int, int>> loadedChunkCoordinates() const;
    // Returns chunks whose block or lighting data changed since the last render
    // bridge sync. Persistence dirtiness is kept separately until a region save.
    std::vector<std::pair<int, int>> consumeMeshDirtyChunks();
    size_t meshDirtyChunkCount() const { return meshDirtyChunks.size(); }
    uint32_t getSeed() const { return generator.getSeed(); }
    bool saveRegion(const std::string& filename) const;
    // Merges loaded region data with the currently loaded chunks and writes only
    // the current in-memory changes over the existing region state.
    bool saveRegionIncremental(const std::string& filename);
    bool loadRegion(const std::string& filename);
    size_t dirtyChunkCount() const { return dirtyChunks.size(); }

private:
    using ChunkKey = std::pair<int, int>;
    struct KeyHash {
        std::size_t operator()(const ChunkKey& k) const noexcept {
            return std::hash<int>() (k.first) ^ (std::hash<int>() (k.second) << 1);
        }
    };
    std::unordered_map<ChunkKey, std::unique_ptr<Chunk>, KeyHash> chunks;
    std::unordered_set<ChunkKey, KeyHash> dirtyChunks;
    std::unordered_set<ChunkKey, KeyHash> meshDirtyChunks;
    std::unordered_map<ChunkKey, std::future<std::unique_ptr<Chunk>>, KeyHash> pendingLoads;
    Generator generator;
    int streamingCenterX = 0;
    int streamingCenterZ = 0;
    int streamingRadius = 0;
    bool lightingDirty = false;

    bool inStreamingWindow(const ChunkKey& key) const;
    size_t commitReadyLoads();
    void discardPendingLoads();
};
