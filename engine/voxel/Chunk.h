#pragma once
#include <array>
#include <cstdint>

// Simple fixed‑size voxel chunk (16×16×16)
class Chunk {
public:
    static constexpr int SIZE = 16;
    using BlockData = std::array<std::array<std::array<uint8_t, SIZE>, SIZE>, SIZE>;

    Chunk();
    uint8_t get(int x, int y, int z) const;
    void set(int x, int y, int z, uint8_t id);
    const BlockData& data() const { return blocks; }

private:
    BlockData blocks{}; // inicializa com 0 (ar).
};
