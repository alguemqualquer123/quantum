// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "VoxelMesh.h"
#include "VoxelTiles.h"
#include "VoxelLight.h"

#include "Scene.h"
#include "io/FileData.h"
#include "math/AABB.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#ifdef QUANTUM_PHYSICS_3D
#include "component/Body3DComponent.h"
#endif
#include "VoxelProbe.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace voxel {

using namespace quantum;

const char* const VOXEL_ATLAS_GENERATED = "textures/voxel/atlas.png";
const char* const VOXEL_ATLAS_FROM_PACK = "textures/voxel/atlas_minecraft.png";

const char* voxelAtlasPath() {
    // Asked through the engine's own resolver, because that is the path Texture() will open. A
    // project-relative probe of the current directory would answer about a different file in an
    // exported build, where the asset root is not the cwd.
    static const std::string chosen = [] {
        std::error_code ec;
        if (std::filesystem::exists(FileData::getSystemPath(VOXEL_ATLAS_FROM_PACK), ec))
            return std::string(VOXEL_ATLAS_FROM_PACK);
        return std::string(VOXEL_ATLAS_GENERATED);
    }();
    return chosen.c_str();
}

// One interleaved vertex, 48 bytes, in the order the attribute offsets below declare. A block face
// is 4 of these and 6 indices, so a chunk of 16x16x128 can exceed 65535 vertices easily: indices
// are 32-bit for that reason (the old mesher overflowed them and drew garbage).
struct VoxelVertex {
    float p[3];
    float n[3];
    float uv[2];
    float c[4];
};

static const unsigned int VOXEL_VERTEX_STRIDE = sizeof(VoxelVertex);
static const size_t OFF_POSITION = offsetof(VoxelVertex, p);
static const size_t OFF_NORMAL = offsetof(VoxelVertex, n);
static const size_t OFF_TEXCOORD = offsetof(VoxelVertex, uv);
static const size_t OFF_COLOR = offsetof(VoxelVertex, c);

struct FaceSpec {
    int n[3];
    int t1[3];
    int t2[3];
    float shade;
};

// cross(t1, t2) == n, so corners walked (-1,-1) (1,-1) (1,1) (-1,1) are counter-clockwise seen
// from outside, which is the winding the engine's meshes use.
static const FaceSpec g_faces[FACE_COUNT] = {
    { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, 0.80f },   // +X
    { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 0.80f },  // -X
    { { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 }, 1.00f },   // +Y
    { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, 0.52f },  // -Y
    { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, 0.68f },   // +Z
    { { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0, 0 }, 0.74f },  // -Z
};

static const float AO_LEVELS[4] = { 1.00f, 0.82f, 0.66f, 0.52f };

static uint32_t g_lastBlocks = 0;
uint32_t lastMeshedBlockCount() { return g_lastBlocks; }

// What the probe reads: how many quads have ever been emitted for a liquid, and how many quads came
// out with no area at all. A draw box of zero size turns a face into a line - the mesh uploads,
// reports its vertices and paints nothing - so "the sea is invisible" and "the sea is drawn" differ
// by exactly this number, which is the only part of the story a headless log can tell.
static unsigned long long g_liquidQuads = 0;
static unsigned long long g_flatQuads = 0;
unsigned long long liquidQuadCount() { return g_liquidQuads; }
unsigned long long flatQuadCount() { return g_flatQuads; }

static float g_daylight = 1.0f;
void setMeshDaylight(float factor) {
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    g_daylight = factor;
}
float meshDaylight() { return g_daylight; }

struct MeshAccum {
    std::vector<VoxelVertex> verts;
    std::vector<uint32_t> idx;
    void reserve(size_t faces) { verts.reserve(faces * 4); idx.reserve(faces * 6); }
    void quad(const Vector3 corner[4], const Vector3& normal, const Vector2 uv[4], const float col[4][4]) {
        // The two diagonally-adjacent edges of the quad, crossed: a face whose corners all share an
        // axis in two of the three coordinates has no area, and the rasteriser is handed a line.
        const float e1x = corner[1].x - corner[0].x, e1y = corner[1].y - corner[0].y, e1z = corner[1].z - corner[0].z;
        const float e2x = corner[3].x - corner[0].x, e2y = corner[3].y - corner[0].y, e2z = corner[3].z - corner[0].z;
        const float span = std::fabs(e1x * e2y - e1y * e2x) + std::fabs(e1y * e2z - e1z * e2y)
                         + std::fabs(e1z * e2x - e1x * e2z);
        if (span < 1e-8f) g_flatQuads++;
        uint32_t base = (uint32_t)verts.size();
        for (int i = 0; i < 4; i++) {
            VoxelVertex v;
            v.p[0] = corner[i].x; v.p[1] = corner[i].y; v.p[2] = corner[i].z;
            v.n[0] = normal.x; v.n[1] = normal.y; v.n[2] = normal.z;
            v.uv[0] = uv[i].x; v.uv[1] = uv[i].y;
            v.c[0] = col[i][0]; v.c[1] = col[i][1]; v.c[2] = col[i][2]; v.c[3] = col[i][3];
            verts.push_back(v);
        }
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    }
};

// Whether the neighbour's drawn volume hides `face` of `self`. The two boxes have to meet on the
// plane their cells share - so this face has to be flush with the edge of its own cell and that one
// has to reach the edge from the other side - and the neighbour has to span this rectangle on the two
// axes along the plane. A face that hangs inside its cell (the side of a slab, a cactus, a fence
// post) has air in front of it no matter what stands in the next cell, so nothing can hide it.
static inline bool shapeCovers(const BlockBox& self, int face, const BlockBox& nb) {
    static const float EPS = 1e-4f;
    const float sMin[3] = { self.x0, self.y0, self.z0 };
    const float sMax[3] = { self.x1, self.y1, self.z1 };
    const float nMin[3] = { nb.x0, nb.y0, nb.z0 };
    const float nMax[3] = { nb.x1, nb.y1, nb.z1 };
    const int axis = face >> 1;                      // faces are +x, -x, +y, -y, +z, -z
    const bool positive = (face & 1) == 0;
    if (positive) {
        if (sMax[axis] < 1.0f - EPS || nMin[axis] > EPS) return false;
    } else {
        if (sMin[axis] > EPS || nMax[axis] < 1.0f - EPS) return false;
    }
    for (int a = 0; a < 3; a++) {
        if (a == axis) continue;
        if (nMin[a] > sMin[a] + EPS || nMax[a] < sMax[a] - EPS) return false;
    }
    return true;
}

// A face of `self` is drawn when `other` does not hide it. Water never draws a face between two
// water voxels (that put a hollow skin inside every ocean), and two identical transparent blocks
// cull their shared face, which is what makes glass and leaves read as one surface.
//
// Hiding, though, is a question about geometry and not only about the block table. An opaque
// neighbour covers this face only where its drawn volume reaches the plane the two cells share and
// spans the whole rectangle: a cactus is 12/16 across, a slab 8/16 tall and a fence post 4/16 wide,
// so the face they share with the block beside them leaves a strip of air that the eye looks
// straight through. Culling on the voxel alone therefore opens the box on that side, back-face
// culling hides its inside with it, and the block reads as a hole in the world - which is what an
// invisible row of cacti, a stack of slabs or a line of fences actually was.
static inline bool faceVisible(uint16_t self, uint16_t other, int face, const BlockBox& selfBox,
                               const BlockBox& otherBox) {
    if (other == BLOCK_AIR) return true;
    const uint32_t sf = block(self).flags;
    const uint32_t of = block(other).flags;
    if ((of & FLAG_OPAQUE) && shapeCovers(selfBox, face, otherBox)) return false;
    if (sf & FLAG_OPAQUE) return true;
    if ((sf & FLAG_LIQUID) && (of & FLAG_LIQUID)) return false;
    if (self == other) return false;
    return true;
}

// The floor both channels ride on: a cave is dark, never black, and the day/night curve keeps its
// soft top so terraces stay readable.
static const float LIGHT_AMBIENT = 0.10f;

static inline float lightCurve(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > (float)LIGHT_MAX) level = (float)LIGHT_MAX;
    return 0.90f * std::pow(level / (float)LIGHT_MAX, 0.65f);
}

// The two light channels are two different colours, and the difference is the whole tell that a
// torch is doing something the sun does not: daylight is a white that goes cold blue as the sun
// sets, torchlight is orange. One grey ramp made them the same light at different brightness, which
// is why a lit cave read as "nothing happened" the moment the sun went down. The channels add, each
// through its own curve, and the ambient floor rides on the sky tint so an unlit corner at midnight
// is a dark blue rather than a hole.
static inline void lightColour(int sky, int blockLight, float daylight, float out[3]) {
    const float cool[3] = { 0.55f + 0.45f * daylight, 0.66f + 0.34f * daylight, 1.00f };
    const float warm[3] = { 1.00f, 0.63f, 0.30f };
    const float s = lightCurve((float)sky * daylight);
    const float b = lightCurve((float)blockLight);
    for (int a = 0; a < 3; a++) {
        const float v = (LIGHT_AMBIENT + s) * cool[a] + b * warm[a];
        out[a] = v > 1.0f ? 1.0f : v;
    }
}

// Skylight before the light solver is wired in: full sun above the column top, one level less per
// block below it. Matches what a real solver produces on a flat surface, so the world looks right
// from the first frame and only caves need the solver.
static int fallbackSky(const Chunk& c, int lx, int wy, int lz) {
    int top = c.topAt(lx, lz);
    if (wy >= top) return LIGHT_MAX;
    int d = top - wy;
    return d > LIGHT_MAX ? 0 : LIGHT_MAX - d;
}

void setupVoxelMaterial(MeshComponent& mesh, bool liquid) {
    mesh.numSubmeshes = 1;
    mesh.loaded = false;
    mesh.loadCalled = false;
    // The liquid is unlit on purpose: every shade it has is already baked into its vertices (face
    // shade, AO, skylight, the depth ramp), and the PBR path adds a specular of the sky that, at the
    // grazing angles a flat sea is seen from, is a Fresnel mirror and washes the whole surface to the
    // colour of the clouds. Measured: a lit sea over sand came out rgb(145,153,172) - the same pale
    // sheet the player reads as "no water". Solids keep the light, so the sun still shapes terrain.
    mesh.receiveLights = !liquid;
    mesh.receiveIBL = false;
    mesh.castShadows = !liquid;
    mesh.receiveShadows = true;
    mesh.transparent = liquid;
    mesh.autoTransparency = false;
    mesh.lodEnabled = false;
    mesh.cullingMode = CullingMode::BACK;

    Material& m = mesh.submeshes[0].material;
    m.name = liquid ? "voxel_liquid" : "voxel_blocks";
    // The liquid's opacity is authored per quad by the mesher's depth ramp, so the material must
    // contribute a factor of exactly 1: any alpha here would silently rescale the whole ramp.
    m.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    m.metallicFactor = 0.0f;
    m.roughnessFactor = 0.92f;
    m.alphaCutoff = 0.5f;
    m.alphaMode = liquid ? MaterialAlphaMode::BLEND : MaterialAlphaMode::MASK;
    m.baseColorTexture = Texture(voxelAtlasPath());
    // Nearest filtering is the whole look: a 16x16 tile linear-filtered turns to mush at distance.
    m.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
    m.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
    m.baseColorTexture.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    m.baseColorTexture.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.submeshes[0].faceCulling = true;
    mesh.submeshes[0].textureRect = Rect(0.0, 0.0, 1.0, 1.0);
    mesh.submeshes[0].needUpdateTexture = true;
}

static Entity makeChunkEntity(Scene* scene, const std::string& name, int cx, int cz, bool liquid) {
    Entity e = scene->createEntity();
    scene->setEntityName(e, name);

    MeshComponent mc;
    setupVoxelMaterial(mc, liquid);
    scene->addComponent<MeshComponent>(e, std::move(mc));

    Transform t;
    t.position = Vector3((float)(cx * CHUNK_SX), 0.0f, (float)(cz * CHUNK_SZ));
    t.rotation = Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    t.scale = Vector3(1.0f, 1.0f, 1.0f);
    t.visible = true;
    scene->addComponent<Transform>(e, t);
    return e;
}

static void uploadMesh(Scene* scene, Entity entity, MeshAccum& acc, float maxY, bool collidable) {
    if (!scene || !scene->isEntityCreated(entity)) return;
    MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

    mesh.buffer.clearAll();
    // InterleavedBuffer declares its own addAttribute overloads, which hide the typed ones on
    // Buffer, so the explicit data type + byte offset has to go through a Buffer& view.
    Buffer& geom = mesh.buffer;
    geom.addAttribute(AttributeType::POSITION, AttributeDataType::FLOAT, 3, OFF_POSITION);
    geom.addAttribute(AttributeType::NORMAL, AttributeDataType::FLOAT, 3, OFF_NORMAL);
    geom.addAttribute(AttributeType::TEXCOORD1, AttributeDataType::FLOAT, 2, OFF_TEXCOORD);
    geom.addAttribute(AttributeType::COLOR, AttributeDataType::FLOAT, 4, OFF_COLOR);
    mesh.buffer.setStride(VOXEL_VERTEX_STRIDE);
    mesh.buffer.setUsage(BufferUsage::IMMUTABLE);

    mesh.indices.clearAll();
    mesh.indices.addAttribute(AttributeType::INDEX, AttributeDataType::UNSIGNED_INT, 1, 0);
    mesh.indices.setStride(sizeof(uint32_t));
    mesh.indices.setUsage(BufferUsage::IMMUTABLE);

    if (acc.verts.empty()) {
        // An entity with no geometry is an entity the loader will retry forever. Hide it instead.
        Transform& t = scene->getComponent<Transform>(entity);
        t.visible = false;
        mesh.loaded = true;
        mesh.needReload = false;
        return;
    }

    mesh.buffer.importData(acc.verts.data(), acc.verts.size() * VOXEL_VERTEX_STRIDE);
    mesh.buffer.setCount((unsigned int)acc.verts.size());
    mesh.indices.importData(acc.idx.data(), acc.idx.size() * sizeof(uint32_t));
    mesh.indices.setCount((unsigned int)acc.idx.size());

    mesh.vertexCount = (unsigned int)acc.verts.size();
    mesh.aabb = AABB(Vector3(0.0f, 0.0f, 0.0f), Vector3((float)CHUNK_SX, maxY, (float)CHUNK_SZ));
    mesh.verticesAABB = mesh.aabb;
    mesh.needUpdateAABB = false;
    mesh.needReload = true;      // buffers changed size: rebuild the pipeline bindings
    mesh.loaded = false;

#ifdef QUANTUM_PHYSICS_3D
    // Install the static collision only after the CPU vertex/index buffers exist. Creating the
    // body alongside an empty MeshComponent makes PhysicsSystem report a failed cook every frame
    // until the upload lands, which is noisy and needlessly retries invalid geometry.
    if (collidable && scene->findComponent<Body3DComponent>(entity) == nullptr) {
        Body3DComponent body;
        body.type = BodyType::STATIC;
        body.numShapes = 1;
        body.shapes[0] = Shape3D();
        body.shapes[0].type = Shape3DType::MESH;
        body.shapes[0].source = Shape3DSource::ENTITY_MESH;
        body.shapes[0].sourceEntity = entity;
        body.shapes[0].density = 1000.0f;
        scene->addComponent<Body3DComponent>(entity, std::move(body));
    }
#endif
}

void buildChunkMesh(VoxelWorld& world, Chunk& chunk) {
    Scene* scene = world.scene();
    if (!scene) return;

    // A remesh replaces the geometry wholesale and the buffers are immutable GPU allocations, so
    // the cheap-and-correct move is to drop the entities and make new ones. Persistent dynamic
    // buffers are the optimisation to do after the frame budget is measured.
    // The slot must be fetched *after* the drop: destroyRender erases the map node, so a reference
    // taken before it would be freed memory by the time the entity ids were written into it.
    world.destroyRender(chunk.cx, chunk.cz);
    ChunkRender& render = world.render(chunk.cx, chunk.cz);

    char nameBuf[64];
    snprintf(nameBuf, sizeof(nameBuf), "VoxelChunk_%d_%d", chunk.cx, chunk.cz);
    render.opaqueEntity = (int64_t)makeChunkEntity(scene, nameBuf, chunk.cx, chunk.cz, false);
    snprintf(nameBuf, sizeof(nameBuf), "VoxelWater_%d_%d", chunk.cx, chunk.cz);
    render.liquidEntity = (int64_t)makeChunkEntity(scene, nameBuf, chunk.cx, chunk.cz, true);

    MeshAccum opaque, liquid;
    opaque.reserve(512); liquid.reserve(128);

    const float daylight = meshDaylight();
    uint32_t blocksMeshed = 0;

    const bool lit = chunk.lit;
    const int baseX = chunk.cx * CHUNK_SX;
    const int baseZ = chunk.cz * CHUNK_SZ;

    for (int y = 0; y < WORLD_HEIGHT; y++) {
        for (int z = 0; z < CHUNK_SZ; z++) {
            for (int x = 0; x < CHUNK_SX; x++) {
                uint16_t id = chunk.blocks[voxelIndex(x, y, z)];
                uint16_t type = stateType(id);
                if (type == BLOCK_AIR) continue;
                const BlockDef& def = block(type);
                if (def.flags & FLAG_NO_MESH) continue;

                const bool plant = (def.flags & FLAG_CUTOUT) && def.shapeCount == 0;
                MeshAccum& acc = (def.flags & FLAG_LIQUID) ? liquid : opaque;
                blocksMeshed++;

                int wx = baseX + x, wz = baseZ + z;

                // A plant is two crossed quads: no face culling logic applies, it has no solid side.
                if (plant) {
                    int ly = y > 0 ? stateType(chunk.get(x, y - 1, z)) : BLOCK_STONE;
                    if (ly == BLOCK_AIR) continue;   // floating plant, nothing to grow from
                    const VoxelTileRect tile = quantum::voxelTileRect(def.tiles[FACE_PY]);
                    float lum[3];
                    lightColour(lit ? chunk.skyAt(x, y, z) : fallbackSky(chunk, x, y, z),
                                lit ? chunk.blockAt(x, y, z) : 0, daylight, lum);
                    float col[4][4] = { { lum[0], lum[1], lum[2], 1 }, { lum[0], lum[1], lum[2], 1 },
                                        { lum[0], lum[1], lum[2], 1 }, { lum[0], lum[1], lum[2], 1 } };
                    const float k = 0.70710678f;
                    static const int diag[2][2] = { { 1, 1 }, { 1, -1 } };
                    for (int d = 0; d < 2; d++) {
                        Vector3 normal(diag[d][0] * k, 0.0f, diag[d][1] * k);
                        for (int side = 0; side < 2; side++) {
                            Vector3 n = side == 0 ? normal : Vector3(-normal.x, 0.0f, -normal.z);
                            // the diagonal runs between two opposite corners of the block
                            float sx = diag[d][0] * 0.5f, sz = diag[d][1] * 0.5f;
                            Vector3 corner[4];
                            corner[0] = Vector3(x + 0.5f - sx, (float)y, z + 0.5f - sz);
                            corner[1] = Vector3(x + 0.5f - sx, (float)y + 1.0f, z + 0.5f - sz);
                            corner[2] = Vector3(x + 0.5f + sx, (float)y + 1.0f, z + 0.5f + sz);
                            corner[3] = Vector3(x + 0.5f + sx, (float)y, z + 0.5f + sz);
                            if (side == 1) { std::swap(corner[0], corner[3]); std::swap(corner[1], corner[2]); }
                            // corners 0/3 are the feet and 1/2 the head, and v0 is the tile's top edge.
                            Vector2 uv[4] = { { tile.u0, tile.v1 }, { tile.u0, tile.v0 }, { tile.u1, tile.v0 }, { tile.u1, tile.v1 } };
                            acc.quad(corner, n, uv, col);
                        }
                    }
                    continue;
                }

                // Collision shape drives the drawn shape: a slab is half a cube, a fence a post, and
                // the upper-slab state lifts that half to the ceiling of the cell. Read once per
                // block, because the face cull below needs the same box the quad is built from - a
                // face is only hidden where two drawn volumes actually meet.
                BlockBox self = blockBox(id);
                if (def.flags & FLAG_LIQUID) {
                    // A liquid is swum through, so it has no collision shape at all and shapes[0]
                    // is the zero box the table leaves it. Taking that as the draw box collapses
                    // every face onto one vertical line: the mesh uploads, reports its vertices,
                    // and paints nothing. The water's box is therefore authored here - the full
                    // footprint of the voxel, resting on its floor.
                    self.x0 = 0.0f; self.y0 = 0.0f; self.z0 = 0.0f;
                    self.x1 = 1.0f; self.z1 = 1.0f;
                    // Water fills the voxel only to its level, so a 3/4 flow reads as a 3/4
                    // surface. Level 15 is a source block; the generator always writes one.
                    const int level = state::waterLevel(id);
                    if (type == BLOCK_WATER) self.y1 = 0.125f + 0.8125f * ((float)level / 15.0f);
                    else self.y1 = 0.9f;
                }
                const float b0[3] = { self.x0, self.y0, self.z0 };
                const float b1[3] = { self.x1, self.y1, self.z1 };
                const float half[3] = { (b1[0] - b0[0]) * 0.5f, (b1[1] - b0[1]) * 0.5f,
                                        (b1[2] - b0[2]) * 0.5f };
                const float centre[3] = { (b0[0] + b1[0]) * 0.5f, (b0[1] + b1[1]) * 0.5f,
                                          (b0[2] + b1[2]) * 0.5f };

                for (int f = 0; f < FACE_COUNT; f++) {
                    if ((def.flags & FLAG_LIQUID) && f == FACE_NY) continue;   // no bottom face on a liquid
                    const FaceSpec& fs = g_faces[f];
                    int nxp = wx + fs.n[0], nyp = y + fs.n[1], nzp = wz + fs.n[2];
                    const uint16_t otherId = world.sampleForMesh(nxp, nyp, nzp);
                    const uint16_t neighbour = stateType(otherId);
                    if (!faceVisible(type, neighbour, f, self, blockBox(otherId))) continue;

                    // std::fabs(int) picks the double overload, so the axis vectors are read as floats.
                    float halfN = std::fabs((float)fs.n[0]) * half[0] + std::fabs((float)fs.n[1]) * half[1] + std::fabs((float)fs.n[2]) * half[2];
                    float halfT1 = std::fabs((float)fs.t1[0]) * half[0] + std::fabs((float)fs.t1[1]) * half[1] + std::fabs((float)fs.t1[2]) * half[2];
                    float halfT2 = std::fabs((float)fs.t2[0]) * half[0] + std::fabs((float)fs.t2[1]) * half[1] + std::fabs((float)fs.t2[2]) * half[2];

                    const VoxelTileRect tile = quantum::voxelTileRect(def.tiles[f]);

                    // How much water stands under this voxel. The atlas paints a puddle and an ocean
                    // with the same blue tile at the same 0.7 alpha, and one translucent quad over
                    // bright sand is what makes a sea read as an empty hole: the eye needs the
                    // column, not the sheet. Water absorbs as light crosses it - red first, then
                    // green, almost no blue - so depth is a colour shift, not a darkening.
                    float deepR = 1.0f, deepG = 1.0f, deepB = 1.0f, deepAlpha = 1.0f;
                    if (def.flags & FLAG_LIQUID) {
                        // Ten blocks is the whole ramp: past that the column is already opaque, and a
                        // deeper exponent only drives the red term to zero, which turns the sea into
                        // ink and reads as a hole again.
                        int under = 1;
                        while (under < 11 &&
                               (blockFlags(stateType(world.sampleForMesh(wx, y - under, wz))) & FLAG_LIQUID)) under++;
                        const float path = (float)(under - 1);
                        // The absorption ramp is water's: the same factors would turn a lava column
                        // brown-blue, which is the opposite of what glowing rock does as it deepens.
                        // Measured against the real thing, the exponents are gentle - the tile is
                        // already blue, and this only has to take the shallow end off the sand colour.
                        if (type == BLOCK_WATER) {
                            deepR = std::pow(0.80f, path);
                            deepG = std::pow(0.90f, path);
                            deepB = std::pow(0.97f, path);
                        }
                        // A column is not one layer: n layers of a tile that transmits t leave t^n
                        // of the bed visible, so the composite alpha is 1 - t^n. The vertex colour is
                        // multiplied into the material alpha before the blend, which is the engine's
                        // only way to ask for more opacity than the texture alone carries.
                        const float transmit = std::pow(0.294f, path + 1.0f);
                        deepAlpha = (1.0f - transmit) / 0.706f;
                        if (deepAlpha > 1.42f) deepAlpha = 1.42f;
                    }

                    int skyN = lit ? world.getSky(nxp, nyp, nzp) : fallbackSky(chunk, x + fs.n[0], y + fs.n[1], z + fs.n[2]);
                    int blkN = lit ? world.getBlockLight(nxp, nyp, nzp) : 0;
                    float lum[3];
                    lightColour(skyN, blkN, daylight, lum);

                    Vector3 corner[4];
                    Vector2 uv[4];
                    float col[4][4];
                    const float sign[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
                    for (int i = 0; i < 4; i++) {
                        float s1 = sign[i][0], s2 = sign[i][1];
                        Vector3 p;
                        p.x = x + centre[0] + fs.n[0] * halfN + fs.t1[0] * s1 * halfT1 + fs.t2[0] * s2 * halfT2;
                        p.y = y + centre[1] + fs.n[1] * halfN + fs.t1[1] * s1 * halfT1 + fs.t2[1] * s2 * halfT2;
                        p.z = z + centre[2] + fs.n[2] * halfN + fs.t1[2] * s1 * halfT1 + fs.t2[2] * s2 * halfT2;
                        corner[i] = p;

                        float u = (s1 + 1.0f) * 0.5f;
                        float v = (s2 + 1.0f) * 0.5f;
                        // s2 = -1 is the foot of the face and v1 is the tile's bottom edge: the atlas is
                        // sampled with v = 0 at its first row, so up on the block is down in v.
                        uv[i] = Vector2(tile.u0 + (tile.u1 - tile.u0) * u, tile.v1 + (tile.v0 - tile.v1) * v);

                        // Ambient occlusion: the two blocks beside the corner and the one on its
                        // diagonal. Two filled sides darken fully, one is a soft edge.
                        int o1 = 0, o2 = 0, o3 = 0;
                        if (f < 2) {   // normal along x: tangents are y and z
                            int sy = (int)(fs.t1[1] * s1), sz = (int)(fs.t2[2] * s2);
                            o1 = world.opaqueAt(nxp, nyp + sy, nzp);
                            o2 = world.opaqueAt(nxp, nyp, nzp + sz);
                            o3 = world.opaqueAt(nxp, nyp + sy, nzp + sz);
                        } else if (f < 4) {
                            int sx = (int)(fs.t1[0] * s1), sz = (int)(fs.t2[2] * s2);
                            o1 = world.opaqueAt(nxp + sx, nyp, nzp);
                            o2 = world.opaqueAt(nxp, nyp, nzp + sz);
                            o3 = world.opaqueAt(nxp + sx, nyp, nzp + sz);
                        } else {
                            int sx = (int)(fs.t1[0] * s1), sy = (int)(fs.t2[1] * s2);
                            o1 = world.opaqueAt(nxp + sx, nyp, nzp);
                            o2 = world.opaqueAt(nxp, nyp + sy, nzp);
                            o3 = world.opaqueAt(nxp + sx, nyp + sy, nzp);
                        }
                        int level = (o1 && o2) ? 3 : (o1 + o2 + o3);
                        float shade = fs.shade * AO_LEVELS[level & 3];
                        float tl = def.r / 255.0f, tg = def.g / 255.0f, tb = def.b / 255.0f;
                        col[i][0] = tl * shade * lum[0] * deepR;
                        col[i][1] = tg * shade * lum[1] * deepG;
                        col[i][2] = tb * shade * lum[2] * deepB;
                        col[i][3] = deepAlpha;
                    }
                    Vector3 normal((float)fs.n[0], (float)fs.n[1], (float)fs.n[2]);
                    acc.quad(corner, normal, uv, col);
                    if (def.flags & FLAG_LIQUID) g_liquidQuads++;
                }
            }
        }
    }

    g_lastBlocks = blocksMeshed;

    uploadMesh(scene, (Entity)render.opaqueEntity, opaque, (float)WORLD_HEIGHT, true);
    uploadMesh(scene, (Entity)render.liquidEntity, liquid, (float)WORLD_HEIGHT, false);
    render.hasMesh = !opaque.verts.empty() || !liquid.verts.empty();
    render.opaqueTris = (uint32_t)(opaque.idx.size() / 3);
    render.liquidTris = (uint32_t)(liquid.idx.size() / 3);
    render.meshVersion = world.editCounter();
}

// ---- probe ----

namespace {

// True when the engine would find this file when it opens it. Same resolver, same answer.
bool atlasOnDisk(const char* path) {
    std::error_code ec;
    return std::filesystem::exists(FileData::getSystemPath(path), ec);
}

// "<name> <index> <col> <row> <cutout> <liquid>", the row format both atlas scripts write next to
// the PNG they write. The first four fields are the slot layout the mesher's UVs come from; the
// last two are painters' hints the runtime reads from VoxelTiles.h instead.
struct AtlasRow {
    std::string name;
    int index = -1;
    int col = -1;
    int row = -1;

    bool samePlace(const AtlasRow& o) const {
        return name == o.name && index == o.index && col == o.col && row == o.row;
    }
};

// The sidecar of an atlas PNG. Both scripts write them together, so the manifest is found by name
// and nothing has to be configured twice.
std::string manifestFor(const char* pngPath) {
    std::string p(pngPath);
    const size_t dot = p.rfind(".png");
    if (dot != std::string::npos) p.replace(dot, 4, ".txt");
    return p;
}

std::vector<AtlasRow> readAtlasManifest(const std::string& path) {
    std::vector<AtlasRow> rows;
    std::ifstream in(FileData::getSystemPath(path).c_str());
    if (!in) return rows;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream cells(line);
        AtlasRow r;
        int cutout = 0, liquid = 0;
        if (!(cells >> r.name >> r.index >> r.col >> r.row)) break;   // a short row is a broken manifest
        cells >> cutout >> liquid;
        rows.push_back(r);
    }
    return rows;
}

// The edge of a PNG straight out of its IHDR, with no image decoder and no GPU: width and height
// are big-endian uint32s at offsets 16 and 20, after the 8-byte signature and the 4-byte chunk
// length plus "IHDR". Returns false for anything that is not a PNG header, so a truncated or
// replaced file is reported rather than measured as zero.
bool pngEdge(const char* path, int& width, int& height) {
    static const unsigned char SIG[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    unsigned char head[24];
    std::ifstream in(FileData::getSystemPath(path).c_str(), std::ios::binary);
    if (!in.read((char*)head, (std::streamsize)sizeof(head))) return false;
    for (int i = 0; i < 8; i++) if (head[i] != SIG[i]) return false;
    if (head[12] != 'I' || head[13] != 'H' || head[14] != 'D' || head[15] != 'R') return false;
    const auto be32 = [&head](int at) {
        return (int)(((unsigned)head[at] << 24) | ((unsigned)head[at + 1] << 16) |
                     ((unsigned)head[at + 2] << 8) | (unsigned)head[at + 3]);
    };
    width = be32(16);
    height = be32(20);
    return true;
}

}  // namespace

void probeVoxelAtlas(Probe& probe) {
    // The mesher's own invariant, before anything about textures: a face has to cover something.
    // Water and lava carry no collision shape, so their draw box is authored in the mesher, and a
    // box that reads back as zero makes an ocean of 22k vertices that paints not one pixel.
    {
        ProbeModule mesh(probe, "mesh");
        probe.check("every quad the mesher emits has area", flatQuadCount() == 0,
                    "%llu flat quads, %llu of them liquid, last chunk %u blocks",
                    flatQuadCount(), liquidQuadCount(), lastMeshedBlockCount());
        probe.check("the liquid is meshed, not only counted", liquidQuadCount() > 0,
                    "%llu liquid quads since boot", liquidQuadCount());
    }

    ProbeModule mod(probe, "atlas");

    const std::string chosen = voxelAtlasPath();
    const bool packOnDisk = atlasOnDisk(VOXEL_ATLAS_FROM_PACK);

    probe.check("the mesher binds an atlas that is on disk", atlasOnDisk(chosen.c_str()),
                "%s", chosen.c_str());
    probe.check("the generated atlas always ships as the fallback",
                atlasOnDisk(VOXEL_ATLAS_GENERATED), "%s", VOXEL_ATLAS_GENERATED);
    // The swap rule in one line: the stitched atlas is used exactly while it exists, and nothing
    // else can make the game draw it. A world saved against either atlas stays readable, because
    // both use the same slots - which is the next check.
    probe.check("the pack atlas is chosen exactly when it is installed",
                (chosen == VOXEL_ATLAS_FROM_PACK) == packOnDisk,
                "chosen %s, pack on disk %d", chosen.c_str(), packOnDisk ? 1 : 0);

    // setupVoxelMaterial is what every chunk, drop cube and crack overlay is built from, so the
    // resolver only matters if the material carries its answer.
    {
        MeshComponent mc;
        setupVoxelMaterial(mc, false);
        const std::string bound = mc.submeshes[0].material.baseColorTexture.getPath();
        probe.check("the chunk material carries the chosen atlas", bound == chosen,
                    "material path %s", bound.c_str());

        // The liquid's two authored invariants, both of which fail without a word of complaint.
        // Unlit: a lit translucent sheet, seen at the grazing angles a flat sea offers, is a Fresnel
        // mirror of the sky and paints the ocean the colour of the clouds - measured rgb(145,153,172)
        // over sand, which is what "the water is still transparent" actually was. And the material
        // alpha has to be exactly one, because the per-vertex depth ramp is the only thing that owns
        // the opacity; a factor here would silently rescale every column in the world.
        MeshComponent lc;
        setupVoxelMaterial(lc, true);
        const Material& lm = lc.submeshes[0].material;
        probe.check("the liquid is unlit and its opacity belongs to the depth ramp",
                    !lc.receiveLights && lm.baseColorFactor.w == 1.0f,
                    "receiveLights %d, material alpha %.3f, alphaMode %d",
                    lc.receiveLights ? 1 : 0, (double)lm.baseColorFactor.w, (int)lm.alphaMode);
    }

    if (!packOnDisk) {
        probe.skip("no stitched atlas on disk: the pack is the user's own files and is not committed");
        return;
    }

    // The invariant the whole swap rests on, measured on the files rather than on the scripts that
    // wrote them: a saved chunk stores slot numbers, so a manifest that disagrees with the base
    // atlas's would repaint every block in the world with the wrong tile.
    const std::string baseTxt = manifestFor(VOXEL_ATLAS_GENERATED);
    const std::string mineTxt = manifestFor(VOXEL_ATLAS_FROM_PACK);
    const std::vector<AtlasRow> base = readAtlasManifest(baseTxt);
    const std::vector<AtlasRow> mine = readAtlasManifest(mineTxt);
    const size_t common = base.size() < mine.size() ? base.size() : mine.size();
    size_t drift = 0;
    for (size_t i = 0; i < common; i++) if (!base[i].samePlace(mine[i])) drift++;
    probe.check("both atlases put every tile in the same slot",
                !base.empty() && base.size() == mine.size() && drift == 0,
                "%d of %d shared rows differ, %zu vs %zu rows in %s and %s",
                (int)drift, (int)common, base.size(), mine.size(), baseTxt.c_str(), mineTxt.c_str());

    // voxelTileRect() hardcodes 16 columns of normalised UVs, so an atlas that is not a square
    // multiple of 16 would sample the wrong cells while still loading without a word of complaint.
    int w = 0, h = 0;
    const bool square = pngEdge(chosen.c_str(), w, h) && w == h && w > 0 && (w % 16) == 0;
    probe.check("the chosen atlas is a square grid of 16 columns", square,
                "%s is %dx%d, tile would be %d px", chosen.c_str(), w, h, w / 16);
}

// ---- shape probe ----

namespace {

// What one block cell of the mesh the GPU was handed actually occupies, measured in units of that
// cell. The mesher writes chunk-local positions, so a voxel at (lx,ly,lz) puts its corners inside
// [0,1] of that cell and the scan subtracts the origin: the assertions then read as the 1/16ths the
// BlockShape table is authored in.
struct CellGeometry {
    int faceCount = 0;
    float minX = 1e9f, maxX = -1e9f;
    float minY = 1e9f, maxY = -1e9f;
    float minZ = 1e9f, maxZ = -1e9f;
    float maxLum = 0.0f;
    // How orange or how blue the cell reads, as (red - blue) over the whole run for a single vertex -
    // a ratio, not a difference, because every face carries its own shading and ambient occlusion on
    // top of the light and a bare channel difference would rank the darkest vertex of a dark room
    // above the brightest face of a sunlit one. Per-vertex too: a face lit by the torch and a face
    // lit by the sky alone share a cell, and two maxima taken from different vertices would report a
    // tint no vertex ever had.
    float maxHue = -1e9f, minHue = 1e9f;

    int quads() const { return faceCount; }
    bool empty() const { return faceCount == 0; }
};

inline bool uvInTile(const VoxelVertex& v, const VoxelTileRect& r) {
    // Half a texel of slack either way: voxelTileRect insets the rect by half a texel to stop
    // bleeding, and the quad corners are authored against that same inset. v0 is the tile's TOP
    // edge, which is the *smaller* coordinate - the atlas is sampled with v = 0 at its first row.
    static const float EPS = 1e-3f;
    return v.uv[0] >= r.u0 - EPS && v.uv[0] <= r.u1 + EPS &&
           v.uv[1] >= r.v0 - EPS && v.uv[1] <= r.v1 + EPS;
}

// The vertices of one cell that belong to `def`. Whole quads only: the buffer is written four
// vertices at a time by MeshAccum::quad(), so a group of four is exactly one face and a face that
// straddles the plane between two cells belongs to neither of them alone. Counting vertices instead
// would credit the four corners a slab shares with the shelf above it to both scans.
// Position alone is not enough either: the floor under a slab puts its top face on exactly the plane
// the slab's bottom edge sits on. The tile a vertex samples is what separates them - every block in
// the table has its own slot, and the atlas is the one thing a voxel mesh carries for free.
CellGeometry scanMeshCell(MeshComponent& mc, int lx, int ly, int lz, const BlockDef& def) {
    CellGeometry g;
    const unsigned char* base = mc.buffer.getData();
    const unsigned int stride = mc.buffer.getStride();
    const unsigned int count = mc.buffer.getCount();
    if (!base || stride < VOXEL_VERTEX_STRIDE || count < 4) return g;

    VoxelTileRect rects[FACE_COUNT];
    for (int f = 0; f < FACE_COUNT; f++) rects[f] = quantum::voxelTileRect(def.tiles[f]);

    const unsigned int faces = count / 4;
    for (unsigned int q = 0; q < faces; q++) {
        VoxelVertex v[4];
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        bool mine = true;
        for (int i = 0; i < 4; i++) {
            std::memcpy(&v[i], base + (size_t)(q * 4 + i) * stride, sizeof(VoxelVertex));
            const float r[3] = { v[i].p[0] - (float)lx, v[i].p[1] - (float)ly,
                                 v[i].p[2] - (float)lz };
            for (int a = 0; a < 3; a++) {
                if (r[a] < lo[a]) lo[a] = r[a];
                if (r[a] > hi[a]) hi[a] = r[a];
            }
            bool inTile = false;
            for (int f = 0; f < FACE_COUNT && !inTile; f++) inTile = uvInTile(v[i], rects[f]);
            if (!inTile) mine = false;
        }
        // A face whose four corners are all on one plane inside the cell still counts: that is what
        // the top of a shelf is.
        if (!mine) continue;
        for (int a = 0; a < 3; a++)
            if (lo[a] < -0.001f || hi[a] > 1.001f) { mine = false; break; }
        if (!mine) continue;

        g.faceCount++;
        if (lo[0] < g.minX) g.minX = lo[0];   if (hi[0] > g.maxX) g.maxX = hi[0];
        if (lo[1] < g.minY) g.minY = lo[1];   if (hi[1] > g.maxY) g.maxY = hi[1];
        if (lo[2] < g.minZ) g.minZ = lo[2];   if (hi[2] > g.maxZ) g.maxZ = hi[2];
        for (int i = 0; i < 4; i++) {
            const float lum = std::max(v[i].c[0], std::max(v[i].c[1], v[i].c[2]));
            if (lum > g.maxLum) g.maxLum = lum;
            // A vertex dark enough to be an unlit corner carries no readable colour, and dividing its
            // channels would turn rounding into a hue.
            const float sum = v[i].c[0] + v[i].c[1] + v[i].c[2];
            if (sum > 0.05f) {
                const float hue = (v[i].c[0] - v[i].c[2]) / sum;
                if (hue > g.maxHue) g.maxHue = hue;
                if (hue < g.minHue) g.minHue = hue;
            }
        }
    }
    return g;
}

inline float span(const CellGeometry& g) {
    return g.empty() ? 0.0f : (g.maxX - g.minX) + (g.maxY - g.minY) + (g.maxZ - g.minZ);
}

}  // namespace

// Every shape claim this file makes, measured on the vertices that reached the mesh buffer rather
// than on the loop that wrote them: a slab is a half-height shelf, an upper slab hangs from the top
// of its cell, a cactus is inset 2/16 on every side, a fence is a 4/16 post, a torch is crossed
// quads - and none of them arrives black, which is how a lit-but-unreadable world would present.
// Builds the shapes in a carved arena in the live focus column, meshes it, and puts every voxel back.
void probeVoxelMesh(VoxelWorld& world, Probe& probe) {
    ProbeModule mod(probe, "shape");
    Scene* scene = world.scene();
    Chunk* chunk = world.chunk(world.focusChunkX(), world.focusChunkZ());
    if (!scene || !chunk || !chunk->generated) {
        probe.skip("the focus column has not streamed");
        return;
    }

    VoxelPatch patch(world);
    int ax = 0, ay = 0, az = 0;
    if (!carveArena(world, patch, ax, ay, az, 7, 3, 4)) {
        probe.skip("the column under the focus chunk has no room to build in");
        return;
    }

    patch.set(ax + 1, ay, az + 1, makeState(BLOCK_SLAB_STONE, 0));
    patch.set(ax + 2, ay, az + 1, BLOCK_TORCH);
    patch.set(ax + 3, ay, az + 1, BLOCK_CACTUS);
    patch.set(ax + 5, ay, az + 1, BLOCK_FENCE);
    patch.set(ax + 1, ay + 1, az + 1, makeState(BLOCK_SLAB_STONE, state::SLAB_TOP_BIT));

    lightChunk(world, *chunk);
    buildChunkMesh(world, *chunk);
    chunk->dirtyMesh = false;

    // The opaque mesh has to be found again after every remesh: a remesh drops the entities and
    // makes new ones, so a component pointer taken from the previous pass is freed memory.
    auto opaqueMesh = [&]() -> MeshComponent* {
        ChunkRender& r = world.render(chunk->cx, chunk->cz);
        if (r.opaqueEntity < 0 || !scene->isEntityCreated((Entity)r.opaqueEntity)) return nullptr;
        return &scene->getComponent<MeshComponent>((Entity)r.opaqueEntity);
    };
    MeshComponent* mc = opaqueMesh();
    if (!mc) {
        probe.skip("the remesh produced no opaque entity to read back");
        patch.restore();
        return;
    }

    const int lx = (ax + 1) & 15, lz = (az + 1) & 15;
    const CellGeometry lower = scanMeshCell(*mc, lx, ay, lz, block(BLOCK_SLAB_STONE));
    const CellGeometry upper = scanMeshCell(*mc, lx, ay + 1, lz, block(BLOCK_SLAB_STONE));
    const CellGeometry cactus = scanMeshCell(*mc, (ax + 3) & 15, ay, lz, block(BLOCK_CACTUS));
    const CellGeometry fence = scanMeshCell(*mc, (ax + 5) & 15, ay, lz, block(BLOCK_FENCE));
    const CellGeometry torch = scanMeshCell(*mc, (ax + 2) & 15, ay, lz, block(BLOCK_TORCH));

    // A shelf on the floor of its cell: four walls, the top, and no bottom - the floor it stands on
    // is opaque, so the face between them is culled by the same rule that hides a buried block.
    probe.check("a stone slab reaches the mesh as a half-height shelf",
                lower.quads() == 5 && std::fabs(lower.maxY - 0.5f) < 0.01f &&
                std::fabs(lower.minY) < 0.01f && std::fabs(lower.minX) < 0.01f &&
                std::fabs(lower.maxX - 1.0f) < 0.01f,
                "%d quads, y %.3f..%.3f, x %.3f..%.3f, z %.3f..%.3f", lower.quads(),
                (double)lower.minY, (double)lower.maxY, (double)lower.minX, (double)lower.maxX,
                (double)lower.minZ, (double)lower.maxZ);
    // The state bit exists, is saved and is sent on the wire, so a mesh that ignores it draws the
    // ceiling slab as a floor slab: the picture and the collision box then disagree by half a block.
    probe.check("an upper stone slab hangs from the top of its cell",
                upper.quads() >= 4 && std::fabs(upper.minY - 0.5f) < 0.01f &&
                std::fabs(upper.maxY - 1.0f) < 0.01f,
                "%d quads, y %.3f..%.3f (expected 0.500..1.000)", upper.quads(),
                (double)upper.minY, (double)upper.maxY);
    probe.check("a cactus reaches the mesh inset 2/16 on every side",
                cactus.quads() == 5 && std::fabs(cactus.minX - 0.125f) < 0.01f &&
                std::fabs(cactus.maxX - 0.875f) < 0.01f && std::fabs(cactus.maxY - 1.0f) < 0.01f,
                "%d quads, x %.3f..%.3f, y %.3f..%.3f", cactus.quads(),
                (double)cactus.minX, (double)cactus.maxX, (double)cactus.minY, (double)cactus.maxY);
    probe.check("a fence reaches the mesh as a 4/16 post",
                fence.quads() == 5 && std::fabs(fence.minX - 0.375f) < 0.01f &&
                std::fabs(fence.maxX - 0.625f) < 0.01f,
                "%d quads, x %.3f..%.3f, z %.3f..%.3f", fence.quads(),
                (double)fence.minX, (double)fence.maxX, (double)fence.minZ, (double)fence.maxZ);
    probe.check("a torch reaches the mesh as crossed quads",
                torch.quads() == 4 && std::fabs(torch.maxY - 1.0f) < 0.01f,
                "%d quads, y %.3f..%.3f, span %.3f", torch.quads(),
                (double)torch.minY, (double)torch.maxY, (double)span(torch));

    // Black is not invisible, but it is just as unreadable, and it is what a light channel that
    // never got solved looks like: the mesher multiplies the vertex colour by the baked light, so an
    // unsolved column paints the shape in zero.
    probe.check("no shape arrives at the mesh black",
                lower.maxLum > 0.0f && cactus.maxLum > 0.0f && fence.maxLum > 0.0f &&
                torch.maxLum > 0.0f,
                "slab %.3f, cactus %.3f, fence %.3f, torch %.3f",
                (double)lower.maxLum, (double)cactus.maxLum, (double)fence.maxLum,
                (double)torch.maxLum);

    // The two light channels are two different colours, and that difference is the tell a torch is
    // doing something the sun does not. Check the arithmetic where the tint lives, then the buffer
    // the GPU was handed: a mesher that went back to one grey ramp would pass the first and fail the
    // second. The dusk pass is what makes the measurement mean anything - at full noon the sky
    // channel saturates red and blue alike and a torch is indistinguishable from the sun.
    float dayCol[3], emitterCol[3], nightCol[3];
    lightColour(LIGHT_MAX, 0, 1.0f, dayCol);
    lightColour(0, LIGHT_MAX, 1.0f, emitterCol);
    lightColour(LIGHT_MAX, 0, 0.0f, nightCol);
    probe.check("sunlight is white, torchlight is orange and a moonless sky is blue",
                std::fabs(dayCol[0] - dayCol[2]) < 0.02f &&
                emitterCol[0] > emitterCol[2] + 0.2f &&
                nightCol[2] > nightCol[0] + 0.02f,
                "day %.3f/%.3f, torch %.3f/%.3f, night %.3f/%.3f (red/blue)",
                (double)dayCol[0], (double)dayCol[2], (double)emitterCol[0], (double)emitterCol[2],
                (double)nightCol[0], (double)nightCol[2]);

    const float prevDaylight = meshDaylight();
    setMeshDaylight(0.3f);
    buildChunkMesh(world, *chunk);
    chunk->dirtyMesh = false;
    MeshComponent* dusk = opaqueMesh();
    const CellGeometry besideTorch = dusk
        ? scanMeshCell(*dusk, (ax + 3) & 15, ay, lz, block(BLOCK_CACTUS)) : CellGeometry();

    // The same block at the same hour with the emitter taken out. Anything the sun lights on its own
    // has to read cool; only the torch turns it warm, so the sign of the difference between the two
    // measurements is the claim, not its size.
    patch.set(ax + 2, ay, az + 1, BLOCK_AIR);
    lightChunk(world, *chunk);
    buildChunkMesh(world, *chunk);
    chunk->dirtyMesh = false;
    dusk = opaqueMesh();
    const CellGeometry sunOnly = dusk
        ? scanMeshCell(*dusk, (ax + 3) & 15, ay, lz, block(BLOCK_CACTUS)) : CellGeometry();

    probe.check("the mesh carries a warm torch channel and a cool sky channel",
                !besideTorch.empty() && !sunOnly.empty() &&
                besideTorch.maxHue > 0.05f && sunOnly.maxHue < 0.0f,
                "warmest vertex (red-blue)/rgb: %.3f with the torch, %.3f without",
                (double)besideTorch.maxHue, (double)sunOnly.maxHue);

    setMeshDaylight(prevDaylight);
    patch.restore();
    lightChunk(world, *chunk);
    buildChunkMesh(world, *chunk);
    chunk->dirtyMesh = false;
}

}  // namespace voxel
