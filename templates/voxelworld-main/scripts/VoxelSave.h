// (c) SR VINIX
// SPDX-License-Identifier: MIT
#pragma once
#include <string>
namespace voxel { class VoxelWorld; struct Probe; }
// Installs world.loadChunk / world.saveChunk and turns on disk caching under `dir`
// (one file per chunk column, plus a small index). `dir` is a project-relative or absolute path.
void installVoxelSave(voxel::VoxelWorld& world, const std::string& dir);
// Flush every dirty chunk now (called on quit). Safe to call twice.
void flushVoxelSave(voxel::VoxelWorld& world);
// "chunks on disk, bytes, pending writes" for the debug overlay.
std::string saveStats(const voxel::VoxelWorld& world);
// The measured proof of what this module claims: an edited column reaches the disk as a file, the
// file reads back byte-identical, and every way a file can be damaged (flipped payload byte,
// truncated tail, foreign format version, contents that disagree with the name) is refused instead
// of loaded, counted, and left for the generator to replace. Runs in a scratch directory under the
// OS temp path and puts the world's own save directory back. Called by the host on VOXEL_PROBE.
void probeVoxelSave(voxel::VoxelWorld& world, voxel::Probe& probe);
