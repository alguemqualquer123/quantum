// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "SaveGame.h"

#include "System.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace quantum {
namespace {

constexpr char MAGIC[] = "QSV1";
constexpr uint32_t CURRENT_VERSION = 3;
constexpr uint32_t MAX_ENTRIES = 4096;
constexpr uint32_t MAX_KEY_BYTES = 256;
constexpr uint32_t MAX_VALUE_BYTES = 1024 * 1024;
constexpr size_t MAX_TOTAL_BYTES = 16u * 1024u * 1024u;

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

void appendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

uint32_t checksum(const uint8_t* bytes, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

bool readString(std::istream& stream, std::string& value, uint32_t limit) {
    uint32_t length = 0;
    if (!readU32(stream, length) || length > limit) return false;
    value.assign(length, '\0');
    if (length != 0) stream.read(value.data(), length);
    return static_cast<bool>(stream);
}

bool validKey(const std::string& key) {
    return !key.empty() && key.size() <= MAX_KEY_BYTES && key.find('=') == std::string::npos;
}

std::unordered_map<uint32_t, SaveGame::MigrationFunction>& migrations() {
    static std::unordered_map<uint32_t, SaveGame::MigrationFunction> value;
    return value;
}

bool migrateV1ToV2(std::unordered_map<std::string, std::string>&) {
    //Version 2 keeps the primitive key/value representation. The explicit
    //step is intentional: future releases can add schema changes without
    //silently accepting an incompatible file.
    return true;
}

bool migrateV2ToV3(std::unordered_map<std::string, std::string>&) {
    // Version 3 adds a checksum to newly written files. Older payloads remain
    // valid because the checksum is validated only when it is present on disk.
    return true;
}

void ensureBuiltInMigrations() {
    static const bool registered = SaveGame::registerMigration(1, migrateV1ToV2);
    (void)registered;
    static const bool registeredV2 = SaveGame::registerMigration(2, migrateV2ToV3);
    (void)registeredV2;
}

bool replaceAtomically(const std::filesystem::path& temporary,
                       const std::filesystem::path& target) {
#if defined(_WIN32)
    // std::filesystem::rename cannot replace an existing file on Windows. Removing the
    // destination first would leave a valid save with no recoverable predecessor if the
    // replacement failed, so use the native replace-and-flush operation instead.
    return MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    return !error;
#endif
}

}

SaveGame::SaveGame(const std::string& slotName) : slot(slotName) {}

bool SaveGame::registerMigration(uint32_t fromVersion, MigrationFunction migration) {
    if (fromVersion == 0 || fromVersion >= CURRENT_VERSION || migration == nullptr) return false;
    migrations()[fromVersion] = migration;
    return true;
}

bool SaveGame::validName(const std::string& value) {
    if (value.empty() || value == "." || value == ".." || value.size() > 64
        || value.find("..") != std::string::npos) return false;
    for (const char c : value) {
        if (c == '/' || c == '\\' || c == ':' || c == '\0') return false;
    }
    return true;
}

bool SaveGame::isValid() const { return validName(slot); }

std::string SaveGame::path() const {
    if (!isValid()) return {};
    // Headless tools can exercise persistence before a platform System exists; runtime
    // builds still use the platform-owned user-data directory.
    const std::filesystem::path root = System::hasInstance()
        ? std::filesystem::path(System::instance().getUserDataPath())
        : std::filesystem::current_path();
    return (root / "saves" / (slot + ".qsave")).string();
}

bool SaveGame::exists() const {
    const std::string filename = path();
    return !filename.empty() && std::filesystem::is_regular_file(filename);
}

bool SaveGame::deleteSlot() {
    const std::string filename = path();
    if (filename.empty()) return false;
    values.clear();
    std::error_code error;
    const bool removed = std::filesystem::remove(filename, error);
    return !error && (removed || !std::filesystem::exists(filename));
}

bool SaveGame::save() const {
    const std::string filename = path();
    if (filename.empty() || values.size() > MAX_ENTRIES) return false;

    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(values.size());
    for (const auto& entry : values) {
        if (!validKey(entry.first) || entry.second.size() > MAX_VALUE_BYTES) return false;
        entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    std::ostringstream payload(std::ios::out | std::ios::binary);
    writeU32(payload, static_cast<uint32_t>(entries.size()));
    size_t totalBytes = sizeof(uint32_t);
    for (const auto& entry : entries) {
        const size_t entryBytes = sizeof(uint32_t) + entry.first.size()
            + sizeof(uint32_t) + entry.second.size();
        if (entryBytes > MAX_TOTAL_BYTES - totalBytes) return false;
        totalBytes += entryBytes;
        writeU32(payload, static_cast<uint32_t>(entry.first.size()));
        payload.write(entry.first.data(), static_cast<std::streamsize>(entry.first.size()));
        writeU32(payload, static_cast<uint32_t>(entry.second.size()));
        payload.write(entry.second.data(), static_cast<std::streamsize>(entry.second.size()));
    }
    const std::string payloadBytes = payload.str();
    if (payloadBytes.size() > MAX_TOTAL_BYTES) return false;

    const std::filesystem::path target(filename);
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) return false;

    const std::filesystem::path temporary = target.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(MAGIC, 4);
    writeU32(stream, CURRENT_VERSION);
    stream.write(payloadBytes.data(), static_cast<std::streamsize>(payloadBytes.size()));
    writeU32(stream, checksum(reinterpret_cast<const uint8_t*>(payloadBytes.data()), payloadBytes.size()));
    stream.flush();
    if (!stream) return false;
    stream.close();
    if (!replaceAtomically(temporary, target)) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

bool SaveGame::load() {
    const std::string filename = path();
    if (filename.empty()) return false;
    std::ifstream stream(filename, std::ios::binary);
    if (!stream) return false;
    char magic[4]{};
    stream.read(magic, sizeof(magic));
    uint32_t version = 0, count = 0;
    if (!stream || std::memcmp(magic, MAGIC, sizeof(magic)) != 0 || !readU32(stream, version)
        || version == 0 || version > CURRENT_VERSION || !readU32(stream, count)
        || count > MAX_ENTRIES) return false;
    std::vector<uint8_t> payload;
    payload.reserve(sizeof(uint32_t));
    appendU32(payload, count);
    std::unordered_map<std::string, std::string> loaded;
    size_t totalBytes = sizeof(uint32_t);
    for (uint32_t i = 0; i < count; ++i) {
        std::string key, value;
        const uint32_t keyLimit = totalBytes + sizeof(uint32_t) <= MAX_TOTAL_BYTES
            ? static_cast<uint32_t>(std::min<size_t>(MAX_KEY_BYTES,
                MAX_TOTAL_BYTES - totalBytes - sizeof(uint32_t))) : 0;
        if (!readString(stream, key, keyLimit) || !validKey(key)) return false;
        totalBytes += sizeof(uint32_t) + key.size();
        const uint32_t valueLimit = totalBytes + sizeof(uint32_t) <= MAX_TOTAL_BYTES
            ? static_cast<uint32_t>(std::min<size_t>(MAX_VALUE_BYTES,
                MAX_TOTAL_BYTES - totalBytes - sizeof(uint32_t))) : 0;
        if (!readString(stream, value, valueLimit)) return false;
        totalBytes += sizeof(uint32_t) + value.size();
        appendU32(payload, static_cast<uint32_t>(key.size()));
        payload.insert(payload.end(), key.begin(), key.end());
        appendU32(payload, static_cast<uint32_t>(value.size()));
        payload.insert(payload.end(), value.begin(), value.end());
        if (!loaded.emplace(key, value).second) return false;
    }
    if (version >= 3) {
        uint32_t storedChecksum = 0;
        if (!readU32(stream, storedChecksum)
            || storedChecksum != checksum(payload.data(), payload.size())
            || stream.peek() != std::char_traits<char>::eof()) return false;
    }
    ensureBuiltInMigrations();
    while (version < CURRENT_VERSION) {
        const auto migration = migrations().find(version);
        if (migration == migrations().end() || !migration->second(loaded)) return false;
        ++version;
    }
    values = std::move(loaded);
    return true;
}

void SaveGame::clear() { values.clear(); }

void SaveGame::setString(const std::string& key, const std::string& value) {
    if (validKey(key) && value.size() <= MAX_VALUE_BYTES) values[key] = value;
}

void SaveGame::setBlob(const std::string& key, const std::vector<uint8_t>& value) {
    if (value.size() > MAX_VALUE_BYTES) return;
    if (value.empty()) {
        setString(key, {});
        return;
    }
    setString(key, std::string(reinterpret_cast<const char*>(value.data()), value.size()));
}

void SaveGame::setInt(const std::string& key, int64_t value) { setString(key, std::to_string(value)); }

void SaveGame::setNumber(const std::string& key, double value) {
    if (std::isfinite(value)) setString(key, std::to_string(value));
}

std::string SaveGame::getString(const std::string& key, const std::string& defaultValue) const {
    const auto it = values.find(key);
    return it == values.end() ? defaultValue : it->second;
}

bool SaveGame::getBlob(const std::string& key, std::vector<uint8_t>& value) const {
    const auto it = values.find(key);
    if (it == values.end()) return false;
    value.assign(reinterpret_cast<const uint8_t*>(it->second.data()),
        reinterpret_cast<const uint8_t*>(it->second.data() + it->second.size()));
    return true;
}

int64_t SaveGame::getInt(const std::string& key, int64_t defaultValue) const {
    const std::string value = getString(key);
    if (value.empty()) return defaultValue;
    int64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc() && result.ptr == value.data() + value.size() ? parsed : defaultValue;
}

double SaveGame::getNumber(const std::string& key, double defaultValue) const {
    const std::string value = getString(key);
    if (value.empty()) return defaultValue;
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(value.c_str(), &end);
    return errno == 0 && end == value.c_str() + value.size() && std::isfinite(parsed) ? parsed : defaultValue;
}

bool SaveGame::has(const std::string& key) const { return values.find(key) != values.end(); }
void SaveGame::remove(const std::string& key) { values.erase(key); }

}
