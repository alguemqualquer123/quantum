// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelEntity.h"

#include "VoxelCollide.h"
#include "VoxelConfig.h"
#include "VoxelBlocks.h"
#include "VoxelProbe.h"
#include "VoxelWorld.h"

// The model module is this module's one optional dependency: where it is compiled in, a mob is six
// posed, skinned boxes instead of one tinted primitive, and the flat box below is not built at all.
// Where it is not, the pool still draws every mob with the geometry it makes for itself.
#if defined(__has_include)
#  if __has_include("VoxelModel.h")
#    include "VoxelModel.h"
#    define VOXEL_HAVE_MODEL 1
#  endif
#endif

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "math/AABB.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "texture/Material.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace voxel {

using namespace quantum;

// The voxel grid is the collider. A mob carries a Transform and nothing else physical: no Body3D, no
// collider, no physics scene, because BlockDef::shapes already describe every surface it can touch
// and moveBoxVoxel() resolves them exactly. What hangs off that Transform is a skinned rig when the
// model module is in the build and a single tinted box when it is not.

static const int MAX_MOBS = 32;              // pool cap: what is on screen, not a population target
static const float MOB_HALF = 0.45f;         // 0.9 x 1.4 x 0.9 body: fits a 1-block doorway diagonally
static const float MOB_HEIGHT = 1.4f;        // ... and stops under a 2-block ceiling with room to jump
static const float MOB_GRAVITY = 28.0f;      // the player's gravity, so both fall on the same beat
static const float MOB_JUMP = 7.5f;
static const float MOB_SPEED = 1.9f;
static const float MOB_TERMINAL = 40.0f;
static const float MOB_SWIM_UP = 1.15f;      // buoyancy: a pushed-up ease, not a rocket
static const float MOB_MAX_FRAME = 0.1f;     // a hitch must not teleport a mob through a wall
static const float MOB_LOOK_AHEAD = 0.35f;   // how far past the body the cliff probe reaches
static const float MOB_LIQUID_SLOW = 0.55f;  // water is thick: the same fraction of the walk speed

#ifdef VOXEL_HAVE_MODEL
// The skin every mob wears. A path, never in-memory pixels: the engine's TextureData is a shallow
// copy that frees with stbi_image_free when it believes it owns the buffer.
static const char* const MOB_SKIN_PATH = "textures/skins/default.png";
static const float MOB_STRIDE = 1.6f;        // blocks per full walk cycle, so the feet match the limbs
#endif

struct MobTint {
    float r, g, b;
};

// A critter is a colour, not a species: the pool reuses one box primitive for all of them.
static const MobTint MOB_TINTS[4] = {
    { 0.42f, 0.72f, 0.36f },   // grass side
    { 0.64f, 0.49f, 0.32f },   // dirt side
    { 0.56f, 0.59f, 0.64f },   // stone side
    { 0.76f, 0.52f, 0.60f },   // the odd one out
};
static const int MOB_TINT_COUNT = 4;

// -------------------------------------------------------------------------------- mob bookkeeping

struct Mob {
    Entity entity = NULL_ENTITY;
    bool built = false;      // this slot's picture was uploaded once and stays for the whole session
    bool alive = false;
    float x = 0.0f, y = 0.0f, z = 0.0f;   // feet centre, in world blocks
    float vy = 0.0f;
    float heading = 0.0f;    // radians, on the XZ plane
    float turnTimer = 0.0f;
    float jumpCooldown = 0.0f;
    int kind = 0;            // tint, picked when the slot's mesh was built and never changed again
    bool grounded = false;
    bool inLiquid = false;
#ifdef VOXEL_HAVE_MODEL
    // `entity` is the anchor: it carries the Transform every claim below measures, and the rig's six
    // parts carry the picture. walkPhase advances by the distance actually walked, so a mob stopped
    // mid-step plants its feet instead of sliding, and lastX/lastZ are what that distance is measured
    // from (only the XZ travel counts: a fall is not a step).
    VoxelModelRig rig;
    float walkPhase = 0.0f;
    float lastX = 0.0f, lastZ = 0.0f;
    bool lastValid = false;
#endif
};

struct MobPool {
    VoxelWorld* world = nullptr;
    Scene* scene = nullptr;
    Mob mobs[MAX_MOBS];
    int spawnedTotal = 0;
    int culledTotal = 0;
    uint32_t rng = 0x9e3779b9u;
};

static MobPool g_pool;

// xorshift32: enough to make a wander look random, and it needs no RNG state to reset. A saved
// world does not store mobs, so nothing here has to be reproducible across runs.
static uint32_t nextRandom(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static float randomFloat(uint32_t& s) {
    return (float)(nextRandom(s) >> 8) / 16777216.0f;   // 24 bits over 2^24: [0, 1)
}

static VoxelBox mobBox(const Mob& m) {
    VoxelBox b;
    b.minX = m.x - MOB_HALF;
    b.maxX = m.x + MOB_HALF;
    b.minY = m.y;
    b.maxY = m.y + MOB_HEIGHT;
    b.minZ = m.z - MOB_HALF;
    b.maxZ = m.z + MOB_HALF;
    return b;
}

// The pool belongs to one world: a reload or a join hands us a different VoxelWorld, whose chunks
// have nothing to do with the entities we made against the old one, so every slot is torn down and
// starts empty again. The mesh buffers are IMMUTABLE GPU allocations, which makes destroy-and-
// rebuild the only honest option; it happens once per world, never per respawn.
static void rebindPool(VoxelWorld* world, Scene* scene) {
    if (g_pool.world == world && g_pool.scene == scene) return;

    const bool sameScene = g_pool.scene && g_pool.scene == scene;
    for (int i = 0; i < MAX_MOBS; i++) {
        Mob& m = g_pool.mobs[i];
        // Entities live in a Scene: when the scene itself changed, it took them down with it and
        // only a world swap inside one scene can leave ours behind.
        if (sameScene && m.entity != NULL_ENTITY && scene->isEntityCreated(m.entity))
            scene->destroyEntity(m.entity);
#ifdef VOXEL_HAVE_MODEL
        // Same rule for the six parts of the rig, and the same teardown: they are entities of the
        // scene we are leaving, and their buffers are IMMUTABLE GPU allocations.
        if (sameScene) destroyVoxelModelRig(scene, m.rig);
        m.rig = VoxelModelRig();
        m.walkPhase = 0.0f;
        m.lastValid = false;
#endif
        m.entity = NULL_ENTITY;
        m.built = false;
        m.alive = false;
    }
    g_pool.world = world;
    g_pool.scene = scene;
    g_pool.spawnedTotal = 0;
    g_pool.culledTotal = 0;
}

// ------------------------------------------------------------------------------ the box primitive

// The fallback picture, compiled only where the model module is not in the build: with it in, these
// would be dead statics and the project builds the target at /W4 and -Wall -Wextra.
#ifndef VOXEL_HAVE_MODEL

// The same interleaved layout the mesher uploads, minus the texcoord: with no TEXCOORD1 attribute
// the renderer never samples the base-colour texture, so a vertex-coloured box needs no atlas slot.
struct MobVertex {
    float p[3];
    float n[3];
    float c[4];
};

static const unsigned int MOB_VERTEX_STRIDE = sizeof(MobVertex);
static const size_t MOB_OFF_POSITION = offsetof(MobVertex, p);
static const size_t MOB_OFF_NORMAL = offsetof(MobVertex, n);
static const size_t MOB_OFF_COLOR = offsetof(MobVertex, c);

struct MobFaceSpec {
    int n[3];
    int t1[3];
    int t2[3];
    float shade;
};

// Copied from the mesher's table: cross(t1, t2) == n, so walking the corners
// (-1,-1) (1,-1) (1,1) (-1,1) is counter-clockwise seen from outside - the engine's winding - and
// the face shading matches the chunks, which is what makes a mob read as part of this world.
static const MobFaceSpec MOB_FACES[FACE_COUNT] = {
    { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, 0.80f },
    { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 0.80f },
    { { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 }, 1.00f },
    { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, 0.52f },
    { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, 0.68f },
    { { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0, 0 }, 0.74f },
};

static const float MOB_CORNER_S[8] = { -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f };

static void addBoxQuads(std::vector<MobVertex>& verts, std::vector<uint32_t>& idx,
                        float cx, float cy, float cz, float sx, float sy, float sz,
                        float r, float g, float b) {
    for (int f = 0; f < FACE_COUNT; f++) {
        const MobFaceSpec& face = MOB_FACES[f];
        // t1/t2 are axis-aligned unit vectors, so this selects the box's own half-extent along the
        // axis each tangent points to - no per-face table of sizes to keep in sync with the box.
        const float e1 = (std::abs((float)face.t1[0]) * sx + std::abs((float)face.t1[1]) * sy +
                          std::abs((float)face.t1[2]) * sz) * 0.5f;
        const float e2 = (std::abs((float)face.t2[0]) * sx + std::abs((float)face.t2[1]) * sy +
                          std::abs((float)face.t2[2]) * sz) * 0.5f;
        const float center[3] = {
            cx + (float)face.n[0] * sx * 0.5f,
            cy + (float)face.n[1] * sy * 0.5f,
            cz + (float)face.n[2] * sz * 0.5f,
        };
        const float shade = face.shade;
        const uint32_t base = (uint32_t)verts.size();
        for (int i = 0; i < 4; i++) {
            const float s1 = MOB_CORNER_S[i * 2];
            const float s2 = MOB_CORNER_S[i * 2 + 1];
            MobVertex v;
            v.p[0] = center[0] + (float)face.t1[0] * s1 * e1 + (float)face.t2[0] * s2 * e2;
            v.p[1] = center[1] + (float)face.t1[1] * s1 * e1 + (float)face.t2[1] * s2 * e2;
            v.p[2] = center[2] + (float)face.t1[2] * s1 * e1 + (float)face.t2[2] * s2 * e2;
            v.n[0] = (float)face.n[0];
            v.n[1] = (float)face.n[1];
            v.n[2] = (float)face.n[2];
            v.c[0] = r * shade;
            v.c[1] = g * shade;
            v.c[2] = b * shade;
            v.c[3] = 1.0f;
            verts.push_back(v);
        }
        idx.push_back(base + 0);
        idx.push_back(base + 1);
        idx.push_back(base + 2);
        idx.push_back(base + 0);
        idx.push_back(base + 2);
        idx.push_back(base + 3);
    }
}

static void setupMobMaterial(MeshComponent& mesh) {
    mesh.numSubmeshes = 1;
    mesh.loaded = false;
    mesh.loadCalled = false;
    mesh.receiveLights = true;
    mesh.receiveIBL = false;
    mesh.castShadows = true;
    mesh.receiveShadows = true;
    mesh.transparent = false;
    mesh.autoTransparency = false;
    mesh.lodEnabled = false;
    mesh.cullingMode = CullingMode::BACK;

    Material& m = mesh.submeshes[0].material;
    m.name = "voxel_mob";
    m.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    m.metallicFactor = 0.0f;
    m.roughnessFactor = 0.95f;
    m.alphaMode = MaterialAlphaMode::ALPHA_OPAQUE;
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.submeshes[0].faceCulling = true;
    mesh.submeshes[0].needUpdateTexture = false;
}

// Body plus a head, in local space with the feet at y = 0 so the Transform position is the box's
// own feet and the collision box and the picture cannot drift apart. The head is centred: a critter
// with no visible face cannot be drawn facing the wrong way, which is the one thing a symmetric
// primitive is genuinely better at.
static void buildMobGeometry(MeshComponent& mesh, const MobTint& tint) {
    std::vector<MobVertex> verts;
    std::vector<uint32_t> idx;
    verts.reserve(48);
    idx.reserve(72);

    addBoxQuads(verts, idx, 0.0f, 0.50f, 0.0f, 0.82f, 1.00f, 0.66f, tint.r, tint.g, tint.b);
    const float hr = std::min(1.0f, tint.r * 1.18f);
    const float hg = std::min(1.0f, tint.g * 1.18f);
    const float hb = std::min(1.0f, tint.b * 1.18f);
    addBoxQuads(verts, idx, 0.0f, 1.16f, 0.0f, 0.62f, 0.44f, 0.62f, hr, hg, hb);

    mesh.buffer.clearAll();
    // InterleavedBuffer declares its own addAttribute overloads, which hide the typed ones on
    // Buffer, so the explicit data type + byte offset has to go through a Buffer& view.
    Buffer& geom = mesh.buffer;
    geom.addAttribute(AttributeType::POSITION, AttributeDataType::FLOAT, 3, MOB_OFF_POSITION);
    geom.addAttribute(AttributeType::NORMAL, AttributeDataType::FLOAT, 3, MOB_OFF_NORMAL);
    geom.addAttribute(AttributeType::COLOR, AttributeDataType::FLOAT, 4, MOB_OFF_COLOR);
    mesh.buffer.setStride(MOB_VERTEX_STRIDE);
    mesh.buffer.setUsage(BufferUsage::IMMUTABLE);

    mesh.indices.clearAll();
    mesh.indices.addAttribute(AttributeType::INDEX, AttributeDataType::UNSIGNED_INT, 1, 0);
    mesh.indices.setStride((unsigned int)sizeof(uint32_t));
    mesh.indices.setUsage(BufferUsage::IMMUTABLE);

    mesh.buffer.importData(verts.data(), verts.size() * MOB_VERTEX_STRIDE);
    mesh.buffer.setCount((unsigned int)verts.size());
    mesh.indices.importData(idx.data(), idx.size() * (size_t)sizeof(uint32_t));
    mesh.indices.setCount((unsigned int)idx.size());

    mesh.vertexCount = (unsigned int)verts.size();
    // The drawn bounds, not the collision box: a loose AABB only ever culls late, but a wrong one
    // culls a mob that is on screen.
    mesh.aabb = AABB(Vector3(-0.41f, 0.0f, -0.33f), Vector3(0.41f, 1.38f, 0.33f));
    mesh.verticesAABB = mesh.aabb;
    mesh.needUpdateAABB = false;
    mesh.needReload = true;
    mesh.loaded = false;
}

#endif  // !VOXEL_HAVE_MODEL

static Entity makeMobEntity(Scene* scene, int slot, const MobTint& tint) {
    char nameBuf[32];
    std::snprintf(nameBuf, sizeof(nameBuf), "VoxelMob_%d", slot);

#ifdef VOXEL_HAVE_MODEL
    // The anchor. It carries no mesh: it exists so the pool has one entity per mob to name, to place
    // where the sim left it, and to hide, and so the six parts of the rig hang off the same slot.
    Entity e = scene->createEntity();
    scene->setEntityName(e, std::string(nameBuf));

    Transform t;
    t.position = Vector3(0.0f, -32.0f, 0.0f);
    t.rotation = Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    t.scale = Vector3(1.0f, 1.0f, 1.0f);
    t.visible = false;
    scene->addComponent<Transform>(e, t);

    Mob& m = g_pool.mobs[slot];
    // The rig lives on the slot rather than on the anchor entity: the pool already treats one entity
    // per mob as the thing it names and hides, and the model module needs no parent to pose from.
    createVoxelModelRig(scene, m.rig, slot, MOB_SKIN_PATH, tint.r, tint.g, tint.b);
    return e;
#else
    Entity e = scene->createEntity();
    scene->setEntityName(e, std::string(nameBuf));

    MeshComponent mc;
    setupMobMaterial(mc);
    buildMobGeometry(mc, tint);
    scene->addComponent<MeshComponent>(e, std::move(mc));

    Transform t;
    t.position = Vector3(0.0f, -32.0f, 0.0f);
    t.rotation = Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    t.scale = Vector3(1.0f, 1.0f, 1.0f);
    t.visible = false;
    scene->addComponent<Transform>(e, t);
    return e;
#endif
}

// Only the transform moves: position and yaw are the whole animation, so a mob costs one write per
// frame and no buffer traffic at all.
#ifdef VOXEL_HAVE_MODEL
static Transform* rigJointTransform(Scene* scene, int64_t part) {
    const Entity e = (Entity)part;
    if (e == NULL_ENTITY || !scene->isEntityCreated(e)) return nullptr;
    return scene->findComponent<Transform>(e);
}

// The whole animation is a handful of numbers: where the feet are, which way the body faces, how far
// the cycle has turned and how far the limbs swing. The swing is the speed the body actually moves
// at, so a mob wading through water beats its arms slowly and one pinned against a wall stops
// swinging the frame it stops travelling.
static void poseMobModel(Scene* scene, const Mob& m) {
    VoxelModelPose pose;
    pose.x = m.x;
    pose.y = m.y;
    pose.z = m.z;
    pose.yaw = voxelModelYawDeg(m.heading);
    pose.walkPhase = m.walkPhase;
    pose.swing = voxelModelSwing(m.inLiquid ? MOB_SPEED * MOB_LIQUID_SLOW : MOB_SPEED);
    pose.scale = voxelModelScaleForHeight(MOB_HEIGHT);
    pose.visible = true;
    poseVoxelModelRig(scene, m.rig, pose);
}
#endif

static void placeMobEntity(Scene* scene, const Mob& m) {
    if (m.entity == NULL_ENTITY || !scene->isEntityCreated(m.entity)) return;
    Transform* t = scene->findComponent<Transform>(m.entity);
    if (!t) return;
    t->position = Vector3(m.x, m.y, m.z);
    t->visible = true;
    t->needUpdate = true;
#ifdef VOXEL_HAVE_MODEL
    // The anchor carries no geometry, so its rotation is not a picture: the rig parts turn, and the
    // identity here keeps them from being double-rotated by anything that inherits from an entity.
    t->rotation = Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    t->scale = Vector3(1.0f, 1.0f, 1.0f);
    poseMobModel(scene, m);
#else
    Quaternion rot;
    // Yaw in degrees, the same YXZ order the player's camera uses. The box is symmetric, so this is
    // the direction the sim travels rather than a face; the model build derives its yaw from the same
    // heading, through the engine's own convention.
    rot.fromEulerAngles(0.0f, m.heading * 57.2957795131f, 0.0f, RotationOrder::YXZ);
    t->rotation = rot;
#endif
}

static void hideMobEntity(Scene* scene, const Mob& m) {
    if (m.entity == NULL_ENTITY || !scene->isEntityCreated(m.entity)) return;
    Transform* t = scene->findComponent<Transform>(m.entity);
    if (!t) return;
    t->visible = false;
    t->needUpdate = true;
#ifdef VOXEL_HAVE_MODEL
    VoxelModelPose pose;
    pose.x = m.x;
    pose.y = m.y;
    pose.z = m.z;
    pose.scale = voxelModelScaleForHeight(MOB_HEIGHT);
    pose.visible = false;
    poseVoxelModelRig(scene, m.rig, pose);
#endif
}

static void despawnMob(Scene* scene, int slot) {
    Mob& m = g_pool.mobs[slot];
    if (m.entity != NULL_ENTITY && scene->isEntityCreated(m.entity)) hideMobEntity(scene, m);
    m.alive = false;
    g_pool.culledTotal++;
}

static int freeMobSlot() {
    for (int i = 0; i < MAX_MOBS; i++) {
        if (!g_pool.mobs[i].alive) return i;
    }
    return -1;
}

// -------------------------------------------------------------------------------------- public API

int spawnVoxelMobs(VoxelWorld& world, float px, float pz, int radius, int count) {
    Scene* scene = world.scene();
    if (!scene || count <= 0) return 0;
    rebindPool(&world, scene);
    if (radius < 1) radius = 1;

    int made = 0;
    // A column that has not streamed in has nothing to stand on, so each mob gets a few random
    // tries; the caller is expected to keep calling every frame anyway.
    for (int attempt = 0; attempt < count * 8 && made < count; attempt++) {
        const int slot = freeMobSlot();
        if (slot < 0) break;   // pool full: respawn happens as soon as one wanders out of range

        const float ang = randomFloat(g_pool.rng) * 6.28318530718f;
        const float dist = radius * std::sqrt(randomFloat(g_pool.rng));   // sqrt keeps it uniform on the disc
        const int wx = (int)std::floor(px + std::cos(ang) * dist);
        const int wz = (int)std::floor(pz + std::sin(ang) * dist);

        const int top = world.getTop(wx, wz);
        if (top < 0 || top + 1 >= WORLD_HEIGHT - 2) continue;

        Mob& m = g_pool.mobs[slot];
        m.x = (float)wx + 0.5f;
        m.y = (float)(top + 1);
        m.z = (float)wz + 0.5f;
        m.vy = 0.0f;
        const VoxelBox box = mobBox(m);
        // getTop() is the topmost *opaque* block, which is not the same as the block that stops a
        // foot: a slab top, a fence or a plant all leave it off by design. Ask the real question.
        if (boxOverlapsSolid(world, box)) continue;
        if (!boxSupported(world, box)) continue;

        m.heading = randomFloat(g_pool.rng) * 6.28318530718f;
        m.turnTimer = 0.6f + randomFloat(g_pool.rng) * 3.0f;
        m.jumpCooldown = 0.0f;
        m.grounded = true;
        m.inLiquid = false;
        m.alive = true;
#ifdef VOXEL_HAVE_MODEL
        // A new mob starts its cycle fresh, and with no previous position to measure its first step
        // from: the first frame after a spawn poses the rig at whatever phase it will reach.
        m.walkPhase = 0.0f;
        m.lastValid = false;
#endif

        if (!m.built) {
            // One mesh per slot for the life of the world: the buffers are IMMUTABLE GPU allocations,
            // so a respawn must not reallocate them. The tint is a property of the slot, not of the
            // mob that happens to be wearing it.
            m.kind = (int)(nextRandom(g_pool.rng) % (uint32_t)MOB_TINT_COUNT);
            if (m.entity != NULL_ENTITY && scene->isEntityCreated(m.entity)) scene->destroyEntity(m.entity);
            m.entity = makeMobEntity(scene, slot, MOB_TINTS[m.kind]);
            m.built = true;
        }
        placeMobEntity(scene, m);
        made++;
    }

    g_pool.spawnedTotal += made;
    return made;
}

void updateVoxelMobs(VoxelWorld& world, float dt, float px, float pz, float despawnRadius) {
    Scene* scene = world.scene();
    if (!scene) return;
    rebindPool(&world, scene);
    if (dt < 0.0f) dt = 0.0f;
    if (dt > MOB_MAX_FRAME) dt = MOB_MAX_FRAME;
    if (despawnRadius < 1.0f) despawnRadius = 1.0f;
    const float despawnSq = despawnRadius * despawnRadius;

    for (int i = 0; i < MAX_MOBS; i++) {
        Mob& m = g_pool.mobs[i];
        if (!m.alive) continue;

        const float ddx = m.x - px;
        const float ddz = m.z - pz;
        const int ix = (int)std::floor(m.x);
        const int iz = (int)std::floor(m.z);
        // Out of sight, or the ground it stood on was unloaded: either way there is nothing left to
        // simulate, and the slot is worth more to the next spawn near the player.
        if (ddx * ddx + ddz * ddz > despawnSq || world.getTop(ix, iz) < 0) {
            despawnMob(scene, i);
            continue;
        }

        m.turnTimer -= dt;
        if (m.turnTimer <= 0.0f) {
            m.heading += (randomFloat(g_pool.rng) * 2.0f - 1.0f) * 2.4f;
            m.turnTimer = 1.2f + randomFloat(g_pool.rng) * 3.6f;
        }
        if (m.jumpCooldown > 0.0f) m.jumpCooldown -= dt;

        float speed = MOB_SPEED;
        if (m.inLiquid) speed *= MOB_LIQUID_SLOW;

        // Turn at a cliff: slide a copy of the body one step ahead and see whether anything would
        // still hold it up. A mob that walks off a ledge just falls, which looks stupid.
        if (m.grounded && !m.inLiquid) {
            VoxelBox probe = mobBox(m);
            moveBoxVoxel(world, probe, std::cos(m.heading) * (MOB_HALF + MOB_LOOK_AHEAD), 0.0f,
                         std::sin(m.heading) * (MOB_HALF + MOB_LOOK_AHEAD));
            if (!boxSupported(world, probe)) {
                m.heading += 2.2f;
                m.turnTimer = 0.4f + randomFloat(g_pool.rng) * 0.8f;
            }
        }

        float dx = std::cos(m.heading) * speed * dt;
        float dz = std::sin(m.heading) * speed * dt;
        if (m.inLiquid) {
            // VOXEL_IN_LIQUID says the box is in water: ease toward a slow rise so a mob that fell
            // in swims back to the surface, then gravity takes over and it bobs.
            m.vy += (MOB_SWIM_UP - m.vy) * std::min(1.0f, dt * 3.0f);
        } else {
            m.vy -= MOB_GRAVITY * dt;
            if (m.vy < -MOB_TERMINAL) m.vy = -MOB_TERMINAL;
        }

        VoxelBox box = mobBox(m);
        const int flags = moveBoxVoxel(world, box, dx, m.vy * dt, dz);
        m.x = (box.minX + box.maxX) * 0.5f;
        m.y = box.minY;
        m.z = (box.minZ + box.maxZ) * 0.5f;
        m.inLiquid = (flags & VOXEL_IN_LIQUID) != 0;
        m.grounded = (flags & VOXEL_HIT_GROUND) != 0;

        if (flags & VOXEL_HIT_GROUND) {
            m.vy = 0.0f;
        } else if (flags & VOXEL_HIT_CEIL) {
            if (m.vy > 0.0f) m.vy = 0.0f;
        }

        if ((flags & VOXEL_HIT_WALL) != 0 && m.grounded) {
            // moveBoxVoxel already tried to step up, so a wall stop means the step was refused (a
            // ceiling, or a two-block rise). Jump once, and if it still cannot get through, turn.
            if (m.jumpCooldown <= 0.0f && !m.inLiquid) {
                m.vy = MOB_JUMP;
                m.jumpCooldown = 0.5f;
            } else {
                m.heading += 1.7f + randomFloat(g_pool.rng) * 1.2f;
                m.turnTimer = 0.6f;
            }
        }

#ifdef VOXEL_HAVE_MODEL
        // The cycle turns with the ground covered, never with the clock: that is what keeps the feet
        // planted when the frame rate changes, when a mob is stopped against a wall, and when it is
        // falling. A fall moves no ground under the feet, so only the XZ travel counts.
        const float moved = m.lastValid
                                ? std::sqrt((m.x - m.lastX) * (m.x - m.lastX) +
                                            (m.z - m.lastZ) * (m.z - m.lastZ))
                                : 0.0f;
        m.walkPhase = voxelModelWalkPhase(m.walkPhase, moved, MOB_STRIDE);
        m.lastX = m.x;
        m.lastZ = m.z;
        m.lastValid = true;
#endif

        placeMobEntity(scene, m);
    }
}

int aliveMobCount(VoxelWorld& world) {
    if (g_pool.world != &world) return 0;
    int n = 0;
    for (int i = 0; i < MAX_MOBS; i++) {
        if (g_pool.mobs[i].alive) n++;
    }
    return n;
}

std::string mobStats(VoxelWorld& world) {
    int alive = 0, spawned = 0, culled = 0;
    if (g_pool.world == &world) {
        alive = aliveMobCount(world);
        spawned = g_pool.spawnedTotal;
        culled = g_pool.culledTotal;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Mobs: %d (spawned %d, culled %d)", alive, spawned, culled);
    return std::string(buf);
}

// ------------------------------------------------------------------------------- gravity blocks

struct FallingBlock {
    int x, y, z;
    float offset;   // how far of the next cell it has already slid: this is the animation
    float vy;
};

static std::vector<FallingBlock> g_falling;
static size_t g_editCursor = 0;
static VoxelWorld* g_fallingWorld = nullptr;

static const float FALL_GRAVITY = 22.0f;
static const float FALL_MAX_SPEED = 16.0f;   // cells/s: a tall drop still takes a beat to land
static const size_t FALLING_MAX = 512;       // a cave-in bigger than this is the integrator's problem
static const int FALLING_MOVES_PER_FRAME = 48;

static bool gravityBlock(uint16_t type) {
    return (block(type).flags & FLAG_GRAVITY) != 0;
}

// What can a gravity block fall through: nothing, a plant or snow layer, or a liquid it displaces.
static bool fallThrough(const VoxelWorld& world, int x, int y, int z) {
    const uint16_t type = stateType(world.get(x, y, z));
    if (type == BLOCK_AIR) return true;
    const uint32_t f = block(type).flags;
    return (f & (FLAG_REPLACEABLE | FLAG_LIQUID)) != 0;
}

static void eraseFallingAt(size_t i) {
    const size_t last = g_falling.size() - 1;
    if (i != last) g_falling[i] = g_falling[last];
    g_falling.pop_back();
}

static void addFallingCandidate(const VoxelWorld& world, int x, int y, int z) {
    if (y < 0 || y >= WORLD_HEIGHT) return;
    if (g_falling.size() >= FALLING_MAX) return;
    if (!gravityBlock(stateType(world.get(x, y, z)))) return;
    // The journal adds a handful of edits per frame and the list is capped, so a linear dedupe is
    // cheaper here than the hash it would need to beat.
    for (size_t i = 0; i < g_falling.size(); i++) {
        const FallingBlock& f = g_falling[i];
        if (f.x == x && f.y == y && f.z == z) return;
    }
    FallingBlock f;
    f.x = x;
    f.y = y;
    f.z = z;
    f.offset = 0.0f;
    f.vy = 0.0f;
    g_falling.push_back(f);
}

void updateFallingBlocks(VoxelWorld& world, float dt) {
    if (g_fallingWorld != &world) {
        g_fallingWorld = &world;
        g_falling.clear();
        g_editCursor = 0;
    }
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    // The edit journal is the only cheap signal that a column lost its support, and it is read as a
    // diff: the world may or may not clear it, so remember the position we reached instead of a
    // generation counter. Full list scans stay O(new edits).
    const std::vector<BlockEdit>& edits = world.pendingEdits();
    if (edits.size() < g_editCursor) g_editCursor = 0;
    for (size_t i = g_editCursor; i < edits.size(); i++) {
        const BlockEdit& e = edits[i];
        addFallingCandidate(world, e.x, e.y + 1, e.z);   // what the edit just uncovered
        addFallingCandidate(world, e.x, e.y, e.z);       // sand placed in midair
    }
    g_editCursor = edits.size();

    if (g_falling.empty()) return;

    int moves = 0;
    for (size_t i = 0; i < g_falling.size();) {
        const int fx = g_falling[i].x;
        const int fy = g_falling[i].y;
        const int fz = g_falling[i].z;
        const uint16_t id = world.get(fx, fy, fz);
        const uint16_t type = stateType(id);
        // Gone, mined, or the block under it arrived since: the entry is stale, and a resting block
        // costs nothing to re-discover the next time the journal says something underneath it left.
        if (!gravityBlock(type) || fy <= 0 || !fallThrough(world, fx, fy - 1, fz)) {
            eraseFallingAt(i);
            continue;
        }

        FallingBlock& f = g_falling[i];
        f.vy += FALL_GRAVITY * dt;
        if (f.vy > FALL_MAX_SPEED) f.vy = FALL_MAX_SPEED;
        f.offset += f.vy * dt;
        if (f.offset >= 1.0f && moves < FALLING_MOVES_PER_FRAME) {
            f.offset -= 1.0f;
            // Two writes through setBlock, not setBlockQuiet: the chunk has to be re-lit and re-meshed
            // and the edit has to reach the save/net journals like any other player-visible change.
            world.setBlock(fx, fy, fz, BLOCK_AIR);
            world.setBlock(fx, fy - 1, fz, id);
            f.y = fy - 1;
            moves++;
        }
        ++i;
    }
}

// __________________________________________________________________________________ behaviour probe

void probeVoxelEntities(VoxelWorld& world, voxel::Probe& probe) {
    ProbeModule mod(probe, "entity");
    Scene* scene = world.scene();
    if (!scene) {
        probe.skip("no scene, so a mob has nowhere to be drawn");
        return;
    }
    rebindPool(&world, scene);
    if (g_pool.world != &world || g_pool.scene != scene) {
        probe.skip("the pool refused to bind to this world");
        return;
    }

    // All of this happens in the game's own pool inside the streamed world, and the frame the probe
    // runs in is a frame the host keeps simulating after it returns - so the slots, the counters, the
    // RNG and every voxel are taken back on the single exit path at the bottom.
    Mob saved[MAX_MOBS];
    bool hadMesh[MAX_MOBS];
    int savedAlive = 0;
    for (int i = 0; i < MAX_MOBS; i++) {
        saved[i] = g_pool.mobs[i];
        hadMesh[i] = g_pool.mobs[i].built;
        if (g_pool.mobs[i].alive) savedAlive++;
    }
    const int savedSpawned = g_pool.spawnedTotal;
    const int savedCulled = g_pool.culledTotal;
    const uint32_t savedRng = g_pool.rng;

    const int sx0 = world.focusChunkX() * CHUNK_SX + 4;
    const int sz0 = world.focusChunkZ() * CHUNK_SZ + 4;
    const int preTop = world.getTop(sx0, sz0);
    const uint16_t floorWas = preTop >= 0 ? world.get(sx0 + 4, preTop, sz0 + 2) : (uint16_t)BLOCK_AIR;

    VoxelPatch patch(world);
    int ax = 0, ay = 0, az = 0;
    const char* why = nullptr;

    do {
        if (!carveArena(world, patch, ax, ay, az, 9, 5, 4)) {
            why = "the focus column has not streamed, so there is no floor to stand on";
            break;
        }
        // The tests dig below the arena floor, which carveArena never writes: remember that shell so
        // the scaffolding has somewhere to be undone from.
        for (int x = ax; x < ax + 9; x++) {
            for (int z = az; z < az + 5; z++) {
                for (int y = ay - 4; y <= ay - 2; y++) patch.remember(x, y, z);
            }
        }
        const float px = (float)ax + 4.5f, pz = (float)az + 2.5f;
        // The module's own despawn rule, used as a reset: a player 4 000 blocks away and a 1-block
        // range puts every mob out of sight, which is how the pool is emptied between claims.
        auto clearPool = [&]() { updateVoxelMobs(world, 0.016f, px + 4000.0f, pz + 4000.0f, 1.0f); };
        auto firstAlive = []() {
            for (int i = 0; i < MAX_MOBS; i++) if (g_pool.mobs[i].alive) return i;
            return -1;
        };

        clearPool();
        const int made = spawnVoxelMobs(world, px, pz, 4, 4);
        int wedged = 0, floating = 0;
        for (int i = 0; i < MAX_MOBS; i++) {
            if (!g_pool.mobs[i].alive) continue;
            const VoxelBox b = mobBox(g_pool.mobs[i]);
            if (boxOverlapsSolid(world, b)) wedged++;
            if (!boxSupported(world, b)) floating++;
        }
        probe.check("a mob spawns standing on the block that holds it, not inside one",
                    made >= 1 && wedged == 0 && floating == 0 && aliveMobCount(world) == made,
                    "%d asked for, %d placed, %d alive, %d inside a block, %d with nothing under the feet",
                    4, made, aliveMobCount(world), wedged, floating);

        const int more = spawnVoxelMobs(world, px, pz, 6, MAX_MOBS * 2);
        probe.check("the pool is a hard cap: a request for twice what it holds comes back short",
                    aliveMobCount(world) <= MAX_MOBS && more > 0 && more <= MAX_MOBS,
                    "%d asked for, %d placed, %d alive against a cap of %d",
                    MAX_MOBS * 2, more, aliveMobCount(world), MAX_MOBS);

        clearPool();
        spawnVoxelMobs(world, px, pz, 4, 4);
        float startX[MAX_MOBS], startZ[MAX_MOBS];
        for (int i = 0; i < MAX_MOBS; i++) { startX[i] = g_pool.mobs[i].x; startZ[i] = g_pool.mobs[i].z; }
        for (int f = 0; f < 90; f++) updateVoxelMobs(world, 1.0f / 60.0f, px, pz, 64.0f);
        float furthest = 0.0f;
        int walked = 0, drifted = 0, buried = 0;
        for (int i = 0; i < MAX_MOBS; i++) {
            const Mob& m = g_pool.mobs[i];
            if (!m.alive) continue;
            const float d = std::sqrt((m.x - startX[i]) * (m.x - startX[i]) +
                                      (m.z - startZ[i]) * (m.z - startZ[i]));
            if (d > furthest) furthest = d;
            if (d > 0.05f) walked++;
            Transform* t = (m.entity != NULL_ENTITY && scene->isEntityCreated(m.entity))
                               ? scene->findComponent<Transform>(m.entity) : nullptr;
            if (!t || std::fabs(t->position.x - m.x) > 1e-3f || std::fabs(t->position.y - m.y) > 1e-3f ||
                std::fabs(t->position.z - m.z) > 1e-3f) drifted++;
            if (boxOverlapsSolid(world, mobBox(m))) buried++;
        }
        probe.check("a mob walks, and the entity is drawn exactly where the sim left it",
                    furthest > 0.05f && drifted == 0 && buried == 0,
                    "furthest %.2f blocks in 1.5 s, %d of %d moved, %d drawn elsewhere, %d wedged in terrain",
                    furthest, walked, aliveMobCount(world), drifted, buried);

#ifdef VOXEL_HAVE_MODEL
        // The claim above is about the anchor; the picture lives on six other entities. A rig is hung
        // by its joints, so the measurable promise is that the pool put the hip and the neck at the
        // heights the model module promises for this body - which also says the mob is standing where
        // the sim has it rather than the rig being posed at its spawn point.
        int posed = 0, mishung = 0;
        const float rigScale = voxelModelScaleForHeight(MOB_HEIGHT);
        const float hipY = voxelModelInfo(VOXEL_PART_LEG_RIGHT).jointY * rigScale;
        const float neckY = voxelModelInfo(VOXEL_PART_HEAD).jointY * rigScale;
        for (int i = 0; i < MAX_MOBS; i++) {
            const Mob& m = g_pool.mobs[i];
            if (!m.alive || !m.rig.built) continue;
            posed++;
            Transform* leg = rigJointTransform(scene, m.rig.parts[VOXEL_PART_LEG_RIGHT]);
            Transform* head = rigJointTransform(scene, m.rig.parts[VOXEL_PART_HEAD]);
            if (!leg || !head || !leg->visible || !head->visible ||
                std::fabs(leg->position.y - (m.y + hipY)) > 1e-3f ||
                std::fabs(head->position.y - (m.y + neckY)) > 1e-3f) mishung++;
        }
        probe.check("the pool hangs the rig on the body the sim stands on: hip and neck at their heights",
                    posed > 0 && mishung == 0, "%d mobs posed, %d with a joint off its height",
                    posed, mishung);
#endif

        const int pitSlot = firstAlive();
        if (pitSlot < 0) { why = "no mob survived the walk"; break; }
        Mob& faller = g_pool.mobs[pitSlot];
        // Stood up in the middle of the carved room rather than left wherever the 90-frame walk put it:
        // a mob that wandered into the sea does not fall into a hole, it floats and steps onto the next
        // wet column, and the pit would then measure the shoreline instead of the module. The arena
        // floor is dry stone, and its plate is nine by five, so a three-by-three hole in the centre
        // leaves three blocks of floor between the mob and any edge.
        faller.x = (float)ax + 4.5f; faller.y = (float)ay; faller.z = (float)az + 2.5f;
        faller.vy = 0.0f;
        const float y0 = faller.y;
        const int fx = (int)std::floor(faller.x), fz = (int)std::floor(faller.z);
        const int fy = (int)std::floor(y0);
        if (fy <= 5) { why = "the mob stands too close to bedrock to dig under it"; break; }
        // Dug under the feet, not at a fixed depth: the claim is about the block the body rests on.
        // Three deep so the 1-block jump the module offers against a wall cannot be how it gets out;
        // three wide so it cannot walk off the new floor while it is in the air, and cannot climb back
        // out once it has landed.
        const int landY = fy - 3;
        for (int dx = -1; dx <= 1; dx++) {
            for (int dz = -1; dz <= 1; dz++) {
                for (int dy = 1; dy <= 3; dy++) patch.set(fx + dx, fy - dy, fz + dz, BLOCK_AIR);
                patch.set(fx + dx, fy - 4, fz + dz, BLOCK_STONE);
            }
        }
        // Measured over the fall, not at the last frame: a mob that lands in a 3-wide pit walks there
        // and jumps at the walls, so the snapshot of frame 180 is as likely to catch it mid-jump as
        // resting. What the claim needs is that the lowest it ever got is the new floor, flush, and that
        // it rested **on that floor, inside the hole** - not merely at that height somewhere. The count
        // of frames spent outside is printed because a mob that left the room is a harness bug, not a
        // behaviour, and saying so in the message is cheaper than working it out afterwards.
        float lowest = y0;
        int resting = 0, escaped = 0, wedgedOnFloor = 0;
        for (int f = 0; f < 180; f++) {
            updateVoxelMobs(world, 1.0f / 60.0f, px, pz, 64.0f);
            if (!faller.alive) break;
            if (faller.y < lowest) lowest = faller.y;
            const int ex = (int)std::floor(faller.x), ez = (int)std::floor(faller.z);
            const bool inPit = ex >= fx - 1 && ex <= fx + 1 && ez >= fz - 1 && ez <= fz + 1;
            if (!inPit) escaped++;
            const VoxelBox b = mobBox(faller);
            if (inPit && boxSupported(world, b) && std::fabs(b.minY - (float)landY) < 0.05f) {
                resting++;
                if (boxOverlapsSolid(world, b)) wedgedOnFloor++;
            }
        }
        probe.check("the floor going away is what drops a mob: it falls and lands on the next one",
                    faller.alive && std::fabs(lowest - (float)landY) < 0.05f && resting > 0 &&
                        wedgedOnFloor == 0,
                    "feet %.3f, lowest %.3f against a pit floor at %.0f, %d frames resting in the pit, "
                    "%d of them inside a block, %d it walked outside",
                    y0, lowest, (float)landY, resting, wedgedOnFloor, escaped);

        // Re-carve: the arena is the floor this section stands on, and the pit is now part of it.
        carveArena(world, patch, ax, ay, az, 9, 5, 4);
        clearPool();
        spawnVoxelMobs(world, px, pz, 2, 1);
        const int swimSlot = firstAlive();
        if (swimSlot < 0) { why = "no mob could be placed for the water test"; break; }
        Mob& swimmer = g_pool.mobs[swimSlot];
        const float wy0 = swimmer.y;
        const int wx = (int)std::floor(swimmer.x), wz = (int)std::floor(swimmer.z);
        const int wyInt = (int)std::floor(wy0);
        // Four cells of water with two of air above, around the feet of the mob the pool actually
        // gave us: a column that ends at its own head height would measure the ceiling, not the
        // swim. Five wide because the mob drifts while rising, and a 3-wide pool lets it walk out of
        // the liquid in the time this takes - the claim is about the water holding it up.
        for (int dx = -2; dx <= 2; dx++) {
            for (int dz = -2; dz <= 2; dz++) {
                for (int dy = 0; dy < 4; dy++) {
                    patch.set(wx + dx, wyInt + dy, wz + dz, makeState(BLOCK_WATER, 15));
                }
                patch.set(wx + dx, wyInt + 4, wz + dz, BLOCK_AIR);
                patch.set(wx + dx, wyInt + 5, wz + dz, BLOCK_AIR);
            }
        }
        // Only the swim integrator lifts a body inside a liquid (the module refuses a jump while
        // inLiquid), so counting the frames that rise happens wet is what separates "it swam" from
        // "it bounced off a wall and jumped" - both move the feet up, only one of them is the claim.
        float highest = wy0;
        int swamFrames = 0;
        for (int f = 0; f < 90; f++) {
            updateVoxelMobs(world, 1.0f / 60.0f, px, pz, 64.0f);
            if (!swimmer.alive) break;
            if (swimmer.inLiquid && swimmer.y > highest) highest = swimmer.y;
            if (swimmer.inLiquid && swimmer.vy > 0.2f) swamFrames++;
        }
        probe.check("water holds a mob up: it swims toward the surface instead of sinking through it",
                    swimmer.alive && highest > wy0 + 0.5f && swamFrames > 0,
                    "feet start %.3f, the highest it got while wet is %.3f, rising in liquid %d frames",
                    wy0, highest, swamFrames);

        const int aliveBefore = aliveMobCount(world);
        const int watched = firstAlive();
        const Entity we = watched >= 0 ? g_pool.mobs[watched].entity : NULL_ENTITY;
#ifdef VOXEL_HAVE_MODEL
        const VoxelModelRig wrig = watched >= 0 ? g_pool.mobs[watched].rig : VoxelModelRig();
#endif
        clearPool();
        Transform* wt = (we != NULL_ENTITY && scene->isEntityCreated(we))
                            ? scene->findComponent<Transform>(we) : nullptr;
        // A hidden anchor proves nothing if the six parts are still drawing: the cull has to reach the
        // picture, not only the entity the pool keeps its transform on. -1 says "no rig to watch".
        int partsLeft = -1;
#ifdef VOXEL_HAVE_MODEL
        partsLeft = 0;
        for (int p = 0; p < VOXEL_PART_COUNT; p++) {
            Transform* pt = rigJointTransform(scene, wrig.parts[p]);
            if (!pt || pt->visible) partsLeft++;
        }
#endif
        probe.check("a mob the player left behind is culled and every part of it stops drawing",
                    aliveBefore > 0 && aliveMobCount(world) == 0 && wt && !wt->visible && partsLeft <= 0,
                    "%d alive before, %d after, drawn %d, %d of six parts still drawing, "
                    "%d culled across the session",
                    aliveBefore, aliveMobCount(world), wt ? (int)wt->visible : -1, partsLeft,
                    g_pool.culledTotal);

        spawnVoxelMobs(world, px, pz, 4, 3);
        const std::string stats = mobStats(world);
        // The overlay line is produced by mobStats() in this same file, so the probe reads it back
        // with a hand parse: sscanf is the CRT-deprecated one here, and a prefix comparison says
        // what the format has to be in the code that checks it.
        int reported = -1;
        const char* prefix = "Mobs: ";
        if (stats.compare(0, 6, prefix) == 0) {
            reported = 0;
            for (size_t i = 6; i < stats.size() && stats[i] >= '0' && stats[i] <= '9'; i++) {
                reported = reported * 10 + (stats[i] - '0');
            }
        }
        probe.check("the overlay's mob line reports the live count",
                    reported > 0 && reported == aliveMobCount(world), "%s", stats.c_str());
        clearPool();
        carveArena(world, patch, ax, ay, az, 9, 5, 4);

        // ---------------------------------------------------------------- gravity blocks
        // Discovery runs off the edit journal, so each case starts by clearing it: a leftover edit
        // would queue a cell the previous case already settled.
        world.clearPendingEdits();
        const int bx = ax + 1, bz = az + 1;
        patch.remember(bx, ay, bz);
        world.setBlock(bx, ay, bz, BLOCK_SAND);
        for (int f = 0; f < 30; f++) updateFallingBlocks(world, 1.0f / 60.0f);
        probe.check("a gravity block with something under it stays exactly where it was put",
                    stateType(world.get(bx, ay, bz)) == BLOCK_SAND && g_falling.empty(),
                    "the cell holds %d, %zu entries still queued",
                    (int)stateType(world.get(bx, ay, bz)), g_falling.size());

        patch.set(bx, ay - 2, bz, BLOCK_STONE);   // guarantee what it lands on
        world.breakBlock(bx, ay - 1, bz);         // the player's pick, loud
        for (int f = 0; f < 240; f++) updateFallingBlocks(world, 1.0f / 60.0f);
        probe.check("mining the block under sand drops it, one cell at a time, onto the block below",
                    stateType(world.get(bx, ay, bz)) == BLOCK_AIR &&
                    stateType(world.get(bx, ay - 1, bz)) == BLOCK_SAND &&
                    stateType(world.get(bx, ay - 2, bz)) == BLOCK_STONE,
                    "the cell it left holds %d, one down %d, the floor %d",
                    (int)stateType(world.get(bx, ay, bz)), (int)stateType(world.get(bx, ay - 1, bz)),
                    (int)stateType(world.get(bx, ay - 2, bz)));

        bool journalled = false;
        const std::vector<BlockEdit>& edits = world.pendingEdits();
        for (size_t i = 0; i < edits.size(); i++) {
            const BlockEdit& e = edits[i];
            if (e.x == bx && e.y == ay - 1 && e.z == bz && stateType(e.id) == BLOCK_SAND) journalled = true;
        }
        probe.check("the fall goes through the loud write path, so the save and net journals see it",
                    journalled, "%zu edits since the pick, the landing among them %d",
                    edits.size(), (int)journalled);

        const int gx = ax + 7, gz = az + 3;
        patch.remember(gx, ay + 2, gz);
        world.setBlock(gx, ay + 2, gz, BLOCK_GRAVEL);   // the other discovery branch: midair placement
        for (int f = 0; f < 400; f++) updateFallingBlocks(world, 1.0f / 60.0f);
        probe.check("gravel placed in midair is not held up by nothing: it falls to the floor",
                    stateType(world.get(gx, ay, gz)) == BLOCK_GRAVEL &&
                    stateType(world.get(gx, ay + 1, gz)) == BLOCK_AIR &&
                    stateType(world.get(gx, ay + 2, gz)) == BLOCK_AIR,
                    "the floor cell holds %d, one up %d, two up %d",
                    (int)stateType(world.get(gx, ay, gz)), (int)stateType(world.get(gx, ay + 1, gz)),
                    (int)stateType(world.get(gx, ay + 2, gz)));

        const int vx = ax + 5, vz = az + 3;
        patch.set(vx, ay - 2, vz, BLOCK_STONE);
        patch.set(vx, ay - 1, vz, makeState(BLOCK_WATER, 15));   // quiet: the sim never sees this water
        patch.remember(vx, ay, vz);
        world.setBlock(vx, ay, vz, BLOCK_SAND);
        for (int f = 0; f < 300; f++) updateFallingBlocks(world, 1.0f / 60.0f);
        probe.check("a falling block goes through water and stops on the solid underneath it",
                    stateType(world.get(vx, ay - 1, vz)) == BLOCK_SAND &&
                    stateType(world.get(vx, ay, vz)) == BLOCK_AIR,
                    "the cell that held water now holds %d, the one above %d",
                    (int)stateType(world.get(vx, ay - 1, vz)), (int)stateType(world.get(vx, ay, vz)));

        for (int f = 0; f < 60; f++) updateFallingBlocks(world, 1.0f / 60.0f);
        probe.check("a settled world leaves nothing queued: the falling list is empty again",
                    g_falling.empty(), "%zu entries still in the list", g_falling.size());
    } while (0);

    if (why) probe.skip(why);

    world.clearPendingEdits();
    patch.restore();
    g_falling.clear();
    for (int i = 0; i < MAX_MOBS; i++) {
        Mob& m = g_pool.mobs[i];
        if (!hadMesh[i] && m.built) {
            // A picture this probe built for a slot that had none: the buffers are IMMUTABLE GPU
            // allocations, so leaving the entity behind would hold them for nothing.
#ifdef VOXEL_HAVE_MODEL
            // The six parts are not children of the anchor - the rig has no hierarchy - so each one
            // is torn down on its own before the pool's state goes back.
            destroyVoxelModelRig(scene, m.rig);
#endif
            if (m.entity != NULL_ENTITY && scene->isEntityCreated(m.entity))
                scene->destroyEntity(m.entity);
        }
        m = saved[i];
        if (m.alive) placeMobEntity(scene, m); else hideMobEntity(scene, m);
    }
    g_pool.spawnedTotal = savedSpawned;
    g_pool.culledTotal = savedCulled;
    g_pool.rng = savedRng;

    probe.check("the probe leaves the pool, the queue and every voxel it dug exactly as it found them",
                aliveMobCount(world) == savedAlive && g_falling.empty() &&
                world.pendingEdits().empty() && world.get(sx0 + 4, preTop, sz0 + 2) == floorWas,
                "%d alive of %d, %zu queued, %zu edits left, the arena floor is %d and was %d",
                aliveMobCount(world), savedAlive, g_falling.size(), world.pendingEdits().size(),
                (int)world.get(sx0 + 4, preTop, sz0 + 2), (int)floorWas);
}

}  // namespace voxel
