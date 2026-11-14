// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "ResourcePack.h"

#include "System.h"
#include "util/Checksum.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#ifndef _WIN32
#include <sys/types.h>
#endif

using namespace quantum;

namespace {

    constexpr char PACK_MAGIC_V1[] = {'D', 'X', 'P', 'K', '1'};
    constexpr char PACK_MAGIC_V2[] = {'D', 'X', 'P', 'K', '2'};
    constexpr const char* PACK_FILENAME = "resources.pak";

#ifdef _WIN32
    typedef __int64 pack_offset_t;
#else
    typedef off_t pack_offset_t;
#endif

    // Still 32-bit on the armeabi-v7a and x86 ABIs, so a larger pack is rejected
    constexpr uint64_t MAX_PACK_OFFSET = static_cast<uint64_t>(std::numeric_limits<pack_offset_t>::max());

    struct PackEntry {
        uint64_t offset = 0;
        uint64_t size = 0;
        uint8_t key = 0;
        uint32_t shift = 0;
        bool hasDigest = false;
        std::array<unsigned char, 32> digest{};
    };

    struct PackRange {
        uint64_t offset = 0;
        uint64_t size = 0;
        uint8_t key = 0;
        uint32_t shift = 0;
        bool hasDigest = false;
        std::array<unsigned char, 32> digest{};
    };

    struct PackState {
        std::string filename;
        std::map<std::string, PackEntry> entries;
    };

    // Same static-destruction guard as TextureDataPool::getMap()
    PackState& state() {
        static PackState* packState = new PackState();
        return *packState;
    }

    // Filled once, then read-only, so lookups take no lock
    std::once_flag loadFlag;

    // Keys are '/'-separated and relative to the export root. "." and ".." resolve
    // like FileData::simplifyPath, so a glTF "models/../textures/a.png" still hits.
    std::string normalizePath(std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');

        std::vector<std::string> parts;
        for (size_t start = 0; start <= path.size(); ) {
            const size_t end = path.find('/', start);
            const std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);

            if (part == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!part.empty() && part != ".") {
                parts.push_back(part);
            }

            if (end == std::string::npos) break;
            start = end + 1;
        }

        std::string result;
        for (size_t i = 0; i < parts.size(); i++) {
            if (i > 0) result += '/';
            result += parts[i];
        }
        return result;
    }

    bool startsWith(const std::string& value, const std::string& prefix) {
        return value.rfind(prefix, 0) == 0;
    }

    // Follows FileData::getSystemPath, but only the two packed roots have keys
    std::string getPackKey(std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');
        if (path.empty()) return "";

        if (startsWith(path, "asset://")) return "assets/" + normalizePath(path.substr(8));
        if (startsWith(path, "lua://")) return "lua/" + normalizePath(path.substr(6));

        if (path.find("://") != std::string::npos) return "";
        if (path.front() == '/' || (path.size() > 1 && path[1] == ':')) return "";

        return "assets/" + normalizePath(path);
    }

    bool readExact(FILE* file, void* dst, size_t size) {
        return std::fread(dst, 1, size, file) == size;
    }

    bool readU8(FILE* file, uint8_t& value) {
        return readExact(file, &value, 1);
    }

    bool readU16(FILE* file, uint16_t& value) {
        unsigned char bytes[2];
        if (!readExact(file, bytes, sizeof(bytes))) return false;
        value = static_cast<uint16_t>(bytes[0])
              | (static_cast<uint16_t>(bytes[1]) << 8);
        return true;
    }

    bool readU32(FILE* file, uint32_t& value) {
        unsigned char bytes[4];
        if (!readExact(file, bytes, sizeof(bytes))) return false;
        value = static_cast<uint32_t>(bytes[0])
              | (static_cast<uint32_t>(bytes[1]) << 8)
              | (static_cast<uint32_t>(bytes[2]) << 16)
              | (static_cast<uint32_t>(bytes[3]) << 24);
        return true;
    }

    bool readU64(FILE* file, uint64_t& value) {
        unsigned char bytes[8];
        if (!readExact(file, bytes, sizeof(bytes))) return false;
        value = 0;
        for (int i = 0; i < 8; i++) {
            value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
        }
        return true;
    }

    bool seekFile(FILE* file, uint64_t offset) {
#ifdef _WIN32
        return _fseeki64(file, static_cast<pack_offset_t>(offset), SEEK_SET) == 0;
#else
        return fseeko(file, static_cast<pack_offset_t>(offset), SEEK_SET) == 0;
#endif
    }

    bool getFileSize(FILE* file, uint64_t& size) {
#ifdef _WIN32
        if (_fseeki64(file, 0, SEEK_END) != 0) return false;
        const pack_offset_t end = _ftelli64(file);
#else
        if (fseeko(file, 0, SEEK_END) != 0) return false;
        const pack_offset_t end = ftello(file);
#endif
        if (end < 0) return false;

        size = static_cast<uint64_t>(end);
        return seekFile(file, 0);
    }

    std::string digestHex(const std::array<unsigned char, 32>& digest) {
        static const char* digits = "0123456789abcdef";
        std::string result;
        result.reserve(digest.size() * 2);
        for (unsigned char byte : digest) {
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 0x0f]);
        }
        return result;
    }

    void deobfuscate(std::vector<unsigned char>& data, uint8_t key, uint32_t shift);

    // Keep this in lockstep with ResourceCompilerCli's deterministic metadata.
    uint64_t deterministicEntryHash(const std::string& path, uint64_t size) {
        uint64_t hash = 1469598103934665603ULL;
        auto add = [&hash](unsigned char byte) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        };
        for (unsigned char byte : path) add(byte);
        for (int i = 0; i < 8; ++i) add(static_cast<unsigned char>((size >> (i * 8)) & 0xff));
        return hash;
    }

    bool verifyPack(const std::string& filename) {
        // Verification is also called by the standalone resource compiler before
        // the engine platform singleton exists, so it must not bootstrap System.
        FILE* file = std::fopen(filename.c_str(), "rb");
        if (!file) return false;

        uint64_t packSize = 0;
        char magic[sizeof(PACK_MAGIC_V1)] = {};
        uint32_t fileCount = 0;
        bool hasDigest = false;
        bool ok = getFileSize(file, packSize)
            && packSize <= MAX_PACK_OFFSET
            && readExact(file, magic, sizeof(magic))
            && ((std::equal(std::begin(PACK_MAGIC_V1), std::end(PACK_MAGIC_V1), magic))
                || (hasDigest = std::equal(std::begin(PACK_MAGIC_V2), std::end(PACK_MAGIC_V2), magic)))
            && readU32(file, fileCount);

        constexpr uint64_t PACK_HEADER_SIZE = sizeof(PACK_MAGIC_V1) + sizeof(uint32_t);
        const uint64_t packEntryMinSize = sizeof(uint16_t) + 1 + sizeof(uint64_t)
            + sizeof(uint64_t) + sizeof(uint8_t) + sizeof(uint32_t)
            + (hasDigest ? 32 : 0);
        const uint64_t maxEntries = packSize >= PACK_HEADER_SIZE
            ? (packSize - PACK_HEADER_SIZE) / packEntryMinSize : 0;
        if (ok && static_cast<uint64_t>(fileCount) > maxEntries) ok = false;

        std::set<std::string> paths;
        std::vector<PackRange> ranges;
        if (ok) ranges.reserve(fileCount);

        uint64_t headerEnd = PACK_HEADER_SIZE;
        for (uint32_t i = 0; ok && i < fileCount; ++i) {
            uint16_t pathLength = 0;
            if (!readU16(file, pathLength) || pathLength == 0) {
                ok = false;
                break;
            }

            std::string path(pathLength, '\0');
            if (!readExact(file, path.data(), pathLength)
                || path.find('\0') != std::string::npos
                || normalizePath(path) != path
                || !(startsWith(path, "assets/") || startsWith(path, "lua/"))
                || !paths.insert(path).second) {
                ok = false;
                break;
            }

            PackEntry entry;
            if (!readU64(file, entry.offset)
                || !readU64(file, entry.size)
                || !readU8(file, entry.key)
                || !readU32(file, entry.shift)) {
                ok = false;
                break;
            }
            entry.hasDigest = hasDigest;
            if (hasDigest && !readExact(file, entry.digest.data(), entry.digest.size())) {
                ok = false;
                break;
            }

            headerEnd += sizeof(uint16_t) + pathLength + sizeof(uint64_t) + sizeof(uint64_t)
                + sizeof(uint8_t) + sizeof(uint32_t) + (hasDigest ? entry.digest.size() : 0);
            if (entry.offset < headerEnd
                || entry.offset > packSize
                || entry.size > packSize - entry.offset
                || entry.size > UINT32_MAX) {
                ok = false;
                break;
            }

            const uint64_t hash = deterministicEntryHash(path, entry.size);
            const uint8_t expectedKey = static_cast<uint8_t>((hash % 255ULL) + 1ULL);
            const uint32_t expectedShift = entry.size == 0
                ? 0 : static_cast<uint32_t>(hash % entry.size);
            if (entry.key != expectedKey || entry.shift != expectedShift) {
                ok = false;
                break;
            }

            ranges.push_back({entry.offset, entry.size, entry.key, entry.shift,
                              entry.hasDigest, entry.digest});
        }

        if (ok) {
            std::sort(ranges.begin(), ranges.end(), [](const PackRange& left, const PackRange& right) {
                if (left.offset != right.offset) return left.offset < right.offset;
                return left.size < right.size;
            });

            uint64_t coveredEnd = headerEnd;
            for (const PackRange& range : ranges) {
                if (range.offset != coveredEnd) {
                    ok = false;
                    break;
                }
                coveredEnd = std::max(coveredEnd, range.offset + range.size);
            }
            if (ok && coveredEnd != packSize) ok = false;
        }

        // Reading the bodies makes verification fail on a file that changes or
        // becomes unreadable after its size/index passed the structural checks.
        if (ok) {
            std::vector<unsigned char> buffer(64 * 1024);
            for (const PackRange& range : ranges) {
                if (!seekFile(file, range.offset)) {
                    ok = false;
                    break;
                }
                uint64_t remaining = range.size;
                std::vector<unsigned char> body;
                if (range.hasDigest) body.reserve(static_cast<size_t>(range.size));
                while (remaining > 0) {
                    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
                    if (std::fread(buffer.data(), 1, chunk, file) != chunk) {
                        ok = false;
                        break;
                    }
                    if (range.hasDigest) body.insert(body.end(), buffer.begin(), buffer.begin() + chunk);
                    remaining -= chunk;
                }
                if (ok && range.hasDigest) {
                    deobfuscate(body, range.key, range.shift);
                    if (sha256Hex(body.data(), body.size()) != digestHex(range.digest)) ok = false;
                }
                if (!ok) break;
            }
        }

        std::fclose(file);
        return ok;
    }

    bool loadPack(const std::string& filename) {
        FILE* file = System::instance().platformFopen(filename.c_str(), "rb");
        if (!file) return false;

        uint64_t packSize = 0;
        char magic[sizeof(PACK_MAGIC_V1)] = {};
        uint32_t fileCount = 0;
        bool ok = getFileSize(file, packSize)
            && packSize <= MAX_PACK_OFFSET
            && readExact(file, magic, sizeof(magic))
            && (std::equal(std::begin(PACK_MAGIC_V1), std::end(PACK_MAGIC_V1), magic)
                || std::equal(std::begin(PACK_MAGIC_V2), std::end(PACK_MAGIC_V2), magic))
            && readU32(file, fileCount);

        const bool digestPack = std::equal(std::begin(PACK_MAGIC_V2), std::end(PACK_MAGIC_V2), magic);

        std::map<std::string, PackEntry> entries;
        for (uint32_t i = 0; ok && i < fileCount; i++) {
            uint16_t pathLength = 0;
            if (!readU16(file, pathLength) || pathLength == 0) {
                ok = false;
                break;
            }

            std::string path(pathLength, '\0');
            if (!readExact(file, path.data(), pathLength)) {
                ok = false;
                break;
            }

            PackEntry entry;
            if (!readU64(file, entry.offset)
                || !readU64(file, entry.size)
                || !readU8(file, entry.key)
                || !readU32(file, entry.shift)) {
                ok = false;
                break;
            }
            entry.hasDigest = digestPack;
            if (digestPack && !readExact(file, entry.digest.data(), entry.digest.size())) {
                ok = false;
                break;
            }

            // Bounds come from the file, so a corrupt one must not become an allocation
            if (entry.offset > packSize || entry.size > packSize - entry.offset
                || entry.size > UINT32_MAX) {
                ok = false;
                break;
            }

            entries[normalizePath(path)] = entry;
        }

        std::fclose(file);

        if (!ok) return false;

        state().filename = filename;
        state().entries = std::move(entries);
        return true;
    }

    void loadPackOnce() {
#ifndef QUANTUM_EDITOR
        // Beside the executable on desktop, at the asset root inside an APK
        if (loadPack(PACK_FILENAME)) return;

        // Used as given: absolute in a macOS bundle, or wherever QUANTUM_ASSET_PATH points
        const std::string assetRoot = System::instance().getAssetPath();
        if (!assetRoot.empty()) {
            loadPack(assetRoot + "/" + PACK_FILENAME);
        }
#endif
    }

    const PackEntry* findEntry(const std::string& path) {
        std::call_once(loadFlag, loadPackOnce);

        const PackState& pack = state();
        if (pack.entries.empty()) return nullptr;

        const std::string key = getPackKey(path);
        if (key.empty()) return nullptr;

        auto it = pack.entries.find(key);
        return it != pack.entries.end() ? &it->second : nullptr;
    }

    // Inverse of Exporter::obfuscate, which rotates right and then XORs
    void deobfuscate(std::vector<unsigned char>& data, uint8_t key, uint32_t shift) {
        if (data.empty()) return;

        for (unsigned char& byte : data) {
            byte ^= key;
        }

        shift %= data.size();
        if (shift > 0) {
            std::rotate(data.begin(), data.begin() + shift, data.end());
        }
    }

}

bool ResourcePack::contains(const std::string& path) {
    return findEntry(path) != nullptr;
}

bool ResourcePack::read(const std::string& path, std::vector<unsigned char>& outData) {
    const PackEntry* entry = findEntry(path);
    if (!entry) return false;

    FILE* file = System::instance().platformFopen(state().filename.c_str(), "rb");
    if (!file) return false;

    bool ok = seekFile(file, entry->offset);
    if (ok) {
        outData.resize(static_cast<size_t>(entry->size));
        ok = entry->size == 0 || readExact(file, outData.data(), static_cast<size_t>(entry->size));
    }
    std::fclose(file);

    if (!ok) {
        outData.clear();
        return false;
    }

    deobfuscate(outData, entry->key, entry->shift);
    if (entry->hasDigest && sha256Hex(outData.data(), outData.size()) != digestHex(entry->digest)) {
        outData.clear();
        return false;
    }
    return true;
}

bool ResourcePack::verify(const std::string& packPath) {
    return !packPath.empty() && verifyPack(packPath);
}
