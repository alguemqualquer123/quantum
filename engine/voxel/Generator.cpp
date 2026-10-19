#include "Generator.h"

void Generator::generate(Chunk& chunk) {
    // exemplo simples: cria uma camada de pedra (id=1) até y=4
    for (int x = 0; x < Chunk::SIZE; ++x) {
        for (int z = 0; z < Chunk::SIZE; ++z) {
            for (int y = 0; y < 5; ++y) {
                chunk.set(x, y, z, 1); // 1 = pedra
            }
        }
    }
}
