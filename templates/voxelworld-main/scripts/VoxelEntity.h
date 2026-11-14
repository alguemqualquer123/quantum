// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace voxel {

class VoxelWorld;

// Voxel mobs: box-collided critters that wander, fall, swim and get culled. They share the
// player's collision code, and the voxel grid is the collider - there is no Body3D and no physics
// engine component anywhere here, because the block shapes already are one.

// Spawn up to `count` mobs on solid ground within `radius` blocks of (px,pz). Returns how many
// actually appeared (a column that has not streamed in yet has nowhere to stand, and the pool is
// capped), so the caller can retry next frame.
int spawnVoxelMobs(voxel::VoxelWorld& world, float px, float pz, int radius, int count);

// Walk, fall, swim, turn at cliffs, despawn beyond `despawnRadius` of (px,pz).
void updateVoxelMobs(voxel::VoxelWorld& world, float dt, float px, float pz, float despawnRadius);

int aliveMobCount(voxel::VoxelWorld& world);

// "Mobs: N (spawned M, culled K)" for the overlay.
std::string mobStats(voxel::VoxelWorld& world);

// Sand, gravel and mud fall when the block under them stops supporting them. Driven off the
// world's pending edits, so it costs nothing until a block actually goes away; call it every
// frame next to updateVoxelMobs.
void updateFallingBlocks(voxel::VoxelWorld& world, float dt);

struct Probe;

// The measured proof of what this file claims: a mob appears standing on something, walks with its
// entity drawn exactly where the sim put it, drops when its floor goes away, swims when water takes
// the floor's place, and is culled once the player leaves - and a gravity block falls one cell at a
// time, through water, onto the first solid below it, through the same loud write path a player edit
// uses. Runs in an arena carved in the focus column and puts the pool and every voxel back. Called by
// the host on VOXEL_PROBE.
void probeVoxelEntities(voxel::VoxelWorld& world, voxel::Probe& probe);

}  // namespace voxel
