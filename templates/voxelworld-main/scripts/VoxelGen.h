// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace voxel { class VoxelWorld; }

// Installs the terrain generator into world.generate. Also fills Chunk::biome and Chunk::height.
void installVoxelGen(voxel::VoxelWorld& world);

// Pure height query: the SAME number for the same (worldX, worldZ) no matter which chunk asks.
// The tree placer in a neighbouring column depends on this, so it must never read chunk data.
int  terrainHeight(uint64_t worldSeed, int worldX, int worldZ);   // 0..WORLD_HEIGHT-1, surface top block y
uint8_t biomeAt(uint64_t worldSeed, int worldX, int worldZ);      // returns a voxel::BiomeId

// The nearest coast over the same pure height field the generator uses: dry ground at the waterline
// with most of the ring around it under water. Nothing here reads a chunk, so the answer exists
// before the world has streamed a single column and cannot disagree with what gets generated.
// Scans Chebyshev rings around (startX, startZ) four blocks apart and stops at the first ring that
// holds a spot with at least five of its eight neighbours below SEA_LEVEL, so the result is the
// closest stretch of open water rather than the first puddle. outX/outZ is the column to stand on
// and towardX/towardZ is the average offset of the submerged neighbours - the direction to look.
// False when maxRadius finds nothing, which leaves the caller on the column it was given.
bool findShoreColumn(uint64_t worldSeed, int startX, int startZ, int maxRadius,
                     int& outX, int& outZ, float& towardX, float& towardZ);
