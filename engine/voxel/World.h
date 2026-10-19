#pragma once
#include "Generator.h"
#include <unordered_map>
#include <memory>

// Gerencia múltiplos chunks em um mundo infinito (simplificado)
class World {
public:
    World();
    ~World();

    // Obtém (ou cria) um chunk na posição de chunk (cx, cz)
    Chunk& getChunk(int cx, int cz);

    // Atualiza o mundo – aqui só gera chunks vizinhos simples
    void update();

private:
    using ChunkKey = std::pair<int, int>;
    struct KeyHash {
        std::size_t operator()(const ChunkKey& k) const noexcept {
            return std::hash<int>() (k.first) ^ (std::hash<int>() (k.second) << 1);
        }
    };
    std::unordered_map<ChunkKey, std::unique_ptr<Chunk>, KeyHash> chunks;
    class Generator generator; // gera novos chunks
};
