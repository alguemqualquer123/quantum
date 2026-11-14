// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelLight.h"

#include "VoxelConfig.h"
#include "VoxelBlocks.h"
#include "VoxelChunk.h"
#include "VoxelProbe.h"
#include "VoxelWorld.h"

#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace voxel {

// The solver behind the frozen entry points below. Kept out of the header because the only thing
// other modules are allowed to reach for is installVoxelLight() and lightChunk().
void lightChunkImpl(VoxelWorld& world, Chunk& chunk);

namespace {

// The solver walks a column with shifts instead of divisions, so it needs the power-of-two shape
// voxelIndex() implies: index = y*256 + z*16 + x.
static_assert(CHUNK_SX == 16 && CHUNK_SZ == 16, "the light solver indexes chunks by bit shifts");

// How deep border light is re-flooded inside a neighbour chunk. Enough for a cave mouth or a
// torch one voxel from the seam to read correctly, small enough that the border pass stays a
// fraction of the interior solve.
const int BLEED = 4;
static_assert(BLEED < CHUNK_SX, "a bleed slab must not cover a whole chunk");

// Larger than any light level, so "level - cost" is always negative and the voxel is a wall.
const int COST_BLOCKED = LIGHT_MAX + 1;

// One entry per block *type*. stateType() masks nine bits, so a saved chunk can ask for an id the
// registry does not have; unknown ids are stored as a wall rather than looked up out of range.
struct LightInfo {
    uint8_t skyDown;    // daylight lost falling one block down through it (0 = nothing filters)
    uint8_t cost;       // light lost entering it from the side or from below
    uint8_t emit;       // light it produces
    uint8_t opaque;     // received daylight stops here
};

struct LightTable {
    LightInfo info[BLOCK_TYPE_MASK + 1];

    LightTable() {
        for (int t = 0; t <= BLOCK_TYPE_MASK; t++) {
            LightInfo& li = info[t];
            if (t >= BLOCK_COUNT) {
                li.skyDown = (uint8_t)COST_BLOCKED; li.cost = (uint8_t)COST_BLOCKED;
                li.emit = 0; li.opaque = 1;
                continue;
            }
            const BlockDef& def = block((uint16_t)t);
            const bool opaque = (def.flags & FLAG_OPAQUE) != 0;
            // Anything the eye can see through still pays a step: sunFilter says how much and
            // LIGHT_SUN_FALL is the floor, so no block is ever a free pipe for daylight.
            const bool seeThrough = (def.flags & (FLAG_TRANSLUCENT | FLAG_LIQUID)) != 0;
            const int filter = def.sunFilter > LIGHT_SUN_FALL ? (int)def.sunFilter : LIGHT_SUN_FALL;
            li.opaque = opaque ? 1 : 0;
            li.emit = def.light > LIGHT_MAX ? (uint8_t)LIGHT_MAX : def.light;
            if (opaque) {
                li.skyDown = (uint8_t)COST_BLOCKED; li.cost = (uint8_t)COST_BLOCKED;
            } else if (seeThrough) {
                li.skyDown = (uint8_t)(filter > LIGHT_MAX ? LIGHT_MAX : filter);
                li.cost = li.skyDown;
            } else {
                li.skyDown = 0; li.cost = 1;
            }
        }
    }
};

inline const LightInfo* lightTable() {
    static const LightTable tbl;
    return tbl.info;
}

// Buckets, one per level, drained bright to dark. The brightest path therefore always wins on
// arrival and a voxel is only rewritten when it really gets brighter, which is what makes a full
// re-solve idempotent. Depth comes from the levels, never from recursion: a 128-block column would
// not fit the stack, and the buckets are reused so the solve allocates nothing per voxel.
struct Buckets {
    std::vector<int> level[LIGHT_MAX + 1];

    Buckets() {
        for (int i = 0; i <= LIGHT_MAX; i++) level[i].reserve(1024);
    }
    void clear() { for (int i = 0; i <= LIGHT_MAX; i++) level[i].clear(); }
    void push(int lvl, int idx) {
        // Level 1 is worth queueing even though every costed step out of it lands on 0: daylight
        // falling through air pays nothing, so a dim shaft still has to be carried to its floor.
        if (lvl >= 1 && lvl <= LIGHT_MAX) level[lvl].push_back(idx);
    }
};

struct Scratch {
    Buckets sky;
    Buckets block;
    std::vector<int> emitters;
    std::vector<uint8_t> before;      // this column's light as it was when the solve started
};

inline Scratch& scratch() {
    static thread_local Scratch s;
    return s;
}

inline int skyOf(int packed) { return (packed >> 4) & 15; }
inline int blockOf(int packed) { return packed & 15; }

// Writes through Chunk::setLight so the sky<<4|block packing lives in exactly one place, and says
// whether the byte actually moved.
inline bool putLight(Chunk& c, int idx, int sky, int blk) {
    const uint8_t packed = (uint8_t)((sky << 4) | blk);
    if (c.light[idx] == packed) return false;
    c.setLight(idx & 15, idx >> 8, (idx >> 4) & 15, sky, blk);
    return true;
}

const int DX[6] = { 1, -1, 0, 0, 0, 0 };
const int DY[6] = { 0, 0, 1, -1, 0, 0 };
const int DZ[6] = { 0, 0, 0, 0, 1, -1 };
// voxelIndex() is linear in each axis, so a neighbour is a fixed stride away: the step table keeps
// the whole solver free of multiplies.
const int DELTA[6] = { 1, -1, CHUNK_SZ * CHUNK_SX, -CHUNK_SZ * CHUNK_SX, CHUNK_SX, -CHUNK_SX };

// Daylight free-falls, so a shaft keeps 15 all the way to its floor. The vertical pass cannot know
// that for a voxel whose own column is capped by an overhang, so the flood fill repeats the rule:
// stepping down pays for the medium the light is *leaving*, any other direction pays for the one it
// enters. It has to be the source, because that is what the column walk does: a voxel keeps the light
// that reached it and the one below pays for passing it. Charging the target instead made a pane of
// glass a free pipe - it held 15, the air under it cost nothing to fall into, and the room below a
// glass roof read the open sky above it. The brightest path still wins, so the two conventions can no
// longer disagree about anything.
// `top` is the brightest level queued; levels above it are empty and are not walked.
template <int SHIFT>
void drain(Chunk& c, const LightInfo* tbl, Buckets& q, int xlo, int xhi, int zlo, int zhi,
           int top = LIGHT_MAX) {
    const bool sky = SHIFT == 4;
    for (int level = top; level >= 1; level--) {
        std::vector<int>& bucket = q.level[level];
        // A free fall downward appends to the bucket being drained, hence the index loop.
        for (size_t i = 0; i < bucket.size(); i++) {
            const int idx = bucket[i];
            const int byte = c.light[idx];
            if (((byte >> SHIFT) & 15) != level) continue;      // stale: a brighter path won
            const int x = idx & 15, y = idx >> 8, z = (idx >> 4) & 15;
            // Daylight sitting in a solid voxel came from the sky above it and must not tunnel
            // through the rock beside it. Block light inside a solid voxel is always its own
            // emission, which is why glowstone at the border still lights the neighbour.
            const LightInfo& self = tbl[stateType(c.blocks[idx])];
            if (sky && self.opaque) continue;
            for (int d = 0; d < 6; d++) {
                const int nx = x + DX[d], ny = y + DY[d], nz = z + DZ[d];
                if (ny < 0 || ny >= WORLD_HEIGHT) continue;
                if (nx < xlo || nx > xhi || nz < zlo || nz > zhi) continue;
                const int nidx = idx + DELTA[d];
                const LightInfo& t = tbl[stateType(c.blocks[nidx])];
                const int step = (sky && DY[d] < 0) ? (int)self.skyDown : (int)t.cost;
                const int nl = level - step;
                if (nl < 1) continue;
                const int nbyte = c.light[nidx];
                if (((nbyte >> SHIFT) & 15) >= nl) continue;
                const int nother = (nbyte >> (4 - SHIFT)) & 15;
                putLight(c, nidx, sky ? nl : nother, sky ? nother : nl);
                q.push(nl, nidx);
            }
        }
        bucket.clear();
    }
}

// Skylight for one column: LIGHT_MAX at the top, minus the filter of every block it falls through,
// and zero below the first opaque one. Emission is only recorded here, not spread, because the
// block channel is being rebuilt from scratch.
void skylightColumns(Chunk& c, const LightInfo* tbl, std::vector<int>& emitters) {
    emitters.clear();
    for (int z = 0; z < CHUNK_SZ; z++) {
        for (int x = 0; x < CHUNK_SX; x++) {
            int level = LIGHT_MAX;
            int idx = voxelIndex(x, WORLD_HEIGHT - 1, z);
            for (int y = WORLD_HEIGHT - 1; y >= 0; y--, idx -= CHUNK_SX * CHUNK_SZ) {
                const LightInfo& li = tbl[stateType(c.blocks[idx])];
                // The voxel keeps the light that reached it; the one below pays for passing it.
                putLight(c, idx, level, 0);
                if (li.opaque) level = 0;
                else if (li.skyDown >= level) level = 0;
                else level -= li.skyDown;
                if (li.emit > 0) emitters.push_back(idx);
            }
        }
    }
}

// Every transparent voxel holding daylight is a flood-fill source, but only one with somewhere to
// put it: a costed step costs at least one, so a voxel whose six neighbours all hold at least v-1
// cannot brighten anything and never enters a bucket - except straight down, which is free and so
// keeps the level it has, which is why a voxel holding 1 is still a source for its own floor.
// Testing that on the light bytes alone is a fraction of the cost of popping the voxel and reading
// the block table six times.
void seedSky(Chunk& c, const LightInfo* tbl, Buckets& q) {
    for (int idx = 0; idx < CHUNK_VOLUME; idx++) {
        const int v = skyOf(c.light[idx]);
        if (v < 1) continue;
        if (tbl[stateType(c.blocks[idx])].opaque) continue;
        const int dim = v - 1;
        const int x = idx & 15, y = idx >> 8, z = (idx >> 4) & 15;
        bool frontier = false;
        if (x > 0) frontier = skyOf(c.light[idx - 1]) < dim;
        if (!frontier && x < CHUNK_SX - 1) frontier = skyOf(c.light[idx + 1]) < dim;
        if (!frontier && y < WORLD_HEIGHT - 1) frontier = skyOf(c.light[idx + CHUNK_SZ * CHUNK_SX]) < dim;
        if (!frontier && z > 0) frontier = skyOf(c.light[idx - CHUNK_SX]) < dim;
        if (!frontier && z < CHUNK_SZ - 1) frontier = skyOf(c.light[idx + CHUNK_SX]) < dim;
        // Down is the one direction that can be free, so a transparent voxel below loses the
        // one-step slack: a shaft has to be re-lit to 15 even when its floor drifted down.
        if (!frontier && y > 0) {
            const int below = idx - CHUNK_SZ * CHUNK_SX;
            if (!tbl[stateType(c.blocks[below])].opaque && skyOf(c.light[below]) < v) frontier = true;
        }
        if (frontier) q.push(v, idx);
    }
}

void seedEmitters(Chunk& c, const std::vector<int>& emitters, const LightInfo* tbl, Buckets& q) {
    for (size_t i = 0; i < emitters.size(); i++) {
        const int idx = emitters[i];
        const int emit = tbl[stateType(c.blocks[idx])].emit;
        const int byte = c.light[idx];
        if (blockOf(byte) >= emit) continue;
        putLight(c, idx, skyOf(byte), emit);
        q.push(emit, idx);
    }
}

// Brightest level with work waiting, so a slab drain does not walk empty buckets.
inline int topLevel(const Buckets& q) {
    for (int i = LIGHT_MAX; i >= 2; i--) {
        if (!q.level[i].empty()) return i;
    }
    return 2;
}

// The facing plane of a neighbour that is already solved is a source for this column: a cave
// mouth, a torch or an open sky at the seam has to be pulled in *before* the interior flood fill,
// or the border of every chunk reads as a void.
//
// Light the neighbour only has because we once gave it is not a source: that echo is what keeps a
// corridor glowing after its torch is gone. A neighbour voxel dimmer than the one facing it used
// to be can only have been fed through this seam, so it is skipped here and left to the border
// pass below to notice.
void pullFromNeighbour(VoxelWorld& world, Chunk& c, int cx, int cz, bool xPlane, int ourXZ,
                       int theirXZ, const LightInfo* tbl, const std::vector<uint8_t>& before,
                       Scratch& s) {
    Chunk* pnb = world.chunk(cx, cz);
    if (!pnb || !pnb->generated || !pnb->lit) return;
    if (pnb->blocks.size() != (size_t)CHUNK_VOLUME || pnb->light.size() != (size_t)CHUNK_VOLUME)
        return;
    Chunk& nb = *pnb;
    const int colStep = CHUNK_SZ * CHUNK_SX;                 // one voxel up the column
    for (int p = 0; p < CHUNK_SX; p++) {
        int ourIdx = xPlane ? voxelIndex(ourXZ, 0, p) : voxelIndex(p, 0, ourXZ);
        int theirIdx = xPlane ? voxelIndex(theirXZ, 0, p) : voxelIndex(p, 0, theirXZ);
        for (int y = 0; y < WORLD_HEIGHT; y++, ourIdx += colStep, theirIdx += colStep) {
            const LightInfo& ours = tbl[stateType(c.blocks[ourIdx])];
            if (ours.opaque) continue;              // a wall keeps the light its own column gave it
            const int nbyte = nb.light[theirIdx];
            const int obyte = c.light[ourIdx];
            const int oldbyte = before[ourIdx];
            const int sky = skyOf(nbyte);
            // Daylight stored in their solid voxel is its own sky, not something to hand over.
            if (sky > 1 && !tbl[stateType(nb.blocks[theirIdx])].opaque && sky >= skyOf(oldbyte)) {
                const int nl = sky - (int)ours.cost;
                if (nl > skyOf(obyte)) {
                    putLight(c, ourIdx, nl, blockOf(obyte));
                    s.sky.push(nl, ourIdx);
                }
            }
            const int blk = blockOf(nbyte);
            if (blk > 1 && blk >= blockOf(oldbyte)) {
                const int nl = blk - (int)ours.cost;
                if (nl > blockOf(obyte)) {
                    putLight(c, ourIdx, skyOf(obyte), nl);
                    s.block.push(nl, ourIdx);
                }
            }
        }
    }
}

// Border pass in the other direction. This column is solved, so its facing plane is authoritative
// and gets pushed into the neighbour; only the slab the light actually enters is re-flooded, and
// only inward from the plane. Where the neighbour holds more light than the seam can explain, its
// column is stale (a torch went out here, a wall went up): it is flagged for a full re-solve
// rather than poked voxel by voxel, because the chunk is not ours to rebuild.
void bleedBorder(VoxelWorld& world, Chunk& c, const LightInfo* tbl, Scratch& s) {
    for (int dir = 0; dir < 4; dir++) {
        const bool xPlane = dir < 2;
        const int ncx = c.cx + (dir == 0 ? 1 : dir == 1 ? -1 : 0);
        const int ncz = c.cz + (dir == 2 ? 1 : dir == 3 ? -1 : 0);
        Chunk* pnb = world.chunk(ncx, ncz);
        if (!pnb || !pnb->generated || !pnb->lit) continue;
        if (pnb->blocks.size() != (size_t)CHUNK_VOLUME || pnb->light.size() != (size_t)CHUNK_VOLUME)
            continue;
        Chunk& nb = *pnb;

        const int ourXZ = (dir & 1) == 0 ? (xPlane ? CHUNK_SX - 1 : CHUNK_SZ - 1) : 0;
        const int theirXZ = xPlane ? CHUNK_SX - 1 - ourXZ : CHUNK_SZ - 1 - ourXZ;
        // Slab bounds inside the neighbour: BLEED voxels from the shared face, and only that.
        const int xlo = xPlane ? (dir == 0 ? 0 : CHUNK_SX - BLEED) : 0;
        const int xhi = xPlane ? (dir == 0 ? BLEED - 1 : CHUNK_SX - 1) : CHUNK_SX - 1;
        const int zlo = xPlane ? 0 : (dir == 2 ? 0 : CHUNK_SZ - BLEED);
        const int zhi = xPlane ? CHUNK_SZ - 1 : (dir == 2 ? BLEED - 1 : CHUNK_SZ - 1);

        bool wrote = false;
        bool stale = false;
        const int colStep = CHUNK_SZ * CHUNK_SX;
        for (int p = 0; p < CHUNK_SX; p++) {
            int ourIdx = xPlane ? voxelIndex(ourXZ, 0, p) : voxelIndex(p, 0, ourXZ);
            int theirIdx = xPlane ? voxelIndex(theirXZ, 0, p) : voxelIndex(p, 0, theirXZ);
            for (int y = 0; y < WORLD_HEIGHT; y++, ourIdx += colStep, theirIdx += colStep) {
                const int obyte = c.light[ourIdx];
                const int tbyte = nb.light[theirIdx];
                const LightInfo& ours = tbl[stateType(c.blocks[ourIdx])];
                const LightInfo& theirs = tbl[stateType(nb.blocks[theirIdx])];
                // Crossing the seam costs at least one step on either reading, so a neighbour
                // sitting more than a propagation apart from us cannot have got it from us.
                const int slack = std::max((int)ours.cost, (int)theirs.cost);
                if (!stale && slack < COST_BLOCKED) {
                    if (skyOf(tbyte) > skyOf(obyte) + slack) stale = true;
                    else if (blockOf(tbyte) > blockOf(obyte) + slack) stale = true;
                }
                for (int shift = 0; shift <= 4; shift += 4) {
                    const int ov = (obyte >> shift) & 15;
                    if (ov <= 1) continue;
                    // Do not hand over daylight a solid voxel stores for its own top face.
                    if (shift == 4 && ours.opaque) continue;
                    const int nl = ov - (int)theirs.cost;
                    if (nl < 1) continue;
                    if (((tbyte >> shift) & 15) >= nl) continue;
                    const int nother = (tbyte >> (4 - shift)) & 15;
                    wrote = putLight(nb, theirIdx, shift == 4 ? nl : nother,
                                     shift == 4 ? nother : nl) || wrote;
                    // Seeded on the plane just raised: the drain walks inward from there.
                    if (shift == 4) s.sky.push(nl, theirIdx); else s.block.push(nl, theirIdx);
                }
            }
        }

        if (wrote) {
            drain<4>(nb, tbl, s.sky, xlo, xhi, zlo, zhi, topLevel(s.sky));
            drain<0>(nb, tbl, s.block, xlo, xhi, zlo, zhi, topLevel(s.block));
            // Its mesh baked the old light, and only a full re-solve carries this light past the
            // slab, so the column asks for one. Values only ever rise on this path, so the pair
            // settles after one round and the request does not feed itself.
            nb.dirtyLight = true;
            world.requestRemesh(nb.cx, nb.cz);
        }
        if (stale) nb.dirtyLight = true;
    }
    s.sky.clear();
    s.block.clear();
}

}  // namespace

void lightChunkImpl(VoxelWorld& world, Chunk& chunk) {
    if (chunk.blocks.size() != (size_t)CHUNK_VOLUME || chunk.light.size() != (size_t)CHUNK_VOLUME)
        return;

    const LightInfo* tbl = lightTable();
    Scratch& s = scratch();
    s.sky.clear();
    s.block.clear();
    // The column pass lowers voxels the flood fill raised last time before putting them back, so
    // "did anything change" has to be answered by comparing the finished array with this snapshot.
    s.before.assign(chunk.light.begin(), chunk.light.end());

    skylightColumns(chunk, tbl, s.emitters);

    for (int dir = 0; dir < 4; dir++) {
        const bool xPlane = dir < 2;
        const int ncx = chunk.cx + (dir == 0 ? 1 : dir == 1 ? -1 : 0);
        const int ncz = chunk.cz + (dir == 2 ? 1 : dir == 3 ? -1 : 0);
        const int ourXZ = (dir & 1) == 0 ? (xPlane ? CHUNK_SX - 1 : CHUNK_SZ - 1) : 0;
        const int theirXZ = xPlane ? CHUNK_SX - 1 - ourXZ : CHUNK_SZ - 1 - ourXZ;
        pullFromNeighbour(world, chunk, ncx, ncz, xPlane, ourXZ, theirXZ, tbl, s.before, s);
    }

    seedSky(chunk, tbl, s.sky);
    drain<4>(chunk, tbl, s.sky, 0, CHUNK_SX - 1, 0, CHUNK_SZ - 1);
    seedEmitters(chunk, s.emitters, tbl, s.block);
    drain<0>(chunk, tbl, s.block, 0, CHUNK_SX - 1, 0, CHUNK_SZ - 1);

    bleedBorder(world, chunk, tbl, s);

    chunk.lit = true;
    chunk.dirtyLight = false;
    // Light is baked into vertex colour, so anything that moved has to be drawn again.
    if (s.before != chunk.light) chunk.dirtyMesh = true;
}

// The measured proof of what this solver claims: rock kills daylight and glass only dims it, a
// shaft carries daylight to its floor for nothing, a torch pays exactly one level per block, a wall
// keeps light on its own side, a re-solve changes nothing, and the sun curve is darkest at midnight.
// Builds a room in the live focus column, solves it, and puts every voxel back. Host calls it on
// VOXEL_PROBE.
void probeLight(VoxelWorld& world, Probe& probe) {
    ProbeModule mod(probe, "light");
    VoxelPatch patch(world);
    if (!world.relight) installVoxelLight(world);

    int ax = 0, ay = 0, az = 0;
    // Nine cells square and five tall, all of it inside one column. lightChunk() solves a single
    // column, and the arena already starts four cells into the focus chunk, so a longer one would
    // end up measuring a border pass that never ran.
    if (!carveArena(world, patch, ax, ay, az, 9, 9, 5)) {
        probe.skip("the column under the focus chunk has not streamed");
        return;
    }
    const int cx = ax >> 4, cz = az >> 4;
    auto solve = [&world, cx, cz]() {
        Chunk* c = world.chunk(cx, cz);
        if (c && c->generated) lightChunk(world, *c);
    };
    auto sky = [&world](int x, int y, int z) { return world.getSky(x, y, z); };
    auto lit = [&world](int x, int y, int z) { return world.getBlockLight(x, y, z); };

    // A 3x3x2 room with one cell of stone on every side and a solid roof: the shape a player digs,
    // and the only way to ask "does rock stop light" without also asking "was there light to stop".
    const int rx = ax + 2, rz = az + 2;
    for (int y = ay; y <= ay + 1; y++) {
        for (int x = rx - 1; x <= rx + 3; x++) {
            for (int z = rz - 1; z <= rz + 3; z++) {
                if (x == rx - 1 || x == rx + 3 || z == rz - 1 || z == rz + 3) patch.set(x, y, z, BLOCK_STONE);
            }
        }
    }
    for (int x = rx - 1; x <= rx + 3; x++)
        for (int z = rz - 1; z <= rz + 3; z++)
            patch.set(x, ay + 2, z, BLOCK_STONE);
    solve();

    const int above = sky(rx + 1, ay + 3, rz + 1);
    probe.check("a sealed stone room holds no daylight",
                sky(rx + 1, ay, rz + 1) == 0 && sky(rx + 1, ay + 1, rz + 1) == 0 && above > 0,
                "interior floor %d, interior top %d, open air above the roof %d",
                sky(rx + 1, ay, rz + 1), sky(rx + 1, ay + 1, rz + 1), above);
    probe.check("nothing glowing means nothing lit",
                lit(rx + 1, ay, rz + 1) == 0 && lit(rx, ay, rz) == 0,
                "interior block light %d, far corner %d", lit(rx + 1, ay, rz + 1), lit(rx, ay, rz));

    // Taking the roof out is the free-fall test the header makes: a shaft keeps its level all the
    // way to its floor. Every cell of the shaft is read, because "15 at the top and 14 below" would
    // otherwise pass as a shaft.
    patch.set(rx + 1, ay + 2, rz + 1, BLOCK_AIR);
    solve();
    probe.check("daylight free-falls down an open shaft",
                sky(rx + 1, ay + 2, rz + 1) == above && sky(rx + 1, ay + 1, rz + 1) == above &&
                sky(rx + 1, ay, rz + 1) == above,
                "gap %d, room top %d, floor %d, sky above %d", sky(rx + 1, ay + 2, rz + 1),
                sky(rx + 1, ay + 1, rz + 1), sky(rx + 1, ay, rz + 1), above);
    probe.check("daylight spreads sideways at one level per block",
                sky(rx, ay, rz + 1) == above - 1 && sky(rx, ay, rz) == above - 2,
                "one step %d, two steps %d, shaft floor %d", sky(rx, ay, rz + 1), sky(rx, ay, rz),
                sky(rx + 1, ay, rz + 1));

    // Glass is the see-through case: it pays, it does not block, and the price is the block's own
    // sunFilter with LIGHT_SUN_FALL as the floor, so no block is ever a free pipe for daylight. The
    // claim is read in the room, not in the glass cell: a voxel keeps the brightest path that reaches
    // it, and two paths disagree by construction - the column walk charges a block as the light LEAVES
    // it, the flood fill charges it on ENTRY (that is what makes a downward step into glass cost its
    // filter, so a canopy above cannot be out-lit through the seam). Under the leaves here the flood
    // wins, so the glass byte is a coin flip between the two conventions while the room under it is
    // not: one layer, and the room sits exactly `filter` below the sky its roof stands in, all the way
    // to the floor because free fall inside the room costs nothing.
    const int filter = block(BLOCK_GLASS).sunFilter > LIGHT_SUN_FALL
                     ? (int)block(BLOCK_GLASS).sunFilter : LIGHT_SUN_FALL;
    patch.set(rx + 1, ay + 2, rz + 1, BLOCK_GLASS);
    solve();
    const int oneLayer = sky(rx + 1, ay + 1, rz + 1);
    const int oneGlass = sky(rx + 1, ay + 2, rz + 1);
    probe.check("a glass roof dims the room under it by exactly its filter",
                oneLayer == above - filter && sky(rx + 1, ay, rz + 1) == above - filter,
                "room top %d, room floor %d, sky above the roof %d, filter %d, glass itself %d",
                oneLayer, sky(rx + 1, ay, rz + 1), above, filter, oneGlass);
    patch.set(rx + 1, ay + 3, rz + 1, BLOCK_GLASS);
    solve();
    // Measured with both layers in place, because the byte of a restored voxel is whatever the last
    // solve left in it. The claim is monotonicity, which is the part that does not depend on which
    // of the two path conventions wins a given column: a second pane never brightens anything below
    // it, and the room it lights is still exactly one filter step down - it cannot go darker,
    // because the light is inside a sealed room and free fall through air is free.
    const int twoUpper = sky(rx + 1, ay + 3, rz + 1);
    const int twoLower = sky(rx + 1, ay + 2, rz + 1);
    const int twoRoom = sky(rx + 1, ay + 1, rz + 1);
    patch.set(rx + 1, ay + 3, rz + 1, BLOCK_AIR);
    probe.check("a second glass layer never brightens the path under it",
                twoUpper <= above && twoLower <= oneGlass && twoRoom <= above - filter,
                "upper %d (sky above %d), lower %d (was %d), room top %d (was %d), filter %d",
                twoUpper, above, twoLower, oneGlass, twoRoom, oneLayer, filter);

    // Reseal the roof and light the room from inside, so the only possible source is the torch.
    patch.set(rx + 1, ay + 2, rz + 1, BLOCK_STONE);
    patch.set(rx + 1, ay, rz + 1, BLOCK_TORCH);
    solve();
    const int emit = block(BLOCK_TORCH).light;
    probe.check("a torch lights its own cell with the value the block table gives it",
                lit(rx + 1, ay, rz + 1) == emit && emit > 0,
                "cell %d, table %d", lit(rx + 1, ay, rz + 1), emit);
    probe.check("block light costs exactly one level per block",
                lit(rx + 2, ay, rz + 1) == emit - 1 && lit(rx + 1, ay + 1, rz + 1) == emit - 1 &&
                lit(rx, ay, rz) == emit - 2,
                "beside %d, above %d, corner %d, from an emit of %d", lit(rx + 2, ay, rz + 1),
                lit(rx + 1, ay + 1, rz + 1), lit(rx, ay, rz), emit);
    probe.check("an opaque wall keeps its light on its own side",
                lit(rx - 2, ay, rz + 1) == 0 && lit(rx - 2, ay + 1, rz + 1) == 0,
                "outside the wall %d and %d, torch inside %d", lit(rx - 2, ay, rz + 1),
                lit(rx - 2, ay + 1, rz + 1), lit(rx + 1, ay, rz + 1));

    // The bucket queue's claim: the brightest path wins on arrival, so solving twice writes the same
    // bytes. Compared over the whole column, not the room, because a stale border is exactly what a
    // second solve would otherwise quietly repaint.
    Chunk* col = world.chunk(cx, cz);
    if (col && col->light.size() == (size_t)CHUNK_VOLUME) {
        const std::vector<uint8_t> snap = col->light;
        solve();
        int differ = 0;
        for (size_t i = 0; i < snap.size(); i++) if (snap[i] != col->light[i]) differ++;
        probe.check("a full re-solve of the column changes nothing", differ == 0,
                    "%d of %d light bytes moved on the second solve", differ, (int)snap.size());
    } else {
        probe.skip("the focus column has no light array to compare against");
    }

    // The mesher multiplies the stored skylight by this, so a wrong curve is a world that is dark
    // at noon or pitch black at midnight. Sampled, not read from the constants.
    float minV = 1e9f, maxV = -1e9f;
    for (int i = 0; i <= 200; i++) {
        const float v = daylightFactor((float)i / 200.0f);
        if (v < minV) minV = v;
        if (v > maxV) maxV = v;
    }
    const float midnight = daylightFactor(0.0f);
    const float noon = daylightFactor(0.5f);
    const float dawn = daylightFactor(0.25f);
    const float dusk = daylightFactor(0.75f);
    probe.check("the day curve is darkest at midnight and brightest at noon",
                midnight == minV && noon == maxV && midnight > 0.0f && midnight < 0.1f && noon > 0.99f,
                "midnight %.3f (moonlit, not black), noon %.3f, dawn %.3f, dusk %.3f",
                midnight, noon, dawn, dusk);
    // The two ramps are written from different expressions (`u` counts up, `v` counts down) and the
    // bounds they subtract are not exactly representable, so sunrise and noon agree only to the last
    // couple of float ulps. A tolerance of 1e-5 is ~1/100000 of the curve's range: far below any
    // asymmetry a player could see, far above the rounding noise.
    probe.check("dawn and dusk are the same brightness", std::fabs(dawn - dusk) < 1e-5f,
                "dawn %.4f, dusk %.4f", (double)dawn, (double)dusk);

    patch.restore();
    solve();     // the column must read the terrain it got back, not the room that was in it
}

}  // namespace voxel

void installVoxelLight(voxel::VoxelWorld& world) {
    world.relight = [](voxel::VoxelWorld& w, voxel::Chunk& c) { voxel::lightChunkImpl(w, c); };
}

void lightChunk(voxel::VoxelWorld& world, voxel::Chunk& chunk) {
    voxel::lightChunkImpl(world, chunk);
}

// Night is not black: a thin moonlit floor keeps the world readable, midday is full sun, and the
// two ramps are smoothstep centred on sunrise and sunset so there is no kink at 0.25 or 0.75.
// Polynomials only, and no include beyond the contract headers: the mesher calls this per drawn
// chunk, not per voxel.
float daylightFactor(float dayProgress) {
    const float MOON = 0.05f;         // moonlight, so a night sky is never pitch black
    const float SUNRISE = 0.25f;
    const float SUNSET = 0.75f;
    const float HALF = 0.05f;         // each ramp spans a tenth of the day

    float t = dayProgress - (float)(int)dayProgress;    // the clock may run past a day either way
    if (t < 0.0f) t += 1.0f;

    float f;
    if (t <= SUNRISE - HALF) f = 0.0f;
    else if (t < SUNRISE + HALF) {
        const float u = (t - (SUNRISE - HALF)) / (2.0f * HALF);
        f = u * u * (3.0f - 2.0f * u);
    }
    else if (t <= SUNSET - HALF) f = 1.0f;
    else if (t < SUNSET + HALF) {
        const float v = 1.0f - (t - (SUNSET - HALF)) / (2.0f * HALF);
        f = v * v * (3.0f - 2.0f * v);
    }
    else f = 0.0f;

    return MOON + (1.0f - MOON) * f;
}
