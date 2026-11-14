// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "Chunk.h"
#include <cstdint>

class Generator {
public:
    enum class Biome : uint8_t {
        Plains,
        Desert,
        Tundra,
        Forest,
    };

    explicit Generator(uint32_t seed = 0x51F15EEDu) : seed(seed) {}
    void setSeed(uint32_t value) { seed = value; }
    uint32_t getSeed() const { return seed; }
    Biome getBiome(int worldX, int worldZ) const;
    void generate(Chunk& chunk, int chunkX = 0, int chunkZ = 0) const;

private:
    uint32_t seed;
};
