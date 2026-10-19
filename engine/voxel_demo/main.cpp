#include <iostream>
#include "../voxel/World.h"

int main() {
    World world;
    world.update(); // gera chunks ao redor da origem
    auto &chunk = world.getChunk(0,0);
    std::cout << "Chunk (0,0) block[0,0,0] = " << (int)chunk.get(0,0,0) << std::endl;
    return 0;
}
