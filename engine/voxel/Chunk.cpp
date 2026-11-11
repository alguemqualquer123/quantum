// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "Chunk.h"

#include <algorithm>
#include <array>
#include <queue>

namespace {

struct LightNode {
    int x;
    int y;
    int z;
    uint8_t level;
};

bool isInside(int x, int y, int z) {
    return Chunk::validCoordinate(x, y, z);
}

uint8_t emissionForBlock(uint8_t block) {
    // Block id 3 is the stable built-in torch convention until a block registry exists.
    return block == 3 ? 14 : 0;
}

}

Chunk::Chunk() = default;

uint8_t Chunk::get(int x, int y, int z) const {
    return validCoordinate(x, y, z) ? blocks[x][y][z] : 0;
}

void Chunk::set(int x, int y, int z, uint8_t id) {
    if (validCoordinate(x, y, z)) blocks[x][y][z] = id;
}

size_t Chunk::nonAirCount() const {
    size_t count = 0;
    for (const auto& plane : blocks)
        for (const auto& row : plane)
            for (uint8_t value : row)
                if (value != 0) ++count;
    return count;
}

uint8_t Chunk::getSunlight(int x, int y, int z) const {
    return validCoordinate(x, y, z) ? sunlight[x][y][z] : 0;
}

uint8_t Chunk::getBlockLight(int x, int y, int z) const {
    return validCoordinate(x, y, z) ? blockLight[x][y][z] : 0;
}

uint8_t Chunk::getLight(int x, int y, int z) const {
    return std::max(getSunlight(x, y, z), getBlockLight(x, y, z));
}

void Chunk::clearLighting() {
    sunlight = {};
    blockLight = {};
}

void Chunk::rebuildLighting() {
    clearLighting();

    std::queue<LightNode> sunlightQueue;
    std::queue<LightNode> blockLightQueue;

    for (int x = 0; x < SIZE; ++x) {
        for (int z = 0; z < SIZE; ++z) {
            bool blocked = false;
            for (int y = SIZE - 1; y >= 0; --y) {
                if (blocks[x][y][z] != 0) {
                    blocked = true;
                } else if (!blocked) {
                    sunlight[x][y][z] = 15;
                    sunlightQueue.push({x, y, z, 15});
                }
            }
        }
    }

    for (int x = 0; x < SIZE; ++x) {
        for (int y = 0; y < SIZE; ++y) {
            for (int z = 0; z < SIZE; ++z) {
                const uint8_t emission = emissionForBlock(blocks[x][y][z]);
                if (emission != 0) {
                    blockLight[x][y][z] = emission;
                    blockLightQueue.push({x, y, z, emission});
                }
            }
        }
    }

    constexpr std::array<std::array<int, 3>, 6> directions = {
        std::array<int, 3>{1, 0, 0}, std::array<int, 3>{-1, 0, 0},
        std::array<int, 3>{0, 1, 0}, std::array<int, 3>{0, -1, 0},
        std::array<int, 3>{0, 0, 1}, std::array<int, 3>{0, 0, -1}
    };

    auto propagate = [this, &directions](std::queue<LightNode>& queue, BlockData& target) {
        while (!queue.empty()) {
            const LightNode current = queue.front();
            queue.pop();
            if (current.level <= 1) continue;
            for (const auto& direction : directions) {
                const int nx = current.x + direction[0];
                const int ny = current.y + direction[1];
                const int nz = current.z + direction[2];
                if (!isInside(nx, ny, nz) || blocks[nx][ny][nz] != 0) continue;
                const uint8_t nextLevel = static_cast<uint8_t>(current.level - 1);
                if (target[nx][ny][nz] >= nextLevel) continue;
                target[nx][ny][nz] = nextLevel;
                queue.push({nx, ny, nz, nextLevel});
            }
        }
    };

    propagate(sunlightQueue, sunlight);
    propagate(blockLightQueue, blockLight);
}
