// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "ResourceIndex.h"
#include "Sha1.h"

#include <algorithm>
#include <ctime>

namespace doriax::rs {

namespace {

// Streams "path sha1\n" lines of the whole index into the digest: the
// server's only derived artifact, rebuilt whenever the index changes.
std::string computeManifest(const std::map<std::string, IndexEntry>& entries) {
    Sha1 sha;
    for (const auto& kv : entries) {
        sha.update(kv.first.data(), kv.first.size());
        sha.update(" ", 1);
        sha.update(kv.second.sha1.data(), kv.second.sha1.size());
        sha.update("\n", 1);
    }
    return sha.hexDigest();
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

ResourceIndex::ResourceIndex(const fs::path& projectDir) {
    std::error_code ec;
    projectDir_ = fs::absolute(projectDir, ec);
    if (ec) projectDir_ = projectDir;
    canonicalRoot_ = fs::weakly_canonical(projectDir_, ec);
    if (ec) canonicalRoot_ = projectDir_;
}

bool ResourceIndex::scanOnce(ChangeSet* outChanges) {
    std::error_code ec;
    lastScanUnix_ = static_cast<long long>(std::time(nullptr));

    std::map<std::string, IndexEntry> fresh;
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
            } else {
                std::string hashErr;
                if (!hashFile(it->path().u8string(), e.sha1, hashErr))
                    continue;  // unreadable file: leave it out of this scan
            }
            fresh[key] = std::move(e);
        }
    }
    std::sort(rootsFound.begin(), rootsFound.end());
    foundRoots_ = std::move(rootsFound);

    ChangeSet changes;
    for (const auto& kv : fresh) {
        auto prev = entries_.find(kv.first);
        if (prev == entries_.end()) changes.added.push_back(kv.first);
        else if (prev->second.sha1 != kv.second.sha1) changes.modified.push_back(kv.first);
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
