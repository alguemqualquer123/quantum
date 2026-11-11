// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "World.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr char REGION_MAGIC[] = "QVX1";
constexpr uint32_t REGION_VERSION = 3;
constexpr uint32_t LEGACY_REGION_VERSION = 1;
constexpr uint32_t RLE_REGION_VERSION = 2;
constexpr uint32_t MAX_REGION_CHUNKS = 4096;
constexpr uint32_t CHUNK_BYTES = Chunk::SIZE * Chunk::SIZE * Chunk::SIZE;
constexpr uint32_t MAX_ENCODED_CHUNK_BYTES = CHUNK_BYTES * 2;
constexpr size_t MAX_REGION_BYTES = 256u * 1024u * 1024u;

struct WorldLightNode {
    int chunkX;
    int chunkZ;
    int x;
    int y;
    int z;
    uint8_t level;
    bool blockLight;
};

int floorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    const int remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

int positiveMod(int value, int divisor) {
    const int remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

uint8_t emissionForBlock(uint8_t block) {
    return block == 3 ? 14 : 0;
}

void writeU32(std::ostream& stream, uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xffu), static_cast<char>((value >> 8) & 0xffu),
        static_cast<char>((value >> 16) & 0xffu), static_cast<char>((value >> 24) & 0xffu)
    };
    stream.write(bytes, sizeof(bytes));
}

bool readU32(std::istream& stream, uint32_t& value) {
    unsigned char bytes[4]{};
    stream.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!stream) return false;
    value = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8)
        | (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
    return true;
}

uint32_t checksum(const char* bytes, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint8_t>(bytes[i]);
        hash *= 16777619u;
    }
    return hash;
}

void writeI32(std::ostream& stream, int value) {
    writeU32(stream, static_cast<uint32_t>(value));
}

bool readI32(std::istream& stream, int& value) {
    uint32_t encoded = 0;
    if (!readU32(stream, encoded)) return false;
    value = static_cast<int32_t>(encoded);
    return true;
}

std::vector<uint8_t> flattenChunk(const Chunk& chunk) {
    std::vector<uint8_t> raw;
    raw.reserve(CHUNK_BYTES);
    for (const auto& plane : chunk.data())
        for (const auto& row : plane)
            raw.insert(raw.end(), row.begin(), row.end());
    return raw;
}

std::vector<uint8_t> encodeRle(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> encoded;
    encoded.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        const uint8_t value = raw[i];
        size_t count = 1;
        while (i + count < raw.size() && raw[i + count] == value && count < 255) ++count;
        encoded.push_back(static_cast<uint8_t>(count));
        encoded.push_back(value);
        i += count;
    }
    return encoded;
}

bool decodeRle(const std::vector<uint8_t>& encoded, size_t expectedSize, std::vector<uint8_t>& raw) {
    if ((encoded.size() % 2) != 0) return false;
    raw.clear();
    raw.reserve(expectedSize);
    for (size_t i = 0; i < encoded.size(); i += 2) {
        const size_t count = encoded[i];
        if (count == 0 || raw.size() + count > expectedSize) return false;
        raw.insert(raw.end(), count, encoded[i + 1]);
    }
    return raw.size() == expectedSize;
}

bool writeChunk(std::ostream& stream, const Chunk& chunk) {
    const std::vector<uint8_t> raw = flattenChunk(chunk);
    const std::vector<uint8_t> encoded = encodeRle(raw);
    if (encoded.size() > MAX_ENCODED_CHUNK_BYTES) return false;
    writeU32(stream, static_cast<uint32_t>(encoded.size()));
    writeU32(stream, static_cast<uint32_t>(raw.size()));
    stream.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    return static_cast<bool>(stream);
}

bool readChunk(std::istream& stream, Chunk& chunk, uint32_t version) {
    std::vector<uint8_t> raw;
    if (version == LEGACY_REGION_VERSION) {
        raw.resize(CHUNK_BYTES);
        stream.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        if (!stream) return false;
    } else if (version == RLE_REGION_VERSION || version == REGION_VERSION) {
        uint32_t encodedSize = 0;
        uint32_t rawSize = 0;
        if (!readU32(stream, encodedSize) || !readU32(stream, rawSize)
            || rawSize != CHUNK_BYTES || encodedSize == 0 || encodedSize > MAX_ENCODED_CHUNK_BYTES)
            return false;
        std::vector<uint8_t> encoded(encodedSize);
        stream.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        if (!stream || !decodeRle(encoded, rawSize, raw)) return false;
    }

    Chunk::BlockData data{};
    size_t offset = 0;
    for (auto& plane : data)
        for (auto& row : plane)
            for (uint8_t& value : row)
                value = raw[offset++];
    chunk.assign(data);
    return true;
}

bool replaceAtomically(const std::filesystem::path& temporary, const std::filesystem::path& target) {
#if defined(_WIN32)
    return MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    return !error;
#endif
}

}

World::World(uint32_t seed) : generator(seed) {}
World::~World() { discardPendingLoads(); }

const Chunk* World::findChunk(int cx, int cz) const {
    const auto it = chunks.find({cx, cz});
    return it == chunks.end() ? nullptr : it->second.get();
}

std::vector<std::pair<int, int>> World::loadedChunkCoordinates() const {
    std::vector<std::pair<int, int>> coordinates;
    coordinates.reserve(chunks.size());
    for (const auto& entry : chunks) coordinates.push_back(entry.first);
    std::sort(coordinates.begin(), coordinates.end());
    return coordinates;
}

std::vector<std::pair<int, int>> World::consumeMeshDirtyChunks() {
    std::vector<std::pair<int, int>> coordinates;
    coordinates.reserve(meshDirtyChunks.size());
    for (const auto& entry : meshDirtyChunks) coordinates.push_back(entry);
    std::sort(coordinates.begin(), coordinates.end());
    meshDirtyChunks.clear();
    return coordinates;
}

bool World::inStreamingWindow(const ChunkKey& key) const {
    const int dx = key.first - streamingCenterX;
    const int dz = key.second - streamingCenterZ;
    return std::abs(dx) <= streamingRadius && std::abs(dz) <= streamingRadius;
}

size_t World::commitReadyLoads() {
    size_t committed = 0;
    for (auto it = pendingLoads.begin(); it != pendingLoads.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        std::unique_ptr<Chunk> loaded = it->second.get();
        const ChunkKey key = it->first;
        if (loaded && inStreamingWindow(key) && chunks.find(key) == chunks.end()) {
            chunks.emplace(key, std::move(loaded));
            meshDirtyChunks.insert(key);
            ++committed;
        }
        it = pendingLoads.erase(it);
    }
    return committed;
}

void World::discardPendingLoads() {
    for (auto& entry : pendingLoads) {
        // Always consume the future before erasing it; the worker owns no World
        // state, so waiting here is safe during shutdown or a region reload.
        entry.second.wait();
        entry.second.get();
    }
    pendingLoads.clear();
}

uint8_t World::getBlock(int worldX, int worldY, int worldZ) const {
    if (worldY < 0 || worldY >= Chunk::SIZE) return 0;
    const int chunkX = floorDiv(worldX, Chunk::SIZE);
    const int chunkZ = floorDiv(worldZ, Chunk::SIZE);
    const auto it = chunks.find({chunkX, chunkZ});
    if (it == chunks.end()) return 0;
    return it->second->get(positiveMod(worldX, Chunk::SIZE), worldY,
        positiveMod(worldZ, Chunk::SIZE));
}

VoxelRayHit World::raycast(float originX, float originY, float originZ,
    float directionX, float directionY, float directionZ, float maxDistance) const {
    VoxelRayHit result;
    if (maxDistance < 0.0f) return result;
    const float length = std::sqrt(directionX * directionX + directionY * directionY
        + directionZ * directionZ);
    if (length <= std::numeric_limits<float>::epsilon()) return result;
    directionX /= length;
    directionY /= length;
    directionZ /= length;

    int x = static_cast<int>(std::floor(originX));
    int y = static_cast<int>(std::floor(originY));
    int z = static_cast<int>(std::floor(originZ));
    if (getBlock(x, y, z) != 0) {
        result.hit = true;
        result.blockX = x;
        result.blockY = y;
        result.blockZ = z;
        result.block = getBlock(x, y, z);
        return result;
    }

    const int stepX = directionX > 0.0f ? 1 : (directionX < 0.0f ? -1 : 0);
    const int stepY = directionY > 0.0f ? 1 : (directionY < 0.0f ? -1 : 0);
    const int stepZ = directionZ > 0.0f ? 1 : (directionZ < 0.0f ? -1 : 0);
    const float infinity = std::numeric_limits<float>::infinity();
    const float deltaX = stepX == 0 ? infinity : std::abs(1.0f / directionX);
    const float deltaY = stepY == 0 ? infinity : std::abs(1.0f / directionY);
    const float deltaZ = stepZ == 0 ? infinity : std::abs(1.0f / directionZ);
    float nextX = stepX > 0 ? (static_cast<float>(x + 1) - originX) / directionX
        : (stepX < 0 ? (static_cast<float>(x) - originX) / directionX : infinity);
    float nextY = stepY > 0 ? (static_cast<float>(y + 1) - originY) / directionY
        : (stepY < 0 ? (static_cast<float>(y) - originY) / directionY : infinity);
    float nextZ = stepZ > 0 ? (static_cast<float>(z + 1) - originZ) / directionZ
        : (stepZ < 0 ? (static_cast<float>(z) - originZ) / directionZ : infinity);

    float distance = 0.0f;
    for (int steps = 0; steps < 4096 && distance <= maxDistance; ++steps) {
        int normalX = 0;
        int normalY = 0;
        int normalZ = 0;
        if (nextX <= nextY && nextX <= nextZ) {
            distance = nextX;
            nextX += deltaX;
            x += stepX;
            normalX = -stepX;
        } else if (nextY <= nextZ) {
            distance = nextY;
            nextY += deltaY;
            y += stepY;
            normalY = -stepY;
        } else {
            distance = nextZ;
            nextZ += deltaZ;
            z += stepZ;
            normalZ = -stepZ;
        }
        if (distance > maxDistance) break;
        const uint8_t block = getBlock(x, y, z);
        if (block == 0) continue;
        result.hit = true;
        result.distance = distance;
        result.blockX = x;
        result.blockY = y;
        result.blockZ = z;
        result.normalX = normalX;
        result.normalY = normalY;
        result.normalZ = normalZ;
        result.block = block;
        return result;
    }
    return result;
}

Chunk& World::getChunk(int cx, int cz) {
    ChunkKey key = {cx, cz};
    auto it = chunks.find(key);
    if (it == chunks.end()) {
        auto pending = pendingLoads.find(key);
        if (pending != pendingLoads.end()) {
            std::unique_ptr<Chunk> loaded = pending->second.get();
            pendingLoads.erase(pending);
            if (loaded) {
                auto inserted = chunks.emplace(key, std::move(loaded));
                return *inserted.first->second;
            }
        }
        it = chunks.find(key);
    }
    if (it == chunks.end()) {
        auto newChunk = std::make_unique<Chunk>();
        generator.generate(*newChunk, cx, cz);
        newChunk->rebuildLighting();
        Chunk& ref = *newChunk;
        chunks.emplace(key, std::move(newChunk));
        return ref;
    }
    return *it->second;
}

bool World::setBlock(int worldX, int worldY, int worldZ, uint8_t block) {
    if (worldY < 0 || worldY >= Chunk::SIZE) return false;
    const int chunkX = floorDiv(worldX, Chunk::SIZE);
    const int chunkZ = floorDiv(worldZ, Chunk::SIZE);
    Chunk& chunk = getChunk(chunkX, chunkZ);
    const int localX = positiveMod(worldX, Chunk::SIZE);
    const int localZ = positiveMod(worldZ, Chunk::SIZE);
    if (chunk.get(localX, worldY, localZ) == block) return false;
    chunk.set(localX, worldY, localZ, block);
    markChunkDirty(chunkX, chunkZ);
    if (localX == 0) markChunkDirty(chunkX - 1, chunkZ);
    if (localX == Chunk::SIZE - 1) markChunkDirty(chunkX + 1, chunkZ);
    if (localZ == 0) markChunkDirty(chunkX, chunkZ - 1);
    if (localZ == Chunk::SIZE - 1) markChunkDirty(chunkX, chunkZ + 1);
    lightingDirty = true;
    return true;
}

void World::markChunkDirty(int chunkX, int chunkZ) {
    if (chunks.find({chunkX, chunkZ}) != chunks.end()) {
        dirtyChunks.insert({chunkX, chunkZ});
        meshDirtyChunks.insert({chunkX, chunkZ});
    }
}

void World::update(int centerChunkX, int centerChunkZ, int radius) {
    if (radius < 0) radius = 0;
    streamingCenterX = centerChunkX;
    streamingCenterZ = centerChunkZ;
    streamingRadius = radius;
    // Synchronous callers own the full update budget, so pending work must be
    // resolved before the deterministic path continues.
    discardPendingLoads();
    for (int cx = centerChunkX - radius; cx <= centerChunkX + radius; ++cx) {
        for (int cz = centerChunkZ - radius; cz <= centerChunkZ + radius; ++cz) {
            getChunk(cx, cz);
        }
    }

    for (auto it = chunks.begin(); it != chunks.end();) {
        const int dx = it->first.first - centerChunkX;
        const int dz = it->first.second - centerChunkZ;
        if (std::abs(dx) > radius || std::abs(dz) > radius)
            it = chunks.erase(it);
        else
            ++it;
    }

    rebuildLighting();
}

size_t World::updateAsync(int centerChunkX, int centerChunkZ, int radius, size_t maxJobs) {
    if (radius < 0) radius = 0;
    streamingCenterX = centerChunkX;
    streamingCenterZ = centerChunkZ;
    streamingRadius = radius;

    const size_t committed = commitReadyLoads();
    std::vector<ChunkKey> missing;
    for (int cx = centerChunkX - radius; cx <= centerChunkX + radius; ++cx) {
        for (int cz = centerChunkZ - radius; cz <= centerChunkZ + radius; ++cz) {
            const ChunkKey key{cx, cz};
            if (chunks.find(key) == chunks.end() && pendingLoads.find(key) == pendingLoads.end()) {
                missing.push_back(key);
            }
        }
    }
    std::sort(missing.begin(), missing.end());
    const size_t jobs = (std::min)(maxJobs, missing.size());
    const uint32_t seed = generator.getSeed();
    for (size_t i = 0; i < jobs; ++i) {
        const ChunkKey key = missing[i];
        try {
            pendingLoads.emplace(key, std::async(std::launch::async, [seed, key]() {
                try {
                    Generator workerGenerator(seed);
                    auto chunk = std::make_unique<Chunk>();
                    workerGenerator.generate(*chunk, key.first, key.second);
                    chunk->rebuildLighting();
                    return chunk;
                } catch (...) {
                    // A worker must never let an exception escape through the
                    // future boundary; the next update can retry this chunk.
                    return std::unique_ptr<Chunk>();
                }
            }));
        } catch (...) {
            // std::async may fail to create a worker. Leave the key absent so a
            // later call can retry without making the streaming API throw.
        }
    }

    for (auto it = chunks.begin(); it != chunks.end();) {
        const int dx = it->first.first - centerChunkX;
        const int dz = it->first.second - centerChunkZ;
        if (std::abs(dx) > radius || std::abs(dz) > radius)
            it = chunks.erase(it);
        else
            ++it;
    }
    if (committed > 0 || lightingDirty) rebuildLighting();
    return committed;
}

void World::rebuildLighting() {
    constexpr std::array<std::array<int, 3>, 6> directions = {
        std::array<int, 3>{1, 0, 0}, std::array<int, 3>{-1, 0, 0},
        std::array<int, 3>{0, 1, 0}, std::array<int, 3>{0, -1, 0},
        std::array<int, 3>{0, 0, 1}, std::array<int, 3>{0, 0, -1}
    };
    std::queue<WorldLightNode> queue;
    for (auto& entry : chunks) {
        meshDirtyChunks.insert(entry.first);
        entry.second->clearLighting();
        for (int x = 0; x < Chunk::SIZE; ++x) {
            for (int z = 0; z < Chunk::SIZE; ++z) {
                bool blocked = false;
                for (int y = Chunk::SIZE - 1; y >= 0; --y) {
                    if (entry.second->get(x, y, z) != 0) {
                        blocked = true;
                    } else if (!blocked) {
                        entry.second->setSunlightValue(x, y, z, 15);
                        queue.push({entry.first.first, entry.first.second, x, y, z, 15, false});
                    }
                }
            }
        }
        for (int x = 0; x < Chunk::SIZE; ++x) {
            for (int y = 0; y < Chunk::SIZE; ++y) {
                for (int z = 0; z < Chunk::SIZE; ++z) {
                    const uint8_t emission = emissionForBlock(entry.second->get(x, y, z));
                    if (emission != 0) {
                        entry.second->setBlockLightValue(x, y, z, emission);
                        queue.push({entry.first.first, entry.first.second, x, y, z, emission, true});
                    }
                }
            }
        }
    }

    while (!queue.empty()) {
        const WorldLightNode current = queue.front();
        queue.pop();
        if (current.level <= 1) continue;
        for (const auto& direction : directions) {
            int nextChunkX = current.chunkX;
            int nextChunkZ = current.chunkZ;
            int nx = current.x + direction[0];
            const int ny = current.y + direction[1];
            int nz = current.z + direction[2];
            if (ny < 0 || ny >= Chunk::SIZE) continue;
            if (nx < 0) { --nextChunkX; nx += Chunk::SIZE; }
            if (nx >= Chunk::SIZE) { ++nextChunkX; nx -= Chunk::SIZE; }
            if (nz < 0) { --nextChunkZ; nz += Chunk::SIZE; }
            if (nz >= Chunk::SIZE) { ++nextChunkZ; nz -= Chunk::SIZE; }
            auto targetIt = chunks.find({nextChunkX, nextChunkZ});
            if (targetIt == chunks.end() || targetIt->second->get(nx, ny, nz) != 0) continue;
            const uint8_t nextLevel = static_cast<uint8_t>(current.level - 1);
            const uint8_t existing = current.blockLight
                ? targetIt->second->getBlockLight(nx, ny, nz)
                : targetIt->second->getSunlight(nx, ny, nz);
            if (existing >= nextLevel) continue;
            if (current.blockLight) targetIt->second->setBlockLightValue(nx, ny, nz, nextLevel);
            else targetIt->second->setSunlightValue(nx, ny, nz, nextLevel);
            queue.push({nextChunkX, nextChunkZ, nx, ny, nz, nextLevel, current.blockLight});
        }
    }
    lightingDirty = false;
}

bool World::saveRegion(const std::string& filename) const {
    if (filename.empty() || chunks.size() > MAX_REGION_CHUNKS) return false;

    std::vector<std::pair<int, int>> coordinates;
    coordinates.reserve(chunks.size());
    for (const auto& entry : chunks) coordinates.push_back(entry.first);
    std::sort(coordinates.begin(), coordinates.end());
    std::ostringstream body(std::ios::out | std::ios::binary);
    writeU32(body, static_cast<uint32_t>(coordinates.size()));
    for (const auto& coordinate : coordinates) {
        const auto it = chunks.find(coordinate);
        if (it == chunks.end()) return false;
        writeI32(body, coordinate.first);
        writeI32(body, coordinate.second);
        if (!writeChunk(body, *it->second)) return false;
    }
    const std::string bodyBytes = body.str();
    if (bodyBytes.size() > MAX_REGION_BYTES) return false;

    const std::filesystem::path target(filename);
    std::error_code error;
    if (!target.parent_path().empty()) {
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) return false;
    }
    const std::filesystem::path temporary = target.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(REGION_MAGIC, sizeof(REGION_MAGIC) - 1);
    writeU32(stream, REGION_VERSION);
    stream.write(bodyBytes.data(), static_cast<std::streamsize>(bodyBytes.size()));
    writeU32(stream, checksum(bodyBytes.data(), bodyBytes.size()));
    stream.flush();
    stream.close();
    if (!stream) return false;
    if (!replaceAtomically(temporary, target)) {
        error.clear();
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

bool World::saveRegionIncremental(const std::string& filename) {
    if (filename.empty()) return false;
    if (dirtyChunks.empty() && std::filesystem::exists(filename)) return true;

    World merged(generator.getSeed());
    if (std::filesystem::exists(filename)) {
        if (!merged.loadRegion(filename)) return false;
    }
    for (const auto& entry : chunks) {
        if (dirtyChunks.find(entry.first) == dirtyChunks.end()
            && merged.chunks.find(entry.first) != merged.chunks.end()) {
            continue;
        }
        auto copy = std::make_unique<Chunk>();
        copy->assign(entry.second->data());
        copy->rebuildLighting();
        merged.chunks[entry.first] = std::move(copy);
    }
    if (!merged.saveRegion(filename)) return false;
    dirtyChunks.clear();
    return true;
}

bool World::loadRegion(const std::string& filename) {
    if (filename.empty()) return false;
    discardPendingLoads();
    std::error_code fileError;
    const uintmax_t fileSize = std::filesystem::file_size(filename, fileError);
    if (fileError || fileSize > MAX_REGION_BYTES) return false;
    std::ifstream file(filename, std::ios::binary);
    if (!file) return false;
    std::string fileBytes(static_cast<size_t>(fileSize), '\0');
    if (fileSize != 0) file.read(fileBytes.data(), static_cast<std::streamsize>(fileSize));
    if (!file) return false;
    std::istringstream stream(fileBytes, std::ios::in | std::ios::binary);
    char magic[sizeof(REGION_MAGIC) - 1]{};
    stream.read(magic, sizeof(magic));
    uint32_t version = 0;
    uint32_t count = 0;
    if (!stream || std::memcmp(magic, REGION_MAGIC, sizeof(magic)) != 0
        || !readU32(stream, version)
        || (version != LEGACY_REGION_VERSION && version != RLE_REGION_VERSION
            && version != REGION_VERSION)) return false;
    const std::streampos bodyStart = stream.tellg();
    if (bodyStart < 0 || !readU32(stream, count) || count > MAX_REGION_CHUNKS) return false;

    std::unordered_map<ChunkKey, std::unique_ptr<Chunk>, KeyHash> loaded;
    for (uint32_t i = 0; i < count; ++i) {
        int cx = 0;
        int cz = 0;
        if (!readI32(stream, cx) || !readI32(stream, cz)) return false;
        auto chunk = std::make_unique<Chunk>();
        if (!readChunk(stream, *chunk, version)
            || !loaded.emplace(ChunkKey{cx, cz}, std::move(chunk)).second)
            return false;
        loaded.at(ChunkKey{cx, cz})->rebuildLighting();
    }
    if (version == REGION_VERSION) {
        const std::streampos bodyEnd = stream.tellg();
        uint32_t storedChecksum = 0;
        if (bodyStart < 0 || bodyEnd < bodyStart || !readU32(stream, storedChecksum)
            || storedChecksum != checksum(fileBytes.data() + static_cast<size_t>(bodyStart),
                static_cast<size_t>(bodyEnd - bodyStart))
            || stream.peek() != std::char_traits<char>::eof()) return false;
    }
    chunks.swap(loaded);
    dirtyChunks.clear();
    for (const auto& entry : chunks) meshDirtyChunks.insert(entry.first);
    rebuildLighting();
    return true;
}
