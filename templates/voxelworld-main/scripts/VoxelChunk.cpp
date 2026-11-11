// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "VoxelChunk.h"

#include <cstring>

namespace voxel {

static void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)(v)); out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)(v >> 16)); out.push_back((uint8_t)(v >> 24));
}
static void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back((uint8_t)(v)); out.push_back((uint8_t)(v >> 8));
}

struct Reader {
    const uint8_t* p; size_t left;
    bool ok = true;
    uint32_t u32() { if (left < 4) { ok = false; return 0; } uint32_t v = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); p += 4; left -= 4; return v; }
    uint16_t u16() { if (left < 2) { ok = false; return 0; } uint32_t v = p[0] | (p[1] << 8); p += 2; left -= 2; return (uint16_t)v; }
    uint8_t u8() { if (left < 1) { ok = false; return 0; } return *p++; }
    void take(uint8_t* dst, size_t n) {
        if (left < n) { ok = false; return; }
        memcpy(dst, p, n); p += n; left -= n;
    }
};

std::vector<uint8_t> Chunk::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(24 + blocks.size() * 2 + light.size() + height.size() + biome.size());
    put32(out, MAGIC);
    put32(out, (uint32_t)cx);
    put32(out, (uint32_t)cz);
    put32(out, (uint32_t)WORLD_HEIGHT);
    put32(out, (uint32_t)blocks.size());
    for (size_t i = 0; i < blocks.size(); i++) put16(out, blocks[i]);
    out.insert(out.end(), light.begin(), light.end());
    out.insert(out.end(), height.begin(), height.end());
    out.insert(out.end(), biome.begin(), biome.end());
    return out;
}

bool Chunk::deserialize(const uint8_t* data, size_t size) {
    Reader r{ data, size };
    if (r.u32() != MAGIC) return false;
    cx = (int32_t)r.u32();
    cz = (int32_t)r.u32();
    if ((int)r.u32() != WORLD_HEIGHT) return false;      // a column of another height is another format
    uint32_t count = r.u32();
    if (count != (uint32_t)CHUNK_VOLUME) return false;
    allocate();
    for (uint32_t i = 0; i < count; i++) blocks[i] = r.u16();
    r.take(light.data(), light.size());
    r.take(height.data(), height.size());
    r.take(biome.data(), biome.size());
    if (!r.ok) return false;
    generated = true;
    dirtyMesh = true;
    dirtySave = false;
    return true;
}

}  // namespace voxel
