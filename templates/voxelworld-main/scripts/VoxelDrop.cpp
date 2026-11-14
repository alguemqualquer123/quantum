// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelDrop.h"

#include "VoxelBlocks.h"
#include "VoxelCollide.h"
#include "VoxelMesh.h"
#include "VoxelProbe.h"
#include "VoxelTiles.h"
#include "VoxelWorld.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "math/AABB.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace voxel {

using namespace quantum;

bool (*dropCollectFn)(uint16_t, int, float, float, float) = nullptr;

namespace {

// One interleaved vertex, 48 bytes, in the same order the chunk mesher and the model rig declare:
// one vertex format is one pipeline for the whole game.
struct DropVertex {
    float p[3];
    float n[3];
    float uv[2];
    float c[4];
};

struct DropFace {
    int n[3];
    int t1[3];
    int t2[3];
    float shade;
};

// cross(t1,t2) == n, corners walked (-1,-1) (1,-1) (1,1) (-1,1) counter-clockwise seen from outside.
// The mesher's table, copied rather than shared because VoxelMesh.cpp keeps its own static.
const DropFace g_dropFaces[FACE_COUNT] = {
    { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, 0.80f },   // +X
    { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 0.80f },  // -X
    { { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 }, 1.00f },   // +Y
    { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, 0.52f },  // -Y
    { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, 0.68f },   // +Z
    { { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0, 0 }, 0.74f },  // -Z
};

struct Drop {
    Entity entity = NULL_ENTITY;
    uint16_t meshType = 0;    // the block the uploaded cube draws; 0 = nothing built in this slot
    bool alive = false;
    uint16_t type = 0;
    int amount = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;   // the cube's centre, in world blocks
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    float age = 0.0f;
    float askTimer = 0.0f;   // a full inventory is refused; the next ask waits DROP_ASK_SECONDS
    bool grounded = false;
};

struct DropPool {
    VoxelWorld* world = nullptr;
    Scene* scene = nullptr;
    Drop drops[MAX_DROPS];
    float mergeTimer = 0.0f;
    uint64_t spawnedTotal = 0;
    uint64_t pickedTotal = 0;
    uint64_t mergedTotal = 0;
    uint64_t expiredTotal = 0;
    uint64_t culledTotal = 0;
    uint64_t lostTotal = 0;
    uint32_t rng = 0x9e3779b9u;
};

static DropPool g_drops;

// xorshift32, like the mob pool: enough to make a hop look tossed, and nothing to reset. A saved
// world does not store items, so no number here has to be reproducible across runs.
static uint32_t nextRandom(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static float randomFloat(uint32_t& s) {
    return (float)(nextRandom(s) >> 8) / 16777216.0f;   // 24 bits over 2^24: [0, 1)
}

static const float DROP_HALF = DROP_CUBE * 0.5f;
// A cube turning inside a box that does not: the AABB is the circumscribed sphere of the cube, so
// the picture can never be culled while it is still on screen.
static const float DROP_AABB = DROP_CUBE * 0.87f;

static VoxelBox dropBox(const Drop& d) {
    VoxelBox b;
    b.minX = d.x - DROP_HALF;
    b.maxX = d.x + DROP_HALF;
    b.minY = d.y - DROP_HALF;
    b.maxY = d.y + DROP_HALF;
    b.minZ = d.z - DROP_HALF;
    b.maxZ = d.z + DROP_HALF;
    return b;
}

static void centreFromBox(const VoxelBox& b, Drop& d) {
    d.x = (b.minX + b.maxX) * 0.5f;
    d.y = (b.minY + b.maxY) * 0.5f;
    d.z = (b.minZ + b.maxZ) * 0.5f;
}

// The cube, in local space with its centre at the origin: the Transform *is* the item's centre, so
// the spin turns it about the middle of the picture and the collision box and the mesh cannot drift.
static void buildDropGeometry(MeshComponent& mesh, uint16_t type, float lum) {
    const BlockDef& def = block(type);
    const float tl = def.r / 255.0f, tg = def.g / 255.0f, tb = def.b / 255.0f;

    std::vector<DropVertex> verts;
    std::vector<uint32_t> idx;
    verts.reserve(FACE_COUNT * 4);
    idx.reserve(FACE_COUNT * 6);

    const float sign[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int f = 0; f < FACE_COUNT; f++) {
        const DropFace& fs = g_dropFaces[f];
        const VoxelTileRect tile = quantum::voxelTileRect(def.tiles[f]);
        const uint32_t base = (uint32_t)verts.size();
        for (int i = 0; i < 4; i++) {
            const float s1 = sign[i][0], s2 = sign[i][1];
            DropVertex v;
            v.p[0] = (float)fs.n[0] * DROP_HALF + (float)fs.t1[0] * s1 * DROP_HALF +
                     (float)fs.t2[0] * s2 * DROP_HALF;
            v.p[1] = (float)fs.n[1] * DROP_HALF + (float)fs.t1[1] * s1 * DROP_HALF +
                     (float)fs.t2[1] * s2 * DROP_HALF;
            v.p[2] = (float)fs.n[2] * DROP_HALF + (float)fs.t1[2] * s1 * DROP_HALF +
                     (float)fs.t2[2] * s2 * DROP_HALF;
            v.n[0] = (float)fs.n[0]; v.n[1] = (float)fs.n[1]; v.n[2] = (float)fs.n[2];
            const float u = (s1 + 1.0f) * 0.5f, vv = (s2 + 1.0f) * 0.5f;
            // s2 = -1 is the foot of the face and v1 the tile's bottom edge: the atlas is sampled with
            // v = 0 at its first row, so up on the block is down in v - the mesher's rule again.
            v.uv[0] = tile.u0 + (tile.u1 - tile.u0) * u;
            v.uv[1] = tile.v1 + (tile.v0 - tile.v1) * vv;
            const float shade = fs.shade;
            v.c[0] = tl * shade * lum;
            v.c[1] = tg * shade * lum;
            v.c[2] = tb * shade * lum;
            v.c[3] = 1.0f;
            verts.push_back(v);
        }
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    }

    mesh.buffer.clearAll();
    // InterleavedBuffer declares its own addAttribute overloads, which hide the typed ones on
    // Buffer, so the explicit data type + byte offset has to go through a Buffer& view.
    Buffer& geom = mesh.buffer;
    geom.addAttribute(AttributeType::POSITION, AttributeDataType::FLOAT, 3, offsetof(DropVertex, p));
    geom.addAttribute(AttributeType::NORMAL, AttributeDataType::FLOAT, 3, offsetof(DropVertex, n));
    geom.addAttribute(AttributeType::TEXCOORD1, AttributeDataType::FLOAT, 2, offsetof(DropVertex, uv));
    geom.addAttribute(AttributeType::COLOR, AttributeDataType::FLOAT, 4, offsetof(DropVertex, c));
    mesh.buffer.setStride((unsigned int)sizeof(DropVertex));
    mesh.buffer.setUsage(BufferUsage::IMMUTABLE);

    mesh.indices.clearAll();
    mesh.indices.addAttribute(AttributeType::INDEX, AttributeDataType::UNSIGNED_INT, 1, 0);
    mesh.indices.setStride((unsigned int)sizeof(uint32_t));
    mesh.indices.setUsage(BufferUsage::IMMUTABLE);

    mesh.buffer.importData(verts.data(), verts.size() * sizeof(DropVertex));
    mesh.buffer.setCount((unsigned int)verts.size());
    mesh.indices.importData(idx.data(), idx.size() * sizeof(uint32_t));
    mesh.indices.setCount((unsigned int)idx.size());

    mesh.vertexCount = (unsigned int)verts.size();
    const AABB bounds(Vector3(-DROP_AABB, -DROP_AABB, -DROP_AABB),
                      Vector3(DROP_AABB, DROP_AABB, DROP_AABB));
    mesh.aabb = bounds;
    mesh.verticesAABB = bounds;
    mesh.needUpdateAABB = false;
    mesh.needReload = true;      // buffers changed size: rebuild the pipeline bindings
    mesh.loaded = false;
}

// The block's own light, baked once when the cube is built. Voxel light is vertex colour, and an
// item that re-lit itself every frame would re-upload an IMMUTABLE buffer every frame for a number
// that only changes if the sky does.
static float dropLuminance(VoxelWorld& world, float x, float y, float z) {
    const int sky = world.getSky((int)std::floor(x), (int)std::floor(y), (int)std::floor(z));
    const int blk = world.getBlockLight((int)std::floor(x), (int)std::floor(y), (int)std::floor(z));
    float v = (float)(sky > blk ? sky : blk) / (float)LIGHT_MAX;
    v *= std::max(0.25f, meshDaylight());
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return 0.10f + 0.90f * std::pow(v, 0.65f);
}

static Entity makeDropEntity(Scene* scene, int slot, uint16_t type, float lum) {
    char nameBuf[32];
    std::snprintf(nameBuf, sizeof(nameBuf), "VoxelDrop_%d", slot);

    Entity e = scene->createEntity();
    scene->setEntityName(e, std::string(nameBuf));

    MeshComponent mc;
    // The chunk material, with the chunk atlas: MASK alpha is what lets a flower's cube keep its
    // shape, and NEAREST filtering is what makes a 16x16 tile legible on a third-of-a-block cube.
    setupVoxelMaterial(mc, false);
    buildDropGeometry(mc, type, lum);
    scene->addComponent<MeshComponent>(e, std::move(mc));

    Transform t;
    t.position = Vector3(0.0f, -32.0f, 0.0f);
    t.rotation = Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    t.scale = Vector3(1.0f, 1.0f, 1.0f);
    t.visible = false;
    scene->addComponent<Transform>(e, t);
    return e;
}

// Only the transform moves: position, spin and bob are the whole animation, so an item costs one
// write per frame and no buffer traffic at all.
static void placeDropEntity(Scene* scene, const Drop& d) {
    if (d.entity == NULL_ENTITY || !scene->isEntityCreated(d.entity)) return;
    Transform* t = scene->findComponent<Transform>(d.entity);
    if (!t) return;
    t->position = Vector3(d.x, d.y + (d.grounded ? dropBobY(d.age) : 0.0f), d.z);
    Quaternion q;
    q.fromEulerAngles(0.0f, dropSpinDeg(d.age), 0.0f, RotationOrder::YXZ);
    t->rotation = q;
    t->visible = true;
    t->needUpdate = true;
}

static void hideDropEntity(Scene* scene, const Drop& d) {
    if (d.entity == NULL_ENTITY || !scene->isEntityCreated(d.entity)) return;
    Transform* t = scene->findComponent<Transform>(d.entity);
    if (t) {
        t->visible = false;
        t->needUpdate = true;
    }
}

// The pool belongs to one world, exactly like the mob pool: a reload or a join hands us a different
// VoxelWorld whose chunks have nothing to do with the entities made against the old one.
static void rebindPool(VoxelWorld* world, Scene* scene) {
    if (g_drops.world == world && g_drops.scene == scene) return;

    const bool sameScene = g_drops.scene && g_drops.scene == scene;
    for (int i = 0; i < MAX_DROPS; i++) {
        Drop& d = g_drops.drops[i];
        if (sameScene && d.entity != NULL_ENTITY && scene->isEntityCreated(d.entity))
            scene->destroyEntity(d.entity);
        d.entity = NULL_ENTITY;
        d.meshType = 0;
        d.alive = false;
    }
    g_drops.world = world;
    g_drops.scene = scene;
    g_drops.mergeTimer = 0.0f;
    g_drops.spawnedTotal = 0;
    g_drops.pickedTotal = 0;
    g_drops.mergedTotal = 0;
    g_drops.expiredTotal = 0;
    g_drops.culledTotal = 0;
    g_drops.lostTotal = 0;
}

static void releaseDrop(Scene* scene, int slot) {
    Drop& d = g_drops.drops[slot];
    hideDropEntity(scene, d);
    d.alive = false;
    d.amount = 0;
}

static int freeDropSlot() {
    for (int i = 0; i < MAX_DROPS; i++) {
        if (!g_drops.drops[i].alive) return i;
    }
    return -1;
}

// The nearest pile of the same block that still has room, within `radius`. This is what makes
// mining a vein produce one stack on the ground instead of thirty cubes to walk over.
static int mergeTarget(VoxelWorld& world, float x, float y, float z, uint16_t type, int amount,
                       float radius) {
    int best = -1;
    float bestSq = radius * radius;
    for (int i = 0; i < MAX_DROPS; i++) {
        const Drop& d = g_drops.drops[i];
        if (!d.alive || d.type != type || d.amount + amount > DROP_MAX_STACK) continue;
        if (std::fabs(d.y - y) > 1.6f) continue;
        const float dx = d.x - x, dz = d.z - z;
        const float sq = dx * dx + dz * dz;
        if (sq < bestSq && world.getTop((int)std::floor(d.x), (int)std::floor(d.z)) >= 0) {
            bestSq = sq;
            best = i;
        }
    }
    return best;
}

static void mergePass(float dt) {
    g_drops.mergeTimer += dt;
    if (g_drops.mergeTimer < DROP_MERGE_SECONDS) return;
    g_drops.mergeTimer = 0.0f;

    const float radiusSq = DROP_MERGE_RADIUS * DROP_MERGE_RADIUS;
    for (int i = 0; i < MAX_DROPS; i++) {
        Drop& a = g_drops.drops[i];
        if (!a.alive) continue;
        for (int j = i + 1; j < MAX_DROPS; j++) {
            Drop& b = g_drops.drops[j];
            if (!b.alive || b.type != a.type) continue;
            if (a.amount + b.amount > DROP_MAX_STACK) continue;
            const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
            if (dx * dx + dy * dy + dz * dz > radiusSq) continue;
            a.amount += b.amount;
            b.amount = 0;
            releaseDrop(g_drops.scene, j);
            g_drops.mergedTotal++;
        }
    }
}

}  // namespace

// -------------------------------------------------------------------------------------- public API

float dropSpinDeg(float age) {
    if (age <= 0.0f) return 0.0f;
    float deg = std::fmod(age * DROP_SPIN_DEG_PER_SEC, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    return deg;
}

float dropBobY(float age) {
    return std::sin(age * DROP_BOB_RATE) * DROP_BOB;
}

bool spawnVoxelDrop(VoxelWorld& world, float x, float y, float z, uint16_t type, int count) {
    Scene* scene = world.scene();
    if (!scene || count <= 0) return false;
    type = stateType(type);
    if (type == BLOCK_AIR || type >= BLOCK_COUNT) return false;
    rebindPool(&world, scene);

    int left = count;
    bool any = false;

    // Fill the pile that is already there before asking for an entity: one cube on the ground is
    // both cheaper to draw and easier to walk over than thirty.
    const int join = mergeTarget(world, x, y, z, type, 0, DROP_MERGE_RADIUS * 4.0f);
    if (join >= 0) {
        Drop& d = g_drops.drops[join];
        const int take = std::min(left, DROP_MAX_STACK - d.amount);
        d.amount += take;
        left -= take;
        any = true;
        g_drops.mergedTotal++;
    }

    while (left > 0) {
        const int slot = freeDropSlot();
        if (slot < 0) {
            g_drops.lostTotal++;
            break;
        }
        const int take = std::min(left, DROP_MAX_STACK);
        Drop& d = g_drops.drops[slot];
        d.alive = true;
        d.type = type;
        d.amount = take;
        d.x = x; d.y = y; d.z = z;
        // A hop and a shove: a block that simply appeared where the voxel used to be would sit inside
        // the floor it fell through, and an item that never moves looks like a texture glitch.
        const float ang = randomFloat(g_drops.rng) * 6.28318530718f;
        const float push = 0.5f + randomFloat(g_drops.rng) * 0.9f;
        d.vx = std::cos(ang) * push;
        d.vz = std::sin(ang) * push;
        d.vy = DROP_THROW_UP * (0.75f + randomFloat(g_drops.rng) * 0.5f);
        d.age = 0.0f;
        d.askTimer = 0.0f;
        d.grounded = false;

        // One cube per slot for the life of the world, and the buffers are IMMUTABLE GPU allocations,
        // so a slot that changes what it draws is rebuilt - which is what makes mining forty stone and
        // forty dirt cost two uploads rather than eighty.
        if (d.meshType != type) {
            if (d.entity != NULL_ENTITY && scene->isEntityCreated(d.entity))
                scene->destroyEntity(d.entity);
            d.entity = makeDropEntity(scene, slot, type, dropLuminance(world, x, y, z));
            d.meshType = type;
        }
        placeDropEntity(scene, d);

        left -= take;
        any = true;
        g_drops.spawnedTotal++;
    }
    return any;
}

void updateVoxelDrops(VoxelWorld& world, float dt, float eyeX, float eyeY, float eyeZ,
                      float despawnRadius) {
    Scene* scene = world.scene();
    if (!scene) return;
    rebindPool(&world, scene);
    if (dt < 0.0f) dt = 0.0f;
    if (dt > DROP_MAX_FRAME) dt = DROP_MAX_FRAME;
    if (despawnRadius < 1.0f) despawnRadius = 1.0f;

    const float chestY = eyeY - DROP_CHEST_BELOW_EYE;
    const float despawnSq = despawnRadius * despawnRadius;
    const float magnetSq = DROP_MAGNET_RADIUS * DROP_MAGNET_RADIUS;
    const float pickSq = DROP_PICKUP_RADIUS * DROP_PICKUP_RADIUS;

    for (int i = 0; i < MAX_DROPS; i++) {
        Drop& d = g_drops.drops[i];
        if (!d.alive) continue;

        const float ex = d.x - eyeX, ez = d.z - eyeZ;
        // Out of sight or out of a streamed column: either there is nothing left to simulate, or the
        // floor it rested on no longer exists and it would fall forever.
        if (ex * ex + ez * ez > despawnSq ||
            world.getTop((int)std::floor(d.x), (int)std::floor(d.z)) < 0) {
            releaseDrop(scene, i);
            g_drops.culledTotal++;
            continue;
        }

        d.age += dt;
        if (d.age > DROP_LIFETIME) {
            releaseDrop(scene, i);
            g_drops.expiredTotal++;
            continue;
        }

        // ---- pull, then integrate, then resolve against the grid ----
        const float tx = eyeX - d.x, ty = chestY - d.y, tz = eyeZ - d.z;
        const float tsq = tx * tx + ty * ty + tz * tz;
        float dx = d.vx * dt, dy = d.vy * dt, dz = d.vz * dt;

        if (d.age > DROP_DELAY && tsq < magnetSq && tsq > 0.0001f) {
            const float dist = std::sqrt(tsq);
            const float k = DROP_MAGNET_SPEED * dt / dist;
            dx += tx * k; dy += ty * k; dz += tz * k;
        } else {
            d.vy -= DROP_GRAVITY * dt;
            if (d.vy < -40.0f) d.vy = -40.0f;
        }

        VoxelBox box = dropBox(d);
        const int hits = moveBoxVoxel(world, box, dx, dy, dz);
        centreFromBox(box, d);

        const bool inLiquid = (hits & VOXEL_IN_LIQUID) != 0;
        d.grounded = (hits & VOXEL_HIT_GROUND) != 0;

        if (d.grounded && d.vy < 0.0f) d.vy = 0.0f;
        if (inLiquid) {
            // Water lifts an item instead of burying it, which is what makes a drop that landed in
            // the sea recoverable rather than lost.
            d.vy += DROP_SWIM_UP * dt;
            if (d.vy > 2.0f) d.vy = 2.0f;
            d.vx *= 0.86f; d.vz *= 0.86f;
        } else if (d.grounded) {
            d.vx *= 0.72f; d.vz *= 0.72f;
        }

        placeDropEntity(scene, d);

        // Measured from where the item ended up, not from where it started the frame: the magnet's own
        // pull is what usually closes the last few centimetres, and the stale distance would leave the
        // item hovering one frame longer than it needs to.
        if (d.askTimer > 0.0f) d.askTimer -= dt;
        const float gx = eyeX - d.x, gy = chestY - d.y, gz = eyeZ - d.z;
        if (d.age > DROP_DELAY && gx * gx + gy * gy + gz * gz < pickSq && dropCollectFn &&
            d.askTimer <= 0.0f) {
            if (dropCollectFn(d.type, d.amount, d.x, d.y, d.z)) {
                g_drops.pickedTotal++;
                releaseDrop(scene, i);
                continue;
            }
            // The inventory is full: the item stays here and asks again later rather than spending the
            // frame calling a collector that has already said no.
            d.askTimer = DROP_ASK_SECONDS;
        }
    }

    mergePass(dt);
}

int liveDropCount() {
    int n = 0;
    for (int i = 0; i < MAX_DROPS; i++) {
        if (g_drops.drops[i].alive) n++;
    }
    return n;
}

std::string dropStats() {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Drops: %d (%llu spawned, %llu picked, %llu merged, %llu expired, %llu lost)",
                  liveDropCount(), (unsigned long long)g_drops.spawnedTotal,
                  (unsigned long long)g_drops.pickedTotal, (unsigned long long)g_drops.mergedTotal,
                  (unsigned long long)g_drops.expiredTotal, (unsigned long long)g_drops.lostTotal);
    return std::string(buf);
}

uint64_t dropsSpawned() { return g_drops.spawnedTotal; }
uint64_t dropsPickedUp() { return g_drops.pickedTotal; }
uint64_t dropsMerged() { return g_drops.mergedTotal; }
uint64_t dropsExpired() { return g_drops.expiredTotal; }
uint64_t dropsLost() { return g_drops.lostTotal; }

void clearVoxelDrops() {
    Scene* scene = g_drops.scene;
    for (int i = 0; i < MAX_DROPS; i++) {
        if (!g_drops.drops[i].alive) continue;
        if (scene) releaseDrop(scene, i);
        else { g_drops.drops[i].alive = false; }
    }
}

bool dropSnapshot(int slot, DropSnapshot& out) {
    if (slot < 0 || slot >= MAX_DROPS) return false;
    const Drop& d = g_drops.drops[slot];
    if (!d.alive) return false;
    out.type = d.type;
    out.amount = d.amount;
    out.x = d.x; out.y = d.y; out.z = d.z;
    out.spinDeg = dropSpinDeg(d.age);
    out.bobY = d.grounded ? dropBobY(d.age) : 0.0f;
    out.grounded = d.grounded;
    return true;
}

// __________________________________________________________________________________ behaviour probe

namespace {

// What the probe pretends to hold: everything the collector was asked for, and whether it was
// allowed to take it. A probe that only checked "the item vanished" would pass with a collector that
// silently swallowed the world.
struct FakeInventory {
    uint16_t lastType = 0;
    int lastCount = 0;
    int asks = 0;
    bool accept = true;
};

FakeInventory g_fake;

static bool fakeCollect(uint16_t type, int count, float, float, float) {
    g_fake.asks++;
    if (!g_fake.accept) return false;
    g_fake.lastType = type;
    g_fake.lastCount = count;
    return true;
}

static int findDropSlot(uint16_t type) {
    for (int i = 0; i < MAX_DROPS; i++) {
        if (g_drops.drops[i].alive && g_drops.drops[i].type == type) return i;
    }
    return -1;
}

}  // namespace

void probeVoxelDrops(VoxelWorld& world, Probe& probe) {
    ProbeModule module(probe, "drops");
    Scene* scene = world.scene();
    if (!scene) {
        probe.skip("no scene to build a cube in");
        return;
    }

    int ax = 0, ay = 0, az = 0;
    VoxelPatch patch(world);
    if (!carveArena(world, patch, ax, ay, az)) {
        probe.skip("the focus column has not streamed");
        return;
    }

    // Everything here measures the chest the magnet aims at, so the probe has to hand over the same
    // number the host does - and stand far away with it. The magnet and the pickup test are the only
    // parts of this module that care where the body is; run the landing, spin and merge checks with
    // the eye on top of the item and the item is collected before any of them can measure it.
    const float floorTop = (float)ay;              // the arena's air starts here, one above the stone
    const float eyeY = floorTop + 1.62f;
    const float eyeZ = (float)az + 0.5f;
    const float farX = (float)ax + 0.5f + 20.0f;   // past the magnet, well inside the despawn radius
    const float nearX = (float)ax + 0.5f;

    const bool oldAccept = g_fake.accept;
    const bool hadCollector = (dropCollectFn != nullptr);
    bool (*oldCollect)(uint16_t, int, float, float, float) = dropCollectFn;
    dropCollectFn = fakeCollect;
    g_fake = FakeInventory();
    g_fake.accept = false;   // nothing is taken until the checks that are about taking it

    const float dt = 1.0f / 60.0f;
    auto step = [&](int frames, float eyeX) {
        for (int i = 0; i < frames; i++) updateVoxelDrops(world, dt, eyeX, eyeY, eyeZ, 64.0f);
    };

    const uint16_t TEST_TYPE = (uint16_t)BLOCK_STONE;

    clearVoxelDrops();
    const int before = liveDropCount();
    const bool born = spawnVoxelDrop(world, nearX, floorTop + 0.9f, eyeZ, TEST_TYPE, 1);
    int slot = findDropSlot(TEST_TYPE);
    DropSnapshot snap;
    probe.check("a dropped block appears in the world, not in the hand",
                born && slot >= 0 && liveDropCount() == before + 1,
                "born %d slot %d live %d", born ? 1 : 0, slot, liveDropCount());

    const bool heldOne = slot >= 0 && dropSnapshot(slot, snap) && snap.amount == 1 &&
                         snap.type == TEST_TYPE && snap.y > floorTop;
    probe.check("the new item is one of the block it was dropped as and starts above the floor",
                heldOne, "type %d amount %d y %.2f floor %.2f", (int)snap.type, snap.amount,
                (double)snap.y, (double)floorTop);

    // Let it land: the arena is four blocks of air over stone, so a hop plus gravity settles it on
    // the floor within a fraction of a second.
    step(90, farX);
    slot = findDropSlot(TEST_TYPE);
    bool rested = false;
    if (slot >= 0 && dropSnapshot(slot, snap))
        rested = snap.grounded && std::fabs(snap.y - (floorTop + DROP_HALF)) < 0.08f;
    probe.check("gravity stops at the block under it instead of falling through", rested,
                "slot %d y %.3f grounded %d", slot, (double)snap.y, snap.grounded ? 1 : 0);

    const float spinEarly = snap.spinDeg;
    const float restY = snap.y;
    step(30, farX);
    slot = findDropSlot(TEST_TYPE);
    float spinLate = 0.0f;
    if (slot >= 0 && dropSnapshot(slot, snap)) spinLate = snap.spinDeg;
    // 0.5 s at 90 deg/s is a quarter turn; the wrap is why this is a difference and not an equality.
    const float turned = spinLate >= spinEarly ? spinLate - spinEarly : (360.0f - spinEarly) + spinLate;
    probe.check("the item turns slowly while it lies there", turned > 30.0f && turned < 60.0f,
                "advanced %.1f deg in 0.5 s", (double)turned);

    // The bob is a sine, so one reading of it only proves it is inside its own amplitude. What makes
    // it a bob is that the height moves without the item leaving the floor it rests on. Forty frames
    // is most of a full period, which is what makes the "more than half the amplitude" bound below
    // something the phase of the item cannot dodge.
    float bobTravelled = 0.0f, bobHighest = 0.0f, groundedAllAlong = snap.grounded ? 1.0f : 0.0f;
    for (int frame = 0; frame < 40; frame++) {
        step(1, farX);
        DropSnapshot now;
        if (!dropSnapshot(slot, now)) break;
        bobTravelled = std::max(bobTravelled, std::fabs(now.bobY - snap.bobY));
        bobHighest = std::max(bobHighest, std::fabs(now.bobY));
        groundedAllAlong = now.grounded ? groundedAllAlong : 0.0f;
    }
    probe.check("a resting item bobs on the spot",
                groundedAllAlong > 0.0f && bobHighest > DROP_BOB * 0.5f && bobTravelled > 0.005f &&
                    bobHighest <= DROP_BOB + 0.001f,
                "highest %.3f of %.3f, moved %.3f", (double)bobHighest, (double)DROP_BOB,
                (double)bobTravelled);

    // Two piles of the same block, side by side, must join into one stack of two - one cube to walk
    // over rather than two, which is what makes mining a vein bearable.
    const int mergedBefore = (int)dropsMerged();
    const int liveBefore = liveDropCount();
    spawnVoxelDrop(world, snap.x + 0.2f, snap.y, snap.z, TEST_TYPE, 1);
    step(30, farX);
    slot = findDropSlot(TEST_TYPE);
    int joinedAmount = 0;
    if (slot >= 0 && dropSnapshot(slot, snap)) joinedAmount = snap.amount;
    probe.check("two neighbouring piles of the same block become one stack of two",
                joinedAmount == 2 && (int)dropsMerged() > mergedBefore && liveDropCount() == liveBefore,
                "amount %d, merged %llu -> %llu, live %d -> %d", joinedAmount,
                (unsigned long long)mergedBefore, (unsigned long long)dropsMerged(),
                liveBefore, liveDropCount());

    // Now the body walks in on the item. The collector still says no - an inventory that is full has
    // to leave the item in the world, hovering at the chest the magnet aims at rather than buried in
    // the floor it was resting on.
    const int asksBefore = g_fake.asks;
    step(20, nearX);
    slot = findDropSlot(TEST_TYPE);
    float liftedTo = -1000.0f;
    if (slot >= 0 && dropSnapshot(slot, snap)) liftedTo = snap.y;
    probe.check("a body that cannot take it leaves the item in the world",
                liveDropCount() == liveBefore && g_fake.asks > asksBefore,
                "live %d, %d asks refused", liveDropCount(), g_fake.asks - asksBefore);
    probe.check("the magnet pulls a nearby item up to the chest",
                liftedTo > restY + 0.15f && std::fabs(liftedTo - (eyeY - DROP_CHEST_BELOW_EYE)) < 0.3f,
                "y %.3f -> %.3f, chest %.3f", (double)restY, (double)liftedTo,
                (double)(eyeY - DROP_CHEST_BELOW_EYE));

    // And the same item, in the same place, does leave when the answer is yes. It may have to wait
    // out the refusal's retry timer, which is the point of that timer and not an obstacle to it.
    g_fake.accept = true;
    const int pickedBefore = (int)dropsPickedUp();
    for (int frame = 0; frame < 40 && liveDropCount() > 0; frame++)
        updateVoxelDrops(world, dt, nearX, eyeY, eyeZ, 64.0f);
    probe.check("freeing a slot takes the whole stack",
                liveDropCount() == 0 && (int)dropsPickedUp() > pickedBefore &&
                    g_fake.lastType == TEST_TYPE && g_fake.lastCount == 2,
                "live %d picked %llu->%llu last %d x%d", liveDropCount(),
                (unsigned long long)pickedBefore, (unsigned long long)dropsPickedUp(),
                (int)g_fake.lastType, g_fake.lastCount);

    clearVoxelDrops();
    g_fake.accept = oldAccept;
    dropCollectFn = hadCollector ? oldCollect : nullptr;
    probe.check("the probe leaves no item in the world", liveDropCount() == 0,
                "live %d after clearing", liveDropCount());
}

}  // namespace voxel
