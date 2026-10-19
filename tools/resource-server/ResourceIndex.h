// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

// ResourceIndex.h - walks a project's resource roots and keeps a content-hashed
// index with change history per scan. Used by doriax-resource-server.

#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace doriax::rs {

namespace fs = std::filesystem;

struct IndexEntry {
    uint64_t size = 0;
    int64_t mtimeRaw = 0;    // file_time clock ticks; only compared, never rendered
    std::string sha1;        // lowercase hex
};

// Relative paths use '/' separators and are project-rooted. Returns false for
// absolute paths, drive letters, embedded ".." segments, or empty results -
// this is the request-side path-traversal guard. The canonical containment
// check in resolveInside() is the second layer.
bool normalizeRelPath(const std::string& in, std::string& out);

class ResourceIndex {
public:
    struct ChangeSet {
        std::vector<std::string> added, removed, modified;
        bool empty() const { return added.empty() && removed.empty() && modified.empty(); }
    };

    // The roots scanned are assets/, lua/, scenes/, scripts/ (missing ones are
    // tolerated). projectDir is made absolute in the constructor.
    explicit ResourceIndex(const fs::path& projectDir);

    // Re-walks the roots; files whose size and mtime are unchanged keep their
    // cached SHA-1, anything else is re-hashed. When the entry set changes,
    // scanId() is bumped and the change recorded in the history. Returns true
    // when something changed; outChanges (optional) receives this scan's diff.
    bool scanOnce(ChangeSet* outChanges = nullptr);

    uint64_t scanId() const { return scanId_; }
    size_t fileCount() const { return entries_.size(); }
    // SHA-1 over the sorted "path sha1" list; rebuilt whenever the index
    // changes (the server's derived-data artifact).
    const std::string& manifestSha1() const { return manifestSha1_; }
    long long lastScanUnix() const { return lastScanUnix_; }
    const std::vector<std::string>& foundRoots() const { return foundRoots_; }
    const fs::path& projectDir() const { return projectDir_; }

    // Normalizes requestPath (traversal-guarded) and, when it resolves to a
    // real path inside the project, returns it. outRel receives the normalized
    // relative path; false means the request tried to escape or was malformed.
    bool resolveInside(const std::string& requestPath, std::string& outRel) const;

    bool lookup(const std::string& relPath, IndexEntry& out) const;

    // Copy of the whole index for the "index" command (sorted by path).
    std::vector<std::pair<std::string, IndexEntry>> snapshot() const;

    // Union of all changes with scanId in (since, scanId()]. When `since` is
    // older than the retained history, expired is set true and the vectors are
    // empty (the caller should fall back to a full "index").
    void changesSince(uint64_t since, uint64_t& currentScanIdOut, std::vector<std::string>& added,
                      std::vector<std::string>& removed, std::vector<std::string>& modified,
                      bool& expired) const;

private:
    static constexpr const char* kRoots[4] = { "assets", "lua", "scenes", "scripts" };
    static constexpr size_t kMaxHistoryScans = 512;

    fs::path projectDir_;
    fs::path canonicalRoot_;
    std::map<std::string, IndexEntry> entries_;
    uint64_t scanId_ = 0;
    std::string manifestSha1_;
    long long lastScanUnix_ = 0;
    std::vector<std::string> foundRoots_;
    // (scanId at which the changes took effect, changes) - oldest first.
    std::deque<std::pair<uint64_t, ChangeSet>> history_;
};

}
