// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "VoxelModel.h"
#include "VoxelProbe.h"

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
#include <string>

namespace voxel {

using namespace quantum;

namespace {

const float kTwoPi = 6.28318530718f;
const float kDegToRad = 0.01745329252f;
const float kRadToDeg = 57.2957795131f;

}  // namespace

// ------------------------------------------------------------------ the rig's proportions and map

// Vanilla proportions, in units of a 64x64 skin: a 0.5-wide body 0.75 tall, limbs 0.25 x 0.75, a
// 0.5 cube for a head. jointY is the height above the feet the part hangs from - the body's own
// centre, the head's neck, a limb's shoulder or hip - so the sole is at 0 and the crown at
// VOXEL_MODEL_UNIT_HEIGHT. mapU/mapV is the top-left texel of the part's area; those six areas are
// the ones a skin somebody already owns puts its pixels in, which is the whole point of this table.
static const VoxelModelInfo g_modelParts[VOXEL_PART_COUNT] = {
    { 0.5f,  0.50f, 0.5f,  1.500f,  0,  0 },   // head
    { 0.5f,  0.75f, 0.25f, 1.125f, 16, 16 },   // body
    { 0.25f, 0.75f, 0.25f, 1.500f, 40, 16 },   // right arm, from the shoulder
    { 0.25f, 0.75f, 0.25f, 1.500f, 32, 48 },   // left arm
    { 0.25f, 0.75f, 0.25f, 0.750f,  0, 16 },   // right leg, from the hip
    { 0.25f, 0.75f, 0.25f, 0.750f, 16, 48 },   // left leg
};

static const char* const g_modelPartNames[VOXEL_PART_COUNT] = {
    "head", "body", "arm_right", "arm_left", "leg_right", "leg_left",
};

// The side a limb hangs on, in units, positive toward the model's own right - which is +X, because
// the face the skin map calls "right" is the +X one. Two legs fit inside a 0.5-wide body, so they
// sit at +-0.125; two arms hang outside it, so they sit at +-0.375.
static const float g_modelSideX[VOXEL_PART_COUNT] = {
    0.0f, 0.0f, 0.375f, -0.375f, 0.125f, -0.125f,
};

const VoxelModelInfo& voxelModelInfo(int part) {
    const int i = (part >= 0 && part < VOXEL_PART_COUNT) ? part : VOXEL_PART_BODY;
    return g_modelParts[i];
}

const char* voxelModelPartName(int part) {
    if (part < 0 || part >= VOXEL_PART_COUNT) return "unknown";
    return g_modelPartNames[part];
}

// How a box's six faces are packed at (mapU, mapV): the top and bottom faces lying on their sides in
// a row of depth d, then right/front/left/back in a strip of height h. This is the rule that puts a
// head's face at (8,8), its crown at (8,0) and its back at (24,8) in a real skin.
SkinRect voxelModelFaceRect(int part, int face) {
    const VoxelModelInfo& g = voxelModelInfo(part);
    const float w = g.w * 16.0f, h = g.h * 16.0f, d = g.d * 16.0f;
    const float u = (float)g.mapU, v = (float)g.mapV;

    float x0 = u, y0 = v, x1 = u + d, y1 = v + d;
    switch (face) {
        case VOXEL_FACE_TOP:    x0 = u + d;         y0 = v;       x1 = x0 + w; y1 = y0 + d; break;
        case VOXEL_FACE_BOTTOM: x0 = u + d + w;     y0 = v;       x1 = x0 + w; y1 = y0 + d; break;
        case VOXEL_FACE_RIGHT:  x0 = u;             y0 = v + d;   x1 = x0 + d; y1 = y0 + h; break;
        case VOXEL_FACE_FRONT:  x0 = u + d;         y0 = v + d;   x1 = x0 + w; y1 = y0 + h; break;
        case VOXEL_FACE_LEFT:   x0 = u + d + w;     y0 = v + d;   x1 = x0 + d; y1 = y0 + h; break;
        case VOXEL_FACE_BACK:   x0 = u + d + w + d; y0 = v + d;   x1 = x0 + w; y1 = y0 + h; break;
        default: break;
    }
    SkinRect r;
    r.u0 = x0 / (float)VOXEL_SKIN_PX;
    r.v0 = y0 / (float)VOXEL_SKIN_PX;
    r.u1 = x1 / (float)VOXEL_SKIN_PX;
    r.v1 = y1 / (float)VOXEL_SKIN_PX;
    return r;
}

// The size one face's map area has to be, in texels, or the skin is stretched and the body reads as
// wax: the two flat faces are w x d, the two narrow side faces d x h, front and back w x h.
static void voxelModelFaceTexels(int face, float w, float h, float d, float& outW, float& outH) {
    switch (face) {
        case VOXEL_FACE_TOP:
        case VOXEL_FACE_BOTTOM: outW = w; outH = d; break;
        case VOXEL_FACE_RIGHT:
        case VOXEL_FACE_LEFT:   outW = d; outH = h; break;
        default:                outW = w; outH = h; break;
    }
}

// --------------------------------------------------------------------------------- the animation

float voxelModelSwing(float speed) {
    if (speed < 0.0f) speed = 0.0f;
    // Nine degrees of arm per block per second: a wandering critter beats its arms a little, a
    // walking player swings, a sprinting one is near a right angle, and the cap is what keeps an
    // arm from swinging through the chest at a speed no mode reaches.
    const float deg = speed * 9.0f;
    return deg < 55.0f ? deg : 55.0f;
}

float voxelModelWalkPhase(float phase, float distanceMoved, float stride) {
    if (stride <= 0.0f) return phase;
    float p = std::fmod(phase + distanceMoved * (kTwoPi / stride), kTwoPi);
    if (p < 0.0f) p += kTwoPi;
    return p;
}

float voxelModelYawDeg(float heading) {
    // A heading is the direction the sim moves along, (cos, sin) over (x, z); a yaw is a rotation
    // about +Y - by the right-hand rule, which takes the engine's quaternion's own +Z to
    // (sin yaw, cos yaw) - applied to a model whose face is its local +Z. Turning the first into
    // the second is 90 degrees minus the heading, and the probe measures it against the engine.
    return 90.0f - heading * kRadToDeg;
}

VoxelModelPartPose voxelModelPartPose(int part, const VoxelModelPose& pose) {
    const VoxelModelInfo& g = voxelModelInfo(part);
    const float yawRad = pose.yaw * kDegToRad;
    const float cy = std::cos(yawRad), sy = std::sin(yawRad);
    const float s = pose.scale;
    const float swing = pose.swing;

    // The right arm and the left leg lead: the contralateral pair is what reads as a walk rather
    // than a shuffle. A positive pitch tilts a limb toward -Z (the engine rotates +Z down toward
    // -Y about +X), so leading means the negative of the sine.
    float pitchDeg = 0.0f;
    if (part == VOXEL_PART_ARM_RIGHT || part == VOXEL_PART_LEG_LEFT)
        pitchDeg = -swing * std::sin(pose.walkPhase);
    else if (part == VOXEL_PART_ARM_LEFT || part == VOXEL_PART_LEG_RIGHT)
        pitchDeg = swing * std::sin(pose.walkPhase);
    if (part == VOXEL_PART_HEAD) pitchDeg = pose.headPitch;

    VoxelModelPartPose out;
    out.yawDeg = pose.yaw;
    out.rollDeg = 0.0f;
    out.pitchDeg = pitchDeg;
    if (part == VOXEL_PART_HEAD) out.yawDeg = pose.yaw + pose.headYaw;

    // The entity sits at the joint, not at the box's middle: the geometry is built with the joint at
    // its own origin, so the rotation the transform carries is what puts the swinging box where the
    // limb points. A position that also held the box's offset would swing the shoulder along with
    // the hand, which is how a walking model ends up sliding its torso up and down.
    const float sideX = g_modelSideX[(part >= 0 && part < VOXEL_PART_COUNT) ? part : VOXEL_PART_BODY];
    out.px = pose.x + sideX * cy * s;
    out.py = pose.y + g.jointY * s;
    out.pz = pose.z - sideX * sy * s;
    return out;
}

void voxelModelFront(int part, const VoxelModelPose& pose, float out[3]) {
    const VoxelModelPartPose pp = voxelModelPartPose(part, pose);
    Quaternion q;
    q.fromEulerAngles(pp.pitchDeg, pp.yawDeg, pp.rollDeg, RotationOrder::YXZ);
    const Vector3 f = q * Vector3(0.0f, 0.0f, 1.0f);
    out[0] = f.x; out[1] = f.y; out[2] = f.z;
}

// -------------------------------------------------------------------------------------- geometry

// Local space, joint at the origin. A face's corners are walked (-1,-1) (1,-1) (1,1) (-1,1) over
// (t1,t2) with cross(t1,t2) == n, so the triangles are counter-clockwise seen from outside: the
// engine culls BACK, and a face wound the other way is a hole in the model.
struct ModelFaceSpec {
    int n[3];
    int t1[3];
    int t2[3];
    int order;          // the skin map's face index
    float shade;
};

static const ModelFaceSpec g_modelFaces[VOXEL_MODEL_FACE_COUNT] = {
    { { 0, 1, 0 },  { 1, 0, 0 },  { 0, 0, -1 }, VOXEL_FACE_TOP, 1.00f },
    { { 0, -1, 0 }, { 1, 0, 0 },  { 0, 0, 1 },  VOXEL_FACE_BOTTOM, 0.52f },
    { { 1, 0, 0 },  { 0, 0, -1 }, { 0, 1, 0 },  VOXEL_FACE_RIGHT, 0.80f },
    { { 0, 0, 1 },  { 1, 0, 0 },  { 0, 1, 0 },  VOXEL_FACE_FRONT, 0.68f },
    { { -1, 0, 0 }, { 0, 0, 1 },  { 0, 1, 0 },  VOXEL_FACE_LEFT, 0.80f },
    { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 },  VOXEL_FACE_BACK, 0.74f },
};

static const float g_modelCorner[4][2] = {
    { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f },
};

// The half-extent along whichever world axis a unit-axis vector points at. The face specs spell
// their axes out, so the box is sized by lookup rather than by a case per face.
static float modelAxisExtent(const int axis[3], const float half[3]) {
    return std::fabs((float)axis[0]) * half[0] +
           std::fabs((float)axis[1]) * half[1] +
           std::fabs((float)axis[2]) * half[2];
}

// Where a part's box sits relative to its own joint: a limb hangs below it, the head sits above,
// the torso is centred. The rig's bounds and its vertices have to agree, so both ask for this.
static void modelPartCentre(int part, const VoxelModelInfo& g, float out[3]) {
    out[0] = 0.0f; out[2] = 0.0f;
    if (part == VOXEL_PART_HEAD) out[1] = g.h * 0.5f;
    else if (part != VOXEL_PART_BODY) out[1] = -g.h * 0.5f;
    else out[1] = 0.0f;
}

void buildVoxelModelPartGeometry(int part, std::vector<ModelVertex>& verts, std::vector<uint32_t>& idx) {
    const VoxelModelInfo& g = voxelModelInfo(part);
    float centre[3];
    modelPartCentre(part, g, centre);
    const float half[3] = { g.w * 0.5f, g.h * 0.5f, g.d * 0.5f };

    for (int f = 0; f < VOXEL_MODEL_FACE_COUNT; f++) {
        const ModelFaceSpec& fs = g_modelFaces[f];
        const SkinRect uv = voxelModelFaceRect(part, fs.order);
        const float en = modelAxisExtent(fs.n, half);
        const float e1 = modelAxisExtent(fs.t1, half);
        const float e2 = modelAxisExtent(fs.t2, half);
        const uint32_t base = (uint32_t)verts.size();
        for (int i = 0; i < 4; i++) {
            const float s1 = g_modelCorner[i][0], s2 = g_modelCorner[i][1];
            ModelVertex v;
            for (int k = 0; k < 3; k++)
                v.p[k] = centre[k] + (float)fs.n[k] * en +
                         (float)fs.t1[k] * s1 * e1 + (float)fs.t2[k] * s2 * e2;
            v.n[0] = (float)fs.n[0]; v.n[1] = (float)fs.n[1]; v.n[2] = (float)fs.n[2];
            // s2 = -1 is the foot of the face and v0 is the map area's top row, so up on the box is
            // the small v: a skin drawn with its head's eyes at the top of its square lands right.
            const float u = (s1 + 1.0f) * 0.5f;
            const float vv = (s2 + 1.0f) * 0.5f;
            v.uv[0] = uv.u0 + (uv.u1 - uv.u0) * u;
            v.uv[1] = uv.v1 + (uv.v0 - uv.v1) * vv;
            const float shade = fs.shade;
            v.c[0] = shade; v.c[1] = shade; v.c[2] = shade; v.c[3] = 1.0f;
            verts.push_back(v);
        }
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    }
}

void setupVoxelModelMaterial(MeshComponent& mesh, const char* skinPath, float r, float g, float b) {
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
    m.name = "voxel_model";
    m.baseColorFactor = Vector4(r, g, b, 1.0f);
    m.metallicFactor = 0.0f;
    m.roughnessFactor = 0.9f;
    m.alphaMode = MaterialAlphaMode::ALPHA_OPAQUE;
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.submeshes[0].faceCulling = true;
    mesh.submeshes[0].textureRect = Rect(0.0, 0.0, 1.0, 1.0);

    // A skin is loaded by path, never handed over as in-memory pixels: TextureData copies shallow and
    // its destructor frees with stbi_image_free, so a Material carrying its own buffer would be
    // double-freed once the ECS or a pool copied it.
    const bool hasSkin = skinPath != nullptr && skinPath[0] != '\0';
    if (hasSkin) {
        m.baseColorTexture = Texture(skinPath);
        // Nearest, like the block atlas: a skin's 8x8 face is the look, and filtering it to mush at
        // distance is how a player stops resembling the picture somebody drew.
        m.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
        m.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
        m.baseColorTexture.setWrapU(TextureWrap::CLAMP_TO_EDGE);
        m.baseColorTexture.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    }
    mesh.submeshes[0].needUpdateTexture = hasSkin;
}

// ------------------------------------------------------------------------------------------ rigs

void createVoxelModelRig(Scene* scene, VoxelModelRig& rig, int slot, const char* skinPath,
                        float r, float g, float b) {
    if (!scene) return;
    destroyVoxelModelRig(scene, rig);

    char nameBuf[64];
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        std::vector<ModelVertex> verts;
        std::vector<uint32_t> idx;
        verts.reserve(24);
        idx.reserve(36);
        buildVoxelModelPartGeometry(p, verts, idx);
        if (verts.empty()) continue;

        const VoxelModelInfo& info = voxelModelInfo(p);
        float centre[3];
        modelPartCentre(p, info, centre);
        const AABB bounds(Vector3(centre[0] - info.w * 0.5f, centre[1] - info.h * 0.5f, centre[2] - info.d * 0.5f),
                          Vector3(centre[0] + info.w * 0.5f, centre[1] + info.h * 0.5f, centre[2] + info.d * 0.5f));

        Entity e = scene->createEntity();
        std::snprintf(nameBuf, sizeof(nameBuf), "VoxelModel_%d_%s", slot, voxelModelPartName(p));
        scene->setEntityName(e, std::string(nameBuf));

        MeshComponent mc;
        setupVoxelModelMaterial(mc, skinPath, r, g, b);

        mc.buffer.clearAll();
        // InterleavedBuffer declares its own addAttribute overloads, which hide the typed ones on
        // Buffer, so the explicit data type + byte offset has to go through a Buffer& view.
        Buffer& geom = mc.buffer;
        geom.addAttribute(AttributeType::POSITION, AttributeDataType::FLOAT, 3, offsetof(ModelVertex, p));
        geom.addAttribute(AttributeType::NORMAL, AttributeDataType::FLOAT, 3, offsetof(ModelVertex, n));
        geom.addAttribute(AttributeType::TEXCOORD1, AttributeDataType::FLOAT, 2, offsetof(ModelVertex, uv));
        geom.addAttribute(AttributeType::COLOR, AttributeDataType::FLOAT, 4, offsetof(ModelVertex, c));
        mc.buffer.setStride((unsigned int)sizeof(ModelVertex));
        mc.buffer.setUsage(BufferUsage::IMMUTABLE);

        mc.indices.clearAll();
        mc.indices.addAttribute(AttributeType::INDEX, AttributeDataType::UNSIGNED_INT, 1, 0);
        mc.indices.setStride((unsigned int)sizeof(uint32_t));
        mc.indices.setUsage(BufferUsage::IMMUTABLE);

        mc.buffer.importData(verts.data(), verts.size() * sizeof(ModelVertex));
        mc.buffer.setCount((unsigned int)verts.size());
        mc.indices.importData(idx.data(), idx.size() * sizeof(uint32_t));
        mc.indices.setCount((unsigned int)idx.size());

        mc.vertexCount = (unsigned int)verts.size();
        mc.aabb = bounds;
        mc.verticesAABB = bounds;
        mc.needUpdateAABB = false;
        mc.needReload = true;      // buffers changed size: rebuild the pipeline bindings
        mc.loaded = false;
        scene->addComponent<MeshComponent>(e, std::move(mc));

        Transform t;
        t.position = Vector3(0.0f, -32.0f, 0.0f);
        t.rotation = Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
        t.scale = Vector3(1.0f, 1.0f, 1.0f);
        t.visible = false;
        scene->addComponent<Transform>(e, t);

        rig.parts[p] = (int64_t)e;
    }
    rig.built = true;
}

void poseVoxelModelRig(Scene* scene, const VoxelModelRig& rig, const VoxelModelPose& pose) {
    if (!scene) return;
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        const Entity e = (Entity)rig.parts[p];
        if (e == NULL_ENTITY || !scene->isEntityCreated(e)) continue;
        Transform* t = scene->findComponent<Transform>(e);
        if (!t) continue;
        const VoxelModelPartPose pp = voxelModelPartPose(p, pose);
        t->position = Vector3(pp.px, pp.py, pp.pz);
        Quaternion q;
        q.fromEulerAngles(pp.pitchDeg, pp.yawDeg, pp.rollDeg, RotationOrder::YXZ);
        t->rotation = q;
        const float s = pose.scale > 0.0f ? pose.scale : 1.0f;
        t->scale = Vector3(s, s, s);
        t->visible = pose.visible;
        t->needUpdate = true;
    }
}

void destroyVoxelModelRig(Scene* scene, VoxelModelRig& rig) {
    if (scene) {
        for (int p = 0; p < VOXEL_PART_COUNT; p++) {
            const Entity e = (Entity)rig.parts[p];
            if (e != NULL_ENTITY && scene->isEntityCreated(e)) scene->destroyEntity(e);
            rig.parts[p] = 0;
        }
    }
    rig.built = false;
}

// __________________________________________________________________________________ behaviour probe

namespace {

// A local point of a part under its pose, in world space: the probe measures the box's corners
// through the engine's own quaternion, so a claim about where a hand lands is a claim about what
// the renderer will do with the transform, not about this file's arithmetic.
Vector3 modelLocalPoint(int part, const VoxelModelPose& pose, float lx, float ly, float lz) {
    const VoxelModelPartPose pp = voxelModelPartPose(part, pose);
    Quaternion q;
    q.fromEulerAngles(pp.pitchDeg, pp.yawDeg, pp.rollDeg, RotationOrder::YXZ);
    const Vector3 r = q * Vector3(lx * pose.scale, ly * pose.scale, lz * pose.scale);
    return Vector3(pp.px + r.x, pp.py + r.y, pp.pz + r.z);
}

// How far above its joint a part reaches, which is what makes a joint position a height.
float modelTopAboveJoint(int part, const VoxelModelInfo& g) {
    if (part == VOXEL_PART_HEAD) return g.h;
    if (part == VOXEL_PART_BODY) return g.h * 0.5f;
    return 0.0f;
}

}  // namespace

void probeVoxelModel(Scene* scene, voxel::Probe& probe) {
    ProbeModule mod(probe, "model");

    // ---------------------------------------------------------------- the skin map and the parts
    bool inside = true, disjoint = true, sized = true;
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        const VoxelModelInfo& g = voxelModelInfo(p);
        const float wp = g.w * 16.0f, hp = g.h * 16.0f, dp = g.d * 16.0f;
        for (int f = 0; f < VOXEL_MODEL_FACE_COUNT; f++) {
            const SkinRect r = voxelModelFaceRect(p, f);
            if (!(r.u0 >= 0.0f && r.v0 >= 0.0f && r.u1 <= 1.0f && r.v1 <= 1.0f &&
                  r.u1 > r.u0 && r.v1 > r.v0)) inside = false;
            float wantW = 0.0f, wantH = 0.0f;
            voxelModelFaceTexels(f, wp, hp, dp, wantW, wantH);
            const float gotW = (r.u1 - r.u0) * VOXEL_SKIN_PX;
            const float gotH = (r.v1 - r.v0) * VOXEL_SKIN_PX;
            if (std::fabs(gotW - wantW) > 0.01f || std::fabs(gotH - wantH) > 0.01f) sized = false;
        }
        for (int q = p + 1; q < VOXEL_PART_COUNT; q++) {
            for (int f = 0; f < VOXEL_MODEL_FACE_COUNT; f++) {
                for (int g2 = 0; g2 < VOXEL_MODEL_FACE_COUNT; g2++) {
                    const SkinRect a = voxelModelFaceRect(p, f);
                    const SkinRect b = voxelModelFaceRect(q, g2);
                    if (a.u0 < b.u1 && b.u0 < a.u1 && a.v0 < b.v1 && b.v0 < a.v1) disjoint = false;
                }
            }
        }
    }
    probe.check("six parts, every face's map area inside the 64x64 skin and the size of its own face",
                inside && sized, "inside %d, sized %d", (int)inside, (int)sized);
    probe.check("no two parts share any skin texel, so one skin paints six bodies at once",
                disjoint, "overlap-free %d", (int)disjoint);

    const SkinRect headFront = voxelModelFaceRect(VOXEL_PART_HEAD, VOXEL_FACE_FRONT);
    const SkinRect headTop = voxelModelFaceRect(VOXEL_PART_HEAD, VOXEL_FACE_TOP);
    const SkinRect headBack = voxelModelFaceRect(VOXEL_PART_HEAD, VOXEL_FACE_BACK);
    const SkinRect bodyFront = voxelModelFaceRect(VOXEL_PART_BODY, VOXEL_FACE_FRONT);
    const SkinRect armFront = voxelModelFaceRect(VOXEL_PART_ARM_RIGHT, VOXEL_FACE_FRONT);
    probe.check("a head's face sits at the vanilla texels: (8,8) front, (8,0) crown, (24,8) back",
                std::fabs(headFront.u0 * 64.0f - 8.0f) < 0.01f &&
                std::fabs(headFront.v0 * 64.0f - 8.0f) < 0.01f &&
                std::fabs(headTop.u0 * 64.0f - 8.0f) < 0.01f &&
                std::fabs(headTop.v0 * 64.0f - 0.0f) < 0.01f &&
                std::fabs(headBack.u0 * 64.0f - 24.0f) < 0.01f,
                "front %.1f,%.1f crown %.1f,%.1f back %.1f,%.1f",
                headFront.u0 * 64.0f, headFront.v0 * 64.0f, headTop.u0 * 64.0f, headTop.v0 * 64.0f,
                headBack.u0 * 64.0f, headBack.v0 * 64.0f);
    probe.check("a torso's face is at (20,20) and a right arm's at (44,20), where a real skin draws them",
                std::fabs(bodyFront.u0 * 64.0f - 20.0f) < 0.01f &&
                std::fabs(bodyFront.v0 * 64.0f - 20.0f) < 0.01f &&
                std::fabs(armFront.u0 * 64.0f - 44.0f) < 0.01f &&
                std::fabs(armFront.v0 * 64.0f - 20.0f) < 0.01f,
                "torso %.1f,%.1f arm %.1f,%.1f",
                bodyFront.u0 * 64.0f, bodyFront.v0 * 64.0f, armFront.u0 * 64.0f, armFront.v0 * 64.0f);

    // ---------------------------------------------------------------------------------- the mesh
    int badWinding = 0, wrongCount = 0, badVerts = 0;
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        std::vector<ModelVertex> verts;
        std::vector<uint32_t> idx;
        buildVoxelModelPartGeometry(p, verts, idx);
        if (verts.size() != 24 || idx.size() != 36) { wrongCount++; continue; }
        for (size_t i = 0; i < idx.size(); i += 3) {
            const ModelVertex& a = verts[idx[i]];
            const ModelVertex& b = verts[idx[i + 1]];
            const ModelVertex& c = verts[idx[i + 2]];
            const float e1[3] = { b.p[0] - a.p[0], b.p[1] - a.p[1], b.p[2] - a.p[2] };
            const float e2[3] = { c.p[0] - a.p[0], c.p[1] - a.p[1], c.p[2] - a.p[2] };
            const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1],
                                 e1[2] * e2[0] - e1[0] * e2[2],
                                 e1[0] * e2[1] - e1[1] * e2[0] };
            const float dot = n[0] * a.n[0] + n[1] * a.n[1] + n[2] * a.n[2];
            if (dot <= 0.0f) badWinding++;
        }
        for (const ModelVertex& v : verts) {
            for (int k = 0; k < 3; k++) if (!std::isfinite(v.p[k])) badVerts++;
            if (!(v.uv[0] >= 0.0f && v.uv[0] <= 1.0f && v.uv[1] >= 0.0f && v.uv[1] <= 1.0f)) badVerts++;
        }
    }
    probe.check("every part is a closed box of six faces, wound outward, with finite verts inside the map",
                wrongCount == 0 && badWinding == 0 && badVerts == 0,
                "%d parts had the wrong count, %d triangles were wound in, %d verts were bad",
                wrongCount, badWinding, badVerts);

    // -------------------------------------------------------------------------------- the pose
    VoxelModelPose stand;
    stand.x = 100.0f; stand.y = 60.0f; stand.z = 200.0f;
    stand.scale = 1.0f;
    VoxelModelPose walk = stand;
    walk.swing = voxelModelSwing(1.9f);
    walk.walkPhase = voxelModelWalkPhase(0.0f, 0.4f, 1.6f);

    float crown = -1e9f, sole = 1e9f;
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        const VoxelModelInfo& g = voxelModelInfo(p);
        const VoxelModelPartPose pp = voxelModelPartPose(p, stand);
        const float top = pp.py + modelTopAboveJoint(p, g);
        const float bottom = (p == VOXEL_PART_HEAD || p == VOXEL_PART_BODY)
            ? pp.py - g.h * 0.5f : pp.py - g.h;
        if (top > crown) crown = top;
        if (bottom < sole) sole = bottom;
    }
    probe.check("a body stands on its feet: nothing dips below the pose and the crown is two units up",
                std::fabs(sole - stand.y) < 0.02f &&
                std::fabs(crown - (stand.y + VOXEL_MODEL_UNIT_HEIGHT)) < 0.02f,
                "sole %.3f against %.3f, crown %.3f against %.3f",
                (double)sole, (double)stand.y, (double)crown,
                (double)(stand.y + VOXEL_MODEL_UNIT_HEIGHT));

    VoxelModelPose player = stand;
    player.scale = VOXEL_MODEL_PLAYER_SCALE;
    float playerCrown = -1e9f;
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        const VoxelModelPartPose pp = voxelModelPartPose(p, player);
        const float top = pp.py + modelTopAboveJoint(p, voxelModelInfo(p)) * player.scale;
        if (top > playerCrown) playerCrown = top;
    }
    probe.check("a player-sized body is 1.8 blocks tall, so it fits the gap its collision box fits",
                std::fabs(playerCrown - (player.y + VOXEL_MODEL_PLAYER_HEIGHT)) < 0.02f,
                "the scaled crown is at %.3f, the hitbox tops out at %.3f",
                (double)playerCrown, (double)(player.y + VOXEL_MODEL_PLAYER_HEIGHT));

    // The one mistake a rig of boxes with no skeleton makes: a limb built centred on its joint is a
    // body in a T-pose forever, and no swing hides it. An arm at rest has to hang, so its far end
    // lands on the hip line a whole limb below the shoulder rather than level with the shoulder.
    {
        const VoxelModelInfo& arm = voxelModelInfo(VOXEL_PART_ARM_RIGHT);
        const VoxelModelInfo& leg = voxelModelInfo(VOXEL_PART_LEG_RIGHT);
        const VoxelModelPartPose rest = voxelModelPartPose(VOXEL_PART_ARM_RIGHT, stand);
        const float elbow = rest.py - arm.h;
        const float hip = stand.y + leg.jointY;
        probe.check("an arm at rest hangs the limb under its shoulder instead of standing out in a T",
                    std::fabs(rest.py - (stand.y + arm.jointY)) < 0.02f &&
                        std::fabs(elbow - hip) < 0.02f,
                    "shoulder %.3f (want %.3f), elbow %.3f, hip line %.3f, out to the side %.3f",
                    (double)rest.py, (double)(stand.y + arm.jointY), (double)elbow, (double)hip,
                    (double)(rest.px - stand.x));
    }

    VoxelModelPose closed = walk;
    closed.walkPhase = walk.walkPhase + kTwoPi;
    int drifted = 0;
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        const VoxelModelPartPose a = voxelModelPartPose(p, walk);
        const VoxelModelPartPose b = voxelModelPartPose(p, closed);
        if (std::fabs(a.px - b.px) > 1e-3f || std::fabs(a.py - b.py) > 1e-3f ||
            std::fabs(a.pz - b.pz) > 1e-3f || std::fabs(a.pitchDeg - b.pitchDeg) > 1e-3f) drifted++;
    }
    probe.check("the walk cycle closes: one full turn of the phase leaves the body where it was",
                drifted == 0, "%d parts had moved after 2pi", drifted);

    const VoxelModelPartPose armR = voxelModelPartPose(VOXEL_PART_ARM_RIGHT, walk);
    const VoxelModelPartPose armL = voxelModelPartPose(VOXEL_PART_ARM_LEFT, walk);
    const VoxelModelPartPose legR = voxelModelPartPose(VOXEL_PART_LEG_RIGHT, walk);
    const VoxelModelPartPose legL = voxelModelPartPose(VOXEL_PART_LEG_LEFT, walk);
    probe.check("opposite limbs swing in anti-phase and a right arm leads with the left leg",
                std::fabs(armR.pitchDeg + armL.pitchDeg) < 1e-3f &&
                std::fabs(legR.pitchDeg + legL.pitchDeg) < 1e-3f &&
                std::fabs(armR.pitchDeg - legL.pitchDeg) < 1e-3f &&
                std::fabs(armR.pitchDeg) > 1e-3f,
                "arms %.1f/%.1f, legs %.1f/%.1f degrees", armR.pitchDeg, armL.pitchDeg,
                legR.pitchDeg, legL.pitchDeg);

    // The pivot: a limb's own joint does not translate when the limb swings. This is the failure a
    // pose written as "joint plus box offset" produces - a torso that bobs and shoulders that slide.
    VoxelModelPose midStride = stand;
    midStride.swing = 45.0f;
    midStride.walkPhase = kTwoPi * 0.25f;
    int jointsMoved = 0, handsMoved = 0;
    for (int p = VOXEL_PART_ARM_RIGHT; p <= VOXEL_PART_LEG_LEFT; p++) {
        const VoxelModelInfo& g = voxelModelInfo(p);
        const VoxelModelPartPose a = voxelModelPartPose(p, stand);
        const VoxelModelPartPose b = voxelModelPartPose(p, midStride);
        if (std::fabs(a.px - b.px) > 1e-5f || std::fabs(a.py - b.py) > 1e-5f ||
            std::fabs(a.pz - b.pz) > 1e-5f) jointsMoved++;
        const Vector3 handA = modelLocalPoint(p, stand, 0.0f, -g.h, 0.0f);
        const Vector3 handB = modelLocalPoint(p, midStride, 0.0f, -g.h, 0.0f);
        const float d = std::fabs(handA.x - handB.x) + std::fabs(handA.y - handB.y) + std::fabs(handA.z - handB.z);
        if (d > 0.1f) handsMoved++;
    }
    probe.check("a limb turns about its joint: the shoulder stays put while the hand travels",
                jointsMoved == 0 && handsMoved == 4, "%d joints slid, %d hands moved", jointsMoved, handsMoved);

    VoxelModelPose restPhase = walk;
    restPhase.walkPhase = 0.0f;
    int swungAtZero = 0;
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        const VoxelModelPartPose pp = voxelModelPartPose(p, restPhase);
        if (p != VOXEL_PART_HEAD && p != VOXEL_PART_BODY && std::fabs(pp.pitchDeg) > 1e-6f) swungAtZero++;
    }
    VoxelModelPose rest = stand;
    rest.swing = 0.0f;
    int moved = 0;
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        const VoxelModelPartPose a = voxelModelPartPose(p, rest);
        const VoxelModelPartPose b = voxelModelPartPose(p, stand);
        if (std::fabs(a.px - b.px) > 1e-5f || std::fabs(a.py - b.py) > 1e-5f ||
            std::fabs(a.pz - b.pz) > 1e-5f) moved++;
    }
    probe.check("a body at rest stands still: no limb is swung at phase 0 and zero swing moves nothing",
                swungAtZero == 0 && moved == 0, "%d limbs swung at phase 0, %d parts moved", swungAtZero, moved);

    // Which way a swinging limb's tip goes. At a quarter cycle the right arm and the left leg have to
    // reach forward along the body's own +Z while the pair on the other side trails, because a model
    // whose limbs all swing the same way is a march, not a walk. Measured in world space, so a wrong
    // heading-to-yaw map would fail here as well as in the facing check below.
    float tipDeltaZ[VOXEL_PART_COUNT] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    for (int p = VOXEL_PART_ARM_RIGHT; p <= VOXEL_PART_LEG_LEFT; p++) {
        const float h = voxelModelInfo(p).h;
        const Vector3 a = modelLocalPoint(p, stand, 0.0f, -h, 0.0f);
        const Vector3 b = modelLocalPoint(p, midStride, 0.0f, -h, 0.0f);
        tipDeltaZ[p] = b.z - a.z;
    }
    probe.check("at a quarter cycle the right arm and left leg reach forward and the other pair trails",
                tipDeltaZ[VOXEL_PART_ARM_RIGHT] > 0.1f && tipDeltaZ[VOXEL_PART_LEG_LEFT] > 0.1f &&
                tipDeltaZ[VOXEL_PART_ARM_LEFT] < -0.1f && tipDeltaZ[VOXEL_PART_LEG_RIGHT] < -0.1f,
                "tips moved %+.2f / %+.2f / %+.2f / %+.2f along Z (armR, armL, legR, legL)",
                (double)tipDeltaZ[VOXEL_PART_ARM_RIGHT], (double)tipDeltaZ[VOXEL_PART_ARM_LEFT],
                (double)tipDeltaZ[VOXEL_PART_LEG_RIGHT], (double)tipDeltaZ[VOXEL_PART_LEG_LEFT]);

    const float s0 = voxelModelSwing(0.0f), s1 = voxelModelSwing(1.9f), s4 = voxelModelSwing(4.3f),
                s99 = voxelModelSwing(99.0f);
    probe.check("the swing grows with the speed and caps below a right angle",
                s0 == 0.0f && s1 > s0 && s4 > s1 && s99 <= 55.0f && s99 > 0.0f,
                "%.1f / %.1f / %.1f / %.1f degrees at 0, walk, run, and absurd", s0, s1, s4, s99);

    VoxelModelPose turned = stand;
    turned.yaw = 30.0f;
    turned.headYaw = 45.0f;
    const VoxelModelPartPose bodyTurned = voxelModelPartPose(VOXEL_PART_BODY, turned);
    const VoxelModelPartPose headTurned = voxelModelPartPose(VOXEL_PART_HEAD, turned);
    probe.check("a head turn is relative to the body: the torso keeps its yaw and the head adds its own",
                std::fabs(bodyTurned.yawDeg - 30.0f) < 1e-4f &&
                std::fabs(headTurned.yawDeg - 75.0f) < 1e-4f,
                "body %.1f, head %.1f degrees", bodyTurned.yawDeg, headTurned.yawDeg);

    // The heading a sim moves along and the yaw the renderer rotates by have to agree, or every mob
    // walks sideways. Measured against the engine's own quaternion, not this file's arithmetic.
    int facingHeadings = 0;
    for (int i = 0; i < 8; i++) {
        const float heading = (float)i * 0.785398163397f;
        VoxelModelPose q = stand;
        q.yaw = voxelModelYawDeg(heading);
        float front[3];
        voxelModelFront(VOXEL_PART_BODY, q, front);
        const float dot = front[0] * std::cos(heading) + front[2] * std::sin(heading);
        if (dot > 0.999f) facingHeadings++;
    }
    probe.check("a model faces the way its heading travels, measured with the engine's quaternion",
                facingHeadings == 8, "%d of 8 headings had the front face pointing along the walk",
                facingHeadings);

    // A head pitched by 25 degrees has to tilt by 25 degrees out of the horizontal and still look
    // along the body's heading; the sign of its Y is printed rather than asserted, because which
    // way "positive" tilts is the engine's convention to own and a flip here is a visible bug anyway.
    VoxelModelPose looking = stand;
    looking.yaw = voxelModelYawDeg(0.6f);
    looking.headPitch = 25.0f;
    float lookFront[3];
    voxelModelFront(VOXEL_PART_HEAD, looking, lookFront);
    const float horiz = std::sqrt(lookFront[0] * lookFront[0] + lookFront[2] * lookFront[2]);
    const float tilt = std::atan2(std::fabs(lookFront[1]), horiz) * kRadToDeg;
    const float lookDot = lookFront[0] * std::cos(0.6f) + lookFront[2] * std::sin(0.6f);
    probe.check("a pitched head tilts by exactly its pitch and keeps looking where the body points",
                std::fabs(tilt - 25.0f) < 0.5f && (horiz > 0.0f ? lookDot / horiz > 0.999f : false),
                "tilted %.2f degrees, front (%.3f, %.3f, %.3f)", tilt,
                lookFront[0], lookFront[1], lookFront[2]);

    // ------------------------------------------------------------------------------------- the rig
    if (!scene) {
        probe.skip("no scene, so there is nothing to build a rig in");
        return;
    }
    VoxelModelRig rig;
    createVoxelModelRig(scene, rig, 999, nullptr, 1.0f, 1.0f, 1.0f);
    int live = 0;
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        const Entity e = (Entity)rig.parts[p];
        if (e != NULL_ENTITY && scene->isEntityCreated(e)) live++;
    }
    VoxelModelPose shown = stand;
    shown.visible = true;
    shown.scale = VOXEL_MODEL_PLAYER_SCALE;
    poseVoxelModelRig(scene, rig, shown);
    int placed = 0, scaled = 0, meshed = 0;
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        const Entity e = (Entity)rig.parts[p];
        if (e == NULL_ENTITY || !scene->isEntityCreated(e)) continue;
        Transform* t = scene->findComponent<Transform>(e);
        MeshComponent* mc = scene->findComponent<MeshComponent>(e);
        if (!t || !mc) continue;
        if (mc->numSubmeshes == 1 && mc->indices.getCount() > 0 && mc->buffer.getStride() > 0) meshed++;
        if (!t->visible) continue;
        const VoxelModelPartPose want = voxelModelPartPose(p, shown);
        if (std::fabs(t->position.x - want.px) < 1e-3f && std::fabs(t->position.y - want.py) < 1e-3f &&
            std::fabs(t->position.z - want.pz) < 1e-3f) placed++;
        if (std::fabs(t->scale.x - shown.scale) < 1e-5f && std::fabs(t->scale.y - shown.scale) < 1e-5f)
            scaled++;
    }
    probe.check("a rig is one live, meshed entity per part and the pose lands each where the maths says",
                live == VOXEL_PART_COUNT && meshed == VOXEL_PART_COUNT &&
                placed == VOXEL_PART_COUNT && scaled == VOXEL_PART_COUNT,
                "%d/%d created, %d with geometry, %d drawn where the pose puts them, %d at its scale",
                live, VOXEL_PART_COUNT, meshed, placed, scaled);

    VoxelModelPose hidden = shown;
    hidden.visible = false;
    poseVoxelModelRig(scene, rig, hidden);
    int visible = 0;
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        const Entity e = (Entity)rig.parts[p];
        Transform* t = (e != NULL_ENTITY && scene->isEntityCreated(e)) ? scene->findComponent<Transform>(e) : nullptr;
        if (t && t->visible) visible++;
    }
    probe.check("a body nobody sees draws nothing: one flag hides all six parts",
                visible == 0, "%d parts still visible", visible);

    destroyVoxelModelRig(scene, rig);
    int left = 0;
    for (int p = 0; p < VOXEL_PART_COUNT; p++) {
        const Entity e = (Entity)rig.parts[p];
        if (e != NULL_ENTITY && scene->isEntityCreated(e)) left++;
    }
    probe.check("a torn-down rig leaves no entity behind, because the buffers are GPU allocations",
                left == 0 && rig.built == false, "%d entities still alive", left);
}

}  // namespace voxel
