// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelSave.h"

#include "VoxelConfig.h"
#include "VoxelBlocks.h"
#include "VoxelChunk.h"
#include "VoxelProbe.h"
#include "VoxelWorld.h"
#include "thread/ThreadPoolManager.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace voxel {
namespace {

// Column file layout: [4-byte format version][Chunk::serialize() payload][4-byte CRC32 of payload].
// The version is checked before the payload is trusted and the CRC covers the body, so a torn,
// truncated or bit-rotted file is rejected instead of loading garbage into the world.
const uint32_t SAVE_FILE_VERSION = 1;
const char* const SAVE_EXT = ".vck";
const char* const INDEX_NAME = "index.txt";
const char* const JOURNAL_NAME = "edits.log";

// A payload is ~97 KB (64K blocks + 32K light + height/biome), so the byte cap binds well before
// the column cap; both exist so revisiting a neighbourhood never costs a disk read while the
// cache cannot grow without bound.
const size_t MAX_CACHE_COLUMNS = 256;
const size_t MAX_CACHE_BYTES = 16u * 1024u * 1024u;
const size_t MAX_PREFETCHES = 32;
const size_t MAX_FILE_BYTES = 4u * 1024u * 1024u;   // a column file is far below this
const int INDEX_EVERY_WRITES = 32;

enum ColumnRead { READ_BAD = -1, READ_ABSENT = 0, READ_OK = 1 };
// A job slot needs its own pending value, because "absent" is also a finished result.
enum JobState { JOB_FAILED = -1, JOB_PENDING = 0, JOB_LOADED = 1 };

// ---------------------------------------------------------------- crc32 (table based, no deps)

struct Crc32Table {
    uint32_t t[256];
    Crc32Table() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};

uint32_t crc32(const uint8_t* p, size_t n) {
    static const Crc32Table tab;   // initialised once; thread safe since C++11
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab.t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)(v)); out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)(v >> 16)); out.push_back((uint8_t)(v >> 24));
}

uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Hand-rolled integer parsing: std::stoi/stol throw, and this build has no exceptions.
bool parseInt(const char* s, const char* e, int& out) {
    if (s >= e) return false;
    bool neg = false;
    if (*s == '-' || *s == '+') { neg = (*s == '-'); s++; }
    if (s >= e) return false;
    long long v = 0;
    for (; s < e; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (*s - '0');
        if (v > 0x7FFFFFFFll) return false;
    }
    out = neg ? (int)-v : (int)v;
    return true;
}

bool parseInts(const char* s, const char* e, int* out, int count) {
    int n = 0;
    while (n < count) {
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        const char* start = s;
        while (s < e && *s >= '0' && *s <= '9') s++;
        if (s == start && s < e && (*s == '-' || *s == '+')) { s++; while (s < e && *s >= '0' && *s <= '9') s++; }
        if (!parseInt(start, s, out[n])) return false;
        n++;
    }
    return true;
}

// ---------------------------------------------------------------- paths

std::string trimDir(const std::string& dir) {
    size_t end = dir.size();
    while (end > 0 && (dir[end - 1] == '/' || dir[end - 1] == '\\')) end--;
    return dir.substr(0, end);
}

std::string columnName(int cx, int cz) {
    // The coords are in the name, so a suspect file is identifiable without opening it.
    return std::to_string(cx) + "_" + std::to_string(cz) + SAVE_EXT;
}

std::string columnPath(const std::string& dir, int cx, int cz) { return dir + "/" + columnName(cx, cz); }

bool parseColumnName(const std::string& name, int& cx, int& cz) {
    if (name.size() <= 4) return false;
    if (name.compare(name.size() - 4, 4, SAVE_EXT) != 0) return false;
    const std::string stem = name.substr(0, name.size() - 4);
    size_t sep = stem.find('_');
    if (sep == std::string::npos) return false;
    return parseInts(stem.c_str(), stem.c_str() + sep, &cx, 1) &&
           parseInts(stem.c_str() + sep + 1, stem.c_str() + stem.size(), &cz, 1);
}

// ---------------------------------------------------------------- column file I/O

// Version, CRC and the coords stored inside the payload must all agree with the file's own name
// before the bytes are handed to Chunk::deserialize.
int validateColumnFile(const std::vector<uint8_t>& raw, int cx, int cz, std::vector<uint8_t>& payload) {
    if (raw.size() < 12 || raw.size() > MAX_FILE_BYTES) return READ_BAD;
    if (get32(raw.data()) != SAVE_FILE_VERSION) return READ_BAD;
    const size_t body = raw.size() - 8;
    if (body < 24) return READ_BAD;
    if (get32(raw.data() + 4 + body) != crc32(raw.data() + 4, body)) return READ_BAD;
    // Chunk::serialize() repeats the coords it belongs to; a file whose name and contents disagree
    // is not the column we asked for.
    if (get32(raw.data() + 4) != Chunk::MAGIC) return READ_BAD;
    if ((int32_t)get32(raw.data() + 8) != cx || (int32_t)get32(raw.data() + 12) != cz) return READ_BAD;
    payload.assign(raw.begin() + 4, raw.begin() + 4 + (long)body);
    return READ_OK;
}

int readColumnFile(const std::string& path, int cx, int cz, std::vector<uint8_t>& payload) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return READ_ABSENT;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0 || (uint64_t)size > MAX_FILE_BYTES) return READ_BAD;
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> raw;
    raw.resize((size_t)size);
    in.read((char*)raw.data(), (std::streamsize)raw.size());
    if (!in) return READ_BAD;
    return validateColumnFile(raw, cx, cz, payload);
}

// tmp + rename: a crash mid-write leaves the previous good save intact rather than a torn file.
// MinGW's rename can refuse an existing target, so retry once after removing it.
bool writeFileAtomic(const std::string& path, const std::vector<uint8_t>& bytes, const std::string& tag) {
    const std::string tmp = path + "." + tag + ".tmp";
    {
        std::ofstream out(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) return false;
        // An emptied journal writes zero bytes, and a vector that size may not have a buffer at all:
        // the truncating open above already produced the empty file, so only a non-empty body is sent.
        if (!bytes.empty()) out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        out.flush();
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code rm;
        std::filesystem::remove(path, rm);
        ec = std::error_code();
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        std::error_code rm;
        std::filesystem::remove(tmp, rm);
        return false;
    }
    return true;
}

std::vector<uint8_t> buildFileBytes(const std::vector<uint8_t>& payload, uint32_t crc) {
    std::vector<uint8_t> out;
    out.reserve(payload.size() + 8);
    put32(out, SAVE_FILE_VERSION);
    out.insert(out.end(), payload.begin(), payload.end());
    put32(out, crc);
    return out;
}

// ---------------------------------------------------------------- state

struct CacheEntry {
    int64_t key;
    std::vector<uint8_t> payload;
};

// An async read handed to a worker. The worker touches only these bytes and the filesystem; the
// main thread promotes finished payloads into the LRU.
struct PrefetchJob {
    int64_t key = 0;
    std::atomic<int> status{JOB_PENDING};
    std::vector<uint8_t> payload;
    std::future<void> fut;
};

// One in-flight column write. Nothing is thrown across the future (engine builds have
// _HAS_EXCEPTIONS=0), so the worker records the outcome in `status` and flush() only ever waits.
struct PendingWrite {
    std::future<void> fut;
    int64_t key = 0;
    int cx = 0;
    int cz = 0;
    uint32_t seq = 0;          // journal lines with seq <= this are inside the snapshot
    uint32_t bytes = 0;
    uint32_t crc = 0;
    std::shared_ptr<std::atomic<int>> status;
};

struct JournalLine {
    int64_t key = 0;
    uint32_t seq = 0;
    int cx = 0, cz = 0, x = 0, y = 0, z = 0;
    uint32_t id = 0;
};

struct SaveState {
    std::string dir;
    bool dirOk = false;

    std::mutex mu;   // guards everything below; worker lambdas never take it

    std::list<CacheEntry> lru;                                     // front = most recent
    std::unordered_map<int64_t, std::list<CacheEntry>::iterator> lruIndex;
    size_t lruBytes = 0;

    std::vector<PendingWrite> pending;
    std::unordered_map<int64_t, std::vector<uint8_t>> deferred;    // newer snapshot for a busy column
    std::vector<std::shared_ptr<PrefetchJob>> prefetch;
    std::vector<int64_t> seen;                                     // columns this session handled
    std::unordered_map<int64_t, bool> seenIndex;

    std::vector<JournalLine> journal;
    uint32_t journalSeq = 0;
    size_t editsConsumed = 0;   // pendingEdits() entries already journalled from the current list

    std::unordered_map<int64_t, std::pair<uint32_t, uint32_t>> diskInfo;   // key -> (bytes, crc)
    size_t diskBytes = 0;
    int writesSinceIndex = 0;

    std::shared_ptr<std::atomic<int>> failures = std::make_shared<std::atomic<int>>(0);
    std::atomic<int> badReads{0};   // rejected column files, main thread only
    uint64_t writeId = 0;
};

std::mutex g_mutex;
std::unordered_map<const VoxelWorld*, std::shared_ptr<SaveState>> g_states;

std::shared_ptr<SaveState> stateFor(const VoxelWorld* world) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_states.find(world);
    return it == g_states.end() ? std::shared_ptr<SaveState>() : it->second;
}

// ---------------------------------------------------------------- cache (lock held)

void cachePut(SaveState& st, int64_t key, const std::vector<uint8_t>& payload) {
    auto it = st.lruIndex.find(key);
    if (it != st.lruIndex.end()) {
        st.lruBytes -= it->second->payload.size();
        it->second->payload = payload;
        st.lruBytes += payload.size();
        st.lru.splice(st.lru.begin(), st.lru, it->second);
    } else {
        st.lru.push_front(CacheEntry{ key, payload });
        st.lruIndex[key] = st.lru.begin();
        st.lruBytes += payload.size();
    }
    while (st.lru.size() > MAX_CACHE_COLUMNS || st.lruBytes > MAX_CACHE_BYTES) {
        if (st.lru.size() <= 1) break;
        st.lruBytes -= st.lru.back().payload.size();
        st.lruIndex.erase(st.lru.back().key);
        st.lru.pop_back();
    }
}

bool cacheGet(SaveState& st, int64_t key, std::vector<uint8_t>& out) {
    auto it = st.lruIndex.find(key);
    if (it == st.lruIndex.end()) return false;
    st.lru.splice(st.lru.begin(), st.lru, it->second);   // touch
    out = it->second->payload;
    return true;
}

// ---------------------------------------------------------------- journal (lock held)

// The journal is the durability floor: a column is rewritten only when it is dropped or on the
// periodic flush, so every edit is appended here first and a crash costs at most one window.
void journalAppendNew(SaveState& st, const std::vector<BlockEdit>& edits, size_t first) {
    if (edits.size() <= first) return;
    std::string text;
    for (size_t i = first; i < edits.size(); i++) {
        const BlockEdit& e = edits[i];
        JournalLine l;
        l.x = e.x; l.y = e.y; l.z = e.z; l.id = e.id;
        l.cx = e.x >> 4; l.cz = e.z >> 4;
        l.key = chunkKey(l.cx, l.cz);
        l.seq = st.journalSeq++;
        st.journal.push_back(l);
        text += std::to_string(l.cx); text += ' ';
        text += std::to_string(l.cz); text += ' ';
        text += std::to_string(l.x); text += ' ';
        text += std::to_string(l.y); text += ' ';
        text += std::to_string(l.z); text += ' ';
        text += std::to_string(l.id); text += '\n';
    }
    // binary, like every other file this module owns: text mode turns each '\n' into CRLF here, while
    // the rewrite path (journalDropCovered) goes through the binary writer - one log would then carry
    // two line endings, and a byte comparison of a journal line would depend on who wrote it last.
    std::ofstream out((st.dir + "/" + JOURNAL_NAME).c_str(), std::ios::app | std::ios::binary);
    if (!out) return;
    out << text;
}

void journalDropCovered(SaveState& st, int64_t key, uint32_t seq) {
    if (st.journal.empty()) return;
    std::vector<JournalLine> keep;
    keep.reserve(st.journal.size());
    size_t dropped = 0;
    for (const JournalLine& l : st.journal) {
        if (l.key == key && l.seq <= seq) { dropped++; continue; }
        keep.push_back(l);
    }
    if (dropped == 0) return;
    st.journal.swap(keep);
    // Rewrite rather than append: a full column on disk supersedes its lines.
    std::string text;
    for (const JournalLine& l : st.journal) {
        text += std::to_string(l.cx); text += ' ';
        text += std::to_string(l.cz); text += ' ';
        text += std::to_string(l.x); text += ' ';
        text += std::to_string(l.y); text += ' ';
        text += std::to_string(l.z); text += ' ';
        text += std::to_string(l.id); text += '\n';
    }
    std::vector<uint8_t> b(text.begin(), text.end());
    writeFileAtomic(st.dir + "/" + JOURNAL_NAME, b, "j");
}

void harvestEdits(SaveState& st, const VoxelWorld& world) {
    const std::vector<BlockEdit>& e = world.pendingEdits();
    if (e.size() < st.editsConsumed) st.editsConsumed = 0;   // the world cleared the list
    journalAppendNew(st, e, st.editsConsumed);
    st.editsConsumed = e.size();
}

// Replays edits recorded for a column that is only now coming back: they are newer than the file.
void journalApply(SaveState& st, VoxelWorld& world, int64_t key) {
    bool changed = false;
    for (const JournalLine& l : st.journal) {
        if (l.key != key) continue;
        if (world.setBlockQuiet(l.x, l.y, l.z, (uint16_t)l.id) != (uint16_t)l.id) changed = true;
    }
    if (changed) {
        Chunk* c = world.chunk(chunkKeyX(key), chunkKeyZ(key));
        if (c) c->dirtySave = true;   // the next full write folds them in and drops the lines
    }
}

// ---------------------------------------------------------------- index (lock held)

void indexWrite(SaveState& st) {
    if (st.dir.empty()) return;
    std::string text = std::to_string(SAVE_FILE_VERSION) + " " + std::to_string(st.diskInfo.size()) + "\n";
    for (const auto& kv : st.diskInfo) {
        text += std::to_string(chunkKeyX(kv.first)) + ' ';
        text += std::to_string(chunkKeyZ(kv.first)) + ' ';
        text += std::to_string(kv.second.first) + ' ';
        text += std::to_string(kv.second.second) + '\n';
    }
    std::vector<uint8_t> b(text.begin(), text.end());
    writeFileAtomic(st.dir + "/" + INDEX_NAME, b, "i");
}

// ---------------------------------------------------------------- writes (lock held)

void issueWrite(SaveState& st, int64_t key, int cx, int cz, uint32_t seq, std::vector<uint8_t> payload) {
    const uint32_t crc = crc32(payload.data(), payload.size());
    auto bytes = std::make_shared<std::vector<uint8_t>>(buildFileBytes(payload, crc));
    auto status = std::make_shared<std::atomic<int>>(0);

    PendingWrite w;
    w.key = key; w.cx = cx; w.cz = cz; w.seq = seq;
    w.bytes = (uint32_t)payload.size(); w.crc = crc; w.status = status;

    const std::string path = columnPath(st.dir, cx, cz);
    const std::string tag = std::to_string(st.writeId++);
    std::shared_ptr<std::atomic<int>> failures = st.failures;

    // The worker sees only these bytes, one path and two atomics: no Scene, no ECS, no world.
    w.fut = quantum::ThreadPoolManager::getInstance().enqueue(
        [bytes, status, failures, path, tag]() {
            if (!writeFileAtomic(path, *bytes, tag)) {
                failures->fetch_add(1);
                status->store(-1);
                return;
            }
            status->store(1);
        });

    st.pending.push_back(std::move(w));
}

bool queueChunkSave(SaveState& st, const Chunk& c) {
    if (st.dir.empty() || !st.dirOk) return false;
    const int64_t key = chunkKey(c.cx, c.cz);
    std::vector<uint8_t> payload = c.serialize();
    cachePut(st, key, payload);   // keep the fresh bytes hot: a quick re-entry costs no read
    if (st.seenIndex.find(key) == st.seenIndex.end()) { st.seenIndex[key] = true; st.seen.push_back(key); }
    const uint32_t seq = st.journalSeq > 0 ? st.journalSeq - 1 : 0;

    for (const PendingWrite& w : st.pending) {
        if (w.key != key || w.status->load() != 0) continue;
        // A write for this column is still running: park the newer snapshot and land it after, so
        // an older payload can never be renamed over a newer one.
        st.deferred[key] = std::move(payload);
        return true;
    }
    issueWrite(st, key, c.cx, c.cz, seq, std::move(payload));
    return true;
}

// Collects finished writes: refreshes the index, drops the journal lines the snapshot covers, and
// releases whatever newer snapshot waited on that column.
void reapWrites(SaveState& st) {
    for (size_t i = 0; i < st.pending.size(); ) {
        const int64_t key = st.pending[i].key;
        const int cx = st.pending[i].cx;
        const int cz = st.pending[i].cz;
        const uint32_t seq = st.pending[i].seq;
        const uint32_t bytes = st.pending[i].bytes;
        const uint32_t crc = st.pending[i].crc;
        const int status = st.pending[i].status->load();

        if (status == 0) { i++; continue; }

        if (status > 0) {
            auto old = st.diskInfo.find(key);
            if (old != st.diskInfo.end()) st.diskBytes -= old->second.first;
            st.diskInfo[key] = std::make_pair(bytes, crc);
            st.diskBytes += bytes;
            journalDropCovered(st, key, seq);
            if (++st.writesSinceIndex >= INDEX_EVERY_WRITES) { indexWrite(st); st.writesSinceIndex = 0; }
        }

        auto def = st.deferred.find(key);
        if (def != st.deferred.end()) {
            std::vector<uint8_t> payload = std::move(def->second);
            st.deferred.erase(def);
            // The column has a newer snapshot waiting: land it now, whatever just happened. Its
            // seq is taken fresh, so only edits up to this point are considered covered.
            issueWrite(st, key, cx, cz, st.journalSeq > 0 ? st.journalSeq - 1 : 0, std::move(payload));
        }
        st.pending.erase(st.pending.begin() + (long)i);
    }
}

// Promotes finished prefetch reads into the LRU; the worker owns the bytes until then.
void reapPrefetch(SaveState& st) {
    for (size_t i = 0; i < st.prefetch.size(); ) {
        const int s = st.prefetch[i]->status.load();
        if (s == JOB_PENDING) { i++; continue; }
        if (s == JOB_LOADED) cachePut(st, st.prefetch[i]->key, st.prefetch[i]->payload);
        st.prefetch.erase(st.prefetch.begin() + (long)i);
    }
}

void schedulePrefetch(SaveState& st, const VoxelWorld& world, int cx, int cz) {
    if (st.prefetch.size() >= MAX_PREFETCHES) reapPrefetch(st);
    if (st.prefetch.size() >= MAX_PREFETCHES) return;
    const int dx[4] = { 1, -1, 0, 0 };
    const int dz[4] = { 0, 0, 1, -1 };
    for (int n = 0; n < 4; n++) {
        const int ncx = cx + dx[n], ncz = cz + dz[n];
        if (world.loaded(ncx, ncz)) continue;   // already in the world, nothing to warm
        const int64_t key = chunkKey(ncx, ncz);
        if (st.lruIndex.find(key) != st.lruIndex.end()) continue;
        bool busy = false;
        for (const std::shared_ptr<PrefetchJob>& j : st.prefetch)
            if (j->key == key) { busy = true; break; }
        if (busy) continue;
        auto job = std::make_shared<PrefetchJob>();
        job->key = key;
        const std::string path = columnPath(st.dir, ncx, ncz);
        const int pcx = ncx, pcz = ncz;
        // Reads only: the worker fills its own job and the main thread moves it into the cache.
        job->fut = quantum::ThreadPoolManager::getInstance().enqueue([job, path, pcx, pcz]() {
            const int r = readColumnFile(path, pcx, pcz, job->payload);
            job->status.store(r == READ_OK ? JOB_LOADED : JOB_FAILED);
        });
        st.prefetch.push_back(std::move(job));
        if (st.prefetch.size() >= MAX_PREFETCHES) return;
    }
}

// ---------------------------------------------------------------- hooks

bool hookLoad(VoxelWorld& world, const std::string& dir, int cx, int cz, Chunk& out) {
    (void)dir;
    std::shared_ptr<SaveState> holder = stateFor(&world);
    if (!holder) return false;
    SaveState& st = *holder;
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.dir.empty() || !st.dirOk) return false;

    harvestEdits(st, world);
    reapWrites(st);
    reapPrefetch(st);

    const int64_t key = chunkKey(cx, cz);
    // A column the session was asked about is a column the quit flush has to cover. The world calls
    // this hook once per column, before it generates one, so on a new world every column arrives here
    // as "nothing stored" - and if only a successful read counted as seen, an edited column of a new
    // world would never be written, only logged.
    if (st.seenIndex.find(key) == st.seenIndex.end()) { st.seenIndex[key] = true; st.seen.push_back(key); }

    std::vector<uint8_t> payload;
    const bool cached = cacheGet(st, key, payload);
    if (!cached) {
        const int r = readColumnFile(columnPath(st.dir, cx, cz), cx, cz, payload);
        if (r == READ_BAD) st.badReads.fetch_add(1);   // counted, and never trusted
        if (r != READ_OK) {
            if (r == READ_ABSENT) schedulePrefetch(st, world, cx, cz);
            return false;   // the world generates the column instead
        }
    }
    if (!out.deserialize(payload.data(), payload.size())) {
        st.badReads.fetch_add(1);
        return false;   // CRC passed but the body is not a chunk this build can read
    }
    if (!cached) cachePut(st, key, payload);   // only validated bytes occupy the cache
    schedulePrefetch(st, world, cx, cz);
    journalApply(st, world, key);
    return true;
}

bool hookSave(VoxelWorld& world, const std::string& dir, const Chunk& c) {
    (void)dir;
    std::shared_ptr<SaveState> holder = stateFor(&world);
    if (!holder) return false;
    SaveState& st = *holder;
    std::lock_guard<std::mutex> lk(st.mu);
    harvestEdits(st, world);
    reapWrites(st);
    reapPrefetch(st);
    return queueChunkSave(st, c);   // never blocks: the bytes go to the pool and we return
}

// ---------------------------------------------------------------- install helpers

void loadJournalFromDisk(SaveState& st) {
    std::ifstream in((st.dir + "/" + JOURNAL_NAME).c_str(), std::ios::binary | std::ios::ate);
    if (!in) return;
    const std::streamoff size = in.tellg();
    if (size <= 0 || (uint64_t)size > MAX_FILE_BYTES) return;
    in.seekg(0, std::ios::beg);
    std::string text;
    text.resize((size_t)size);
    in.read((char*)text.data(), (std::streamsize)text.size());
    in.close();

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        int v[6];
        if (parseInts(text.c_str() + pos, text.c_str() + eol, v, 6)) {
            JournalLine l;
            l.cx = v[0]; l.cz = v[1]; l.x = v[2]; l.y = v[3]; l.z = v[4];
            l.id = (uint32_t)(uint16_t)v[5];
            l.key = chunkKey(l.cx, l.cz);
            l.seq = st.journalSeq++;
            st.journal.push_back(l);
        }
        pos = eol + 1;
    }
}

// One pass over dir/ so the debug overlay has real numbers before the first write lands.
void scanExisting(SaveState& st) {
    std::error_code ec;
    std::filesystem::directory_iterator it(st.dir, ec);
    if (ec) return;
    for (const std::filesystem::directory_entry& e : it) {
        std::error_code fec;
        if (!e.is_regular_file(fec)) continue;
        const std::string name = e.path().filename().string();
        int cx = 0, cz = 0;
        if (!parseColumnName(name, cx, cz)) continue;
        const uintmax_t size = std::filesystem::file_size(e.path(), fec);
        if (fec) continue;
        const int64_t key = chunkKey(cx, cz);
        st.diskInfo[key] = std::make_pair((uint32_t)size, (uint32_t)0);
        st.diskBytes += (size_t)size;
    }
}

// Waits for every outstanding write of a state. Callers hold st.mu; workers never take it, so
// waiting here cannot deadlock, and nothing is read back through a future.
void drainWrites(SaveState& st) {
    for (int pass = 0; pass < 4096; pass++) {
        if (!st.deferred.empty()) {
            std::unordered_map<int64_t, std::vector<uint8_t>> queued;
            queued.swap(st.deferred);
            for (auto& kv : queued)
                issueWrite(st, kv.first, chunkKeyX(kv.first), chunkKeyZ(kv.first),
                           st.journalSeq > 0 ? st.journalSeq - 1 : 0, std::move(kv.second));
        }
        if (st.pending.empty()) break;
        for (PendingWrite& w : st.pending) {
            if (w.fut.valid()) w.fut.wait();
            // A satisfied future whose status is still 0 means the task never ran (pool torn down).
            if (w.status->load() == 0) { st.failures->fetch_add(1); w.status->store(-1); }
        }
        reapWrites(st);
    }
    st.deferred.clear();   // only reachable after the pass cap, which a healthy pool never hits
}

}  // namespace
}  // namespace voxel

// VoxelSave.h declares the entry points at global scope, so the module internals come into view
// here rather than the other way round.
using namespace voxel;

// ---------------------------------------------------------------- public API

void installVoxelSave(voxel::VoxelWorld& world, const std::string& dir) {
    const std::string path = trimDir(dir);
    if (path.empty()) {
        world.saveDir.clear();
        world.loadChunk = nullptr;
        world.saveChunk = nullptr;
        return;
    }

    auto st = std::make_shared<SaveState>();
    st->dir = path;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    st->dirOk = std::filesystem::is_directory(path, ec);   // no throw overload, failure is recorded

    if (st->dirOk) {
        scanExisting(*st);
        loadJournalFromDisk(*st);
    }

    std::shared_ptr<SaveState> previous;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_states.find(&world);
        if (it != g_states.end()) previous = it->second;
        g_states[&world] = st;
    }
    if (previous) {   // an earlier cache for this world: land its writes before dropping it
        std::lock_guard<std::mutex> lk(previous->mu);
        drainWrites(*previous);
        indexWrite(*previous);
    }

    world.saveDir = path;
    world.loadChunk = [](VoxelWorld& w, const std::string& d, int cx, int cz, Chunk& c) {
        return hookLoad(w, d, cx, cz, c);
    };
    world.saveChunk = [](VoxelWorld& w, const std::string& d, const Chunk& c) {
        return hookSave(w, d, c);
    };
}

void flushVoxelSave(voxel::VoxelWorld& world) {
    std::shared_ptr<SaveState> holder = stateFor(&world);
    if (!holder) return;
    SaveState& st = *holder;
    std::lock_guard<std::mutex> lk(st.mu);
    if (!st.dirOk) return;

    harvestEdits(st, world);
    reapWrites(st);

    // Every column this module has seen and that still holds edits gets a full snapshot.
    for (int64_t key : st.seen) {
        Chunk* c = world.chunk(chunkKeyX(key), chunkKeyZ(key));
        if (c && c->generated && c->dirtySave) {
            if (queueChunkSave(st, *c)) c->dirtySave = false;
        }
    }

    drainWrites(st);
    indexWrite(st);
}

std::string saveStats(const voxel::VoxelWorld& world) {
    std::shared_ptr<SaveState> holder = stateFor(&world);
    if (!holder) return std::string("save: off");
    SaveState& st = *holder;
    std::lock_guard<std::mutex> lk(st.mu);
    std::string s = "save: ";
    s += std::to_string(st.diskInfo.size());
    s += " chunks, ";
    s += std::to_string(st.diskBytes / 1024u);
    s += " KB, ";
    s += std::to_string(st.pending.size() + st.deferred.size());
    s += " pending, ";
    s += std::to_string((int64_t)st.failures->load());
    s += " failed";
    if (st.badReads.load() > 0) { s += ", "; s += std::to_string(st.badReads.load()); s += " bad files"; }
    if (!st.journal.empty()) { s += ", "; s += std::to_string(st.journal.size()); s += " edits logged"; }
    if (!st.dirOk) s += ", dir unavailable";
    return s;
}

// ---------------------------------------------------------------- probe

namespace {

bool slurpBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path.c_str(), std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    if (size <= 0 || (uint64_t)size > 8u * 1024u * 1024u) return false;
    in.seekg(0, std::ios::beg);
    out.assign((size_t)size, 0);
    in.read((char*)out.data(), (std::streamsize)size);
    return (bool)in;
}

bool slurpText(const std::string& path, std::string& out) {
    std::vector<uint8_t> b;
    if (!slurpBytes(path, b)) return false;
    out.assign(b.begin(), b.end());
    return true;
}

// Deliberately NOT writeFileAtomic(): the probe plants files by hand, because a file the probe wrote
// through the module's own writer proves nothing about reading one it did not.
void plantBytes(const std::string& path, const uint8_t* d, size_t n) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (n) out.write((const char*)d, (std::streamsize)n);
}

}  // namespace

void probeVoxelSave(voxel::VoxelWorld& world, voxel::Probe& probe) {
    ProbeModule mod(probe, "save");
    if (!world.loadChunk || !world.saveChunk || world.saveDir.empty()) {
        probe.skip("no save directory is installed, so there is nothing to write");
        return;
    }
    const int cx = world.focusChunkX(), cz = world.focusChunkZ();
    Chunk* live = world.chunk(cx, cz);
    const int top = live ? world.getTop(cx * CHUNK_SX + 5, cz * CHUNK_SZ + 5) : -1;
    if (!live || !live->generated || top < 2) {
        probe.skip("the focus column has not streamed, so there is no terrain to save");
        return;
    }
    std::error_code ec;
    std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
    if (ec || tmp.empty()) {
        probe.skip("the OS temp directory is not reachable");
        return;
    }
    // A probe-owned scratch directory under the OS temp path, never a project directory: it is the
    // place where deliberately damaged column files can be planted, and it is wiped on entry so a
    // run measures a cold start rather than the previous run's debris.
    std::string dir = (tmp / "quantum-voxel-save-probe").string();
    for (char& ch : dir) if (ch == '\\') ch = '/';
    std::filesystem::remove_all(dir, ec);

    const std::string original = world.saveDir;
    // The other probes write loudly too - a pick and a pour have to, or the water sim and the net
    // never see them - and their edits are still sitting in the world's list. The load hook harvests
    // that list into the journal and replays it onto whatever column it reads, so this probe starts
    // by dropping it: otherwise the scaffolding another probe already restored would come back.
    world.clearPendingEdits();
    installVoxelSave(world, dir);

    const int x1 = cx * CHUNK_SX + 5, y1 = top, z1 = cz * CHUNK_SZ + 5;
    const int x2 = x1 + 1, y2 = top, z2 = z1 + 1;
    auto otherThan = [](uint16_t cur) -> uint16_t {
        return cur == BLOCK_GLOWSTONE ? (uint16_t)BLOCK_STONE : (uint16_t)BLOCK_GLOWSTONE;
    };

    // The streaming code asks the load hook for a column exactly once, before it generates one, and
    // this fresh state has never been asked about the focus column (the world streamed it under the
    // directory being replaced). Ask now and take the "nothing stored" answer.
    {
        Chunk warm;
        world.loadChunk(world, world.saveDir, cx, cz, warm);
    }

    VoxelPatch patch(world);
    patch.remember(x1, y1, z1);
    const uint16_t was1 = world.get(x1, y1, z1);
    const uint16_t to1 = otherThan(was1);
    world.setBlock(x1, y1, z1, to1);   // loud, the way a player's pick is loud
    flushVoxelSave(world);

    const std::string vck = dir + "/" + std::to_string(cx) + "_" + std::to_string(cz) + ".vck";
    std::vector<uint8_t> good;
    const bool planted = slurpBytes(vck, good);
    const size_t payloadBytes = live->serialize().size();
    const std::string stats1 = saveStats(world);
    probe.check("a quit flush writes the edited column to a file named after its coords",
                planted && good.size() == payloadBytes + 8 &&
                stats1.find("save: 1 chunks") != std::string::npos,
                "%s, %zu file bytes for a %zu byte payload (version + CRC frame the body), %s",
                planted ? "present" : "MISSING", good.size(), payloadBytes, stats1.c_str());

    installVoxelSave(world, dir);   // cold cache: the bytes below must come off the disk
    Chunk fresh;
    const bool back = world.loadChunk(world, world.saveDir, cx, cz, fresh);
    probe.check("a column read back off the disk is byte-identical to the one that was written",
                back && fresh.cx == cx && fresh.cz == cz && fresh.generated &&
                fresh.blocks == live->blocks && fresh.light == live->light &&
                fresh.height == live->height && fresh.biome == live->biome,
                "loaded %d, coords %d,%d for %d,%d, blocks %d light %d height %d biome %d equal",
                (int)back, fresh.cx, fresh.cz, cx, cz, (int)(fresh.blocks == live->blocks),
                (int)(fresh.light == live->light), (int)(fresh.height == live->height),
                (int)(fresh.biome == live->biome));

    // Every byte flip below goes through this helper. Indexing the file the probe expected is a
    // range check if the module never wrote one, and a hung "Debug Error!" dialog is not a result -
    // an unchecked empty vector means the module produced nothing, and each case must then FAIL.
    auto flipped = [&](size_t at, uint8_t mask) {
        std::vector<uint8_t> d = good;
        if (!d.empty() && at < d.size()) d[at] ^= mask;
        return d;
    };
    // Past the 4-byte version header, so inside the body the CRC covers, and a third of the way in
    // because a real column file is far larger than that.
    const size_t payloadFlip = good.size() > 2048 ? good.size() / 3 : 0;

    // Planted, not written: the column is still cached, so this file must be ignored rather than read.
    const std::vector<uint8_t> broken = flipped(payloadFlip, 0xFF);
    plantBytes(vck, broken.data(), broken.size());
    Chunk hot;
    const bool fromCache = world.loadChunk(world, world.saveDir, cx, cz, hot);
    probe.check("a column still in the cache is answered without opening its file",
                fromCache && hot.blocks == live->blocks &&
                saveStats(world).find("bad files") == std::string::npos,
                "loaded %d, blocks equal %d, %s", (int)fromCache, (int)(hot.blocks == live->blocks),
                saveStats(world).c_str());
    plantBytes(vck, good.data(), good.size());

    std::string index;
    const bool hasIndex = slurpText(dir + "/index.txt", index);
    const std::string row = std::to_string(cx) + " " + std::to_string(cz) + " ";
    probe.check("index.txt carries the format version, the count and that column",
                hasIndex && index.size() >= 4 && index.compare(0, 4, "1 1\n") == 0 &&
                index.find(row) != std::string::npos,
                "%s, header %.4s, %zu rows for the one column on disk",
                hasIndex ? "read" : "UNREADABLE", index.c_str(),
                index.empty() ? (size_t)0 : (size_t)std::count(index.begin(), index.end(), '\n') - 1);

    Chunk nowhere;
    const bool absent = world.loadChunk(world, world.saveDir, cx + 900, cz + 900, nowhere);
    probe.check("a column nobody ever stored answers absent, and is not counted as a bad file",
                !absent && !nowhere.generated && nowhere.blocks.size() == (size_t)CHUNK_VOLUME &&
                nowhere.blocks[0] == 0 && saveStats(world).find("bad files") == std::string::npos,
                "load said %d, chunk generated %d, %s", (int)absent, (int)nowhere.generated,
                saveStats(world).c_str());

    struct Damage { bool refused; bool counted; size_t leftBlocks; bool leftEmpty; };
    // Every damage case gets its own install: the cache must be cold and the bad-file counter must
    // start at zero, or "1 bad files" would say nothing about the file just planted.
    auto damage = [&](const std::string& file, const std::vector<uint8_t>& bytes,
                      int rcx, int rcz) -> Damage {
        plantBytes(file, bytes.data(), bytes.size());
        installVoxelSave(world, dir);
        Chunk c;
        const bool ok = world.loadChunk(world, world.saveDir, rcx, rcz, c);
        size_t nonzero = 0;
        for (size_t i = 0; i < c.blocks.size(); i++) if (c.blocks[i]) nonzero++;
        return Damage{ !ok, saveStats(world).find(", 1 bad files") != std::string::npos,
                       nonzero, !c.generated && nonzero == 0 };
    };

    const Damage bitRot = damage(vck, broken, cx, cz);
    probe.check("one flipped payload byte is refused by the CRC instead of loaded",
                bitRot.refused && bitRot.counted, "refused %d, counted %d, %s",
                (int)bitRot.refused, (int)bitRot.counted, saveStats(world).c_str());
    probe.check("a refused read leaves the column empty so the world generates it",
                bitRot.leftEmpty, "generated %d, %zu of %d blocks filled",
                (int)!bitRot.leftEmpty, bitRot.leftBlocks, CHUNK_VOLUME);

    const std::vector<uint8_t> tornBytes(good.begin(), good.begin() + (long)(good.size() / 2));
    const Damage torn = damage(vck, tornBytes, cx, cz);
    probe.check("a truncated column file is refused", torn.refused && torn.counted,
                "%zu of %zu bytes, refused %d, counted %d", tornBytes.size(), good.size(),
                (int)torn.refused, (int)torn.counted);

    // The version header sits outside the CRC, so this file is otherwise perfectly valid: only the
    // version check can refuse it.
    const std::vector<uint8_t> foreignBytes = flipped(0, 0x02);
    const Damage foreign = damage(vck, foreignBytes, cx, cz);
    probe.check("a file from another format version is refused even though its CRC passes",
                foreign.refused && foreign.counted, "version byte %u, refused %d, counted %d",
                foreignBytes.empty() ? 0u : (unsigned)foreignBytes[0],
                (int)foreign.refused, (int)foreign.counted);
    plantBytes(vck, good.data(), good.size());

    const std::string misplaced = dir + "/" + std::to_string(cx + 3) + "_" + std::to_string(cz) + ".vck";
    const Damage mislabeled = damage(misplaced, good, cx + 3, cz);
    probe.check("a file whose own stored coords disagree with its name is refused",
                mislabeled.refused && mislabeled.counted, "refused %d, counted %d, %s",
                (int)mislabeled.refused, (int)mislabeled.counted, saveStats(world).c_str());
    std::filesystem::remove(misplaced, ec);
    installVoxelSave(world, dir);

    // The journal is the other half of durability, and it can only be measured with a directory
    // whose log holds exactly one line: the one edit this section makes.
    std::filesystem::remove(dir + "/edits.log", ec);
    world.clearPendingEdits();   // so the harvest below cannot re-log an edit from earlier in the probe
    installVoxelSave(world, dir);
    patch.remember(x2, y2, z2);
    const uint16_t was2 = world.get(x2, y2, z2);
    const uint16_t to2 = otherThan(was2);
    world.setBlock(x2, y2, z2, to2);
    {
        // Nothing saves this column now - the log alone has to carry the edit. Asking for a far
        // column nobody stored runs the load hook, and the load hook is what harvests the edit list.
        Chunk nothing;
        world.loadChunk(world, world.saveDir, cx + 901, cz + 901, nothing);
    }
    std::string journal;
    slurpText(dir + "/edits.log", journal);
    const std::string line = std::to_string(cx) + " " + std::to_string(cz) + " " +
                             std::to_string(x2) + " " + std::to_string(y2) + " " +
                             std::to_string(z2) + " " + std::to_string((unsigned)to2) + "\n";
    probe.check("an edit no file has folded in is logged as <cx cz x y z id>", journal == line,
                "log holds %zu bytes, expected %.160s, got %.160s", journal.size(), line.c_str(),
                journal.c_str());
    probe.check("the overlay counts the journal it just wrote",
                saveStats(world).find(", 1 edits logged") != std::string::npos, "%s",
                saveStats(world).c_str());

    // Take the edit out of the live world quietly, so the undo is not itself journalled. The file on
    // disk predates the edit and the world no longer holds it: only the log line does.
    world.setBlockQuiet(x2, y2, z2, was2);
    installVoxelSave(world, dir);
    Chunk cold;
    const bool reloaded = world.loadChunk(world, world.saveDir, cx, cz, cold);
    // The replay goes through the world because that is the chunk the streaming code is filling (the
    // map's own object, which `live` points at) - a probe that hands loadChunk a throwaway column
    // reads the folded-in result back out of the world, not out of that column.
    probe.check("a cold re-read replays the journal, so the edit survives although no file held it",
                reloaded && world.get(x2, y2, z2) == to2,
                "loaded %d, cell is %d, the log promised %d, the file held %d",
                (int)reloaded, (int)world.get(x2, y2, z2), (int)to2, (int)was2);
    probe.check("a replayed column is marked dirty so the next write folds it in and drops its lines",
                live->dirtySave, "dirtySave %d, %s", (int)live->dirtySave, saveStats(world).c_str());

    world.clearPendingEdits();   // a probe edit must never reach the player's journal
    patch.restore();
    installVoxelSave(world, original);
    probe.check("the probe leaves the real save directory and no scaffolding behind",
                world.saveDir == original && world.get(x1, y1, z1) == was1 &&
                world.get(x2, y2, z2) == was2,
                "dir %s, cell one %d was %d, cell two %d was %d", world.saveDir.c_str(),
                (int)world.get(x1, y1, z1), (int)was1, (int)world.get(x2, y2, z2), (int)was2);
}

