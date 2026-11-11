// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "VoxelWorld.h"
#include "VoxelMesh.h"

#include "Scene.h"
#include "Engine.h"
#include "component/MeshComponent.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace voxel {

using quantum::Entity;
using quantum::Scene;

static double nowMs() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double, std::milli>(steady_clock::now() - start).count();
}

const char* biomeName(int biome) {
    switch (biome) {
        case BIOME_OCEAN: return "Ocean";
        case BIOME_BEACH: return "Beach";
        case BIOME_PLAINS: return "Plains";
        case BIOME_FOREST: return "Forest";
        case BIOME_DESERT: return "Desert";
        case BIOME_SAVANNA: return "Savanna";
        case BIOME_TAIGA: return "Taiga";
        case BIOME_SNOWY: return "Snowy Plains";
        case BIOME_MOUNTAINS: return "Mountains";
        default: return "Unknown";
    }
}

uint64_t chunkSeed(uint64_t worldSeed, int cx, int cz) {
    // Integer-only and identical on every platform, the same rule the engine's own noise follows.
    uint64_t h = worldSeed * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)(uint32_t)cx * 0xC2B2AE3D27D4EB4Full;
    h ^= (uint64_t)(uint32_t)cz * 0x165667B19E3779F9ull;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 33;
    return h;
}

void VoxelWorld::init(Scene* scene, uint64_t seed) {
    scenePtr = scene;
    worldSeed = seed;
    chunks.clear();
    renders.clear();
    genQueue.clear();
    meshQueue.clear();
    edits.clear();
    editsMade = 0;
}

void VoxelWorld::shutdown() {
    for (auto& r : renders) destroyRender(chunkKeyX(r.first), chunkKeyZ(r.first));
    renders.clear();
    chunks.clear();
    scenePtr = nullptr;
}

const Chunk* VoxelWorld::chunk(int cx, int cz) const {
    auto it = chunks.find(chunkKey(cx, cz));
    return it == chunks.end() ? nullptr : &it->second;
}

Chunk* VoxelWorld::chunk(int cx, int cz) {
    auto it = chunks.find(chunkKey(cx, cz));
    return it == chunks.end() ? nullptr : &it->second;
}

Chunk* VoxelWorld::ensureEmpty(int cx, int cz) {
    auto it = chunks.find(chunkKey(cx, cz));
    if (it != chunks.end()) return &it->second;
    Chunk& c = chunks[chunkKey(cx, cz)];
    c.cx = cx; c.cz = cz;
    return &c;
}

uint16_t VoxelWorld::get(int wx, int wy, int wz) const {
    const Chunk* c = chunk(wx >> 4, wz >> 4);
    if (!c || !c->generated) return BLOCK_AIR;
    return c->get(wx & 15, wy, wz & 15);
}

int VoxelWorld::getSky(int wx, int wy, int wz) const {
    const Chunk* c = chunk(wx >> 4, wz >> 4);
    if (!c) return 0;
    return c->skyAt(wx & 15, wy, wz & 15);
}

int VoxelWorld::getBlockLight(int wx, int wy, int wz) const {
    const Chunk* c = chunk(wx >> 4, wz >> 4);
    if (!c) return 0;
    return c->blockAt(wx & 15, wy, wz & 15);
}

uint8_t VoxelWorld::getBiome(int wx, int wz) const {
    const Chunk* c = chunk(wx >> 4, wz >> 4);
    if (!c || c->biome.empty()) return BIOME_PLAINS;
    return c->biome[(wz & 15) * CHUNK_SX + (wx & 15)];
}

int VoxelWorld::getTop(int wx, int wz) const {
    const Chunk* c = chunk(wx >> 4, wz >> 4);
    if (!c || !c->generated) return -1;
    return c->topAt(wx & 15, wz & 15);
}

bool VoxelWorld::solidAt(int wx, int wy, int wz) const {
    return isSolid(stateType(get(wx, wy, wz)));
}

bool VoxelWorld::opaqueAt(int wx, int wy, int wz) const {
    return isOpaque(stateType(get(wx, wy, wz)));
}

uint16_t VoxelWorld::setBlock(int wx, int wy, int wz, uint16_t id) {
    uint16_t previous = setBlockQuiet(wx, wy, wz, id);
    if (previous == id) return previous;
    BlockEdit e{ wx, wy, wz, id, previous };
    edits.push_back(e);
    editsMade++;
    markDirty(wx, wy, wz, previous, id);
    return previous;
}

uint16_t VoxelWorld::setBlockQuiet(int wx, int wy, int wz, uint16_t id) {
    if (wy < 0 || wy >= WORLD_HEIGHT) return BLOCK_AIR;
    Chunk* c = chunk(wx >> 4, wz >> 4);
    if (!c || !c->generated) return BLOCK_AIR;
    int lx = wx & 15, lz = wz & 15;
    uint16_t previous = c->get(lx, wy, lz);
    if (previous == id) return previous;
    c->setRaw(lx, wy, lz, id);
    c->recomputeColumnTop(lx, lz);
    c->dirtyMesh = true;
    c->dirtyLight = true;
    return previous;
}

uint16_t VoxelWorld::breakBlock(int wx, int wy, int wz) {
    uint16_t previous = get(wx, wy, wz);
    if (stateType(previous) == BLOCK_AIR) return previous;
    if (block(stateType(previous)).hardness >= 255) return previous;   // bedrock
    setBlock(wx, wy, wz, BLOCK_AIR);
    return previous;
}

bool VoxelWorld::placeBlock(int wx, int wy, int wz, uint16_t id) {
    uint16_t here = stateType(get(wx, wy, wz));
    if (!isReplaceable(here)) return false;
    setBlock(wx, wy, wz, id);
    return true;
}

void VoxelWorld::markDirty(int wx, int wy, int wz, uint16_t oldId, uint16_t newId) {
    requestRemeshAround(wx, wz);
    // An edge voxel changes the face the neighbour chunk draws, so its column must re-mesh too.
    int lx = wx & 15, lz = wz & 15;
    if (lx == 0) requestRemesh((wx >> 4) - 1, wz >> 4);
    if (lx == 15) requestRemesh((wx >> 4) + 1, wz >> 4);
    if (lz == 0) requestRemesh(wx >> 4, (wz >> 4) - 1);
    if (lz == 15) requestRemesh(wx >> 4, (wz >> 4) + 1);
    Chunk* c = chunk(wx >> 4, wz >> 4);
    if (!c) return;
    if (relight) relight(*this, *c);
    // A loud write is the only thing that makes a column worth rewriting: setBlockQuiet() does not
    // come here on purpose, so this is where "edited since the last write to disk" turns true. Skip
    // it and the journal stays the only copy of every edit - no column file is ever written, and the
    // save grows one line per block the player touches for the rest of the session.
    c->dirtySave = true;
    // The mesh lives in the chunk column, so the height of the change is not what decides the remesh.
    (void)wy; (void)oldId; (void)newId;
}

void VoxelWorld::requestRemesh(int cx, int cz) {
    Chunk* c = chunk(cx, cz);
    if (!c || !c->generated) return;
    c->dirtyMesh = true;
}

void VoxelWorld::requestRemeshAround(int wx, int wz) {
    requestRemesh(wx >> 4, wz >> 4);
}

bool VoxelWorld::neighboursReady(int cx, int cz) const {
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            const Chunk* c = chunk(cx + dx, cz + dz);
            if (!c || !c->generated) return false;
        }
    }
    return true;
}

ChunkRender& VoxelWorld::render(int cx, int cz) {
    return renders[chunkKey(cx, cz)];
}

void VoxelWorld::destroyRender(int cx, int cz) {
    int64_t key = chunkKey(cx, cz);
    auto it = renders.find(key);
    if (it == renders.end() || !scenePtr) return;
    if (it->second.opaqueEntity >= 0) scenePtr->destroyEntity((Entity)it->second.opaqueEntity);
    if (it->second.liquidEntity >= 0) scenePtr->destroyEntity((Entity)it->second.liquidEntity);
    renders.erase(it);
}

uint64_t VoxelWorld::totalTriangles() const {
    uint64_t t = 0;
    for (auto& r : renders) t += r.second.opaqueTris + r.second.liquidTris;
    return t;
}

void VoxelWorld::stream() {
    if (!generate) return;
    genQueue.clear();
    for (int dz = -viewRadius; dz <= viewRadius; dz++) {
        for (int dx = -viewRadius; dx <= viewRadius; dx++) {
            int cx = focusCX + dx, cz = focusCZ + dz;
            if (loaded(cx, cz)) continue;
            genQueue.push_back({ cx, cz });
        }
    }
    // nearest first, so the ground under the player appears before the horizon
    std::stable_sort(genQueue.begin(), genQueue.end(),
                     [this](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                         double da = (a.first - focusCX) * (double)(a.first - focusCX) + (a.second - focusCZ) * (a.second - focusCZ);
                         double db = (b.first - focusCX) * (double)(b.first - focusCX) + (b.second - focusCZ) * (b.second - focusCZ);
                         return da < db;
                     });
}

void VoxelWorld::update(float playerX, float playerZ) {
    frameCounter++;
    int fcx = (int)std::floor(playerX / CHUNK_SX);
    int fcz = (int)std::floor(playerZ / CHUNK_SZ);
    if (fcx != focusCX || fcz != focusCZ) {
        focusCX = fcx; focusCZ = fcz;
        stream();
    }

    // 1. generate, nearest first, under a time budget
    double genStart = nowMs();
    chunksGeneratedThisFrame = 0;
    while (!genQueue.empty() && chunksGeneratedThisFrame < GEN_PER_FRAME) {
        std::pair<int, int> next = genQueue.front();
        genQueue.erase(genQueue.begin());
        Chunk* c = ensureEmpty(next.first, next.second);
        if (!c->generated) {
            bool fromDisk = false;
            if (loadChunk && !saveDir.empty())
                fromDisk = loadChunk(*this, saveDir, next.first, next.second, *c);
            if (!fromDisk) {
                // A null generator means the module is not compiled in: the column stays empty but
                // is still marked done, or the queue would retry it every frame forever. Calling an
                // empty std::function would be a bad_function_call, and this build has no
                // exceptions to catch it with - the process would abort.
                if (generate) generate(*this, *c, chunkSeed(worldSeed, next.first, next.second));
                c->generated = true;
                c->dirtyMesh = true;
                c->dirtyLight = true;
                c->dirtySave = false;      // straight from the generator, nothing to write yet
            }
            chunksGeneratedThisFrame++;
            requestRemeshAround(next.first * CHUNK_SX, next.second * CHUNK_SZ);
            for (int d = 1; d <= DIRTY_SPREAD; d++) {
                requestRemesh(next.first - d, next.second); requestRemesh(next.first + d, next.second);
                requestRemesh(next.first, next.second - d); requestRemesh(next.first, next.second + d);
            }
        }
        if (nowMs() - genStart > GEN_BUDGET_MS) break;
    }
    lastGenMs = nowMs() - genStart;

    // 2. mesh what is ready, budgeted
    double meshStart = nowMs();
    chunksMeshedThisFrame = 0;
    if (scenePtr) {
        // collect candidates without holding the map while the mesher writes into it
        std::vector<std::pair<double, int64_t>> candidates;
        for (auto& kv : chunks) {
            if (!kv.second.dirtyMesh || !kv.second.generated) continue;
            int cx = chunkKeyX(kv.first), cz = chunkKeyZ(kv.first);
            if (!neighboursReady(cx, cz)) continue;
            double dx = cx - focusCX, dz = cz - focusCZ;
            candidates.push_back({ dx * dx + dz * dz, kv.first });
        }
        std::sort(candidates.begin(), candidates.end());
        for (auto& cand : candidates) {
            if (chunksMeshedThisFrame >= MESH_PER_FRAME && nowMs() - meshStart > MESH_BUDGET_MS) break;
            Chunk* c = chunks.find(cand.second) == chunks.end() ? nullptr : &chunks[cand.second];
            if (!c || !c->dirtyMesh) continue;
            buildChunkMesh(*this, *c);
            c->dirtyMesh = false;
            chunksMeshedThisFrame++;
            if (nowMs() - meshStart > MESH_BUDGET_MS * 2.0) break;
        }
    }
    lastMeshMs = nowMs() - meshStart;

    // 3. light columns that asked for it (the hook is cheap enough to run inline)
    if (relight) {
        int lit = 0;
        for (auto& kv : chunks) {
            if (!kv.second.dirtyLight || !kv.second.generated) continue;
            relight(*this, kv.second);
            kv.second.dirtyLight = false;
            if (++lit >= 2) break;
        }
    }

    // 4. liquid flow
    if (waterTick) waterTick(*this, (float)quantum::Engine::getDeltatime());

    // 5. drop columns outside the keep radius, saving the ones that changed
    if (frameCounter % 30 == 0) {
        std::vector<int64_t> drop;
        for (auto& kv : chunks) {
            double dx = chunkKeyX(kv.first) - focusCX, dz = chunkKeyZ(kv.first) - focusCZ;
            if (dx * dx + dz * dz > (double)keepRadius * keepRadius) {
                if (kv.second.dirtySave && saveChunk && !saveDir.empty())
                    saveChunk(*this, saveDir, kv.second);
                drop.push_back(kv.first);
            }
        }
        for (int64_t key : drop) {
            destroyRender(chunkKeyX(key), chunkKeyZ(key));
            chunks.erase(key);
        }
    }

    // 6. periodic save of edited columns near the player
    if (frameCounter % 300 == 0) flushSaves();
}

void VoxelWorld::flushSaves() {
    if (!saveChunk || saveDir.empty()) return;
    for (auto& kv : chunks) {
        if (!kv.second.dirtySave) continue;
        if (saveChunk(*this, saveDir, kv.second)) kv.second.dirtySave = false;
    }
    clearPendingEdits();
}

}  // namespace voxel
