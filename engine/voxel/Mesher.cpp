// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "Mesher.h"

#include <algorithm>
#include <array>

namespace {

struct FaceDefinition {
    int dx;
    int dy;
    int dz;
    std::array<std::array<float, 3>, 4> corners;
};

constexpr std::array<FaceDefinition, 6> FACES = {{
    {1, 0, 0, {{{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}}},
    {-1, 0, 0, {{{0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {0, 0, 0}}}},
    {0, 1, 0, {{{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}}},
    {0, -1, 0, {{{0, 0, 1}, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}}}},
    {0, 0, 1, {{{1, 0, 1}, {1, 1, 1}, {0, 1, 1}, {0, 0, 1}}}},
    {0, 0, -1, {{{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}}}
}};

uint8_t neighborBlock(const Chunk& chunk, const VoxelMesher::Neighbors& neighbors,
    size_t face, int x, int y, int z) {
    const FaceDefinition& definition = FACES[face];
    const int nx = x + definition.dx;
    const int ny = y + definition.dy;
    const int nz = z + definition.dz;
    if (Chunk::validCoordinate(nx, ny, nz)) return chunk.get(nx, ny, nz);

    const Chunk* neighbor = neighbors[face];
    if (neighbor == nullptr) return 0;
    const int localX = definition.dx > 0 ? 0 : (definition.dx < 0 ? Chunk::SIZE - 1 : x);
    const int localY = definition.dy > 0 ? 0 : (definition.dy < 0 ? Chunk::SIZE - 1 : y);
    const int localZ = definition.dz > 0 ? 0 : (definition.dz < 0 ? Chunk::SIZE - 1 : z);
    return neighbor->get(localX, localY, localZ);
}

struct GreedyCell {
    uint8_t block = 0;
    uint8_t light = 0;
    bool visible = false;
};

struct LODCell {
    uint8_t block = 0;
    uint8_t light = 0;
    bool solid = false;
};

void appendGreedyQuad(VoxelMeshData& output, size_t face, int plane, int u, int v,
    int width, int height, const GreedyCell& cell) {
    const float p = static_cast<float>(plane);
    const float a = static_cast<float>(u);
    const float b = static_cast<float>(v);
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    std::array<std::array<float, 3>, 4> corners{};
    switch (face) {
        case 0: corners = {{{p + 1, b, a}, {p + 1, b + h, a},
            {p + 1, b + h, a + w}, {p + 1, b, a + w}}}; break;
        case 1: corners = {{{p, b, a + w}, {p, b + h, a + w},
            {p, b + h, a}, {p, b, a}}}; break;
        case 2: corners = {{{a, p + 1, b}, {a, p + 1, b + h},
            {a + w, p + 1, b + h}, {a + w, p + 1, b}}}; break;
        case 3: corners = {{{a, p, b + h}, {a, p, b},
            {a + w, p, b}, {a + w, p, b + h}}}; break;
        case 4: corners = {{{a + w, b, p + 1}, {a + w, b + h, p + 1},
            {a, b + h, p + 1}, {a, b, p + 1}}}; break;
        default: corners = {{{a, b, p}, {a, b + h, p},
            {a + w, b + h, p}, {a + w, b, p}}}; break;
    }
    const size_t first = output.vertices.size();
    for (const auto& corner : corners) {
        output.vertices.push_back({corner[0], corner[1], corner[2], cell.block, cell.light});
    }
    output.indices.insert(output.indices.end(), {
        static_cast<uint32_t>(first), static_cast<uint32_t>(first + 1),
        static_cast<uint32_t>(first + 2), static_cast<uint32_t>(first),
        static_cast<uint32_t>(first + 2), static_cast<uint32_t>(first + 3)
    });
}

LODCell lodCell(const Chunk& chunk, const VoxelMesher::Neighbors& neighbors,
    int scale, int cx, int cy, int cz, size_t face) {
    const int coarseSize = Chunk::SIZE / scale;
    const Chunk* source = &chunk;
    int sx = cx;
    int sy = cy;
    int sz = cz;
    if (cx < 0 || cx >= coarseSize || cy < 0 || cy >= coarseSize || cz < 0 || cz >= coarseSize) {
        if (face >= neighbors.size()) return {};
        source = neighbors[face];
        if (!source) return {};
        sx = cx < 0 ? coarseSize - 1 : (cx >= coarseSize ? 0 : cx);
        sy = cy < 0 ? coarseSize - 1 : (cy >= coarseSize ? 0 : cy);
        sz = cz < 0 ? coarseSize - 1 : (cz >= coarseSize ? 0 : cz);
    }

    LODCell result;
    for (int x = 0; x < scale; ++x) {
        for (int y = 0; y < scale; ++y) {
            for (int z = 0; z < scale; ++z) {
                const int bx = sx * scale + x;
                const int by = sy * scale + y;
                const int bz = sz * scale + z;
                const uint8_t block = source->get(bx, by, bz);
                if (block == 0) continue;
                if (!result.solid || by >= sy * scale + scale - 1) result.block = block;
                result.light = std::max(result.light, source->getLight(bx, by, bz));
                result.solid = true;
            }
        }
    }
    return result;
}

void appendLODQuad(VoxelMeshData& output, size_t face, int x, int y, int z,
    int scale, const LODCell& cell) {
    const float minX = static_cast<float>(x * scale);
    const float minY = static_cast<float>(y * scale);
    const float minZ = static_cast<float>(z * scale);
    const float maxX = static_cast<float>((x + 1) * scale);
    const float maxY = static_cast<float>((y + 1) * scale);
    const float maxZ = static_cast<float>((z + 1) * scale);
    std::array<std::array<float, 3>, 4> corners{};
    switch (face) {
        case 0: corners = {{{maxX, minY, minZ}, {maxX, maxY, minZ}, {maxX, maxY, maxZ}, {maxX, minY, maxZ}}}; break;
        case 1: corners = {{{minX, minY, maxZ}, {minX, maxY, maxZ}, {minX, maxY, minZ}, {minX, minY, minZ}}}; break;
        case 2: corners = {{{minX, maxY, minZ}, {minX, maxY, maxZ}, {maxX, maxY, maxZ}, {maxX, maxY, minZ}}}; break;
        case 3: corners = {{{minX, minY, maxZ}, {minX, minY, minZ}, {maxX, minY, minZ}, {maxX, minY, maxZ}}}; break;
        case 4: corners = {{{maxX, minY, maxZ}, {maxX, maxY, maxZ}, {minX, maxY, maxZ}, {minX, minY, maxZ}}}; break;
        default: corners = {{{minX, minY, minZ}, {minX, maxY, minZ}, {maxX, maxY, minZ}, {maxX, minY, minZ}}}; break;
    }
    const size_t first = output.vertices.size();
    for (const auto& corner : corners) {
        output.vertices.push_back({corner[0], corner[1], corner[2], cell.block, cell.light});
    }
    output.indices.insert(output.indices.end(), {
        static_cast<uint32_t>(first), static_cast<uint32_t>(first + 1),
        static_cast<uint32_t>(first + 2), static_cast<uint32_t>(first),
        static_cast<uint32_t>(first + 2), static_cast<uint32_t>(first + 3)
    });
}

void appendLODTransitionSkirt(VoxelMeshData& output, size_t face, int x, int y, int z,
    int scale, const LODCell& cell) {
    const float minX = static_cast<float>(x * scale);
    const float minY = static_cast<float>(y * scale);
    const float minZ = static_cast<float>(z * scale);
    const float maxX = static_cast<float>((x + 1) * scale);
    const float maxZ = static_cast<float>((z + 1) * scale);
    const float skirtY = minY - static_cast<float>(scale);
    std::array<std::array<float, 3>, 4> corners{};
    switch (face) {
        case 0: corners = {{{maxX, minY, minZ}, {maxX, minY, maxZ},
            {maxX, skirtY, maxZ}, {maxX, skirtY, minZ}}}; break;
        case 1: corners = {{{minX, minY, maxZ}, {minX, minY, minZ},
            {minX, skirtY, minZ}, {minX, skirtY, maxZ}}}; break;
        case 4: corners = {{{maxX, minY, maxZ}, {minX, minY, maxZ},
            {minX, skirtY, maxZ}, {maxX, skirtY, maxZ}}}; break;
        default: corners = {{{minX, minY, minZ}, {maxX, minY, minZ},
            {maxX, skirtY, minZ}, {minX, skirtY, minZ}}}; break;
    }
    const size_t first = output.vertices.size();
    for (const auto& corner : corners) {
        output.vertices.push_back({corner[0], corner[1], corner[2], cell.block, cell.light});
    }
    output.indices.insert(output.indices.end(), {
        static_cast<uint32_t>(first), static_cast<uint32_t>(first + 1),
        static_cast<uint32_t>(first + 2), static_cast<uint32_t>(first),
        static_cast<uint32_t>(first + 2), static_cast<uint32_t>(first + 3)
    });
    ++output.transitionSkirtFaces;
}

GreedyCell cellForFace(const Chunk& chunk, const VoxelMesher::Neighbors& neighbors,
    size_t face, int plane, int u, int v) {
    int x = 0;
    int y = 0;
    int z = 0;
    if (face < 2) { x = plane; y = v; z = u; }
    else if (face < 4) { x = u; y = plane; z = v; }
    else { x = u; y = v; z = plane; }
    const uint8_t block = chunk.get(x, y, z);
    if (block == 0 || neighborBlock(chunk, neighbors, face, x, y, z) != 0) return {};
    return {block, chunk.getLight(x, y, z), true};
}

}

void VoxelMeshData::clear() {
    vertices.clear();
    indices.clear();
    transitionSkirtFaces = 0;
}

void VoxelMesher::build(const Chunk& chunk, const Neighbors& neighbors, VoxelMeshData& output) {
    output.clear();
    for (int x = 0; x < Chunk::SIZE; ++x) {
        for (int y = 0; y < Chunk::SIZE; ++y) {
            for (int z = 0; z < Chunk::SIZE; ++z) {
                const uint8_t block = chunk.get(x, y, z);
                if (block == 0) continue;
                for (size_t face = 0; face < FACES.size(); ++face) {
                    if (neighborBlock(chunk, neighbors, face, x, y, z) != 0) continue;
                    const size_t first = output.vertices.size();
                    const uint8_t light = chunk.getLight(x, y, z);
                    for (const auto& corner : FACES[face].corners) {
                        output.vertices.push_back({
                            static_cast<float>(x) + corner[0],
                            static_cast<float>(y) + corner[1],
                            static_cast<float>(z) + corner[2],
                            block,
                            light
                        });
                    }
                    output.indices.insert(output.indices.end(), {
                        static_cast<uint32_t>(first), static_cast<uint32_t>(first + 1),
                        static_cast<uint32_t>(first + 2), static_cast<uint32_t>(first),
                        static_cast<uint32_t>(first + 2), static_cast<uint32_t>(first + 3)
                    });
                }
            }
        }
    }
}

void VoxelMesher::buildGreedy(const Chunk& chunk, const Neighbors& neighbors, VoxelMeshData& output) {
    output.clear();
    constexpr int SIZE = Chunk::SIZE;
    for (size_t face = 0; face < FACES.size(); ++face) {
        for (int plane = 0; plane < SIZE; ++plane) {
            std::array<std::array<GreedyCell, SIZE>, SIZE> mask{};
            for (int v = 0; v < SIZE; ++v)
                for (int u = 0; u < SIZE; ++u)
                    mask[v][u] = cellForFace(chunk, neighbors, face, plane, u, v);

            for (int v = 0; v < SIZE; ++v) {
                for (int u = 0; u < SIZE; ++u) {
                    GreedyCell& start = mask[v][u];
                    if (!start.visible) continue;
                    int width = 1;
                    while (u + width < SIZE) {
                        const GreedyCell& candidate = mask[v][u + width];
                        if (!candidate.visible || candidate.block != start.block
                            || candidate.light != start.light) break;
                        ++width;
                    }
                    int height = 1;
                    while (v + height < SIZE) {
                        bool same = true;
                        for (int x = 0; x < width; ++x) {
                            const GreedyCell& candidate = mask[v + height][u + x];
                            if (!candidate.visible || candidate.block != start.block
                                || candidate.light != start.light) {
                                same = false;
                                break;
                            }
                        }
                        if (!same) break;
                        ++height;
                    }
                    appendGreedyQuad(output, face, plane, u, v, width, height, start);
                    for (int y = 0; y < height; ++y)
                        for (int x = 0; x < width; ++x)
                            mask[v + y][u + x].visible = false;
                }
            }
        }
    }
}

void VoxelMesher::buildLOD(const Chunk& chunk, const Neighbors& neighbors, int level, VoxelMeshData& output) {
    output.clear();
    if (level <= 0) {
        buildGreedy(chunk, neighbors, output);
        return;
    }

    const int clampedLevel = std::min(level, 30);
    const int scale = 1 << clampedLevel;
    if (scale > Chunk::SIZE) {
        buildGreedy(chunk, neighbors, output);
        return;
    }
    const int coarseSize = Chunk::SIZE / scale;
    for (int x = 0; x < coarseSize; ++x) {
        for (int y = 0; y < coarseSize; ++y) {
            for (int z = 0; z < coarseSize; ++z) {
                const LODCell cell = lodCell(chunk, neighbors, scale, x, y, z, 0);
                if (!cell.solid) continue;
                for (size_t face = 0; face < FACES.size(); ++face) {
                    const int nx = x + FACES[face].dx;
                    const int ny = y + FACES[face].dy;
                    const int nz = z + FACES[face].dz;
                    if (lodCell(chunk, neighbors, scale, nx, ny, nz, face).solid) continue;
                    appendLODQuad(output, face, x, y, z, scale, cell);
                }
                // A coarse boundary can meet several finer quads in the neighbour chunk. The
                // skirt hides the vertical gap caused by that tessellation mismatch without
                // changing the authored block surface or requiring a GPU stitch shader.
                if (x == coarseSize - 1) appendLODTransitionSkirt(output, 0, x, y, z, scale, cell);
                if (x == 0) appendLODTransitionSkirt(output, 1, x, y, z, scale, cell);
                if (z == coarseSize - 1) appendLODTransitionSkirt(output, 4, x, y, z, scale, cell);
                if (z == 0) appendLODTransitionSkirt(output, 5, x, y, z, scale, cell);
            }
        }
    }
}
