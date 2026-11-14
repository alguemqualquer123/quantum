// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

class Chunk {
public:
    static constexpr int SIZE = 16;
    using BlockData = std::array<std::array<std::array<uint8_t, SIZE>, SIZE>, SIZE>;

    Chunk();
    static constexpr bool validCoordinate(int x, int y, int z) {
        return x >= 0 && x < SIZE && y >= 0 && y < SIZE && z >= 0 && z < SIZE;
    }
    uint8_t get(int x, int y, int z) const;
    void set(int x, int y, int z, uint8_t id);
    size_t nonAirCount() const;
    uint8_t getSunlight(int x, int y, int z) const;
    uint8_t getBlockLight(int x, int y, int z) const;
    uint8_t getLight(int x, int y, int z) const;
    void rebuildLighting();
    void assign(const BlockData& value) { blocks = value; }
    const BlockData& data() const { return blocks; }

private:
    friend class World;
    void clearLighting();
    void setSunlightValue(int x, int y, int z, uint8_t value) { sunlight[x][y][z] = value; }
    void setBlockLightValue(int x, int y, int z, uint8_t value) { blockLight[x][y][z] = value; }

    BlockData blocks{}; // Zero is the air block.
    BlockData sunlight{};
    BlockData blockLight{};
};
