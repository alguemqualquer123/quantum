// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelWater.h"

#include "VoxelConfig.h"
#include "VoxelBlocks.h"
#include "VoxelChunk.h"
#include "VoxelProbe.h"
#include "VoxelWorld.h"

#include "Noise.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace voxel {
namespace {

// A voxel of liquid is one of the two liquid types plus a fill level in the low state bits:
// makeState(BLOCK_WATER, level), level 0..15. The mesher lowers the drawn surface with
// `int level = (id >> BLOCK_TYPE_BITS) & 15`, so the level has to live in exactly those bits.
const int MAX_LEVEL = 15;
const uint16_t WATER_SRC = makeState(BLOCK_WATER, MAX_LEVEL);
const uint16_t LAVA_SRC = makeState(BLOCK_LAVA, MAX_LEVEL);
const uint16_t WATER_FLAT = makeState(BLOCK_WATER, 0);
const uint16_t LAVA_FLAT = makeState(BLOCK_LAVA, 0);

// Discrete steps: the tick accumulator makes the flow frame-rate independent, and the two speeds
// are what a player reads as "water runs, lava crawls".
const uint32_t WATER_STEP_MS = 250;
const uint32_t LAVA_STEP_MS = 1000;
const int MAX_CELLS_PER_STEP = 4000;

// How many cells a stream crosses from the source feeding it. The spread distance is the only cap
// on the volume a pour can create, which is what keeps a bucket from flooding the map.
const int WATER_SPREAD = 7;
const int LAVA_SPREAD = 3;

inline uint8_t familyOf(uint16_t id) {
    const uint16_t t = stateType(id);
    if (t == BLOCK_WATER) return 0;
    if (t == BLOCK_LAVA) return 1;
    return 2;
}

inline uint32_t flagsOf(uint16_t type) {
    if (type >= BLOCK_COUNT) return FLAG_SOLID | FLAG_OPAQUE;    // an unknown id blocks the flow
    return block(type).flags;
}

// A level with no bits set is not "no water": the generator and any old save write a bare liquid id
// for a full block, and the mesher would render it as a thin sheet. The first time we tick such a
// voxel we give it level 15, which also makes it the source the sea was meant to be. Our own writes
// never produce level 0, so this test only ever fires on someone else's block.
inline bool isFlatLiquid(uint16_t id) { return id == WATER_FLAT || id == LAVA_FLAT; }

// The level as it really is, flat id included.
inline int levelOf(uint16_t id) {
    if (isFlatLiquid(id)) return MAX_LEVEL;
    return (int)state::waterLevel(id);
}

// Something a liquid may move into: air, and the replaceable blocks that hold no volume (plants,
// snow layers, water, which lava displaces exactly as it does in Minecraft). A block that drops
// nothing is permeable too - that is what lets an engine water block be flowed through - and
// everything else, cobblestone included, is a wall.
inline bool isFree(uint16_t id) {
    const uint16_t t = stateType(id);
    if (t == BLOCK_AIR) return true;
    const uint32_t f = flagsOf(t);
    if (f & FLAG_REPLACEABLE) return true;
    return stateType(block(t).drop) == BLOCK_AIR;
}

// The block a water/lava contact makes. A source is turned into obsidian (by water) or stone (by
// lava), a stream only into cobblestone. Returns 0 when the pair does not react.
inline uint16_t contactBlockFor(bool lava, uint16_t otherId) {
    const uint16_t ot = stateType(otherId);
    if (ot != BLOCK_WATER && ot != BLOCK_LAVA) return 0;
    const bool otherSource = levelOf(otherId) == MAX_LEVEL;
    if (!lava) return otherSource ? BLOCK_OBSIDIAN : BLOCK_COBBLESTONE;   // water onto lava
    return otherSource ? BLOCK_STONE : BLOCK_COBBLESTONE;                 // lava into water
}

const int DIRS[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
const int SIDES[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

// One liquid voxel that might change. Everything the simulator remembers is a slot here: chunks stay
// plain storage, so a saved world needs no water format and an uninstalled sim leaves it untouched.
struct Cell {
    int x;
    int z;
    int16_t y;
    uint8_t family;
    uint8_t dead;
    uint32_t dueMs;

    // 37 bits and no packing trick can collide: x and z are signed columns, y is 0..127.
    static uint64_t posKey(int x, int y, int z) {
        return ((uint64_t)(uint32_t)x << 32) | ((uint64_t)(uint32_t)z << 12) | (uint64_t)(y & 0xFFF);
    }
    static uint64_t key(const Cell& c) { return posKey(c.x, (int)c.y, c.z); }
};

struct LiquidSim {
    std::vector<Cell> cells;
    std::vector<Cell> scratch;
    std::unordered_map<uint64_t, uint32_t> slots;

    double clockMs = 0.0;              // monotone ms, kept far from the uint32 wrap by rebase()
    uint32_t lastWaterStep = 0;
    uint32_t lastLavaStep = 0;
    size_t editCursor = 0;             // how far world.pendingEdits() has been read
    uint64_t steps = 0;
    uint64_t writes = 0;
    int activeNow = 0;

    LiquidSim() {
        cells.reserve(4096);
        scratch.reserve(4096);
        slots.reserve(4096);
    }

    uint32_t now() const { return (uint32_t)clockMs; }

    // Signed so the clock may wrap: the cell is due once its deadline has been reached.
    bool due(uint32_t at) const { return (int32_t)(at - now()) <= 0; }

    void rebase(uint32_t shift) {
        clockMs -= (double)shift;
        lastWaterStep -= shift;
        lastLavaStep -= shift;
        for (Cell& c : cells) c.dueMs -= shift;
    }

    void reset() {
        cells.clear();
        slots.clear();
        clockMs = 0.0;
        lastWaterStep = 0;
        lastLavaStep = 0;
        editCursor = 0;
        steps = 0;
        writes = 0;
        activeNow = 0;
    }

    bool write(VoxelWorld& world, int x, int y, int z, uint16_t id) {
        if (y < 0 || y >= WORLD_HEIGHT) return false;
        // An unloaded column is not our business yet: setBlockQuiet would drop the write on the
        // floor and the liquid only reappears there once the streaming brings the chunk back.
        const Chunk* target = world.chunk(x >> 4, z >> 4);
        if (!target || !target->generated) return false;
        if (target->get(x & 15, y, z & 15) == id) return false;
        if (world.setBlockQuiet(x, y, z, id) == id) return false;
        ++writes;
        // setBlockQuiet is quiet on purpose (a flow step must not spam the save journal), but it
        // leaves the mesh stale and does not dirty the neighbour column a new face was exposed to -
        // that is what these calls add. A voxel that did not change never reaches this, so a settled
        // sea produces no writes, no dirty flags and no re-mesh.
        const int cx = x >> 4, cz = z >> 4;
        world.requestRemesh(cx, cz);
        if ((x & 15) == 0) world.requestRemesh(cx - 1, cz);
        if ((x & 15) == 15) world.requestRemesh(cx + 1, cz);
        if ((z & 15) == 0) world.requestRemesh(cx, cz - 1);
        if ((z & 15) == 15) world.requestRemesh(cx, cz + 1);
        return true;
    }

    void schedule(const VoxelWorld& world, int x, int y, int z, uint8_t family) {
        if (y < 0 || y >= WORLD_HEIGHT) return;
        const Chunk* c = world.chunk(x >> 4, z >> 4);
        if (!c || !c->generated) return;        // nothing to simulate in a column that is not here
        const uint32_t period = family ? LAVA_STEP_MS : WATER_STEP_MS;
        const uint32_t at = now() + period;
        const uint64_t key = Cell::posKey(x, y, z);
        auto it = slots.find(key);
        if (it != slots.end()) {
            Cell& cell = cells[it->second];
            cell.dead = 0;
            if (cell.family == family) {
                if ((int32_t)(at - cell.dueMs) < 0) cell.dueMs = at;
            } else {
                cell.family = family;
                cell.dueMs = at;                // the other liquid ticks on its own clock
            }
            return;
        }
        Cell cell;
        cell.x = x; cell.z = z; cell.y = (int16_t)y;
        cell.family = family; cell.dead = 0; cell.dueMs = at;
        slots[key] = (uint32_t)cells.size();
        cells.push_back(cell);
    }

    // Schedule a voxel that may now move, plus every liquid around it that may now notice it.
    void scheduleAround(const VoxelWorld& world, int x, int y, int z, uint8_t family) {
        schedule(world, x, y, z, family);
        for (int d = 0; d < 6; d++) {
            const int nx = x + DIRS[d][0], ny = y + DIRS[d][1], nz = z + DIRS[d][2];
            const uint8_t f = familyOf(world.get(nx, ny, nz));
            if (f < 2) schedule(world, nx, ny, nz, f);
        }
    }

    // The edit journal is the only way liquid learns about the player's pickaxe, since the write
    // path has no callback: setBlock() appends to it and the save flush empties it. A cursor into it
    // is therefore only valid between flushes, so a shrinking journal restarts the read from zero.
    // schedule() dedupes and a repeated look writes nothing, so that rescan is a few lookups and can
    // never double-pour.
    void scanEdits(const VoxelWorld& world) {
        const std::vector<BlockEdit>& edits = world.pendingEdits();
        if (edits.size() < editCursor) editCursor = 0;
        for (size_t i = editCursor; i < edits.size(); i++) {
            const BlockEdit& e = edits[i];
            const uint8_t f = familyOf(e.id);
            if (f < 2) {
                scheduleAround(world, e.x, e.y, e.z, f);
            } else if (isFree(e.id) && !isFree(e.previous)) {
                // A block was taken away: whatever rested on it or leaned against it re-decides.
                for (int d = 0; d < 6; d++) {
                    const int nx = e.x + DIRS[d][0], ny = e.y + DIRS[d][1], nz = e.z + DIRS[d][2];
                    const uint8_t nf = familyOf(world.get(nx, ny, nz));
                    if (nf < 2) schedule(world, nx, ny, nz, nf);
                }
            }
        }
        editCursor = edits.size();
    }

    // Water + lava contact at a voxel holding the other liquid. `lava` is the family that moved.
    bool reactAt(VoxelWorld& world, int x, int y, int z, uint16_t otherId, bool lava) {
        const uint16_t made = contactBlockFor(lava, otherId);
        if (made == 0) return false;
        return write(world, x, y, z, (uint16_t)makeState(made, 0));
    }

    // Lava eats the block hanging above it: that is how a frozen-over lava well keeps burning
    // through the ice, and water has no such rule.
    void drainLavaRoof(VoxelWorld& world, int x, int y, int z) {
        const uint16_t up = world.get(x, y, z);
        if (familyOf(up) != 2 || isFree(up)) return;
        if (flagsOf(stateType(up)) & (FLAG_SOLID | FLAG_OPAQUE)) return;
        if (write(world, x, y, z, BLOCK_AIR)) scheduleAround(world, x, y, z, 1);
    }

    // What this voxel is worth right now: the column above falls into it for free, sideways
    // neighbours feed it at one level less, and a tail that has spent its spread budget counts as
    // nothing. Levels only ever come from a strict neighbour chain, so no cycle can raise one.
    int inflow(const VoxelWorld& world, int x, int y, int z, uint8_t fam, bool source) const {
        int best = source ? MAX_LEVEL : 0;
        const uint16_t up = world.get(x, y + 1, z);
        if (familyOf(up) == fam) {
            const int v = levelOf(up);
            const int fall = v < MAX_LEVEL - 1 ? v : MAX_LEVEL - 1;   // falling water is never a source
            if (fall > best) best = fall;
        }
        if (source) return best;
        for (int d = 0; d < 4; d++) {
            const uint16_t nb = world.get(x + SIDES[d][0], y, z + SIDES[d][1]);
            if (familyOf(nb) != fam) continue;
            const int v = levelOf(nb) - 1;
            if (v > best) best = v;
        }
        if (best < MAX_LEVEL - (fam ? LAVA_SPREAD : WATER_SPREAD)) return 0;
        return best;
    }

    // Rare on purpose: a cell only asks about one tick in sixteen, and only liquid with open sky
    // above it in a snowy column turns to ice. Freezing takes a source out of the world, so the
    // stream under it dries - a good outcome, and the reason it must not run every tick.
    void tryFreeze(VoxelWorld& world, int x, int y, int z) {
        const uint32_t seed = (uint32_t)(world.seed() >> 24);
        if (quantum::Noise::hash3(x, z, y, seed) < 1.0f / 16.0f) return;
        if (stateType(world.get(x, y + 1, z)) != BLOCK_AIR) return;
        if (world.getBiome(x, z) != BIOME_SNOWY) return;
        if (write(world, x, y, z, BLOCK_ICE)) scheduleAround(world, x, y, z, 0);
    }

    // What to do with the slot after a step: keep it, retire it, or leave it alone because the voxel
    // it points at now holds the other liquid and got a slot for that family.
    enum StepResult { STEP_KEEP = 0, STEP_RETIRE = 1, STEP_RETAGGED = 2 };

    // Returns STEP_RETIRE when the slot has nothing left to do.
    int stepCell(VoxelWorld& world, const Cell& cell) {
        const int x = cell.x, y = (int)cell.y, z = cell.z;
        const uint8_t fam = cell.family;
        const uint16_t id = world.get(x, y, z);
        const uint8_t actual = familyOf(id);
        if (actual != fam) {
            // Air, stone, ice, the other liquid, or a column that was dropped for being too far
            // away (get() reads AIR there). A liquid we mis-tagged keeps its own slot.
            if (actual < 2) { schedule(world, x, y, z, actual); return STEP_RETAGGED; }
            return STEP_RETIRE;
        }
        if (isFlatLiquid(id)) {
            write(world, x, y, z, fam ? LAVA_SRC : WATER_SRC);
            scheduleAround(world, x, y, z, fam);
        }

        const uint16_t type = stateType(id);
        const uint16_t fluid = (uint16_t)(fam ? BLOCK_LAVA : BLOCK_WATER);
        const int level = levelOf(id);
        const bool source = (level == MAX_LEVEL);

        if (y > 0 && isFree(world.get(x, y - 1, z))) {
            // Falling: a column moves down instead of sideways, which is what keeps a waterfall from
            // soaking the air the whole way. The cell below lands one level short of the source, so
            // the column is made of flowing water and dries out when the source on top goes away.
            const uint16_t below = world.get(x, y - 1, z);
            const uint8_t bf = familyOf(below);
            if (bf < 2 && bf != fam) {
                if (reactAt(world, x, y - 1, z, below, fam == 1)) scheduleAround(world, x, y - 1, z, fam);
            } else if (isFree(below) && familyOf(below) != fam) {
                const uint16_t out = (uint16_t)makeState(fluid, (uint16_t)(source ? MAX_LEVEL - 1 : level));
                if (write(world, x, y - 1, z, out)) scheduleAround(world, x, y - 1, z, fam);
            }
            if (!source) {
                // A falling cell still has to give its own level back when the feed above it goes.
                const int feed = inflow(world, x, y, z, fam, false);
                if (feed < 1) {
                    if (write(world, x, y, z, BLOCK_AIR)) scheduleAround(world, x, y, z, fam);
                } else if (feed != level) {
                    if (write(world, x, y, z, (uint16_t)makeState(type, (uint16_t)feed)))
                        scheduleAround(world, x, y, z, fam);
                }
            }
            if (fam == 1) drainLavaRoof(world, x, y + 1, z);
            return STEP_KEEP;
        }

        // Resting on something. Recompute the feed and give the water back in full when nothing
        // feeds this voxel, so a stream dries from its tip instead of hanging in the air forever.
        const int feed = inflow(world, x, y, z, fam, source);
        if (feed < 1) {
            if (write(world, x, y, z, BLOCK_AIR)) scheduleAround(world, x, y, z, fam);
            return STEP_KEEP;
        }
        if (fam == 1) drainLavaRoof(world, x, y + 1, z);

        if (feed != level) {
            if (write(world, x, y, z, (uint16_t)makeState(type, (uint16_t)feed)))
                scheduleAround(world, x, y, z, fam);
        }

        const int out = feed - 1;
        if (out >= MAX_LEVEL - (fam ? LAVA_SPREAD : WATER_SPREAD)) {
            for (int d = 0; d < 4; d++) {
                const int nx = x + SIDES[d][0], nz = z + SIDES[d][1];
                const uint16_t nb = world.get(nx, y, nz);
                const uint8_t nf = familyOf(nb);
                if (nf == fam) continue;               // its own tick settles it
                if (nf < 2) {
                    if (reactAt(world, nx, y, nz, nb, fam == 1)) {
                        scheduleAround(world, nx, y, nz, fam);
                        if (fam == 1) break;           // the wall closed: this front stops here
                    }
                    continue;
                }
                if (!isFree(nb)) continue;
                if (write(world, nx, y, nz, (uint16_t)makeState(fluid, (uint16_t)out)))
                    scheduleAround(world, nx, y, nz, fam);
            }
        }
        // Ice last: the cell that freezes stops feeding its neighbours, so the stream under it dries.
        if (fam == 0) tryFreeze(world, x, y, z);
        return STEP_KEEP;
    }

    void rebuild() {
        scratch.clear();
        for (const Cell& c : cells) if (!c.dead) scratch.push_back(c);
        cells.swap(scratch);
        slots.clear();
        for (uint32_t i = 0; i < (uint32_t)cells.size(); i++) slots[Cell::key(cells[i])] = i;
    }

    void tick(VoxelWorld& world, float dt) {
        if (!(dt >= 0.0f)) dt = 0.0f;                 // also catches a NaN delta
        if (dt > 0.5f) dt = 0.5f;                    // a stalled frame must not buy extra water
        clockMs += (double)dt * 1000.0;
        if (clockMs > 3.0e9) rebase(2000000000u);     // keep the uint32 ms clock off its wrap

        const uint32_t clock = now();
        const bool runWater = (uint32_t)(clock - lastWaterStep) >= WATER_STEP_MS;
        const bool runLava = (uint32_t)(clock - lastLavaStep) >= LAVA_STEP_MS;
        if (!runWater && !runLava) { activeNow = (int)cells.size(); return; }
        if (runWater) { lastWaterStep = clock; ++steps; }
        if (runLava) lastLavaStep = clock;

        // Always read the journal: an idle simulator is woken by the block the player just broke.
        scanEdits(world);
        if (cells.empty()) { activeNow = 0; return; }

        int processed = 0;
        bool retired = false;
        // The snapshot bound is deliberate: cells scheduled by this step land past it and tick next
        // time, and the copy keeps the cell safe against the vector growing while it is read.
        const size_t n = cells.size();
        for (size_t i = 0; i < n; i++) {
            if (cells[i].dead || !due(cells[i].dueMs)) continue;
            if (cells[i].family == 0 && !runWater) continue;
            if (cells[i].family == 1 && !runLava) continue;
            if (processed >= MAX_CELLS_PER_STEP) break;   // the rest waits for the next step
            ++processed;
            const Cell cell = cells[i];
            const int r = stepCell(world, cell);
            if (r == STEP_RETIRE) { cells[i].dead = 1; retired = true; }
        }
        if (retired) rebuild();
        activeNow = (int)cells.size();
    }
};

LiquidSim& sim() {
    static LiquidSim g_sim;
    return g_sim;
}

// One place a source goes. Returns true when the voxel now holds this liquid.
bool fillOne(VoxelWorld& world, LiquidSim& s, int x, int y, int z, uint16_t src, uint8_t fam) {
    if (y < 0 || y >= WORLD_HEIGHT) return false;
    const uint16_t here = world.get(x, y, z);
    if (familyOf(here) == fam) { s.schedule(world, x, y, z, fam); return true; }
    if (familyOf(here) == (uint8_t)(1 - fam)) {
        // Pouring into the other liquid: let the contact rule run, and count the solid it makes.
        if (s.reactAt(world, x, y, z, here, fam == 1)) { s.scheduleAround(world, x, y, z, fam); return true; }
        return false;
    }
    if (!isFree(here)) return false;
    // setBlock, not the quiet one: a pour is a player action, so it belongs in the journal and in
    // the save the same way a placed block does.
    if (world.setBlock(x, y, z, src) == src) return false;
    s.scheduleAround(world, x, y, z, fam);
    return true;
}

}  // namespace

void installVoxelWater(VoxelWorld& world) {
    sim().reset();
    // A captureless lambda: the state lives in the simulator, the world keeps only the hook.
    world.waterTick = [](VoxelWorld& w, float dt) { sim().tick(w, dt); };
}

int placeLiquidSource(VoxelWorld& world, int wx, int wy, int wz, bool lava, int amount) {
    LiquidSim& s = sim();
    if (!world.waterTick) installVoxelWater(world);
    if (amount > MAX_LEVEL) amount = MAX_LEVEL;
    if (amount < 1) amount = 1;
    if (wy < 0 || wy >= WORLD_HEIGHT) return 0;
    if (!world.loaded(wx >> 4, wz >> 4)) return 0;

    const uint16_t src = lava ? LAVA_SRC : WATER_SRC;
    const uint8_t fam = lava ? 1 : 0;
    int placed = 0;

    // Down the column first, so a bucket poured in the air reaches the ground it was aimed at. A
    // cell that takes no source ends the column: the liquid below cannot be reached from here.
    for (int y = wy; placed < amount && y >= 0; y--) {
        if (!fillOne(world, s, wx, y, wz, src, fam)) break;
        ++placed;
    }
    // ...then in rings at the pour height, which is where the surplus of a big amount belongs.
    for (int ring = 1; placed < amount; ring++) {
        int filled = 0;
        for (int i = 0; i < 4 && placed < amount; i++) {
            const int px = wx + SIDES[i][0] * ring, pz = wz + SIDES[i][1] * ring;
            if (fillOne(world, s, px, wy, pz, src, fam)) { ++placed; ++filled; }
        }
        if (filled == 0 && ring > WATER_SPREAD) break;    // walled in on every side
    }
    return placed;
}

std::string waterStats(const VoxelWorld& world) {
    const LiquidSim& s = sim();
    std::string out = world.waterTick ? "Water: " : "Water: off, ";
    out += std::to_string(s.activeNow);
    out += " active cells, ";
    double secs = s.clockMs * 0.001;
    if (!(secs > 1.0)) secs = 1.0;
    out += std::to_string((long long)((double)s.steps / secs));
    out += " ticks/s, ";
    out += (s.activeNow == 0 ? "settled" : "flowing");
    return out;
}

// ---------------------------------------------------------------------------
// Behavioural probe
// ---------------------------------------------------------------------------

// -1 when the voxel holds no liquid. A bare liquid id with no level bits is a full source - the
// generator's sea and every save written before the sim existed are written that way - so the probe
// has to read it exactly as the solver does, or it would call the ocean dry.
static int probeLevel(VoxelWorld& world, int x, int y, int z) {
    const uint16_t id = world.get(x, y, z);
    const uint16_t t = stateType(id);
    if (t != BLOCK_WATER && t != BLOCK_LAVA) return -1;
    const int raw = (int)state::waterLevel(id);
    return raw == 0 ? 15 : raw;
}

// One step per call at 0.25 s: WATER_STEP_MS is 250, so this is the fastest the solver will ever
// run and the only clock a headless test can count on.
static void probeTicks(VoxelWorld& world, int steps) {
    if (!world.waterTick) return;
    for (int i = 0; i < steps; i++) world.waterTick(world, 0.25f);
}

void probeWater(VoxelWorld& world, Probe& probe) {
    ProbeModule mod(probe, "water");
    if (!world.waterTick) installVoxelWater(world);
    VoxelPatch patch(world);

    int ax = 0, ay = 0, az = 0;
    // Sixteen cells long: the spread cap is seven, so a source poured in the middle of the arena
    // cannot reach a cell the patch has not recorded and would leave nothing behind on restore.
    if (!carveArena(world, patch, ax, ay, az, 16, 3, 4)) {
        probe.skip("the column under the focus chunk has not streamed");
        return;
    }
    const int wz = az + 1;
    const int src = ax + 4;

    carveArena(world, patch, ax, ay, az, 16, 3, 4);
    const int poured = placeLiquidSource(world, src, ay, wz, false, 1);
    probeTicks(world, 24);
    probe.check("a source is never drained by the stream it feeds",
                poured == 1 && probeLevel(world, src, ay, wz) == 15,
                "poured %d, source level %d", poured, probeLevel(world, src, ay, wz));

    // The distance the source is measured from is the same number the solver caps on: a cell keeps
    // feeding while its level is still worth a step, so the eighth cell has to stay dry.
    const int atSeven = probeLevel(world, src + 7, ay, wz);
    const int atEight = probeLevel(world, src + 8, ay, wz);
    probe.check("a stream reaches seven cells and stops",
                atSeven == 8 && atEight < 0,
                "d=7 level %d, d=8 level %d", atSeven, atEight);

    carveArena(world, patch, ax, ay, az, 16, 3, 4);
    placeLiquidSource(world, src, ay + 3, wz, false, 1);
    probeTicks(world, 1);
    probe.check("a column falls before it spreads",
                probeLevel(world, src, ay + 2, wz) == 14 && probeLevel(world, src + 1, ay + 3, wz) < 0,
                "below %d, beside the source %d", probeLevel(world, src, ay + 2, wz),
                probeLevel(world, src + 1, ay + 3, wz));

    carveArena(world, patch, ax, ay, az, 16, 3, 4);
    // The barrier has to cross the whole arena. Liquid flows around an obstacle, so a wall one cell
    // wide only redirects the stream into the lane next to it - which is what this check measured
    // the first time it ran. The solver has no upward spread, so one row across the width is a real
    // barrier rather than a step the water climbs.
    for (int z = 0; z < 3; z++) patch.set(src + 1, ay, az + z, BLOCK_STONE);
    placeLiquidSource(world, src, ay, wz, false, 1);
    probeTicks(world, 24);
    probe.check("water does not flow through stone",
                probeLevel(world, src + 1, ay, wz) < 0 && probeLevel(world, src + 2, ay, wz) < 0 &&
                probeLevel(world, src - 1, ay, wz) > 0,
                "behind the wall %d, in front of it %d", probeLevel(world, src + 2, ay, wz),
                probeLevel(world, src - 1, ay, wz));

    // The rate claim, measured instead of asserted from the constants: how many 0.25 s steps each
    // family needs before the cell beside its source exists at all. The bands are the scheduler's
    // own arithmetic, not a tolerance invented here. A slot is only noticed on the tick after the
    // pour, and is then due a full period later; a pass runs when its family's global gate has been
    // shut for a period, so a due cell can wait up to one more period for the next pass. Water: 1 +
    // 250/250 + 250/250 -> 1 or 2 steps (its gate opens every step, so the wait is one step). Lava:
    // 1 + 1000/250 + 1000/250 -> at most 9 steps, at least 4 because a period is four steps.
    carveArena(world, patch, ax, ay, az, 16, 3, 4);
    placeLiquidSource(world, src, ay, wz, false, 1);
    int waterSteps = 0, lavaSteps = 0;
    for (int i = 1; i <= 8 && !waterSteps; i++) {
        probeTicks(world, 1);
        if (probeLevel(world, src + 1, ay, wz) > 0) waterSteps = i;
    }
    carveArena(world, patch, ax, ay, az, 16, 3, 4);
    placeLiquidSource(world, src, ay, wz, true, 1);
    for (int i = 1; i <= 12 && !lavaSteps; i++) {
        probeTicks(world, 1);
        if (probeLevel(world, src + 1, ay, wz) > 0) lavaSteps = i;
    }
    probe.check("lava runs on a clock four times slower than water",
                waterSteps >= 1 && waterSteps <= 2 && lavaSteps >= 4 && lavaSteps <= 9 &&
                lavaSteps >= 4 * waterSteps,
                "water reached its neighbour in %d step(s), lava in %d", waterSteps, lavaSteps);

    // Which liquid arrives first is a race, and so is which rock comes out of it: water onto a lava
    // source is obsidian, lava onto a water source is stone, and either front can win the tick. The
    // claim the solver makes is only that the pair stops being two liquids in the same place.
    carveArena(world, patch, ax, ay, az, 16, 3, 4);
    placeLiquidSource(world, src + 2, ay, wz, true, 1);
    placeLiquidSource(world, src, ay, wz, false, 1);
    probeTicks(world, 12);
    const uint16_t gap = stateType(world.get(src + 1, ay, wz));
    const uint16_t pour = stateType(world.get(src + 2, ay, wz));
    auto isRock = [](uint16_t t) {
        return t == BLOCK_OBSIDIAN || t == BLOCK_COBBLESTONE || t == BLOCK_STONE;
    };
    probe.check("water meeting lava leaves a rock where they touch",
                isRock(gap) || isRock(pour), "gap %s, lava pour %s", block(gap).name, block(pour).name);

    // A settled pool must produce no writes at all: the whole point of a level-driven solver is
    // that an ocean costs nothing per tick, and a snapshot is the only evidence a headless run has.
    // Ice is allowed as a change: in a snowy column a source may freeze, which takes it out of the
    // world instead of leaving the pool sloshing.
    carveArena(world, patch, ax, ay, az, 16, 3, 4);
    placeLiquidSource(world, src, ay, wz, false, 1);
    probeTicks(world, 40);
    uint16_t before[16];
    for (int i = 0; i < 16; i++) before[i] = world.get(ax + i, ay, wz);
    probeTicks(world, 20);
    int moved = 0;
    for (int i = 0; i < 16; i++) {
        const uint16_t now = world.get(ax + i, ay, wz);
        if (now != before[i] && stateType(now) != BLOCK_ICE) moved++;
    }
    probe.check("a pool settles and then changes nothing",
                moved == 0, "%d of 16 cells still moving", moved);

    // The generator writes a bare BLOCK_WATER for the sea. Unticked it would render as a thin sheet,
    // so the first step has to promote it to a source. Loud on purpose: the simulator only learns
    // about liquid by reading the world's edit journal, and a quiet write would never be seen.
    carveArena(world, patch, ax, ay, az, 16, 3, 4);
    world.setBlock(src, ay, wz, BLOCK_WATER);
    probeTicks(world, 1);
    const uint16_t promoted = world.get(src, ay, wz);
    probe.check("a bare water id from the generator becomes a full source",
                stateType(promoted) == BLOCK_WATER && state::waterLevel(promoted) == 15,
                "id 0x%04x level %d", (unsigned)promoted, (int)state::waterLevel(promoted));

    patch.restore();
    probeTicks(world, 8);      // let the simulator notice the world it no longer owns
    world.clearPendingEdits(); // a probe's water must not reach a save or a client
}

}  // namespace voxel
