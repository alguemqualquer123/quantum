// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "VoxelConfig.h"
#include "VoxelBlocks.h"

#include <cstdint>
#include <vector>

namespace voxel {

// One 16x16 column of the world, WORLD_HEIGHT blocks deep. It is deliberately plain data: the
// generator, the light solver, the water sim and the mesher all work on this and nothing else,
// so they can run on a worker thread as long as they touch only their own chunk.
// Mesh entities and GPU state live in VoxelWorld, never here.
struct Chunk {
    int cx = 0;
    int cz = 0;

    bool generated = false;    // terrain filled in
    bool lit = false;          // light channels solved for this column
    bool dirtyLight = true;
    bool dirtyMesh = true;
    bool dirtySave = true;     // edited since the last write to disk

    std::vector<uint16_t> blocks;  // CHUNK_VOLUME, id = 9-bit type | 7-bit state
    std::vector<uint8_t> light;    // CHUNK_VOLUME, sky << 4 | block
    std::vector<uint8_t> height;   // CHUNK_SX*CHUNK_SZ, topmost opaque y (0..WORLD_HEIGHT)
    std::vector<uint8_t> biome;    // CHUNK_SX*CHUNK_SZ, BiomeId of the column

    Chunk() { allocate(); }

    void allocate() {
        blocks.assign(CHUNK_VOLUME, 0);
        light.assign(CHUNK_VOLUME, 0);
        height.assign(CHUNK_SX * CHUNK_SZ, 0);
        biome.assign(CHUNK_SX * CHUNK_SZ, 0);
    }

    // Local coordinates are assumed in range by the inner loops; the world guards them.
    uint16_t get(int x, int y, int z) const {
        if ((unsigned)x >= (unsigned)CHUNK_SX || (unsigned)z >= (unsigned)CHUNK_SZ ||
            (unsigned)y >= (unsigned)WORLD_HEIGHT) return BLOCK_AIR;
        return blocks[voxelIndex(x, y, z)];
    }

    void setRaw(int x, int y, int z, uint16_t id) {
        blocks[voxelIndex(x, y, z)] = id;
    }

    uint16_t typeAt(int x, int y, int z) const { return stateType(get(x, y, z)); }

    int skyAt(int x, int y, int z) const {
        if ((unsigned)x >= (unsigned)CHUNK_SX || (unsigned)z >= (unsigned)CHUNK_SZ ||
            (unsigned)y >= (unsigned)WORLD_HEIGHT) return 0;
        return (light[voxelIndex(x, y, z)] >> 4) & 15;
    }
    int blockAt(int x, int y, int z) const {
        if ((unsigned)x >= (unsigned)CHUNK_SX || (unsigned)z >= (unsigned)CHUNK_SZ ||
            (unsigned)y >= (unsigned)WORLD_HEIGHT) return 0;
        return light[voxelIndex(x, y, z)] & 15;
    }
    void setLight(int x, int y, int z, int sky, int block) {
        if ((unsigned)x >= (unsigned)CHUNK_SX || (unsigned)z >= (unsigned)CHUNK_SZ ||
            (unsigned)y >= (unsigned)WORLD_HEIGHT) return;
        if (sky < 0) sky = 0;
        if (sky > 15) sky = 15;
        if (block < 0) block = 0;
        if (block > 15) block = 15;
        light[voxelIndex(x, y, z)] = (uint8_t)((sky << 4) | block);
    }

    int topAt(int x, int z) const {
        if ((unsigned)x >= (unsigned)CHUNK_SX || (unsigned)z >= (unsigned)CHUNK_SZ) return 0;
        return height[z * CHUNK_SX + x];
    }
    void setTop(int x, int z, int y) {
        if ((unsigned)x >= (unsigned)CHUNK_SX || (unsigned)z >= (unsigned)CHUNK_SZ) return;
        height[z * CHUNK_SX + x] = (uint8_t)(y > WORLD_HEIGHT - 1 ? WORLD_HEIGHT - 1 : y);
    }

    void recomputeColumnTop(int x, int z) {
        for (int y = WORLD_HEIGHT - 1; y >= 0; y--) {
            if (isOpaque(stateType(blocks[voxelIndex(x, y, z)]))) { setTop(x, z, y); return; }
        }
        setTop(x, z, 0);
    }

    // Wire format. Kept byte-explicit (no struct memcpy) so a world saved on x86 loads anywhere,
    // and a future field can be added without breaking the reader.
    static const uint32_t MAGIC = 0x31484356u;  // "VCH1"
    std::vector<uint8_t> serialize() const;
    bool deserialize(const uint8_t* data, size_t size);
};

}  // namespace voxel
