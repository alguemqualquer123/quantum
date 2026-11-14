// (c) SR VINIX
// SPDX-License-Identifier: MIT

// VoxelProbe.h - the behavioural self-test harness every voxel module writes into.
//
// "It compiles and the picture looks right" is not the claim any module makes, so no module is
// finished until a headless run shows it changing the world the way its own header says it does:
// a block placed, water spread, a column lit, a chunk saved and read back. Each module exports
// probeX(VoxelWorld&, Probe&) and the host runs every one of them on VOXEL_PROBE=<frame>, which
// prints one line per check and a per-module tally, and restores whatever it touched.
//
// Header-only on purpose: the project's SCRIPT_SOURCES list is generated, so a new .cpp means
// regenerating CMakeLists.txt, and a harness this small is not worth that churn.

#pragma once

#include "Log.h"
#include "VoxelWorld.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace voxel {

class VoxelWorld;

// Captures are read by eye, so a run needs a switch that turns one visual layer off without
// recompiling: the only way to tell whether a shape on screen is the crack or the chips that fly
// out of it is to remove one of them. std::getenv warns on MSVC, and this project builds clean.
inline int voxelEnvInt(const char* name, int fallback) {
#ifdef _MSC_VER
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) return fallback;
    const int v = (int)std::strtol(buffer, nullptr, 10);
    free(buffer);
    return v;
#else
    const char* v = std::getenv(name);
    return v ? (int)std::strtol(v, nullptr, 10) : fallback;
#endif
}

struct Probe {
    int passed = 0;
    int failed = 0;
    const char* moduleName = "";
    int modulePassed = 0;
    int moduleFailed = 0;

    void begin(const char* name) {
        moduleName = name;
        modulePassed = 0;
        moduleFailed = 0;
    }

    // `what` is the claim the module's header makes, in the words a reader would search for. The
    // detail is measured state, not a restatement of the boolean: a FAIL line has to be enough to
    // diagnose the run without rerunning it under a debugger that does not exist headless.
    void check(const char* what, bool ok, const char* fmt, ...) {
        char detail[256];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(detail, sizeof(detail), fmt, args);
        va_end(args);

        quantum::Log::print("PROBE %s  %s  %s", ok ? "PASS" : "FAIL", what, detail);
        // Log::print goes to buffered stdout and a process that dies on an abort never flushes it, so
        // a crash in a later module would erase every verdict this one just printed.
        std::fflush(stdout);
        if (ok) {
            passed++; modulePassed++;
        } else {
            failed++; moduleFailed++;
        }
    }

    // Not a failure: the world has not streamed far enough for this module to have anything to
    // test. It prints because a silent skip is how a probe ends up testing nothing.
    void skip(const char* why) {
        quantum::Log::print("PROBE SKIP  %s  %s", moduleName, why);
        std::fflush(stdout);
    }

    void end() {
        quantum::Log::print("PROBE %s: %d passed, %d failed", moduleName, modulePassed, moduleFailed);
        std::fflush(stdout);
    }

    // Prints the grand total the grep for the CI-style verdict looks for.
    void finish() const {
        quantum::Log::print("PROBE TOTAL: %d passed, %d failed", passed, failed);
        std::fflush(stdout);
    }
};

// Opens a module's section and closes it on scope exit, so an early return for a column that has
// not streamed still reports the module rather than vanishing from the log.
struct ProbeModule {
    Probe& probe;
    explicit ProbeModule(Probe& p, const char* name) : probe(p) { probe.begin(name); }
    ~ProbeModule() { probe.end(); }

    ProbeModule(const ProbeModule&) = delete;
    ProbeModule& operator=(const ProbeModule&) = delete;
};

// A probe tests against the live world, so everything it writes is remembered on the first touch
// and put back when the patch dies - including on the early return of a module that found nothing
// to test. Writes go through setBlockQuiet() deliberately: the loud setBlock() records the edit for
// the save journal and the net broadcast, so a probe would leak its scaffolding into a saved game
// and show every connected client a block that was never meant to exist.
class VoxelPatch {
public:
    explicit VoxelPatch(VoxelWorld& w) : world(w) {}
    ~VoxelPatch() { restore(); }

    void set(int x, int y, int z, uint16_t id) {
        // The original is remembered whether or not this write changes anything: a cell the arena
        // carved to air needs a record all the same, because the module under test - or the water
        // it spawns - may write that cell itself later, and a restore that has nothing to put back
        // would leave the probe's scaffolding in the world.
        remember(x, y, z);
        if (world.get(x, y, z) == id) return;
        world.setBlockQuiet(x, y, z, id);
    }

    // Record the current value of a cell the probe is about to hand to a module that writes loudly
    // (a pour goes through setBlock so it reaches the water simulator's journal).
    void remember(int x, int y, int z) {
        for (const Entry& e : entries) {
            if (e.x == x && e.y == y && e.z == z) return;
        }
        entries.push_back(Entry{ x, y, z, world.get(x, y, z) });
    }

    uint16_t get(int x, int y, int z) const { return world.get(x, y, z); }

    void restore() {
        // Reverse order: a voxel written twice keeps its original id in the first entry, and the
        // later writes to the same cell are no-ops, but unwinding in order is what makes that true.
        for (size_t i = entries.size(); i-- > 0;)
            world.setBlockQuiet(entries[i].x, entries[i].y, entries[i].z, entries[i].id);
        entries.clear();
    }

    size_t touched() const { return entries.size(); }

private:
    struct Entry { int x, y, z; uint16_t id; };
    VoxelWorld& world;
    std::vector<Entry> entries;
};

// A flat empty room carved out of the streamed world, with a stone floor to stand on: the shape a
// collision or liquid test needs is "air I control", and terrain that happens to be air is not
// that. Returns false when the column has not streamed, so the caller can skip rather than measure
// the void. Re-carving over an existing arena is free - a write of the id already there is dropped.
inline bool carveArena(VoxelWorld& world, VoxelPatch& patch, int& ax, int& ay, int& az,
                       int len = 7, int wide = 3, int high = 4) {
    const int cx = world.focusChunkX() * CHUNK_SX + 4;
    const int cz = world.focusChunkZ() * CHUNK_SZ + 4;
    const int top = world.getTop(cx, cz);
    if (top < 2 || top + high + 2 >= WORLD_HEIGHT) return false;
    ax = cx; ay = top + 1; az = cz;
    for (int x = 0; x < len; x++) {
        for (int z = 0; z < wide; z++) {
            patch.set(ax + x, ay - 1, az + z, BLOCK_STONE);
            for (int y = 0; y < high; y++) patch.set(ax + x, ay + y, az + z, BLOCK_AIR);
        }
    }
    return true;
}

}  // namespace voxel
