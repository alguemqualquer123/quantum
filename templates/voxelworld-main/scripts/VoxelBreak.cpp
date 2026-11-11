// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelBreak.h"

#include "VoxelMesh.h"
#include "VoxelProbe.h"
#include "VoxelTiles.h"
#include "VoxelWorld.h"

#include "Scene.h"
#include "Log.h"
#include "Vector2.h"
#include "Vector3.h"
#include "math/AABB.h"
#include "math/Quaternion.h"
#include "math/Rect.h"
#include "component/MeshComponent.h"
#include "component/PointsComponent.h"
#include "component/Transform.h"
#include "action/Particles.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace voxel {

using namespace quantum;

int breakStage(float progress) {
    if (progress <= 0.0f) return -1;
    if (progress >= 1.0f) return BREAK_STAGE_COUNT - 1;
    int stage = (int)(progress * (float)BREAK_STAGE_COUNT);
    if (stage < 0) stage = 0;
    if (stage >= BREAK_STAGE_COUNT) stage = BREAK_STAGE_COUNT - 1;
    return stage;
}

int breakStageTile(int stage) {
    if (stage < 0 || stage >= BREAK_STAGE_COUNT) return -1;
    return TILE_DESTROY_STAGE_0 + stage;
}

// ---- the overlay ----

// Same 48-byte vertex as the chunk mesher. That layout is not shared because VoxelMesh.cpp keeps
// its struct file-local; a second copy with a comment on the contract is cheaper than widening a
// working module for one consumer.
struct BreakVertex {
    float p[3];
    float n[3];
    float uv[2];
    float c[4];
};

static const unsigned int BREAK_STRIDE = sizeof(BreakVertex);

struct BreakFaceSpec {
    int n[3];
    int t1[3];
    int t2[3];
};

// cross(t1, t2) == n and the corners walk (-1,-1) (1,-1) (1,1) (-1,1), which is the counter-clockwise
// winding every other voxel mesh in this project uses.
static const BreakFaceSpec g_breakFaces[6] = {
    { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } },
    { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
    { { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 } },
    { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
    { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
    { { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0, 0 } },
};

// Half a block: the cube is authored centred on the origin, so an uniform scale inflates it away
// from the face underneath instead of sliding it along +x/+y/+z.
static const float BREAK_HALF = 0.5f;
static const float BREAK_INFLATE = 1.0045f;

bool BreakOverlay::attach(Scene* scene) {
    if (!scene) return false;
    if (entity >= 0 && scenePtr == scene && scene->isEntityCreated((Entity)entity)) return true;

    scenePtr = scene;
    entity = -1;
    currentStage = -1;
    showing = false;

    std::vector<BreakVertex> verts;
    std::vector<uint32_t> idx;
    verts.reserve(24);
    idx.reserve(36);
    const float sign[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int f = 0; f < 6; f++) {
        const BreakFaceSpec& fs = g_breakFaces[f];
        uint32_t base = (uint32_t)verts.size();
        for (int i = 0; i < 4; i++) {
            const float s1 = sign[i][0], s2 = sign[i][1];
            BreakVertex v;
            for (int a = 0; a < 3; a++) {
                v.p[a] = (float)fs.n[a] * BREAK_HALF + (float)fs.t1[a] * s1 * BREAK_HALF +
                         (float)fs.t2[a] * s2 * BREAK_HALF;
                v.n[a] = (float)fs.n[a];
            }
            // The submesh's textureRect uniform maps this 0..1 range onto the stage tile, so the
            // geometry never has to be touched again when the stage moves. v is flipped because the
            // atlas stores a tile's top edge at the smaller v.
            v.uv[0] = (s1 + 1.0f) * 0.5f;
            v.uv[1] = (1.0f - s2) * 0.5f;
            v.c[0] = v.c[1] = v.c[2] = v.c[3] = 1.0f;
            verts.push_back(v);
        }
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    }

    Entity e = scene->createEntity();
    scene->setEntityName(e, "VoxelBreakOverlay");

    MeshComponent mc;
    // A crack is a cutout, not a tint: the atlas tile is mostly empty pixels and only the dark lines
    // get drawn, so the block underneath keeps its colour and its shading. That is an opaque mesh
    // with alphaMode MASK, which is also what the chunk mesher uses for leaves and plants.
    // receiveLights goes off below: a crack that darkens with the block under it is invisible in a
    // cave, and the whole point is legibility.
    setupVoxelMaterial(mc, false);
    mc.receiveLights = false;
    mc.castShadows = false;
    mc.receiveShadows = false;
    // Forced into the transparent pass. MASK does not count as alpha blending for the renderer's
    // autoTransparency, so left opaque the cube is drawn in the opaque pass and the block it sits on
    // loses that race: the capture shows the scene background where the crack tile is transparent,
    // which is the ground never being drawn there at all. After the opaque pass the discarded texels
    // simply leave the block alone.
    mc.transparent = true;
    Material& m = mc.submeshes[0].material;
    m.name = "voxel_break";
    m.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    Submesh& sub = mc.submeshes[0];
    sub.hasTextureRect = true;
    const VoxelTileRect first = quantum::voxelTileRect(breakStageTile(0));
    sub.textureRect = Rect(first.u0, first.v0, first.u1 - first.u0, first.v1 - first.v0);

    scene->addComponent<MeshComponent>(e, std::move(mc));

    MeshComponent& mesh = scene->getComponent<MeshComponent>(e);
    mesh.buffer.clearAll();
    Buffer& geom = mesh.buffer;
    geom.addAttribute(AttributeType::POSITION, AttributeDataType::FLOAT, 3, offsetof(BreakVertex, p));
    geom.addAttribute(AttributeType::NORMAL, AttributeDataType::FLOAT, 3, offsetof(BreakVertex, n));
    geom.addAttribute(AttributeType::TEXCOORD1, AttributeDataType::FLOAT, 2, offsetof(BreakVertex, uv));
    geom.addAttribute(AttributeType::COLOR, AttributeDataType::FLOAT, 4, offsetof(BreakVertex, c));
    mesh.buffer.setStride(BREAK_STRIDE);
    mesh.buffer.setUsage(BufferUsage::IMMUTABLE);
    mesh.indices.clearAll();
    mesh.indices.addAttribute(AttributeType::INDEX, AttributeDataType::UNSIGNED_INT, 1, 0);
    mesh.indices.setStride(sizeof(uint32_t));
    mesh.indices.setUsage(BufferUsage::IMMUTABLE);
    mesh.buffer.importData(verts.data(), verts.size() * BREAK_STRIDE);
    mesh.buffer.setCount((unsigned int)verts.size());
    mesh.indices.importData(idx.data(), idx.size() * sizeof(uint32_t));
    mesh.indices.setCount((unsigned int)idx.size());
    mesh.vertexCount = (unsigned int)verts.size();
    const float h = BREAK_HALF * BREAK_INFLATE;
    mesh.aabb = AABB(Vector3(-h, -h, -h), Vector3(h, h, h));
    mesh.verticesAABB = mesh.aabb;
    mesh.needUpdateAABB = false;
    mesh.needReload = true;
    mesh.loaded = false;

    Transform t;
    t.position = Vector3(0.0f, -1000.0f, 0.0f);
    t.rotation = Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    t.scale = Vector3(BREAK_INFLATE, BREAK_INFLATE, BREAK_INFLATE);
    t.visible = false;
    scene->addComponent<Transform>(e, t);

    entity = (int64_t)e;
    return true;
}

void BreakOverlay::show(int x, int y, int z, int stage) {
    if (stage < 0 || stage >= BREAK_STAGE_COUNT) { hide(); return; }
    if (!scenePtr || entity < 0) { hide(); return; }
    // The other half of the VOXEL_NO_DEBRIS switch: with the crack gone and the chips still
    // flying, whatever is left on the block is the debris and nothing else.
    if (voxelEnvInt("VOXEL_NO_CRACK", 0) != 0) { hide(); return; }
    const Entity e = (Entity)entity;
    if (!scenePtr->isEntityCreated(e)) { hide(); return; }

    Transform& t = scenePtr->getComponent<Transform>(e);
    const bool moved = !showing || bx != x || by != y || bz != z;
    if (moved) {
        t.position = Vector3((float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f);
        t.visible = true;
        bx = x; by = y; bz = z;
        movesMade++;
    }
    // Every frame, not only on the move: the render node picks a transform up when the frame marks
    // it dirty (the same write placeMobEntity makes), and the first frame the crack appears can be
    // the one where the mesh is still loading.
    t.needUpdate = true;
    showing = true;

    if (stage == currentStage) return;
    currentStage = stage;
    MeshComponent& mesh = scenePtr->getComponent<MeshComponent>(e);
    const VoxelTileRect r = quantum::voxelTileRect(breakStageTile(stage));
    mesh.submeshes[0].textureRect = Rect(r.u0, r.v0, r.u1 - r.u0, r.v1 - r.v0);
    writes++;
}

void BreakOverlay::hide() {
    showing = false;
    currentStage = -1;
    if (!scenePtr || entity < 0) return;
    const Entity e = (Entity)entity;
    if (!scenePtr->isEntityCreated(e)) return;
    Transform& t = scenePtr->getComponent<Transform>(e);
    t.visible = false;
    t.needUpdate = true;
}

bool BreakOverlay::isShowing() const { return showing; }
int BreakOverlay::stage() const { return currentStage; }

static const MeshComponent* findOverlayMesh(Scene* scene, int64_t entity) {
    if (!scene || entity < 0) return nullptr;
    const Entity e = (Entity)entity;
    if (!scene->isEntityCreated(e)) return nullptr;
    return scene->findComponent<MeshComponent>(e);
}

bool BreakOverlay::geometryLoaded() const {
    const MeshComponent* mesh = findOverlayMesh(scenePtr, entity);
    return mesh && mesh->loaded;
}

unsigned int BreakOverlay::geometryVertices() const {
    const MeshComponent* mesh = findOverlayMesh(scenePtr, entity);
    return mesh ? mesh->vertexCount : 0u;
}

unsigned long long BreakOverlay::geometryShaderBits() const {
    const MeshComponent* mesh = findOverlayMesh(scenePtr, entity);
    return mesh ? (unsigned long long)mesh->submeshes[0].shaderProperties : 0ull;
}

static BreakOverlay s_overlay;
BreakOverlay& breakOverlay() { return s_overlay; }

// ---- debris ----

// One point emitter per slot. The target entity sits at the origin with an identity rotation, so
// the engine's world-space particle path (ParticlesComponent.localSpace == false) hands the burst
// the coordinates it is given and the chips stay where they were spawned instead of following the
// emitter. That is what lets a slot be reused the moment its predecessor is superseded.
const int DEBRIS_POOL = 8;
const int DEBRIS_MAX_POINTS = 24;

struct DebrisSlot {
    Entity target = NULL_ENTITY;
    Particles* particles = nullptr;
};

static std::vector<DebrisSlot> s_debris;
static Scene* s_debrisScene = nullptr;
static int s_debrisNext = 0;
static uint64_t s_debrisSpawns = 0;
static int s_debrisLastPoints = 0;

static bool addSpriteFrameToPoints(Scene* scene, Entity target, uint16_t type) {
    PointsComponent& points = scene->getComponent<PointsComponent>(target);
    const uint16_t tile = block(type).tiles[FACE_PX];
    const VoxelTileRect r = quantum::voxelTileRect(tile);
    points.framesRect[0] = SpriteFrameData{ "block", Rect(r.u0, r.v0, r.u1 - r.u0, r.v1 - r.v0) };
    points.numFramesRect = 1;
    // The variant with the per-point rect compiled in is chosen when the points object loads, so
    // the flag has to be right before the first frame is drawn, not after.
    if (!points.hasTextureRect) {
        points.hasTextureRect = true;
        points.needReload = true;
    }
    return true;
}

static void buildDebrisPool(Scene* scene) {
    if (s_debrisScene == scene && (int)s_debris.size() == DEBRIS_POOL) return;

    for (DebrisSlot& s : s_debris) delete s.particles;
    s_debris.clear();
    s_debrisScene = scene;
    s_debrisNext = 0;
    if (!scene) return;

    for (int i = 0; i < DEBRIS_POOL; i++) {
        DebrisSlot slot;

        Entity t = scene->createEntity();
        char name[40];
        snprintf(name, sizeof(name), "VoxelDebrisPoints_%d", i);
        scene->setEntityName(t, name);

        PointsComponent pc;
        pc.maxPoints = DEBRIS_MAX_POINTS;
        pc.texture = Texture(voxelAtlasPath());
        pc.texture.setMinFilter(TextureFilter::NEAREST);
        pc.texture.setMagFilter(TextureFilter::NEAREST);
        pc.texture.setWrapU(TextureWrap::CLAMP_TO_EDGE);
        pc.texture.setWrapV(TextureWrap::CLAMP_TO_EDGE);
        pc.transparent = true;
        pc.autoTransparency = false;
        pc.needUpdateTexture = true;
        pc.needUpdate = true;
        scene->addComponent<PointsComponent>(t, std::move(pc));

        Transform tr;
        tr.position = Vector3(0.0f, 0.0f, 0.0f);
        tr.rotation = Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
        tr.scale = Vector3(1.0f, 1.0f, 1.0f);
        tr.visible = true;
        scene->addComponent<Transform>(t, tr);

        slot.target = t;
        slot.particles = new Particles(scene);
        slot.particles->setOwnedTarget(false);
        slot.particles->setTarget(t);
        s_debris.push_back(slot);
    }
}

void spawnBreakDebris(Scene* scene, int x, int y, int z, uint16_t type, int count) {
    if (!scene || count <= 0) return;
    buildDebrisPool(scene);
    if (s_debris.empty()) return;

    if (type == BLOCK_AIR || stateType(type) == BLOCK_AIR) type = BLOCK_STONE;
    type = stateType(type);
    const BlockDef& def = block(type);

    DebrisSlot& slot = s_debris[(size_t)s_debrisNext];
    s_debrisNext = (s_debrisNext + 1) % DEBRIS_POOL;
    if (!slot.particles || !scene->isEntityCreated(slot.target)) return;

    addSpriteFrameToPoints(scene, slot.target, type);
    PointsComponent& points = scene->getComponent<PointsComponent>(slot.target);
    points.needUpdate = true;

    const int chips = count > DEBRIS_MAX_POINTS ? DEBRIS_MAX_POINTS : count;
    s_debrisLastPoints = chips;
    // VOXEL_NO_DEBRIS=1 fires the burst with no points in it. The chips and the crack overlay are
    // both dark, both land on the mined block and both are screen-aligned at capture distance, so
    // a PNG cannot tell them apart - only removing one of them can.
    const bool noDebris = voxelEnvInt("VOXEL_NO_DEBRIS", 0) != 0;

    Particles& p = *slot.particles;
    p.reset();
    p.setMaxParticles((unsigned int)DEBRIS_MAX_POINTS);
    p.setRate(0);                 // bursts only: a rate would keep a puff alive after the block went
    p.setLoop(false);
    p.setEmitter(false);
    p.clearBursts();

    p.setLifeInitializer(0.45f, 0.85f);
    p.setPositionInitializer(Vector3((float)x + 0.25f, (float)y + 0.25f, (float)z + 0.25f),
                             Vector3((float)x + 0.75f, (float)y + 0.75f, (float)z + 0.75f));
    p.setVelocityInitializer(Vector3(-2.2f, 1.4f, -2.2f), Vector3(2.2f, 4.2f, 2.2f));
    p.setAccelerationInitializer(Vector3(0.0f, -22.0f, 0.0f));
    const Vector3 tint((float)def.r / 255.0f, (float)def.g / 255.0f, (float)def.b / 255.0f);
    p.setColorInitializer(Vector3(tint.x * 0.72f, tint.y * 0.72f, tint.z * 0.72f), tint);
    p.setAlphaInitializer(1.0f);
    // The chip is fully lit for most of its flight and fades only on the way down, so a burst read
    // as debris rather than as a halo.
    p.setAlphaModifier(0.55f, 1.0f, 1.0f, 0.0f);
    p.setSizeInitializer(0.06f, 0.14f);
    p.setSpriteIntializer(0, 0);  // frame 0 = this block's own tile
    p.addBurst(0.0f, noDebris ? 0 : chips);
    p.start();

    s_debrisSpawns++;
}

int breakDebrisPoolSize() { return (int)s_debris.size(); }
uint64_t breakDebrisSpawns() { return s_debrisSpawns; }
int breakDebrisLastPoints() { return s_debrisLastPoints; }

// ---- probe ----

void probeVoxelBreak(VoxelWorld& world, Probe& probe) {
    ProbeModule mod(probe, "break");

    // 1. The progress -> stage map is monotonic and covers all ten tiles.
    int prev = -2;
    bool monotonic = true;
    for (int i = 0; i <= 100; i++) {
        const int s = breakStage((float)i / 100.0f);
        if (s < prev) monotonic = false;
        prev = s;
    }
    probe.check("mining progress maps onto stages without going back", monotonic,
                "0%%..100%% ends at stage %d, 1%% is %d", breakStage(1.0f), breakStage(0.01f));
    probe.check("no crack is drawn before the first tick", breakStage(0.0f) == -1,
                "breakStage(0) = %d", breakStage(0.0f));
    probe.check("all ten stages have an atlas tile",
                breakStageTile(0) == TILE_DESTROY_STAGE_0 &&
                    breakStageTile(BREAK_STAGE_COUNT - 1) == TILE_DESTROY_STAGE_9 &&
                    breakStageTile(-1) == -1,
                "slots %d..%d", breakStageTile(0), breakStageTile(BREAK_STAGE_COUNT - 1));

    // 2. The overlay follows a target, and only rewrites the rect when the stage moves.
    Scene* scene = world.scene();
    if (!scene) { probe.skip("no scene to build the overlay entity in"); return; }
    BreakOverlay& ov = breakOverlay();
    if (!ov.attach(scene)) { probe.skip("overlay could not attach"); return; }

    const uint64_t writes0 = ov.stageWrites();
    const uint64_t moves0 = ov.moves();
    ov.show(3, 70, 3, 0);
    ov.show(3, 70, 3, 0);
    ov.show(3, 70, 3, 0);
    ov.show(3, 70, 3, 5);
    ov.show(9, 71, 4, 5);
    const uint64_t writes1 = ov.stageWrites();
    const uint64_t moves1 = ov.moves();
    probe.check("a held stage does not rewrite the texture rect", writes1 - writes0 == 2,
                "rewrites %llu for 5 show() calls", (unsigned long long)(writes1 - writes0));
    probe.check("moving to another block moves the entity, not the rect", moves1 - moves0 == 2,
                "moves %llu", (unsigned long long)(moves1 - moves0));
    probe.check("the overlay reports the stage it is drawing",
                ov.isShowing() && ov.stage() == 5 && ov.blockX() == 9,
                "stage %d at %d %d %d", ov.stage(), ov.blockX(), ov.blockY(), ov.blockZ());

    ov.show(9, 71, 4, -1);
    probe.check("releasing the button hides the crack", !ov.isShowing() && ov.stage() == -1,
                "showing %d stage %d", ov.isShowing() ? 1 : 0, ov.stage());

    // 3. A break queues chips, and the pool never grows past its ring.
    const uint64_t spawns0 = breakDebrisSpawns();
    for (int i = 0; i < DEBRIS_POOL + 4; i++) spawnBreakDebris(scene, 4, 70, 4, BLOCK_STONE, 12);
    const uint64_t spawns1 = breakDebrisSpawns();
    probe.check("every break call fires one burst", spawns1 - spawns0 == (uint64_t)DEBRIS_POOL + 4,
                "%llu bursts, pool %d", (unsigned long long)(spawns1 - spawns0), breakDebrisPoolSize());
    probe.check("the debris pool is a fixed ring of emitters", breakDebrisPoolSize() == DEBRIS_POOL,
                "%d emitters, last burst %d points", breakDebrisPoolSize(), breakDebrisLastPoints());

    // 4. A stage is one atlas rect, and the ten rects are ten different slots. An overlay that drew
    //    the same tile twice would freeze the crack at one level of damage.
    int distinct = 0;
    for (int stage = 0; stage < BREAK_STAGE_COUNT; stage++) {
        const int slot = breakStageTile(stage);
        const VoxelTileRect r = quantum::voxelTileRect(slot);
        bool seen_before = false;
        for (int prev_stage = 0; prev_stage < stage; prev_stage++) {
            if (breakStageTile(prev_stage) == slot) seen_before = true;
        }
        if (!seen_before && r.u1 > r.u0 && r.v1 > r.v0) distinct++;
    }
    probe.check("the ten stages are ten distinct atlas rects", distinct == BREAK_STAGE_COUNT,
                "%d of %d rects unique", distinct, BREAK_STAGE_COUNT);

    // What the probe cannot see is whether the crack's unpainted pixels are genuinely transparent -
    // an opaque overlay would hide the block under it. That is a picture claim, so it is made by the
    // captured frame and not by a number here.
}

}  // namespace voxel
