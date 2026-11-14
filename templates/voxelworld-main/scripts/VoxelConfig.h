// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <cstring>

// One place for the numbers every voxel module agrees on. A chunk column is 16x16 wide and
// WORLD_HEIGHT deep, and it is meshed as one entity: the mesher's index type, the streaming
// radius, the save format and the collision code all read these.
namespace voxel {

const int CHUNK_SX = 16;
const int CHUNK_SZ = 16;
const int WORLD_HEIGHT = 128;
const int CHUNK_VOLUME = CHUNK_SX * CHUNK_SZ * WORLD_HEIGHT;

// Sea level and the generation's vertical envelope. Terrain never exceeds WORLD_HEIGHT.
const int SEA_LEVEL = 42;

// Chunk ids are the packed column coordinate; y is not part of identity.
const int CHUNK_BITS = 20;                       // +-2^19 columns before the key wraps

// A block id is a 9-bit type plus a 7-bit state (water level, slab half, plant age, log axis).
const int BLOCK_TYPE_BITS = 9;
const int BLOCK_TYPE_MASK = (1 << BLOCK_TYPE_BITS) - 1;
const int BLOCK_STATE_MAX = (1 << (16 - BLOCK_TYPE_BITS)) - 1;

inline uint16_t makeState(uint16_t type, uint16_t state) {
    if (state > BLOCK_STATE_MAX) state = BLOCK_STATE_MAX;
    return (uint16_t)(type | (state << BLOCK_TYPE_BITS));
}
inline uint16_t stateType(uint16_t id) { return (uint16_t)(id & BLOCK_TYPE_MASK); }
inline uint16_t stateOf(uint16_t id) { return (uint16_t)(id >> BLOCK_TYPE_BITS); }

// Light is 0..15 in both channels, so two nibbles share one byte per voxel.
const int LIGHT_MAX = 15;
const int LIGHT_SUN_FALL = 1;    // vertical cost of daylight through a translucent block

// Per-frame work budgets. Generating a column and meshing it are the two costs a voxel world pays,
// and the frame rate is what suffers when a frame tries to catch up on all of them at once.
const double GEN_BUDGET_MS = 6.0;
const double MESH_BUDGET_MS = 7.0;
const int GEN_PER_FRAME = 2;
const int MESH_PER_FRAME = 2;

// How far blocks propagate work: a break at the edge of a chunk dirties the neighbour it exposed.
const int DIRTY_SPREAD = 1;

inline int64_t chunkKey(int cx, int cz) {
    return ((int64_t)(uint32_t)cx << 32) | (uint32_t)cz;
}
inline int chunkKeyX(int64_t key) { return (int32_t)(uint32_t)(key >> 32); }
inline int chunkKeyZ(int64_t key) { return (int32_t)(uint32_t)(key & 0xFFFFFFFFll); }

inline int voxelIndex(int x, int y, int z) {
    return (y * CHUNK_SZ + z) * CHUNK_SX + x;
}

// The three rule sets a voxel build runs under, named once so the controller, the vitals module and
// the overlay cannot disagree about what the current one is. The controller owns the answer (it is
// what decides collision, gravity and editing); the vitals module is *told*, because "creative does
// not decay or take damage" is a rule about the bars and a rule a caller can route around by simply
// not calling is not a rule.
enum VoxelGameMode {
    VOXEL_MODE_SURVIVAL = 0,
    VOXEL_MODE_CREATIVE,
    VOXEL_MODE_SPECTATOR,
    VOXEL_MODE_COUNT
};

// The name the log, the overlay and the probe print. Order must match the enum.
inline const char* voxelGameModeName(int mode) {
    switch (mode) {
        case VOXEL_MODE_CREATIVE:  return "creative";
        case VOXEL_MODE_SPECTATOR: return "spectator";
        case VOXEL_MODE_SURVIVAL:  return "survival";
        default:                   return "unknown";
    }
}

// Anything the player did not type is survival: a scene that carries a mode name this build does not
// know has to answer with the strictest ruleset, not with the loosest.
inline int voxelGameModeFromName(const char* name) {
    if (!name) return VOXEL_MODE_SURVIVAL;
    if (std::strcmp(name, "creative") == 0) return VOXEL_MODE_CREATIVE;
    if (std::strcmp(name, "spectator") == 0) return VOXEL_MODE_SPECTATOR;
    return VOXEL_MODE_SURVIVAL;
}

}  // namespace voxel
