// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelSelect.h"

#include "VoxelBlocks.h"
#include "VoxelCollide.h"
#include "VoxelConfig.h"
#include "VoxelMesh.h"
#include "VoxelProbe.h"
#include "VoxelWorld.h"

#include "Scene.h"
#include "Log.h"
#include "Vector3.h"
#include "texture/Texture.h"
#include "math/AABB.h"
#include "math/Quaternion.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace voxel {

using namespace quantum;

// ---- the shape the outline shows ----

// How far the bars float outside the block's own surface, and how wide the two strokes are. The dark
// band is centred on the inflated box so its inner edge clears the face it outlines (a bar that
// overlaps the block loses the depth test on the half inside it and reads as a broken line); the
// light core is a quarter of the width, centred on the same line, so it lands inside the dark band.
static const float SELECT_INFLATE = 0.016f;
static const float SELECT_DARK_T = 0.028f;
static const float SELECT_CORE_T = 0.008f;
static const float SELECT_DARK_A = 0.78f;
static const float SELECT_CORE_A = 0.85f;

int selectStrokeCount() { return 2; }
float selectDarkThickness() { return SELECT_DARK_T; }
float selectCoreThickness() { return SELECT_CORE_T; }

// The 12 edges of an axis-aligned local box, in the order a reader can check against a cube: four
// along x, four along y, four along z.
static int boxSegments(float x0, float y0, float z0, float x1, float y1, float z1,
                       SelectSegment out[], int cap, int written) {
    static const int e[12][6] = {
        { 0, 0, 0, 1, 0, 0 }, { 0, 1, 0, 1, 1, 0 }, { 0, 1, 1, 1, 1, 1 }, { 0, 0, 1, 1, 0, 1 },
        { 0, 0, 0, 0, 1, 0 }, { 1, 0, 0, 1, 1, 0 }, { 1, 0, 1, 1, 1, 1 }, { 0, 0, 1, 0, 1, 1 },
        { 0, 0, 0, 0, 0, 1 }, { 1, 0, 0, 1, 0, 1 }, { 1, 1, 0, 1, 1, 1 }, { 0, 1, 0, 0, 1, 1 },
    };
    const float v[6] = { x0, y0, z0, x1, y1, z1 };
    for (int i = 0; i < 12; i++) {
        if (written >= cap) return written;
        const int* s = e[i];
        // One flag per axis, not one corner per endpoint: each of the six numbers picks min or max
        // *for its own axis*. Reading the whole corner from the first flag would collapse every edge
        // whose two endpoints share an x to a zero-length bar and turn the rest into the cube's
        // diagonal - twelve segments that still span the right bbox, so only the vertex count gives
        // it away.
        out[written].ax = v[s[0] * 3 + 0];
        out[written].ay = v[s[1] * 3 + 1];
        out[written].az = v[s[2] * 3 + 2];
        out[written].bx = v[s[3] * 3 + 0];
        out[written].by = v[s[4] * 3 + 1];
        out[written].bz = v[s[5] * 3 + 2];
        written++;
    }
    return written;
}

// The crossed quads the mesher builds for a plant (VoxelMesh.cpp draws corner 0.5 +- 0.5 on the two
// diagonals, from the voxel floor to the voxel ceiling). Their edges are 4 verticals on the cube's
// own corners plus 4 diagonals on the top and bottom faces: an X seen from above, which is what the
// block actually is. The endpoints are the same numbers the mesher uses, so the outline lands on the
// art instead of next to it.
static int plantSegments(SelectSegment out[], int cap, int written) {
    static const int diag[2][2] = { { 1, 1 }, { 1, -1 } };
    for (int d = 0; d < 2; d++) {
        const float sx = 0.5f + diag[d][0] * 0.5f, sz = 0.5f + diag[d][1] * 0.5f;
        const float ex = 0.5f - diag[d][0] * 0.5f, ez = 0.5f - diag[d][1] * 0.5f;
        // The quad runs (ex,0,ez) - (ex,1,ez) - (sx,1,sz) - (sx,0,sz); its four edges, in order.
        const float q[4][3] = { { ex, 0.0f, ez }, { ex, 1.0f, ez }, { sx, 1.0f, sz }, { sx, 0.0f, sz } };
        for (int i = 0; i < 4; i++) {
            if (written >= cap) return written;
            const int j = (i + 1) & 3;
            out[written].ax = q[i][0]; out[written].ay = q[i][1]; out[written].az = q[i][2];
            out[written].bx = q[j][0]; out[written].by = q[j][1]; out[written].bz = q[j][2];
            written++;
        }
    }
    return written;
}

int selectionSegments(uint16_t id, SelectSegment out[], int cap) {
    if (!out || cap <= 0) return 0;
    const uint16_t type = stateType(id);
    if (type == BLOCK_AIR) return 0;
    const BlockDef& def = block(type);
    if (def.flags & FLAG_NO_MESH) return 0;
    // Water is aimed through, not at: the raycast that feeds this overlay skips liquids, and an
    // outline on the surface the player is swimming in would be a frame around nothing.
    if (def.flags & FLAG_LIQUID) return 0;

    int written = 0;
    // Local space: the same table the player's body resolves against, read with the voxel at the
    // origin so the numbers come back as fractions of one block.
    VoxelBox boxes[4];
    const int n = blockShapeBoxes(makeState(type, 0), 0, 0, 0, boxes);
    for (int i = 0; i < n && written + 12 <= cap; i++) {
        written = boxSegments(boxes[i].minX - SELECT_INFLATE, boxes[i].minY - SELECT_INFLATE,
                              boxes[i].minZ - SELECT_INFLATE, boxes[i].maxX + SELECT_INFLATE,
                              boxes[i].maxY + SELECT_INFLATE, boxes[i].maxZ + SELECT_INFLATE,
                              out, cap, written);
    }
    if (written > 0) return written;
    if (n > 0) return 0;      // shapes exist but the cap is too small: report nothing rather than a partial cube

    const bool plant = (def.flags & FLAG_CUTOUT) && def.shapeCount == 0;
    if (plant) return plantSegments(out, cap, 0);

    // A block with neither a shape nor a sprite: the voxel it fills.
    return boxSegments(-SELECT_INFLATE, -SELECT_INFLATE, -SELECT_INFLATE,
                       1.0f + SELECT_INFLATE, 1.0f + SELECT_INFLATE, 1.0f + SELECT_INFLATE,
                       out, cap, 0);
}

// ---- the overlay ----

// Same 48-byte vertex as the chunk mesher and the crack overlay: POSITION, NORMAL, TEXCOORD1, COLOR.
// The layout is not shared because both modules keep their struct file-local; a third copy with the
// contract on it is cheaper than widening a working module for one more consumer.
struct SelectVertex {
    float p[3];
    float n[3];
    float uv[2];
    float c[4];
};

static const unsigned int SELECT_STRIDE = sizeof(SelectVertex);

struct SelGeom {
    std::vector<SelectVertex> verts;
    std::vector<uint32_t> idx;

    void quad(const float c[4][3], const float n[3], const float col[4]) {
        const uint32_t base = (uint32_t)verts.size();
        for (int i = 0; i < 4; i++) {
            SelectVertex v;
            v.p[0] = c[i][0]; v.p[1] = c[i][1]; v.p[2] = c[i][2];
            v.n[0] = n[0]; v.n[1] = n[1]; v.n[2] = n[2];
            const float uv[4][2] = { { 0.0f, 1.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f } };
            v.uv[0] = uv[i][0]; v.uv[1] = uv[i][1];
            v.c[0] = col[0]; v.c[1] = col[1]; v.c[2] = col[2]; v.c[3] = col[3];
            verts.push_back(v);
        }
        // Both windings: the submesh culls back faces, and a bar is a flat plane with no inside, so
        // one winding would make every edge vanish when seen from the other side. This is how the
        // mesher makes a plant double-sided.
        const uint32_t a = base;
        idx.push_back(a + 0); idx.push_back(a + 1); idx.push_back(a + 2);
        idx.push_back(a + 0); idx.push_back(a + 2); idx.push_back(a + 3);
        idx.push_back(a + 0); idx.push_back(a + 2); idx.push_back(a + 1);
        idx.push_back(a + 0); idx.push_back(a + 3); idx.push_back(a + 2);
    }
};

static inline void sub(const float a[3], const float b[3], float out[3]) {
    out[0] = a[0] - b[0]; out[1] = a[1] - b[1]; out[2] = a[2] - b[2];
}

static inline void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static inline void normalize(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

// Two perpendicular unit vectors around a segment direction. The helper axis is the one the
// direction is closest to being perpendicular to, which keeps the cross product well conditioned on
// the axis-aligned edges a box is made of - and on the 45-degree diagonals of a plant.
static void barFrame(const float d[3], float u[3], float v[3]) {
    const float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
    float e[3] = { 0.0f, 0.0f, 0.0f };
    if (ax <= ay && ax <= az) e[0] = 1.0f;
    else if (ay <= az) e[1] = 1.0f;
    else e[2] = 1.0f;
    cross3(d, e, u);
    normalize(u);
    cross3(d, u, v);
    normalize(v);
}

// One edge of the outline: a bar of width `t` centred on the segment, drawn as two crossed quads so
// it has thickness in screen space whatever the camera is doing.
static void addBar(SelGeom& g, const float a[3], const float b[3], float t, const float col[4]) {
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-6f)) return;
    normalize(d);
    float u[3], v[3];
    barFrame(d, u, v);
    const float h = t * 0.5f;

    for (int plane = 0; plane < 2; plane++) {
        // plane 0: the (d, u) face, offset along v. plane 1: the (d, v) face, offset along u.
        const float* off = plane == 0 ? v : u;
        const float* inPlane = plane == 0 ? u : v;
        float nrm[3];
        cross3(d, inPlane, nrm);
        normalize(nrm);
        float c[4][3];
        for (int i = 0; i < 4; i++) {
            const bool atB = (i == 2 || i == 3);
            const bool positive = (i == 1 || i == 2);
            const float* p = atB ? b : a;
            const float s = positive ? h : -h;
            c[i][0] = p[0] + off[0] * s;
            c[i][1] = p[1] + off[1] * s;
            c[i][2] = p[2] + off[2] * s;
        }
        g.quad(c, nrm, col);
    }
}

// The whole outline for one block: every edge, twice - the dark band then the light core.
static void buildOutline(SelGeom& g, const SelectSegment segs[], int n) {
    const float dark[4] = { 0.02f, 0.02f, 0.02f, SELECT_DARK_A };
    const float core[4] = { 1.0f, 1.0f, 1.0f, SELECT_CORE_A };
    for (int pass = 0; pass < 2; pass++) {
        const float t = pass == 0 ? SELECT_DARK_T : SELECT_CORE_T;
        const float* col = pass == 0 ? dark : core;
        for (int i = 0; i < n; i++) {
            const float a[3] = { segs[i].ax, segs[i].ay, segs[i].az };
            const float b[3] = { segs[i].bx, segs[i].by, segs[i].bz };
            addBar(g, a, b, t, col);
        }
    }
}

static unsigned int uploadOutline(Scene* scene, Entity e, SelGeom& g) {
    MeshComponent& mesh = scene->getComponent<MeshComponent>(e);
    mesh.buffer.clearAll();
    // InterleavedBuffer declares its own addAttribute overloads, which hide the typed ones on
    // Buffer, so the offset has to go through a Buffer& view - same as the chunk mesher.
    Buffer& geom = mesh.buffer;
    geom.addAttribute(AttributeType::POSITION, AttributeDataType::FLOAT, 3, offsetof(SelectVertex, p));
    geom.addAttribute(AttributeType::NORMAL, AttributeDataType::FLOAT, 3, offsetof(SelectVertex, n));
    geom.addAttribute(AttributeType::TEXCOORD1, AttributeDataType::FLOAT, 2, offsetof(SelectVertex, uv));
    geom.addAttribute(AttributeType::COLOR, AttributeDataType::FLOAT, 4, offsetof(SelectVertex, c));
    mesh.buffer.setStride(SELECT_STRIDE);
    mesh.buffer.setUsage(BufferUsage::IMMUTABLE);
    mesh.indices.clearAll();
    mesh.indices.addAttribute(AttributeType::INDEX, AttributeDataType::UNSIGNED_INT, 1, 0);
    mesh.indices.setStride(sizeof(uint32_t));
    mesh.indices.setUsage(BufferUsage::IMMUTABLE);
    mesh.buffer.importData(g.verts.data(), g.verts.size() * SELECT_STRIDE);
    mesh.buffer.setCount((unsigned int)g.verts.size());
    mesh.indices.importData(g.idx.data(), g.idx.size() * sizeof(uint32_t));
    mesh.indices.setCount((unsigned int)g.idx.size());
    mesh.vertexCount = (unsigned int)g.verts.size();
    mesh.needReload = true;
    mesh.loaded = false;
    // The engine reloads a mesh from its draw path on `!loadCalled`, not on `needReload`, and
    // changeLoaded - the only thing that raises `loaded` again - is queued from inside that reload.
    // Without this line the second shape the crosshair finds is written into buffers nobody reads:
    // the cube the overlay was built with stays on screen and a flower draws twelve edges it does
    // not have. The chunk mesher clears the same flag (VoxelMesh).
    mesh.loadCalled = false;
    return (unsigned int)g.verts.size();
}

// What a segment set draws, as one number. Two blocks of the same shape - and most of the world is
// cubes - have the same key, so the buffers are rebuilt when the *shape* changes and not when the
// crosshair moves.
static unsigned long long segmentsKey(const SelectSegment segs[], int n) {
    unsigned long long h = 1469598103934665603ull ^ (unsigned long long)n;
    for (int i = 0; i < n; i++) {
        const float* f = &segs[i].ax;
        for (int k = 0; k < 6; k++) {
            // Quantised to 1/256 of a block: the keys only have to separate shapes that a player can
            // see, and the inflation offset is the same for every box of every block.
            const long q = (long)std::lround(f[k] * 256.0f);
            h ^= (unsigned long long)(q + 0x10000);
            h *= 1099511628211ull;
        }
    }
    return h;
}

bool SelectOverlay::attach(Scene* scene) {
    if (!scene) return false;
    if (entity >= 0 && scenePtr == scene && scene->isEntityCreated((Entity)entity)) return true;

    scenePtr = scene;
    entity = -1;
    drawnId = -1;
    geomKey = 0;
    showing = false;

    // The buffers are never allowed to be empty - an entity with no geometry is an entity the loader
    // retries forever - so the first build is the cube, which is also the most common outline.
    SelectSegment segs[SELECT_MAX_SEGMENTS];
    const int n = selectionSegments(BLOCK_STONE, segs, SELECT_MAX_SEGMENTS);
    SelGeom g;
    buildOutline(g, segs, n);
    segmentsMade = n;
    boxesMade = 1;
    geomKey = segmentsKey(segs, n);
    drawnId = BLOCK_STONE;

    Entity e = scene->createEntity();
    scene->setEntityName(e, "VoxelSelectOverlay");

    MeshComponent mc;
    // The liquid path: unlit and blended, which is the mesh variant this project already has a
    // compiled .sdat for. A lit or alpha-tested outline would need a variant the runtime cannot
    // build, and a variant that does not exist renders nothing without saying so.
    setupVoxelMaterial(mc, true);
    mc.receiveLights = false;
    mc.castShadows = false;
    mc.receiveShadows = false;
    mc.transparent = true;
    Material& m = mc.submeshes[0].material;
    m.name = "voxel_select";
    // No atlas: an empty texture makes the renderer bind its own 8x8 white, so the line is exactly
    // the vertex colour and the block under it is not tinted by whatever tile was sampled.
    m.baseColorTexture = Texture();
    Submesh& sub = mc.submeshes[0];
    sub.hasTextureRect = false;

    scene->addComponent<MeshComponent>(e, std::move(mc));
    vertsMade = uploadOutline(scene, e, g);

    const AABB bounds(Vector3(-0.1f, -0.1f, -0.1f), Vector3(1.1f, 1.1f, 1.1f));
    MeshComponent& mesh = scene->getComponent<MeshComponent>(e);
    mesh.aabb = bounds;
    mesh.verticesAABB = bounds;
    mesh.needUpdateAABB = false;

    Transform t;
    t.position = Vector3(0.0f, -1000.0f, 0.0f);
    t.rotation = Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    t.scale = Vector3(1.0f, 1.0f, 1.0f);
    t.visible = false;
    t.needUpdate = true;
    scene->addComponent<Transform>(e, t);

    entity = (int64_t)e;
    return true;
}

void SelectOverlay::show(int x, int y, int z, uint16_t id) {
    if (!scenePtr || entity < 0) { hide(); return; }
    // The switch that lets a capture prove the frame on the block is this overlay and not the crack.
    if (voxelEnvInt("VOXEL_NO_SELECT", 0) != 0) { hide(); return; }
    const Entity e = (Entity)entity;
    if (!scenePtr->isEntityCreated(e)) { hide(); return; }

    SelectSegment segs[SELECT_MAX_SEGMENTS];
    const int n = selectionSegments(id, segs, SELECT_MAX_SEGMENTS);
    if (n <= 0) { hide(); return; }

    const uint16_t type = stateType(id);
    VoxelBox boxes[4];
    boxesMade = blockShapeBoxes(makeState(type, 0), 0, 0, 0, boxes);

    Transform& t = scenePtr->getComponent<Transform>(e);
    if (!showing || bx != x || by != y || bz != z) {
        t.position = Vector3((float)x, (float)y, (float)z);
        t.visible = true;
        bx = x; by = y; bz = z;
        movesMade++;
    }
    // Every frame, not only on the move: the render node picks a transform up when the frame marks it
    // dirty, and the first frame the outline appears can be the one where the mesh is still loading.
    t.needUpdate = true;
    showing = true;

    const unsigned long long key = segmentsKey(segs, n);
    if (key != geomKey) {
        SelGeom g;
        buildOutline(g, segs, n);
        vertsMade = uploadOutline(scenePtr, e, g);
        geomKey = key;
        drawnId = (int)type;
        uploadsMade++;
    }
    segmentsMade = n;
}

void SelectOverlay::hide() {
    showing = false;
    if (!scenePtr || entity < 0) return;
    const Entity e = (Entity)entity;
    if (!scenePtr->isEntityCreated(e)) return;
    Transform& t = scenePtr->getComponent<Transform>(e);
    t.visible = false;
    t.needUpdate = true;
}

static const MeshComponent* findOutlineMesh(Scene* scene, int64_t entity) {
    if (!scene || entity < 0) return nullptr;
    const Entity e = (Entity)entity;
    if (!scene->isEntityCreated(e)) return nullptr;
    return scene->findComponent<MeshComponent>(e);
}

bool SelectOverlay::geometryLoaded() const {
    const MeshComponent* mesh = findOutlineMesh(scenePtr, entity);
    return mesh && mesh->loaded;
}

unsigned int SelectOverlay::geometryVertices() const {
    const MeshComponent* mesh = findOutlineMesh(scenePtr, entity);
    return mesh ? mesh->vertexCount : 0u;
}

bool SelectOverlay::geometryReloadQueued() const {
    const MeshComponent* mesh = findOutlineMesh(scenePtr, entity);
    return mesh && (mesh->needReload || !mesh->loadCalled);
}

unsigned long long SelectOverlay::geometryShaderBits() const {
    const MeshComponent* mesh = findOutlineMesh(scenePtr, entity);
    return mesh ? (unsigned long long)mesh->submeshes[0].shaderProperties : 0ull;
}

static SelectOverlay g_selectOverlay;

SelectOverlay& selectOverlay() { return g_selectOverlay; }

// ---- probe ----

void probeVoxelSelect(VoxelWorld& world, Probe& probe) {
    ProbeModule mod(probe, "select");

    // The bbox of a segment set, in local units - the measurement every claim below is checked with.
    auto bounds = [&](const SelectSegment segs[], int n, float out[6]) {
        out[0] = out[1] = out[2] = 1e30f;
        out[3] = out[4] = out[5] = -1e30f;
        for (int i = 0; i < n; i++) {
            const float a[3] = { segs[i].ax, segs[i].ay, segs[i].az };
            const float b[3] = { segs[i].bx, segs[i].by, segs[i].bz };
            for (int k = 0; k < 3; k++) {
                if (a[k] < out[k]) out[k] = a[k];
                if (b[k] < out[k]) out[k] = b[k];
                if (a[k] > out[k + 3]) out[k + 3] = a[k];
                if (b[k] > out[k + 3]) out[k + 3] = b[k];
            }
        }
    };

    SelectSegment segs[SELECT_MAX_SEGMENTS];
    float bb[6];

    // Edges that span the same box are not enough: a body diagonal reaches every face too. A box edge
    // moves along exactly one axis, so this counts the segments that are points or cut corners - the
    // mistake a bbox check cannot see.
    auto offAxis = [&](const SelectSegment segs[], int n) {
        int bad = 0;
        for (int i = 0; i < n; i++) {
            int moving = 0;
            if (std::fabs(segs[i].bx - segs[i].ax) > 1e-4f) moving++;
            if (std::fabs(segs[i].by - segs[i].ay) > 1e-4f) moving++;
            if (std::fabs(segs[i].bz - segs[i].az) > 1e-4f) moving++;
            if (moving != 1) bad++;
        }
        return bad;
    };

    // 1. A plain cube is 12 edges and reaches the voxel's own faces.
    const int cubeN = selectionSegments(BLOCK_STONE, segs, SELECT_MAX_SEGMENTS);
    bounds(segs, cubeN, bb);
    probe.check("a full block outlines as the 12 edges of its cube", cubeN == 12 &&
                std::fabs(bb[0]) < 0.02f && std::fabs(bb[3] - 1.0f) < 0.02f,
                "%d edges, x %.3f..%.3f", cubeN, bb[0], bb[3]);
    probe.check("every edge of that cube runs along one axis", offAxis(segs, cubeN) == 0,
                "%d of %d edges cut a corner", offAxis(segs, cubeN), cubeN);

    // 2. The claim the feature exists for: the outline is the collision table, not the cube. A slab
    //    is half high and a fence post is a quarter wide, and both are measured off the same
    //    BlockDef::shapes the player's body is resolved against.
    const int slabN = selectionSegments(BLOCK_SLAB_STONE, segs, SELECT_MAX_SEGMENTS);
    bounds(segs, slabN, bb);
    probe.check("a stone slab outlines at half height, not the full cube", slabN == 12 &&
                std::fabs(bb[1]) < 0.02f && std::fabs(bb[4] - 0.5f) < 0.02f,
                "%d edges, y %.3f..%.3f", slabN, bb[1], bb[4]);

    const int fenceN = selectionSegments(BLOCK_FENCE, segs, SELECT_MAX_SEGMENTS);
    bounds(segs, fenceN, bb);
    probe.check("a fence outlines the post it collides with", fenceN == 12 &&
                std::fabs(bb[0] - 0.375f) < 0.02f && std::fabs(bb[3] - 0.625f) < 0.02f &&
                std::fabs(bb[4] - 1.0f) < 0.02f,
                "%d edges, x %.3f..%.3f y %.3f..%.3f", fenceN, bb[0], bb[3], bb[1], bb[4]);

    const int cactusN = selectionSegments(BLOCK_CACTUS, segs, SELECT_MAX_SEGMENTS);
    bounds(segs, cactusN, bb);
    probe.check("a cactus outlines its inset column", cactusN == 12 &&
                std::fabs(bb[0] - 0.125f) < 0.02f && std::fabs(bb[3] - 0.875f) < 0.02f,
                "%d edges, x %.3f..%.3f", cactusN, bb[0], bb[3]);

    // 3. A plant has no collision at all, so there is no box to show - and outlining the empty cube
    //    would claim a shape the block does not have. It outlines its own crossed quads: 4 verticals
    //    on the cube's corners plus 4 diagonals across the top and bottom faces, so 8 edges, an X
    //    seen from above. The diagonals are the part that proves it is the sprite and not the voxel.
    VoxelBox dummy[4];
    const int poppyBoxes = blockShapeBoxes(makeState(BLOCK_POPPY, 0), 0, 0, 0, dummy);
    const int poppyN = selectionSegments(BLOCK_POPPY, segs, SELECT_MAX_SEGMENTS);
    bounds(segs, poppyN, bb);
    int poppyDiagonals = 0;
    for (int i = 0; i < poppyN; i++) {
        if (std::fabs(segs[i].bx - segs[i].ax) > 0.5f && std::fabs(segs[i].bz - segs[i].az) > 0.5f)
            poppyDiagonals++;
    }
    probe.check("a flower has no collision box", poppyBoxes == 0, "%d boxes from the table", poppyBoxes);
    probe.check("a flower outlines the quads it draws, not the cube around them",
                poppyN == 8 && poppyDiagonals == 4 && std::fabs(bb[4] - 1.0f) < 0.001f &&
                    std::fabs(bb[1]) < 0.001f,
                "%d edges, %d of them diagonal, y %.3f..%.3f", poppyN, poppyDiagonals, bb[1], bb[4]);

    // 4. Nothing is drawn where nothing is aimed at.
    probe.check("air and water outline nothing",
                selectionSegments(BLOCK_AIR, segs, SELECT_MAX_SEGMENTS) == 0 &&
                selectionSegments(makeState(BLOCK_WATER, 15), segs, SELECT_MAX_SEGMENTS) == 0,
                "air %d water %d", selectionSegments(BLOCK_AIR, segs, SELECT_MAX_SEGMENTS),
                selectionSegments(makeState(BLOCK_WATER, 15), segs, SELECT_MAX_SEGMENTS));

    // 5. Every block the table describes yields edges: an id that outlines nothing is a block the
    //    player can break with no feedback at all.
    int silent = 0, silentIds = 0;
    for (int t = 1; t < BLOCK_COUNT; t++) {
        const int n = selectionSegments((uint16_t)t, segs, SELECT_MAX_SEGMENTS);
        if (n <= 0 && !(blockFlags((uint16_t)t) & FLAG_LIQUID)) { silent++; silentIds = t; }
    }
    probe.check("every non-liquid block has an outline", silent == 0,
                "%d blocks with no edges, first id %d", silent, silentIds);

    // 6. The overlay follows the target, and rebuilds geometry only when the *shape* changes.
    Scene* scene = world.scene();
    if (!scene) { probe.skip("no scene to build the overlay entity in"); return; }
    SelectOverlay& ov = selectOverlay();
    if (!ov.attach(scene)) { probe.skip("overlay could not attach"); return; }

    // Start from a hidden overlay and one settled cube, so both counters measure these four calls
    // rather than wherever the crosshair happened to be when the probe ran.
    ov.hide();
    ov.show(3, 69, 3, BLOCK_STONE);
    const uint64_t up0 = ov.uploads();
    const uint64_t mv0 = ov.moves();
    ov.show(3, 70, 3, BLOCK_DIRT);
    ov.show(4, 70, 3, BLOCK_DIRT);
    ov.show(4, 70, 3, BLOCK_GRASS);      // another cube: a different block, the same twelve edges
    ov.show(4, 70, 3, BLOCK_STONE);
    const uint64_t up1 = ov.uploads();
    const uint64_t mv1 = ov.moves();
    probe.check("aiming at cubes of any kind does not rebuild the outline", up1 - up0 == 0,
                "%llu uploads for 4 cube targets", (unsigned long long)(up1 - up0));
    probe.check("only a new voxel moves the entity", mv1 - mv0 == 2,
                "%llu moves for 4 show() calls on 2 voxels", (unsigned long long)(mv1 - mv0));
    probe.check("the overlay reports what it is drawing",
                ov.isShowing() && ov.blockX() == 4 && ov.segments() == 12,
                "showing %d at %d %d %d, %d edges", ov.isShowing() ? 1 : 0,
                ov.blockX(), ov.blockY(), ov.blockZ(), ov.segments());

    ov.show(5, 70, 3, BLOCK_SLAB_STONE);
    const uint64_t up2 = ov.uploads();
    probe.check("a different shape rebuilds the outline exactly once",
                up2 - up1 == 1 && ov.boxes() == 1 && ov.segments() == 12,
                "%llu uploads, %d edges of %d box", (unsigned long long)(up2 - up1),
                ov.segments(), ov.boxes());

    ov.hide();
    probe.check("looking at nothing hides the outline", !ov.isShowing(), "showing %d",
                ov.isShowing() ? 1 : 0);

    // 7. The geometry has to reach the renderer on a variant this project can actually compile. The
    //    bits must carry the vertex colour and must not carry the atlas rect - a variant with no
    //    .sdat on disk leaves the outline invisible and only says so here.
    probe.check("the outline is on the mesh variant already bundled",
                (ov.geometryShaderBits() & (1u << 10)) != 0 &&
                (ov.geometryShaderBits() & (1u << 11)) == 0,
                "bits 0x%08llX, %u vertices, loaded %d", ov.geometryShaderBits(),
                ov.geometryVertices(), ov.geometryLoaded() ? 1 : 0);
    probe.check("each edge is a bar of two crossed faces per stroke",
                ov.builtVertices() == (unsigned int)ov.segments() * 16u,
                "%u vertices built for %d edges", ov.builtVertices(), ov.segments());
    // The two numbers above are what this module wrote; this one is what the component still says it
    // holds. They differ only if the buffer handoff dropped geometry, which draws a partial frame.
    probe.check("the component holds the whole outline it was handed",
                ov.geometryVertices() == ov.builtVertices(),
                "component %u, build %u, loaded %d", ov.geometryVertices(), ov.builtVertices(),
                ov.geometryLoaded() ? 1 : 0);

    // 8. A capture can only show a shape the shipped world actually contains. The flower is the hard
    //    case - it has no collider, so its outline comes from the crossed quads the mesher draws -
    //    and this scan says the seed really grows one and where. The coordinate is the point: it is
    //    what VOXEL_LOOKAT aims a headless frame at, and without it the eight-edge flower is a
    //    number in a test and nothing on screen. The one reported is the nearest to the middle of
    //    the loaded set, which is where the body stands, because a plant five columns away is a
    //    plant no crosshair can reach.
    int sumCx = 0, sumCz = 0, loadedChunks = 0;
    for (int cz = -16; cz <= 16; cz++)
        for (int cx = -16; cx <= 16; cx++)
            if (world.loaded(cx, cz)) { sumCx += cx; sumCz += cz; loadedChunks++; }
    const int midCx = loadedChunks ? (int)std::roundf((float)sumCx / (float)loadedChunks) : 0;
    const int midCz = loadedChunks ? (int)std::roundf((float)sumCz / (float)loadedChunks) : 0;

    int plants = 0, plantAt[3] = { 0, 0, 0 }, plantDist = 1 << 30;
    for (int cz = -16; cz <= 16; cz++) {
        for (int cx = -16; cx <= 16; cx++) {
            if (!world.loaded(cx, cz)) continue;
            const int bx = cx * CHUNK_SX, bz = cz * CHUNK_SZ;
            for (int lz = 0; lz < CHUNK_SZ; lz++) {
                for (int lx = 0; lx < CHUNK_SX; lx++) {
                    const int wx = bx + lx, wz = bz + lz;
                    const int top = world.getTop(wx, wz);
                    if (top < 0 || top + 2 >= WORLD_HEIGHT) continue;
                    for (int y = top + 1; y <= top + 2; y++) {
                        const uint16_t t = stateType(world.get(wx, y, wz));
                        if (t == BLOCK_AIR) continue;
                        if (blockShapeBoxes(makeState(t, 0), 0, 0, 0, dummy) != 0) continue;
                        if (selectionSegments(t, segs, SELECT_MAX_SEGMENTS) <= 0) continue;
                        plants++;
                        const int dx = wx - midCx * CHUNK_SX, dz = wz - midCz * CHUNK_SZ;
                        const int d = dx * dx + dz * dz;
                        if (d < plantDist) { plantDist = d; plantAt[0] = wx; plantAt[1] = y; plantAt[2] = wz; }
                    }
                }
            }
        }
    }
    probe.check("the shipped world grows a block with no collider to aim the outline at", plants > 0,
                "%d plant voxels in the loaded chunks, nearest %d,%d,%d (%d columns from the body)",
                plants, plantAt[0], plantAt[1], plantAt[2],
                plantDist == (1 << 30) ? -1 : (int)std::sqrt((float)plantDist));
}

}  // namespace voxel
