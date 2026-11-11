// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <vector>

namespace quantum { class Scene; struct MeshComponent; }

namespace voxel {

// The player and mob model: a humanoid of six boxes whose UVs sit on the 64x64 skin layout, so a
// skin somebody already owns maps onto the body without being redrawn.
//
// The rig is deliberately not an entity hierarchy and not a skeleton. Each part is its own entity
// and this module writes that entity's world transform from the pose every frame: the mob pool
// already pays one transform write per mob, six is the same cost, and it does not assume the
// renderer composes parent transforms. Nothing here touches a vertex buffer after the rig is
// built, so the parts' buffers stay IMMUTABLE GPU allocations like everything else in this project.

enum VoxelModelPart {
    VOXEL_PART_HEAD = 0,
    VOXEL_PART_BODY,
    VOXEL_PART_ARM_RIGHT,
    VOXEL_PART_ARM_LEFT,
    VOXEL_PART_LEG_RIGHT,
    VOXEL_PART_LEG_LEFT,
    VOXEL_PART_COUNT
};

// The order a box's six faces are packed in the skin map: top, bottom, right, front, left, back.
// Not the world-axis order the chunk mesher uses: the map is laid out like this one.
enum VoxelModelFace {
    VOXEL_FACE_TOP = 0,
    VOXEL_FACE_BOTTOM,
    VOXEL_FACE_RIGHT,
    VOXEL_FACE_FRONT,
    VOXEL_FACE_LEFT,
    VOXEL_FACE_BACK,
    VOXEL_MODEL_FACE_COUNT
};

const int VOXEL_SKIN_PX = 64;         // one texel is 1/64 of the skin in U and in V

// The sole-to-crown height of the parts as drawn: a 64x64 skin is authored for a body that is
// exactly two units tall (legs 0.75 + torso 0.75 + head 0.5), so scale 1 means those texels land
// as drawn. A player here is 1.8 blocks, which is why the rig is posed at a scale rather than 1.
const float VOXEL_MODEL_UNIT_HEIGHT = 2.0f;
const float VOXEL_MODEL_PLAYER_SCALE = 1.8f / VOXEL_MODEL_UNIT_HEIGHT;
const float VOXEL_MODEL_PLAYER_HEIGHT = 1.8f;   // the collision box a body must fit under

struct SkinRect { float u0, v0, u1, v1; };   // normalized, v0 is the map's top row

// One part: its box in units, the height above the feet of the point it hangs from (the body's is
// its own centre), and the texel where its map area starts. Sizes and map areas are the vanilla
// ones, in pixels / 16.
struct VoxelModelInfo {
    float w, h, d;
    float jointY;
    int mapU, mapV;
};

const VoxelModelInfo& voxelModelInfo(int part);
const char* voxelModelPartName(int part);

// The map area of one face of one part, in normalized skin coordinates, derived from the box's
// size and the part's texel origin the way the vanilla map packs a box.
SkinRect voxelModelFaceRect(int part, int face);

// The pose of a whole body. Nothing in here is per-part on purpose: one pose in, six transforms
// out, so a mob, the local player and a remote player all animate from the same four numbers.
struct VoxelModelPose {
    float x = 0.0f, y = 0.0f, z = 0.0f;   // feet centre, in world blocks
    float yaw = 0.0f;                     // degrees, already in the engine's convention
    float headYaw = 0.0f;                 // degrees, relative to the body
    float headPitch = 0.0f;               // degrees, head only
    float walkPhase = 0.0f;               // radians
    float swing = 0.0f;                   // degrees, half-amplitude of the limb swing
    float scale = 1.0f;                   // voxelModelScaleForHeight() of the body's height
    bool visible = true;
};

// The rig scale that makes a body `height` blocks tall: the parts are drawn two units tall because
// that is what a skin is authored for, so a mob shorter than a player is not redrawn, just scaled.
inline float voxelModelScaleForHeight(float height) {
    return VOXEL_MODEL_UNIT_HEIGHT > 0.0f ? height / VOXEL_MODEL_UNIT_HEIGHT : 1.0f;
}

struct VoxelModelPartPose {
    float px, py, pz;
    float yawDeg, pitchDeg, rollDeg;
};

// Where one part sits and how it turns for a pose. Pure arithmetic, so a headless probe can
// measure the animation without a renderer and without a world.
VoxelModelPartPose voxelModelPartPose(int part, const VoxelModelPose& pose);

// The swing a body owes at `speed` blocks/s: nothing standing, a walk modest, a run near a right
// angle, and capped so an arm never swings through the torso.
float voxelModelSwing(float speed);

// Advance the cycle by the distance actually walked, so the feet and the limbs agree at any frame
// rate and a mob that was stopped mid-step does not slide.
float voxelModelWalkPhase(float phase, float distanceMoved, float stride);

// A heading on the XZ plane (radians, the direction the sim moves along) to the engine's yaw in
// degrees, so the part's front face looks the way the body travels.
float voxelModelYawDeg(float heading);

// A box's local +Z under the pose's rotation: the way the model is facing, in world space.
void voxelModelFront(int part, const VoxelModelPose& pose, float out[3]);

// One interleaved vertex, same layout as the chunk mesher's so both are the pipeline the
// renderer already binds for a textured, lit, shadow-casting mesh.
struct ModelVertex {
    float p[3];
    float n[3];
    float uv[2];
    float c[4];
};

// Geometry of one part in local space, with the joint at the origin: a limb's box hangs below it,
// the head's sits above, the body's is centred. Skin UVs included.
void buildVoxelModelPartGeometry(int part, std::vector<ModelVertex>& verts, std::vector<uint32_t>& idx);

void setupVoxelModelMaterial(quantum::MeshComponent& mesh, const char* skinPath, float r, float g, float b);

// A rig: one entity per part, 0 where there is none. Built once per body for the life of the
// world, posed every frame, and torn down when the pool rebinds.
struct VoxelModelRig {
    int64_t parts[VOXEL_PART_COUNT] = { 0, 0, 0, 0, 0, 0 };
    bool built = false;
};

void createVoxelModelRig(quantum::Scene* scene, VoxelModelRig& rig, int slot, const char* skinPath,
                         float r, float g, float b);
void poseVoxelModelRig(quantum::Scene* scene, const VoxelModelRig& rig, const VoxelModelPose& pose);
void destroyVoxelModelRig(quantum::Scene* scene, VoxelModelRig& rig);

struct Probe;

// The measured proof of this file: the six map areas are the vanilla ones and do not overlap, each
// face's UV lies inside the 64x64 grid and is the size of its own face, a box has all six faces,
// the walk cycle closes and opposite limbs are in anti-phase, a body at rest has nothing moving,
// the swing grows with speed and stops short of a right angle, a head turn is relative to the
// body, and a rig built in a scene gets one entity per part that the pose then moves.
void probeVoxelModel(quantum::Scene* scene, voxel::Probe& probe);

}  // namespace voxel
