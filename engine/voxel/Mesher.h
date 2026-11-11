// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Chunk.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

struct VoxelMeshVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint8_t block = 0;
    uint8_t light = 0;
};

struct VoxelMeshData {
    std::vector<VoxelMeshVertex> vertices;
    std::vector<uint32_t> indices;
    size_t transitionSkirtFaces = 0;

    void clear();
    size_t faceCount() const { return indices.size() / 6; }
};

class VoxelMesher {
public:
    // Neighbor order is +X, -X, +Y, -Y, +Z, -Z. A missing neighbor is treated
    // as air, which keeps an isolated chunk renderable while streaming is settling.
    using Neighbors = std::array<const Chunk*, 6>;

    static void build(const Chunk& chunk, const Neighbors& neighbors, VoxelMeshData& output);
    static void buildGreedy(const Chunk& chunk, const Neighbors& neighbors, VoxelMeshData& output);
    // Build a coarse distant mesh. level 1 represents 2x2x2 blocks, level 2
    // represents 4x4x4 blocks, and so on up to the chunk size.
    static void buildLOD(const Chunk& chunk, const Neighbors& neighbors, int level, VoxelMeshData& output);
};
