// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelCollide.h"

#include "VoxelProbe.h"
#include "VoxelWorld.h"
#include "VoxelBlocks.h"

#include <algorithm>
#include <cmath>

namespace voxel {

// A box resting exactly flush against a block face is not *inside* that block, and the whole
// collision scheme depends on being able to tell the two apart: the vertical sweep parks the feet
// exactly on the surface, and the next frame's downward probe must then report "grounded" without
// the resting position itself counting as an overlap.
static const float FLUSH_EPS = 0.0005f;
// A displacement this short is "no room left", not "blocked": keeps the blocked test from firing
// on the float noise of an exact snap.
static const float MOVE_EPS = 0.000001f;

// The collision sub-boxes of a block, in world units. This is the "collision personalised by the
// block" rule: BlockDef::shapes are authored in 1/16ths of the cube and buildChunkMesh() draws
// shapes[0] for every face of the block, so reading the very same table here is what makes the
// body stop where the picture says it should - a stone slab is a half-height shelf you can walk up
// onto, a fence is a thin post you can slip beside, a cactus is an inset box with a gap around it.
// shapeCount == 0 means the block draws as crossed quads (a flower, a torch) or as a liquid and has
// no collision at all. Upper slabs go through blockBox(), the same lift the mesher applies, because
// collision that disagrees with the picture by half a block is what this comment used to document.
int blockShapeBoxes(uint16_t id, int wx, int wy, int wz, VoxelBox out[4]) {
    const uint16_t type = stateType(id);
    if (type == BLOCK_AIR) return 0;
    const BlockDef& def = block(type);
    if (!(def.flags & FLAG_SOLID)) return 0;
    if (def.shapeCount == 0) return 0;

    int n = def.shapeCount;
    if (n > 4) n = 4;
    const float ox = (float)wx, oy = (float)wy, oz = (float)wz;
    for (int i = 0; i < n; i++) {
        const BlockBox b = shapeBox(def.shapes[i], id);
        out[i].minX = ox + b.x0;
        out[i].minY = oy + b.y0;
        out[i].minZ = oz + b.z0;
        out[i].maxX = ox + b.x1;
        out[i].maxY = oy + b.y1;
        out[i].maxZ = oz + b.z1;
    }
    return n;
}

static inline bool boxIntersect(const VoxelBox& a, const VoxelBox& b) {
    return a.minX < b.maxX - FLUSH_EPS && a.maxX > b.minX + FLUSH_EPS &&
           a.minY < b.maxY - FLUSH_EPS && a.maxY > b.minY + FLUSH_EPS &&
           a.minZ < b.maxZ - FLUSH_EPS && a.maxZ > b.minZ + FLUSH_EPS;
}

static inline int floored(float v) { return (int)std::floor(v); }

bool solidBoxAt(VoxelWorld& world, int wx, int wy, int wz, VoxelBox& out) {
    VoxelBox shapes[4];
    const int n = blockShapeBoxes(world.get(wx, wy, wz), wx, wy, wz, shapes);
    if (n <= 0) return false;
    out = shapes[0];
    for (int i = 1; i < n; i++) {
        if (shapes[i].minX < out.minX) out.minX = shapes[i].minX;
        if (shapes[i].minY < out.minY) out.minY = shapes[i].minY;
        if (shapes[i].minZ < out.minZ) out.minZ = shapes[i].minZ;
        if (shapes[i].maxX > out.maxX) out.maxX = shapes[i].maxX;
        if (shapes[i].maxY > out.maxY) out.maxY = shapes[i].maxY;
        if (shapes[i].maxZ > out.maxZ) out.maxZ = shapes[i].maxZ;
    }
    return true;
}

// Voxel span of the box, clamped to the world's vertical range: everything above WORLD_HEIGHT and
// below 0 is air, and get() on those rows is a wasted call in a loop that runs several times a frame.
static inline void boxVoxelRange(const VoxelBox& b, int& x0, int& x1, int& y0, int& y1, int& z0, int& z1) {
    x0 = floored(b.minX); x1 = floored(b.maxX);
    z0 = floored(b.minZ); z1 = floored(b.maxZ);
    y0 = floored(b.minY); y1 = floored(b.maxY);
    if (y0 < 0) y0 = 0;
    if (y1 > WORLD_HEIGHT - 1) y1 = WORLD_HEIGHT - 1;
    if (y0 > y1) { y0 = 1; y1 = 0; }   // empty range
}

bool boxOverlapsSolid(VoxelWorld& world, const VoxelBox& box) {
    int x0, x1, y0, y1, z0, z1;
    boxVoxelRange(box, x0, x1, y0, y1, z0, z1);
    VoxelBox shapes[4];
    for (int z = z0; z <= z1; z++) {
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                const int n = blockShapeBoxes(world.get(x, y, z), x, y, z, shapes);
                for (int i = 0; i < n; i++) {
                    if (boxIntersect(box, shapes[i])) return true;
                }
            }
        }
    }
    return false;
}

bool boxSupported(VoxelWorld& world, const VoxelBox& box) {
    VoxelBox feet = box;
    feet.maxY = box.minY - (FLUSH_EPS * 2.0f);
    feet.minY = box.minY - VOXEL_GROUND_PROBE;
    if (feet.minY > feet.maxY) return false;
    return boxOverlapsSolid(world, feet);
}

// The largest displacement along `axis` (0 x, 1 y, 2 z) that keeps the box out of every solid it
// would sweep through. Taking the tightest limit over all blockers is an exact sweep against static
// boxes, so a body cannot tunnel at any speed the game can reach - no sub-stepping needed.
static float clampAlongAxis(VoxelWorld& world, const VoxelBox& box, int axis, float d) {
    if (d == 0.0f) return 0.0f;

    VoxelBox swept = box;
    if (axis == 0) { if (d > 0.0f) swept.maxX += d; else swept.minX += d; }
    else if (axis == 1) { if (d > 0.0f) swept.maxY += d; else swept.minY += d; }
    else { if (d > 0.0f) swept.maxZ += d; else swept.minZ += d; }

    int x0, x1, y0, y1, z0, z1;
    boxVoxelRange(swept, x0, x1, y0, y1, z0, z1);

    const float lead = axis == 0 ? box.maxX : (axis == 1 ? box.maxY : box.maxZ);
    const float trail = axis == 0 ? box.minX : (axis == 1 ? box.minY : box.minZ);

    float t = d;
    VoxelBox shapes[4];
    for (int z = z0; z <= z1; z++) {
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                const int n = blockShapeBoxes(world.get(x, y, z), x, y, z, shapes);
                for (int i = 0; i < n; i++) {
                    const VoxelBox& sb = shapes[i];
                    // A blocker only counts if the box already overlaps it on the two axes that do
                    // not move; otherwise it is a corner we will slide past.
                    if (axis != 0 && (sb.maxX <= box.minX + FLUSH_EPS || sb.minX >= box.maxX - FLUSH_EPS)) continue;
                    if (axis != 1 && (sb.maxY <= box.minY + FLUSH_EPS || sb.minY >= box.maxY - FLUSH_EPS)) continue;
                    if (axis != 2 && (sb.maxZ <= box.minZ + FLUSH_EPS || sb.minZ >= box.maxZ - FLUSH_EPS)) continue;

                    if (d > 0.0f) {
                        const float nearFace = axis == 0 ? sb.minX : (axis == 1 ? sb.minY : sb.minZ);
                        if (nearFace >= lead - FLUSH_EPS && nearFace - lead < t) t = nearFace - lead;
                    } else {
                        const float farFace = axis == 0 ? sb.maxX : (axis == 1 ? sb.maxY : sb.maxZ);
                        if (farFace <= trail + FLUSH_EPS && farFace - trail > t) t = farFace - trail;
                    }
                }
            }
        }
    }
    return t;
}

static inline bool isBlocked(float t, float d) {
    return d > 0.0f ? (t < d - MOVE_EPS) : (t > d + MOVE_EPS);
}

static inline void translateX(VoxelBox& b, float v) { b.minX += v; b.maxX += v; }
static inline void translateY(VoxelBox& b, float v) { b.minY += v; b.maxY += v; }
static inline void translateZ(VoxelBox& b, float v) { b.minZ += v; b.maxZ += v; }

// One horizontal axis: go as far as the geometry allows, and when a wall stops the body try the
// same move from a box raised by one block. The raised box only fits where the headroom exists, so
// "step up when the head fits" is the same test as "do not clip through the ceiling".
static bool moveHorizontal(VoxelWorld& world, VoxelBox& box, int axis, float d, bool canStep) {
    if (d == 0.0f) return false;

    const float t = clampAlongAxis(world, box, axis, d);
    if (!isBlocked(t, d)) {
        if (axis == 0) translateX(box, t); else translateZ(box, t);
        return false;
    }

    if (canStep) {
        VoxelBox raised = box;
        translateY(raised, VOXEL_STEP_UP);
        if (!boxOverlapsSolid(world, raised)) {           // room for the whole body one block up
            const float tr = clampAlongAxis(world, raised, axis, d);
            // Only take the lift when it actually buys travel. A 1-block wall with a solid roof over
            // it clears the raised test but stops at the same face, and claiming that step would both
            // drop the wall flag the caller needs to cancel its velocity and leave the body floating
            // one block above the floor with nothing under it.
            const bool buysRoom = d > 0.0f ? (tr > t + MOVE_EPS) : (tr < t - MOVE_EPS);
            if (buysRoom) {
                VoxelBox stepped = raised;
                if (axis == 0) translateX(stepped, tr); else translateZ(stepped, tr);
                if (!boxOverlapsSolid(world, stepped)) {
                    // Land on whatever was climbed: the lift is VOXEL_STEP_UP, so the same distance
                    // down is enough to reach the shelf, and the caller's 2cm ground probe never
                    // would. Without this the body floats over the step until the next frame's gravity
                    // drops it, which reads as a hop and leaves the ground flag off.
                    translateY(stepped, clampAlongAxis(world, stepped, 1, -VOXEL_STEP_UP));
                    box = stepped;
                    return false;
                }
            }
        }
    }

    if (axis == 0) translateX(box, t); else translateZ(box, t);
    return true;
}

static bool inLiquidAt(VoxelWorld& world, const VoxelBox& box) {
    const float cx = (box.minX + box.maxX) * 0.5f;
    const float cz = (box.minZ + box.maxZ) * 0.5f;
    const int ix = floored(cx), iz = floored(cz);
    const float ys[3] = { box.minY + 0.1f, (box.minY + box.maxY) * 0.5f, box.maxY - 0.1f };
    for (int i = 0; i < 3; i++) {
        const int iy = floored(ys[i]);
        if (blockFlags(stateType(world.get(ix, iy, iz))) & FLAG_LIQUID) return true;
    }
    return false;
}

int moveBoxVoxel(VoxelWorld& world, VoxelBox& box, float dx, float dy, float dz) {
    int flags = 0;

    // Stepping sideways is only offered to a body that is not already rising, or a jump would turn
    // every wall into a ladder.
    const bool canStep = (dy <= 0.0f) && (dx != 0.0f || dz != 0.0f);

    bool hitX = moveHorizontal(world, box, 0, dx, canStep);
    bool hitZ = moveHorizontal(world, box, 2, dz, canStep);
    if (hitX || hitZ) flags |= VOXEL_HIT_WALL;

    if (dy != 0.0f) {
        const float t = clampAlongAxis(world, box, 1, dy);
        translateY(box, t);
        if (isBlocked(t, dy)) flags |= (dy < 0.0f) ? VOXEL_HIT_GROUND : VOXEL_HIT_CEIL;
    }

    // Settle and report rest even for a body with no vertical velocity this frame (a mob that has
    // landed, a player standing still, anything flying with dy == 0). A body that is rising must NOT
    // be settled: the probe reaches VOXEL_GROUND_PROBE (0.02) below the feet, and a climb slower than
    // that is undone by it every frame - a mob swimming up at 1.15 m/s moves 0.019 per step, gets put
    // back on the floor it just left, and is then told that floor holds it. That is why nothing in
    // water ever surfaced.
    if (dy <= 0.0f && (flags & VOXEL_HIT_GROUND) == 0) {
        const float g = clampAlongAxis(world, box, 1, -VOXEL_GROUND_PROBE);
        if (isBlocked(g, -VOXEL_GROUND_PROBE)) {
            translateY(box, g);
            flags |= VOXEL_HIT_GROUND;
        }
    }

    if (inLiquidAt(world, box)) flags |= VOXEL_IN_LIQUID;
    return flags;
}

bool targetableBlock(uint16_t id, bool liquidsToo, bool plantsToo) {
    const uint16_t type = stateType(id);
    if (type == BLOCK_AIR) return false;
    const BlockDef& def = block(type);
    if (def.flags & FLAG_NO_MESH) return false;
    if (def.flags & FLAG_LIQUID) return liquidsToo;
    // Crossed quads with no collision shape are the plants the mesher draws as sprites; they are
    // only worth hitting when the caller asked for them (a flower is picked, not mined through).
    const bool plant = (def.flags & FLAG_CUTOUT) && def.shapeCount == 0;
    if (plant) return plantsToo;
    return true;
}

VoxelHit raycastVoxel(const VoxelWorld& world, float ox, float oy, float oz,
                      float dirX, float dirY, float dirZ, float maxDist,
                      bool liquidsToo, bool plantsToo) {
    VoxelHit result;

    const float len = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
    if (!(len > 0.0f) || !(maxDist > 0.0f)) return result;
    const float rx = dirX / len, ry = dirY / len, rz = dirZ / len;

    int x = floored(ox), y = floored(oy), z = floored(oz);
    const int stepX = rx > 0.0f ? 1 : (rx < 0.0f ? -1 : 0);
    const int stepY = ry > 0.0f ? 1 : (ry < 0.0f ? -1 : 0);
    const int stepZ = rz > 0.0f ? 1 : (rz < 0.0f ? -1 : 0);

    // Distance, in ray units, to the next voxel boundary on each axis. An axis-aligned ray never
    // crosses a boundary on the other two, so those stay at infinity.
    float tMaxX = stepX == 0 ? 1e30f : (stepX > 0 ? ((float)x + 1.0f - ox) : (ox - (float)x)) / std::fabs(rx);
    float tMaxY = stepY == 0 ? 1e30f : (stepY > 0 ? ((float)y + 1.0f - oy) : (oy - (float)y)) / std::fabs(ry);
    float tMaxZ = stepZ == 0 ? 1e30f : (stepZ > 0 ? ((float)z + 1.0f - oz) : (oz - (float)z)) / std::fabs(rz);
    const float tDeltaX = stepX == 0 ? 1e30f : 1.0f / std::fabs(rx);
    const float tDeltaY = stepY == 0 ? 1e30f : 1.0f / std::fabs(ry);
    const float tDeltaZ = stepZ == 0 ? 1e30f : 1.0f / std::fabs(rz);

    float travelled = 0.0f;
    int nx = 0, ny = 0, nz = 0;
    bool first = true;
    for (int guard = 0; guard < 4096; guard++) {
        if (first) {
            // The voxel the ray starts in is never reported: a block you are inside of is not the
            // block you are looking at, and a buried eye should not break its own ceiling.
            first = false;
        } else {
            const uint16_t id = world.get(x, y, z);
            if (targetableBlock(id, liquidsToo, plantsToo)) {
                result.hit = true;
                result.x = x; result.y = y; result.z = z;
                result.nx = nx; result.ny = ny; result.nz = nz;
                result.dist = travelled;
                result.id = id;
                return result;
            }
        }

        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            if (tMaxX > maxDist) break;
            x += stepX; travelled = tMaxX; tMaxX += tDeltaX;
            nx = -stepX; ny = 0; nz = 0;
        } else if (tMaxY < tMaxZ) {
            if (tMaxY > maxDist) break;
            y += stepY; travelled = tMaxY; tMaxY += tDeltaY;
            nx = 0; ny = -stepY; nz = 0;
        } else {
            if (tMaxZ > maxDist) break;
            z += stepZ; travelled = tMaxZ; tMaxZ += tDeltaZ;
            nx = 0; ny = 0; nz = -stepZ;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Behavioural probe
// ---------------------------------------------------------------------------

// The body the checks use: the player's own 0.6 x 1.8 x 0.6, because every shape in the block table
// was authored so that this box fits a doorway and does not fit a gap.
static VoxelBox probeBody(float centreX, float feetY, float centreZ) {
    VoxelBox b;
    b.minX = centreX - 0.3f; b.maxX = centreX + 0.3f;
    b.minY = feetY;          b.maxY = feetY + 1.8f;
    b.minZ = centreZ - 0.3f; b.maxZ = centreZ + 0.3f;
    return b;
}

void probeCollide(VoxelWorld& world, Probe& probe) {
    ProbeModule mod(probe, "collide");
    VoxelPatch patch(world);

    int ax = 0, ay = 0, az = 0;
    if (!carveArena(world, patch, ax, ay, az)) {
        probe.skip("the column under the focus chunk has not streamed");
        return;
    }

    const int wx = ax + 3;          // the cell every wall test stands in
    const int wz = az + 1;          // the row the body walks along
    const float startX = (float)ax + 1.0f;
    const float feet = (float)ay;

    // One lane, one block, one shove: the face the body stops at is the shape's face, so this is
    // the direct measurement of "collision personalised by the item".
    auto shoveInto = [&](uint16_t id, float bodyZ, float d, VoxelBox& out, int* flagsOut) {
        carveArena(world, patch, ax, ay, az);
        patch.set(wx, ay + 1, wz, id);
        out = probeBody(startX, feet, bodyZ);
        const int f = moveBoxVoxel(world, out, d, 0.0f, 0.0f);
        if (flagsOut) *flagsOut = f;
        return f;
    };

    const float midZ = (float)wz + 0.5f;
    const float farZ = (float)wz + 1.0f;   // the body clears the post's z span and stays in the arena

    VoxelBox body;
    int flags = 0;

    shoveInto(BLOCK_STONE, midZ, 3.0f, body, &flags);
    probe.check("a full cube stops the body exactly at its face",
                (flags & VOXEL_HIT_WALL) != 0 && std::fabs(body.maxX - (float)wx) < 0.002f,
                "maxX %.3f expected %.3f flags=%d", (double)body.maxX, (double)wx, flags);

    shoveInto(BLOCK_FENCE, midZ, 3.0f, body, &flags);
    probe.check("a fence is a post, so the body stops 6/16 into the cell",
                (flags & VOXEL_HIT_WALL) != 0 && std::fabs(body.maxX - ((float)wx + 0.375f)) < 0.002f,
                "maxX %.3f expected %.3f", (double)body.maxX, (double)wx + 0.375);

    shoveInto(BLOCK_FENCE, farZ, 3.0f, body, &flags);
    probe.check("a body that misses the post walks through the cell",
                (flags & VOXEL_HIT_WALL) == 0 && std::fabs(body.maxX - ((float)ax + 4.3f)) < 0.002f,
                "maxX %.3f flags=%d", (double)body.maxX, flags);

    shoveInto(BLOCK_CACTUS, midZ, 3.0f, body, &flags);
    probe.check("a cactus is inset 2/16 on every side",
                std::fabs(body.maxX - ((float)wx + 0.125f)) < 0.002f,
                "maxX %.3f expected %.3f", (double)body.maxX, (double)wx + 0.125);

    shoveInto(BLOCK_GLASS, midZ, 3.0f, body, &flags);
    probe.check("glass is solid although light passes it",
                (flags & VOXEL_HIT_WALL) != 0 && std::fabs(body.maxX - (float)wx) < 0.002f,
                "maxX %.3f flags=%d", (double)body.maxX, flags);

    shoveInto(BLOCK_TORCH, midZ, 3.0f, body, &flags);
    const float torchMax = body.maxX;
    shoveInto(BLOCK_TALL_GRASS, midZ, 3.0f, body, &flags);
    probe.check("a torch and a plant hold no collision at all",
                (flags & VOXEL_HIT_WALL) == 0 && std::fabs(torchMax - ((float)ax + 4.3f)) < 0.002f,
                "torch maxX %.3f, grass maxX %.3f", (double)torchMax, (double)body.maxX);

    // A liquid the player swims in has no collision shape, but the same call still has to report it
    // as liquid or the controller would never know to switch to the swim integrator. The water has
    // to be in place before the shove: the flag is decided by where the body ends up, not by what
    // the cell is called afterwards.
    carveArena(world, patch, ax, ay, az);
    for (int x = wx; x <= wx + 1; x++) {
        patch.set(x, ay, wz, BLOCK_WATER);
        patch.set(x, ay + 1, wz, BLOCK_WATER);
    }
    {
        VoxelBox swim = probeBody(startX, feet, midZ);
        const int f = moveBoxVoxel(world, swim, 3.5f, 0.0f, 0.0f);
        probe.check("water is swum through and reported as liquid",
                    (f & VOXEL_HIT_WALL) == 0 && (f & VOXEL_IN_LIQUID) != 0,
                    "maxX %.3f flags=%d", (double)swim.maxX, f);
    }

    shoveInto(BLOCK_STONE, midZ, 3.0f, body, &flags);
    patch.set(wx, ay + 2, wz, BLOCK_STONE);      // now the headroom above the step is gone
    {
        VoxelBox wall = probeBody(startX, feet, midZ);
        const int f = moveBoxVoxel(world, wall, 3.0f, 0.0f, 0.0f);
        probe.check("a two-block-high wall is not climbed",
                    (f & VOXEL_HIT_WALL) != 0 && std::fabs(wall.maxX - (float)wx) < 0.002f &&
                    std::fabs(wall.minY - feet) < 0.002f,
                    "maxX %.3f feet %.3f flags=%d", (double)wall.maxX, (double)wall.minY, f);
    }

    // The whole point of an exact sweep instead of a position test: the block's face is still there
    // when the body arrives with one frame's worth of 200 blocks/s.
    shoveInto(BLOCK_STONE, midZ, 50.0f, body, &flags);
    probe.check("nothing tunnels, however fast the body moves",
                (flags & VOXEL_HIT_WALL) != 0 && std::fabs(body.maxX - (float)wx) < 0.002f,
                "maxX %.3f expected %.3f", (double)body.maxX, (double)wx);

    // ---- rest, and the ray that decides what a click edits ----
    carveArena(world, patch, ax, ay, az);
    VoxelBox falling = probeBody(startX, feet + 3.0f, midZ);
    const int dropped = moveBoxVoxel(world, falling, 0.0f, -3.0f, 0.0f);
    probe.check("a body dropped on a floor rests flush and reads as grounded",
                (dropped & VOXEL_HIT_GROUND) != 0 && std::fabs(falling.minY - feet) < 0.0001f &&
                !boxOverlapsSolid(world, falling) && boxSupported(world, falling),
                "feet %.4f flags=%d", (double)falling.minY, dropped);

    // The rest probe must not bite a body that is leaving the floor. A swim is exactly this shape:
    // 1.15 m/s is 0.019 of a block per frame, less than VOXEL_GROUND_PROBE, so a settle snap puts the
    // body back on the bottom and reports it grounded - which is how every creature in water ended up
    // walking on the floor of it instead of surfacing.
    VoxelBox rising = probeBody(startX, feet, midZ);
    const int rose = moveBoxVoxel(world, rising, 0.0f, 0.019f, 0.0f);
    probe.check("a rise slower than the ground probe is not cancelled by the snap to the floor",
                std::fabs(rising.minY - (feet + 0.019f)) < 0.0001f && (rose & VOXEL_HIT_GROUND) == 0,
                "feet %.4f -> %.4f, flags=%d", (double)feet, (double)rising.minY, rose);

    patch.set(wx, ay + 1, wz, BLOCK_STONE);
    // The lane behind the body has to be air for the miss test to mean "the ray found nothing"
    // rather than "the ray stopped at a hill". Eight cells is the distance the ray is allowed to go.
    for (int x = ax - 9; x < ax; x++)
        for (int y = ay; y <= ay + 3; y++) patch.set(x, y, wz, BLOCK_AIR);
    const VoxelHit h = raycastVoxel(world, startX, feet + 1.4f, midZ, 1.0f, 0.0f, 0.0f, 8.0f);
    probe.check("a ray reports the cell it entered and the face it crossed",
                h.hit && h.x == wx && h.z == wz && h.nx == -1 && h.ny == 0 && h.nz == 0,
                "hit %d,%d,%d n=%d,%d,%d dist %.2f", h.x, h.y, h.z, h.nx, h.ny, h.nz, (double)h.dist);

    const VoxelHit away = raycastVoxel(world, startX, feet + 1.4f, midZ, -1.0f, 0.0f, 0.0f, 8.0f);
    probe.check("a ray that reaches nothing says so instead of reporting the nearest air",
                !away.hit, "hit=%d id=%d dist %.2f", away.hit ? 1 : 0, (int)away.id, (double)away.dist);
}

}  // namespace voxel
