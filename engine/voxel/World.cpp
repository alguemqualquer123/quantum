#include "World.h"

World::World() {}
World::~World() {}

Chunk& World::getChunk(int cx, int cz) {
    ChunkKey key = {cx, cz};
    auto it = chunks.find(key);
    if (it == chunks.end()) {
        auto newChunk = std::make_unique<Chunk>();
        generator.generate(*newChunk);
        Chunk& ref = *newChunk;
        chunks.emplace(key, std::move(newChunk));
        return ref;
    }
    return *it->second;
}

void World::update() {
    // Exemplo: garante que os chunks ao redor da origem existam
    const int RADIUS = 1;
    for (int cx = -RADIUS; cx <= RADIUS; ++cx) {
        for (int cz = -RADIUS; cz <= RADIUS; ++cz) {
            getChunk(cx, cz);
        }
    }
}
