// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace voxel {

class VoxelWorld;
struct BlockDef;

// A world-space axis-aligned box in blocks (1 unit = 1 voxel). Entities and the player are boxes;
// the voxel grid is the only collider, so everything that moves resolves against these.
struct VoxelBox {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

const int VOXEL_HIT_GROUND = 1, VOXEL_HIT_CEIL = 2, VOXEL_HIT_WALL = 4, VOXEL_IN_LIQUID = 8;

// An entity may lift its feet this far to clear a step. One full block plus a hair: 0.6 (the
// classic value) would clip the top of a 1-block step and leave the entity wedged inside it.
const float VOXEL_STEP_UP = 1.002f;
// How far below the feet to look for something to stand on, for the ground flag and the sneak
// edge guard. Small on purpose: a ledge you would only fall 2cm off is not an edge worth stopping for.
const float VOXEL_GROUND_PROBE = 0.02f;

// Which block's collision shape would stop an entity standing at this voxel. Fills `out` with the
// union of the block's sub-boxes and returns false when the voxel cannot block anything (air,
// water, a flower). See solidBoxAt in the .cpp for why a slab is half a cube here.
bool solidBoxAt(voxel::VoxelWorld& world, int wx, int wy, int wz, VoxelBox& out);

// The collision sub-boxes of a block *type* placed at this voxel, in world units: the same
// BlockDef::shapes table the mesher draws from, so anything that asks "would this block fit here"
// answers with the geometry the player can actually see. Returns how many boxes were written.
int blockShapeBoxes(uint16_t id, int wx, int wy, int wz, VoxelBox out[4]);

// True when any part of `box` is inside a solid block's collision shape.
bool boxOverlapsSolid(voxel::VoxelWorld& world, const VoxelBox& box);

// True when something solid is within VOXEL_GROUND_PROBE below the box's feet.
bool boxSupported(voxel::VoxelWorld& world, const VoxelBox& box);

// Sweep-free, axis-separated move: slides along walls, steps up 1 block when the head fits,
// snaps to the ground when the box rests on a solid and is not rising (a rising body is left
// alone, or a climb slower than the ground probe is cancelled by it). `box` is updated in place
// and the return value is the VOXEL_HIT_* / VOXEL_IN_LIQUID bitmask above.
int moveBoxVoxel(voxel::VoxelWorld& world, VoxelBox& box, float dx, float dy, float dz);

struct VoxelHit {
    bool hit = false;
    int x = 0, y = 0, z = 0;
    int nx = 0, ny = 0, nz = 0;   // face entered: -1, 0 or 1 on exactly one axis
    float dist = 0.0f;            // world units along the ray
    uint16_t id = 0;              // the raw block id (type | state) that was hit
};

// Amanatides & Woo DDA through the grid, skipping non-targetable blocks (air, liquids unless
// `liquidsToo`, plants unless `plantsToo`). maxDist in blocks. `dir` need not be normalised:
// it is normalised here so `dist` always comes back in world units.
VoxelHit raycastVoxel(const voxel::VoxelWorld& world, float ox, float oy, float oz,
                      float dirX, float dirY, float dirZ, float maxDist,
                      bool liquidsToo = false, bool plantsToo = false);

// True when the voxel holds a block a ray can hit and a tool can mine.
bool targetableBlock(uint16_t id, bool liquidsToo, bool plantsToo);

struct Probe;

// The measured proof of the two claims this file makes: a body stops at the block's *shape* rather
// than its cube, and an exact sweep cannot be outrun by speed. Runs in a carved arena of the live
// world and puts every voxel back. Called by the host on VOXEL_PROBE.
void probeCollide(voxel::VoxelWorld& world, voxel::Probe& probe);

}  // namespace voxel
