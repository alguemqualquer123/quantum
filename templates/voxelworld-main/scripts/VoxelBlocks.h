// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include "VoxelConfig.h"

#include <cstdint>

namespace voxel {

// One entry per block type. Tiles are atlas slots from VoxelTiles.h; a face index follows the
// mesher's order: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z.
enum BlockFace : int { FACE_PX = 0, FACE_NX, FACE_PY, FACE_NY, FACE_PZ, FACE_NZ, FACE_COUNT };

enum BlockFlags : uint32_t {
    FLAG_SOLID = 1u << 0,     // has collision, the player cannot walk through it
    FLAG_OPAQUE = 1u << 1,    // culls the neighbour's face and stops light completely
    FLAG_CUTOUT = 1u << 2,    // alpha-tested tile, meshed as crossed quads
    FLAG_LIQUID = 1u << 3,    // flows, and the player swims in it
    FLAG_GAS = 1u << 4,       // no collision, no face of its own unless a neighbour needs one
    FLAG_TRANSLUCENT = 1u << 5, // sorted into the blended submesh, light passes with a cost
    FLAG_REPLACEABLE = 1u << 6, // generation and water overwrite it (plants, snow layers)
    FLAG_GRAVITY = 1u << 7,   // falls when nothing supports it (sand, gravel)
    FLAG_EMISSIVE = 1u << 8,  // lights its own neighbourhood
    FLAG_NO_MESH = 1u << 9,   // draws nothing at all (air)
};

struct BlockShape {
    // Collision and mesh bounds in 1/16ths of a block, from the origin corner. A slab is the lower
    // half, a fence post the middle column, a plant the crossed quads (handled by FLAG_CUTOUT).
    uint8_t x0, y0, z0, x1, y1, z1;
};

const int SHAPE_FULL = 0;

struct BlockDef {
    const char* name;
    uint16_t tiles[FACE_COUNT];
    uint32_t flags;
    uint8_t light;            // 0..15 emitted
    uint8_t sunFilter;        // light lost passing down through this block (0 opaque, 1 translucent)
    uint8_t hardness;         // break ticks with the right tool; 255 means unbreakable
    uint8_t tool;             // 0 none, 1 pickaxe, 2 axe, 3 shovel, 4 shears
    uint8_t minLevel;         // tool level needed to drop anything
    uint16_t drop;            // block type dropped, 0xffff = itself, 0 = nothing
    uint8_t shapeCount;
    BlockShape shapes[4];
    uint8_t r, g, b;          // base tint, multiplied into the vertex colour
};

// Registry: id -> def. Air is 0 and the table is fixed at compile time, so a saved world only
// needs the numeric ids and a client can render a block it has never seen described.
const BlockDef& block(uint16_t type);
uint32_t blockFlags(uint16_t type);
bool isOpaque(uint16_t type);
bool isSolid(uint16_t type);
bool isReplaceable(uint16_t type);

// The cube a block id actually fills, in units of its own cell and *with the state applied*. Both
// the mesher and the collider ask for it, because a face is only hidden when the neighbour's drawn
// volume really covers the shared plane: a cactus is 12/16 across and leaves a 2/16 gap on every
// side, so an inset block beside an opaque one must still paint the face over that gap, and an upper
// slab must occupy the top half rather than the bottom one. Blocks with no shape of their own (air,
// a liquid, a cross-quad plant) get the full cell, which is what the callers want: a liquid's height
// comes from its own level and a plant draws no cullable faces.
struct BlockBox {
    float x0, y0, z0;
    float x1, y1, z1;
};
BlockBox blockBox(uint16_t id);

// The block list in id order. Append only: ids are stored in saved chunks and sent on the wire.
enum BlockType : uint16_t {
    BLOCK_AIR = 0,
    BLOCK_STONE,
    BLOCK_GRASS,
    BLOCK_DIRT,
    BLOCK_COBBLESTONE,
    BLOCK_MOSSY_COBBLESTONE,
    BLOCK_BEDROCK,
    BLOCK_SAND,
    BLOCK_SANDSTONE,
    BLOCK_GRAVEL,
    BLOCK_CLAY,
    BLOCK_LOG_OAK,
    BLOCK_LOG_BIRCH,
    BLOCK_LOG_SPRUCE,
    BLOCK_LEAVES_OAK,
    BLOCK_LEAVES_BIRCH,
    BLOCK_PLANKS_OAK,
    BLOCK_PLANKS_BIRCH,
    BLOCK_PLANKS_SPRUCE,
    BLOCK_BRICKS,
    BLOCK_STONE_BRICKS,
    BLOCK_GLASS,
    BLOCK_WATER,
    BLOCK_ICE,
    BLOCK_PACKED_ICE,
    BLOCK_SNOW,
    BLOCK_COAL_ORE,
    BLOCK_IRON_ORE,
    BLOCK_GOLD_ORE,
    BLOCK_DIAMOND_ORE,
    BLOCK_REDSTONE_ORE,
    BLOCK_LAPIS_ORE,
    BLOCK_EMERALD_ORE,
    BLOCK_TNT,
    BLOCK_GLOWSTONE,
    BLOCK_OBSIDIAN,
    BLOCK_NETHERRACK,
    BLOCK_CRAFTING_TABLE,
    BLOCK_FURNACE,
    BLOCK_BOOKSHELF,
    BLOCK_WOOL_WHITE,
    BLOCK_WOOL_RED,
    BLOCK_WOOL_GREEN,
    BLOCK_WOOL_BLUE,
    BLOCK_WOOL_YELLOW,
    BLOCK_WOOL_BLACK,
    BLOCK_POPPY,
    BLOCK_DANDELION,
    BLOCK_TALL_GRASS,
    BLOCK_DEAD_BUSH,
    BLOCK_CACTUS,
    BLOCK_MELON,
    BLOCK_PUMPKIN,
    BLOCK_LAVA,
    BLOCK_MUD,
    BLOCK_SLAB_STONE,
    BLOCK_FENCE,
    BLOCK_TORCH,
    BLOCK_COUNT
};

// State meanings per block family. Only the blocks listed use them; the mesher and the water
// system read the state field with these helpers so a saved chunk stays interpretable.
namespace state {
    const uint16_t WATER_LEVEL_SHIFT = 0;   // 0..15, 15 = a full source block
    inline uint16_t waterLevel(uint16_t id) { return (uint16_t)(stateOf(id) & 15); }
    const uint16_t SLAB_TOP_BIT = 1;        // stone slab: lower half, or upper half
    const uint16_t LOG_AXIS_SHIFT = 2;      // 0 y, 1 x, 2 z (only affects which face is the ring)
    const uint16_t AGE_SHIFT = 4;           // plant growth 0..7
}

// shapes[] authored in 1/16ths, lifted to the ceiling when the id carries the upper-slab bit. A
// block that fills its cell has nothing to lift, so the test is the box's own height rather than a
// list of which block types are slabs. Lives here because state::SLAB_TOP_BIT is declared above it.
inline BlockBox shapeBox(const BlockShape& s, uint16_t id) {
    BlockBox b;
    b.x0 = (float)s.x0 / 16.0f; b.y0 = (float)s.y0 / 16.0f; b.z0 = (float)s.z0 / 16.0f;
    b.x1 = (float)s.x1 / 16.0f; b.y1 = (float)s.y1 / 16.0f; b.z1 = (float)s.z1 / 16.0f;
    if ((stateOf(id) & state::SLAB_TOP_BIT) != 0) {
        const float gap = 1.0f - (b.y1 - b.y0);
        if (gap > 0.0f) { b.y0 = gap; b.y1 = 1.0f; }
    }
    return b;
}

}  // namespace voxel
