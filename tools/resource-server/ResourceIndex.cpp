// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ResourceIndex.h"
#include "Sha1.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace quantum::rs {

namespace {

constexpr const char* kPersistedMagic = "QRSI1";
constexpr const char* kMetadataMagic = "QAMI1";
constexpr size_t kMaxPersistedBytes = 64u * 1024u * 1024u;
constexpr size_t kMaxPersistedEntries = 500000;

std::string hexEncode(const std::string& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (unsigned char byte : value) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

int hexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool hexDecode(const std::string& value, std::string& result) {
    if ((value.size() & 1u) != 0) return false;
    result.clear();
    result.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        const int high = hexDigit(value[i]);
        const int low = hexDigit(value[i + 1]);
        if (high < 0 || low < 0) return false;
        result.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool parseUnsigned(const std::string& value, uint64_t& result) {
    if (value.empty()) return false;
    result = 0;
    for (const unsigned char digit : value) {
        if (digit < '0' || digit > '9') return false;
        const uint64_t next = result * 10u + static_cast<uint64_t>(digit - '0');
        if (next < result) return false;
        result = next;
    }
    return true;
}

bool parseSigned(const std::string& value, int64_t& result) {
    if (value.empty()) return false;
    size_t offset = 0;
    bool negative = false;
    if (value[0] == '-') {
        negative = true;
        offset = 1;
    }
    if (offset == value.size()) return false;
    uint64_t magnitude = 0;
    if (!parseUnsigned(value.substr(offset), magnitude)) return false;
    if (negative) {
        if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u) return false;
        result = magnitude == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u
            ? std::numeric_limits<int64_t>::min()
            : -static_cast<int64_t>(magnitude);
    } else {
        if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
        result = static_cast<int64_t>(magnitude);
    }
    return true;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> fields;
    size_t begin = 0;
    while (true) {
        const size_t end = line.find('\t', begin);
        fields.push_back(line.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return fields;
}

std::string joinLines(const std::vector<std::string>& values) {
    std::string result;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) result.push_back('\n');
        result += values[i];
    }
    return result;
}

std::vector<std::string> splitLines(const std::string& value) {
    std::vector<std::string> result;
    size_t begin = 0;
    while (begin <= value.size()) {
        const size_t end = value.find('\n', begin);
        result.push_back(value.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (result.size() == 1 && result.front().empty()) result.clear();
    return result;
}

bool validSha1(const std::string& value) {
    if (value.size() != 40) return false;
    for (const char digit : value) if (hexDigit(digit) < 0) return false;
    return true;
}

// Streams "path sha1\n" lines of the whole index into the digest: the
// server's only derived artifact, rebuilt whenever the index changes.
std::string computeManifest(const std::map<std::string, IndexEntry>& entries) {
    Sha1 sha;
    for (const auto& kv : entries) {
        sha.update(kv.first.data(), kv.first.size());
        sha.update(" ", 1);
        sha.update(kv.second.sha1.data(), kv.second.sha1.size());
        sha.update("\n", 1);
        for (const auto& metadata : kv.second.metadata) {
            sha.update("  @ ", 4);
            sha.update(metadata.first.data(), metadata.first.size());
            sha.update("=", 1);
            sha.update(metadata.second.data(), metadata.second.size());
            sha.update("\n", 1);
        }
        for (const std::string& dependency : kv.second.dependencies) {
            sha.update("  -> ", 5);
            sha.update(dependency.data(), dependency.size());
            sha.update("\n", 1);
        }
        for (const std::string& missing : kv.second.missingDependencies) {
            sha.update("  ! ", 4);
            sha.update(missing.data(), missing.size());
            sha.update("\n", 1);
        }
    }
    return sha.hexDigest();
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool looksLikeReference(const std::string& value) {
    if (value.empty() || value.size() > 1024 || value[0] == '#') return false;
    const std::string lowered = lower(value);
    if (lowered.find("://") != std::string::npos) return false;
    static const std::set<std::string> extensions = {
        ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr", ".exr", ".obj", ".fbx",
        ".glb", ".gltf", ".bin", ".wav", ".ogg", ".mp3", ".lua", ".scene", ".yaml",
        ".yml", ".json", ".material", ".mat", ".mtl", ".vert", ".frag", ".glsl",
        ".shader", ".graph", ".gdat", ".ttf", ".otf"
    };
    const size_t dot = lowered.find_last_of('.');
    return dot != std::string::npos && extensions.find(lowered.substr(dot)) != extensions.end();
}

void trimToken(std::string& token) {
    while (!token.empty() && (token.front() == '"' || token.front() == '\'' ||
                              token.front() == ',' || token.front() == ';')) token.erase(0, 1);
    while (!token.empty() && (token.back() == '"' || token.back() == '\'' ||
                              token.back() == ',' || token.back() == ';')) token.pop_back();
    std::replace(token.begin(), token.end(), '\\', '/');
}

std::vector<std::string> dependencyCandidates(const std::string& text) {
    std::set<std::string> unique;
    auto add = [&unique](std::string token) {
        trimToken(token);
        if (looksLikeReference(token)) unique.insert(token);
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '"' || text[i] == '\'') {
            const char quote = text[i++];
            const size_t begin = i;
            while (i < text.size() && text[i] != quote) ++i;
            add(text.substr(begin, i - begin));
            if (i < text.size()) ++i;
            continue;
        }
        const size_t begin = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) &&
               std::string(",;()[]{}<>:=\"'\r\n").find(text[i]) == std::string::npos) ++i;
        if (i > begin) add(text.substr(begin, i - begin));
        else ++i;
    }
    return { unique.begin(), unique.end() };
}

bool isTextFile(const fs::path& path) {
    static const std::set<std::string> extensions = {
        ".c", ".cc", ".cpp", ".frag", ".gltf", ".glsl", ".graph", ".h", ".hpp", ".json", ".lua",
        ".mat", ".material", ".scene", ".shader", ".txt", ".vert", ".yaml", ".yml"
    };
    return extensions.find(lower(path.extension().string())) != extensions.end();
}

uint32_t readLittleU32(const unsigned char* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

void scanDependencies(const fs::path& projectDir, const fs::path& source,
                      const std::string& relative, const std::set<std::string>& available,
                      std::vector<std::string>& dependencies) {
    const std::string extension = lower(source.extension().string());
    const bool glb = extension == ".glb";
    if (!isTextFile(source) && !glb) return;
    std::error_code ec;
    const uintmax_t size = fs::file_size(source, ec);
    if (ec || size > 8 * 1024 * 1024) return;
    std::ifstream stream(source, std::ios::binary);
    if (!stream) return;
    std::string text;
    if (glb) {
        unsigned char header[20] = {};
        if (!stream.read(reinterpret_cast<char*>(header), sizeof(header)) ||
            std::memcmp(header, "glTF", 4) != 0 || readLittleU32(header + 4) != 2 ||
            readLittleU32(header + 16) != 0x4E4F534Au) return;
        const uint32_t jsonSize = readLittleU32(header + 12);
        if (jsonSize == 0 || jsonSize > 8 * 1024 * 1024) return;
        text.resize(jsonSize);
        if (!stream.read(text.data(), static_cast<std::streamsize>(text.size()))) return;
    } else {
        text.resize(static_cast<size_t>(size));
        if (!text.empty() && !stream.read(text.data(), static_cast<std::streamsize>(text.size()))) return;
    }

    std::set<std::string> found;
    for (const std::string& raw : dependencyCandidates(text)) {
        std::vector<fs::path> attempts = {
            source.parent_path() / fs::path(raw),
            projectDir / "assets" / fs::path(raw),
            projectDir / "lua" / fs::path(raw),
            projectDir / fs::path(raw)
        };
        for (const fs::path& attempt : attempts) {
            std::error_code fec;
            const fs::path rel = fs::relative(attempt, projectDir, fec);
            if (fec || rel.empty() || rel.is_absolute()) continue;
            std::string key = rel.generic_string();
            if (key == relative || available.find(key) == available.end()) continue;
            if (key == ".." || key.rfind("../", 0) == 0) continue;
            if (found.insert(key).second) break;
        }
    }
    dependencies.assign(found.begin(), found.end());
}

}

bool normalizeRelPath(const std::string& in, std::string& out) {
    out.clear();
    if (in.empty() || in.size() > 4096) return false;
    // Reject drive letters / UNC / scheme-like tokens outright.
    if (in.find(':') != std::string::npos) return false;
    std::string s;
    s.reserve(in.size());
    for (char c : in) s.push_back(c == '\\' ? '/' : c);
    // Absolute POSIX paths and drive roots are not project-relative.
    if (s.front() == '/') return false;
    // Segment-wise check for traversal and empty segments ("a//b", "a/./b").
    size_t pos = 0;
    while (pos < s.size()) {
        size_t next = s.find('/', pos);
        if (next == std::string::npos) next = s.size();
        std::string seg = s.substr(pos, next - pos);
        if (seg.empty() || seg == "." || seg == "..") return false;
        pos = next + 1;
    }
    out = s;
    return true;
}

using MetadataByPath = std::map<std::string, std::map<std::string, std::string>>;

bool loadMetadataSidecar(const fs::path& path, MetadataByPath& out) {
    out.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return true;
    std::error_code ec;
    const uintmax_t bytes = fs::file_size(path, ec);
    if (ec || bytes > kMaxPersistedBytes) return false;

    std::string line;
    if (!std::getline(stream, line) || line != kMetadataMagic) return false;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> fields = splitTabs(line);
        if (fields.size() != 4 || fields[0] != "entry") return false;
        std::string pathText;
        std::string key;
        std::string value;
        std::string normalizedPath;
        if (!hexDecode(fields[1], pathText) || !normalizeRelPath(pathText, normalizedPath) ||
            !hexDecode(fields[2], key) || key.empty() || !hexDecode(fields[3], value)) {
            return false;
        }
        out[normalizedPath][std::move(key)] = std::move(value);
    }
    return stream.eof();
}

ResourceIndex::ResourceIndex(const fs::path& projectDir) {
    std::error_code ec;
    projectDir_ = fs::absolute(projectDir, ec);
    if (ec) projectDir_ = projectDir;
    canonicalRoot_ = fs::weakly_canonical(projectDir_, ec);
    if (ec) canonicalRoot_ = projectDir_;
    persistencePath_ = projectDir_ / ".quantum" / "resource-index.v1";
    metadataPath_ = projectDir_ / ".quantum" / "asset-metadata.v1";
}

bool ResourceIndex::loadPersisted(std::string& error) {
    error.clear();
    std::ifstream stream(persistencePath_, std::ios::binary);
    if (!stream) return false;
    std::error_code ec;
    const uintmax_t bytes = fs::file_size(persistencePath_, ec);
    if (ec || bytes > kMaxPersistedBytes) {
        error = "persisted resource index is too large or unreadable";
        return false;
    }

    std::string line;
    if (!std::getline(stream, line) || line != kPersistedMagic) {
        error = "unsupported persisted resource index format";
        return false;
    }
    std::map<std::string, IndexEntry> loaded;
    uint64_t loadedScanId = 0;
    int64_t loadedLastScan = 0;
    std::string loadedManifest;
    std::vector<std::string> loadedRoots;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> fields = splitTabs(line);
        if (fields.empty()) continue;
        if (fields[0] == "scanId" && fields.size() == 2) {
            if (!parseUnsigned(fields[1], loadedScanId)) {
                error = "invalid persisted scan id";
                return false;
            }
        } else if (fields[0] == "lastScan" && fields.size() == 2) {
            if (!parseSigned(fields[1], loadedLastScan)) {
                error = "invalid persisted scan timestamp";
                return false;
            }
        } else if (fields[0] == "manifest" && fields.size() == 2) {
            if (!validSha1(fields[1])) {
                error = "invalid persisted manifest hash";
                return false;
            }
            loadedManifest = fields[1];
        } else if (fields[0] == "roots" && fields.size() == 2) {
            std::string decoded;
            if (!hexDecode(fields[1], decoded)) {
                error = "invalid persisted roots";
                return false;
            }
            loadedRoots = splitLines(decoded);
            for (const std::string& root : loadedRoots) {
                if (root != "assets" && root != "lua" && root != "scenes" && root != "scripts") {
                    error = "invalid persisted resource root";
                    return false;
                }
            }
        } else if (fields[0] == "entry" && fields.size() == 6) {
            if (loaded.size() >= kMaxPersistedEntries) {
                error = "persisted resource index has too many entries";
                return false;
            }
            std::string path;
            std::string hash;
            std::string dependencyText;
            uint64_t size = 0;
            int64_t mtime = 0;
            std::string normalizedPath;
            if (!hexDecode(fields[1], path) || !normalizeRelPath(path, normalizedPath)
                    || !parseUnsigned(fields[2], size) || !parseSigned(fields[3], mtime)
                    || !hexDecode(fields[5], dependencyText)
                    || !validSha1(fields[4])) {
                error = "invalid persisted resource entry";
                return false;
            }
            IndexEntry entry;
            entry.size = size;
            entry.mtimeRaw = mtime;
            entry.sha1 = fields[4];
            for (const std::string& raw : splitLines(dependencyText)) {
                std::string dependency;
                if (!normalizeRelPath(raw, dependency)) {
                    error = "invalid persisted dependency";
                    return false;
                }
                entry.dependencies.push_back(std::move(dependency));
            }
            if (!loaded.emplace(std::move(normalizedPath), std::move(entry)).second) {
                error = "duplicate persisted resource entry";
                return false;
            }
        } else if (fields[0] == "metadata" && fields.size() == 4) {
            std::string path;
            std::string key;
            std::string value;
            std::string normalizedPath;
            if (!hexDecode(fields[1], path) || !normalizeRelPath(path, normalizedPath) ||
                !hexDecode(fields[2], key) || key.empty() || !hexDecode(fields[3], value)) {
                error = "invalid persisted resource metadata";
                return false;
            }
            auto entry = loaded.find(normalizedPath);
            if (entry == loaded.end()) {
                error = "metadata references missing persisted resource entry";
                return false;
            }
            entry->second.metadata[std::move(key)] = std::move(value);
        } else {
            error = "unknown persisted resource index record";
            return false;
        }
    }
    if (!stream.eof()) {
        error = "failed to read persisted resource index";
        return false;
    }
    entries_ = std::move(loaded);
    scanId_ = loadedScanId;
    lastScanUnix_ = loadedLastScan;
    manifestSha1_ = std::move(loadedManifest);
    foundRoots_ = std::move(loadedRoots);
    history_.clear();
    return true;
}

bool ResourceIndex::savePersisted(std::string& error) const {
    error.clear();
    std::error_code ec;
    fs::create_directories(persistencePath_.parent_path(), ec);
    if (ec) {
        error = "cannot create resource index directory: " + ec.message();
        return false;
    }
    const fs::path temporary = persistencePath_.parent_path() / "resource-index.v1.tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot write " + temporary.generic_string();
        return false;
    }
    stream << kPersistedMagic << '\n'
           << "scanId\t" << scanId_ << '\n'
           << "lastScan\t" << lastScanUnix_ << '\n'
           << "manifest\t" << manifestSha1_ << '\n'
           << "roots\t" << hexEncode(joinLines(foundRoots_)) << '\n';
    for (const auto& pair : entries_) {
        stream << "entry\t" << hexEncode(pair.first) << '\t'
               << pair.second.size << '\t' << pair.second.mtimeRaw << '\t'
               << pair.second.sha1 << '\t'
               << hexEncode(joinLines(pair.second.dependencies)) << '\n';
        for (const auto& metadata : pair.second.metadata) {
            stream << "metadata\t" << hexEncode(pair.first) << '\t'
                   << hexEncode(metadata.first) << '\t'
                   << hexEncode(metadata.second) << '\n';
        }
    }
    stream.close();
    if (!stream) {
        error = "cannot flush " + temporary.generic_string();
        fs::remove(temporary, ec);
        return false;
    }
    fs::rename(temporary, persistencePath_, ec);
    if (ec) {
        ec.clear();
        fs::remove(persistencePath_, ec);
        ec.clear();
        fs::rename(temporary, persistencePath_, ec);
    }
    if (ec) {
        error = "cannot replace " + persistencePath_.generic_string() + ": " + ec.message();
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

bool ResourceIndex::scanOnce(ChangeSet* outChanges) {
    std::error_code ec;
    lastScanUnix_ = static_cast<long long>(std::time(nullptr));

    std::map<std::string, IndexEntry> fresh;
    MetadataByPath importedMetadata;
    // The sidecar is optional: the standalone server must also serve projects
    // that have not been indexed by the editor CLI yet.
    loadMetadataSidecar(metadataPath_, importedMetadata);
    std::set<std::string> available;
    std::vector<std::string> rootsFound;
    for (const char* root : kRoots) {
        fs::path rootDir = projectDir_ / root;
        if (!fs::is_directory(rootDir, ec)) continue;
        rootsFound.push_back(root);
        fs::recursive_directory_iterator it(rootDir,
            fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (it->is_symlink(fec) || !it->is_regular_file(fec)) continue;
            fs::path rel = fs::relative(it->path(), projectDir_, fec);
            if (fec) continue;
            std::string key;
            if (!normalizeRelPath(rel.generic_u8string(), key)) continue;

            IndexEntry e;
            e.size = static_cast<uint64_t>(it->file_size(fec));
            if (fec) continue;
            auto lt = it->last_write_time(fec);
            if (fec) continue;
            e.mtimeRaw = static_cast<int64_t>(lt.time_since_epoch().count());

            auto prev = entries_.find(key);
            if (prev != entries_.end() && prev->second.size == e.size &&
                prev->second.mtimeRaw == e.mtimeRaw) {
                e.sha1 = prev->second.sha1;  // unchanged: keep cached hash
                e.dependencies = prev->second.dependencies;
            } else {
                std::string hashErr;
                if (!hashFile(it->path().u8string(), e.sha1, hashErr))
                    continue;  // unreadable file: leave it out of this scan
            }
            const auto metadata = importedMetadata.find(key);
            if (metadata != importedMetadata.end()) e.metadata = metadata->second;
            fresh[key] = std::move(e);
            available.insert(key);
        }
    }
    std::sort(rootsFound.begin(), rootsFound.end());
    foundRoots_ = std::move(rootsFound);

    for (auto& pair : fresh) {
        IndexEntry& entry = pair.second;
        if (entry.dependencies.empty()) {
            scanDependencies(projectDir_, projectDir_ / fs::path(pair.first), pair.first,
                             available, entry.dependencies);
        }
        for (const std::string& dependency : entry.dependencies) {
            if (available.find(dependency) == available.end()) entry.missingDependencies.push_back(dependency);
        }
    }

    ChangeSet changes;
    for (const auto& kv : fresh) {
        auto prev = entries_.find(kv.first);
        if (prev == entries_.end()) changes.added.push_back(kv.first);
        else if (prev->second.sha1 != kv.second.sha1 ||
                 prev->second.metadata != kv.second.metadata ||
                 prev->second.dependencies != kv.second.dependencies ||
                 prev->second.missingDependencies != kv.second.missingDependencies) {
            changes.modified.push_back(kv.first);
        }
    }
    for (const auto& kv : entries_) {
        if (fresh.find(kv.first) == fresh.end()) changes.removed.push_back(kv.first);
    }

    bool changed = !changes.empty();
    if (changed) {
        ++scanId_;
        entries_ = std::move(fresh);
        manifestSha1_ = computeManifest(entries_);
        history_.push_back({ scanId_, changes });
        while (history_.size() > kMaxHistoryScans) history_.pop_front();
    }
    if (outChanges && changed) *outChanges = changes;
    return changed;
}

bool ResourceIndex::resolveInside(const std::string& requestPath, std::string& outRel) const {
    if (!normalizeRelPath(requestPath, outRel)) return false;
    std::error_code ec;
    fs::path joined = fs::weakly_canonical(projectDir_ / fs::path(outRel), ec);
    if (ec) return false;
    // Containment: the resolved path must sit inside the canonical root
    // (guards symlink escapes that normalizeRelPath alone cannot see).
    std::string r = joined.generic_u8string();
    std::string root = canonicalRoot_.generic_u8string();
    if (r.size() <= root.size()) return false;
    if (r.compare(0, root.size(), root) != 0) return false;
    char sep = r[root.size()];
    return sep == '/' || sep == '\\';
}

bool ResourceIndex::lookup(const std::string& relPath, IndexEntry& out) const {
    auto it = entries_.find(relPath);
    if (it == entries_.end()) return false;
    out = it->second;
    return true;
}

std::vector<std::pair<std::string, IndexEntry>> ResourceIndex::snapshot() const {
    return { entries_.begin(), entries_.end() };
}

void ResourceIndex::changesSince(uint64_t since, uint64_t& currentScanIdOut,
                                 std::vector<std::string>& added,
                                 std::vector<std::string>& removed,
                                 std::vector<std::string>& modified, bool& expired) const {
    currentScanIdOut = scanId_;
    added.clear(); removed.clear(); modified.clear();
    expired = false;
    if (since == scanId_) return;
    if (since > scanId_) { expired = true; return; }
    if (history_.empty() || since < history_.front().first - 1) { expired = true; return; }
    for (const auto& h : history_) {
        if (h.first <= since) continue;
        added.insert(added.end(), h.second.added.begin(), h.second.added.end());
        removed.insert(removed.end(), h.second.removed.begin(), h.second.removed.end());
        modified.insert(modified.end(), h.second.modified.begin(), h.second.modified.end());
    }
}

}
