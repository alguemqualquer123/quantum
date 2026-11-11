#include "Generator.h"

namespace {

uint32_t hashCoordinates(uint32_t seed, int x, int z) {
    uint32_t value = seed;
    value ^= static_cast<uint32_t>(x) * 0x9E3779B9u;
    value ^= static_cast<uint32_t>(z) * 0x85EBCA6Bu;
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    return value ^ (value >> 16);
}

}

Generator::Biome Generator::getBiome(int worldX, int worldZ) const {
    const uint32_t climate = hashCoordinates(seed ^ 0xA53C9E17u, worldX / 8, worldZ / 8) % 100u;
    if (climate < 20u) {
        return Biome::Desert;
    }
    if (climate < 40u) {
        return Biome::Tundra;
    }
    if (climate < 65u) {
        return Biome::Forest;
    }
    return Biome::Plains;
}

void Generator::generate(Chunk& chunk, int chunkX, int chunkZ) const {
    for (int x = 0; x < Chunk::SIZE; ++x) {
        for (int z = 0; z < Chunk::SIZE; ++z) {
            const int worldX = chunkX * Chunk::SIZE + x;
            const int worldZ = chunkZ * Chunk::SIZE + z;
            const Biome biome = getBiome(worldX, worldZ);
            const int height = 2 + static_cast<int>(hashCoordinates(seed, worldX, worldZ) % 6u)
                + (biome == Biome::Forest ? 1 : 0);
            for (int y = 0; y < height; ++y) {
                uint8_t block = 1; // 1 = stone
                if (y == height - 1) {
                    block = biome == Biome::Desert ? 4 : biome == Biome::Tundra ? 5 : 2;
                } else if (y == height - 2 && biome == Biome::Forest) {
                    block = 6; // 6 = forest soil
                }
                chunk.set(x, y, z, block);
            }
        }
    }
}
