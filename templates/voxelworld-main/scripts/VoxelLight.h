// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

namespace voxel { class VoxelWorld; struct Chunk; struct Probe; }

// Installs the lighting solver into world.relight.
void installVoxelLight(voxel::VoxelWorld& world);

// Solves one column (skylight columns + blocklight BFS), including the bleed into the
// neighbour columns that share a border. Safe to call on a chunk whose neighbours are missing.
void lightChunk(voxel::VoxelWorld& world, voxel::Chunk& chunk);

// 0..1 sunlight multiplier for a given time of day (0 = midnight, 0.25 = sunrise, 0.5 = noon).
// The mesher multiplies baked sky light by this, so night has to come for free.
float daylightFactor(float dayProgress);

namespace voxel {

// The measured proof of what the solver claims: a sealed rock room is dark while the air above it
// is not, an open shaft keeps daylight to its floor, glass dims it by exactly its filter and the
// price stacks, a torch costs one level per block in every direction, an opaque wall keeps its light
// on its own side, a second solve of the column writes nothing, and the sun curve bottoms out at
// midnight. Builds a room in the live focus column and puts every voxel back. Host calls it on
// VOXEL_PROBE.
void probeLight(voxel::VoxelWorld& world, voxel::Probe& probe);

}  // namespace voxel
