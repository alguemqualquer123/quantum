// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>

namespace voxel {

class VoxelWorld;

// Installs the liquid simulator into world.waterTick and turns on the per-tick scheduling.
void installVoxelWater(voxel::VoxelWorld& world);

// Pour `amount` source blocks of liquid at a world position (used by the player's bucket and by
// the headless probe). Returns how many blocks actually got a source.
int  placeLiquidSource(voxel::VoxelWorld& world, int wx, int wy, int wz, bool lava, int amount);

// Text line for the HUD/debug overlay: "Water: active cells, ticks/s, settled".
std::string waterStats(const voxel::VoxelWorld& world);

struct Probe;

// The measured proof of what the solver claims: a source feeds a stream that stops seven cells out,
// falls before it spreads, is stopped by stone, crawls in lava's case, turns to rock against the
// other liquid, and writes nothing once a pool has settled. Pours into the live world and puts
// every voxel back. Called by the host on VOXEL_PROBE.
void probeWater(voxel::VoxelWorld& world, voxel::Probe& probe);

}  // namespace voxel
