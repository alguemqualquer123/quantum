#include "Chunk.h"

Chunk::Chunk() {
    // All blocks default to 0 (air)
    for (auto &plane : blocks)
        for (auto &row : plane)
            for (auto &col : row)
                col = 0;
}

uint8_t Chunk::get(int x, int y, int z) const {
    return blocks[x][y][z];
}

void Chunk::set(int x, int y, int z, uint8_t id) {
    blocks[x][y][z] = id;
}
