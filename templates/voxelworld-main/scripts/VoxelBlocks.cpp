// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "VoxelBlocks.h"
#include "VoxelTiles.h"

namespace voxel {

const BlockShape FULL_BLOCK = { 0, 0, 0, 16, 16, 16 };
const BlockShape LOWER_SLAB = { 0, 0, 0, 16, 8, 16 };
const BlockShape FENCE_POST = { 6, 0, 6, 10, 16, 10 };
const BlockShape SNOW_LAYER = { 0, 0, 0, 16, 4, 16 };

#define ALL6(t) { t, t, t, t, t, t }
#define TOP_BOTTOM(top, bottom, side) { side, side, top, bottom, side, side }

const int BUILD_SOLID = FLAG_SOLID | FLAG_OPAQUE;
const int BUILD_TRANSPARENT = FLAG_SOLID | FLAG_TRANSLUCENT;
const int BUILD_PLANT = FLAG_CUTOUT | FLAG_REPLACEABLE | FLAG_GAS;
const int BUILD_LIQUID = FLAG_LIQUID | FLAG_TRANSLUCENT | FLAG_REPLACEABLE;

// The table is in BlockType order and append-only: ids live in saved chunks and on the wire.
static const BlockDef g_blocks[BLOCK_COUNT] = {
    /* AIR            */ { "air",            ALL6(quantum::TILE_STONE),        FLAG_NO_MESH | FLAG_GAS | FLAG_REPLACEABLE, 0, 0, 0,  0, 0, 0,      0, { },                        255, 255, 255 },
    /* STONE          */ { "stone",          ALL6(quantum::TILE_STONE),        BUILD_SOLID,                                 0, 0, 15, 1, 0, 0xffff, 1, { FULL_BLOCK },             136, 136, 136 },
    /* GRASS          */ { "grass",          TOP_BOTTOM(quantum::TILE_GRASS_TOP, quantum::TILE_DIRT, quantum::TILE_GRASS_SIDE), BUILD_SOLID,    0, 0, 3,  0, 0, BLOCK_DIRT,  1, { FULL_BLOCK },             255, 255, 255 },
    /* DIRT           */ { "dirt",           ALL6(quantum::TILE_DIRT),         BUILD_SOLID,                                 0, 0, 3,  0, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* COBBLESTONE    */ { "cobblestone",    ALL6(quantum::TILE_COBBLESTONE),  BUILD_SOLID,                                 0, 0, 15, 1, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* MOSSY_COBBLE   */ { "mossy_cobblestone", ALL6(quantum::TILE_MOSSY_COBBLESTONE), BUILD_SOLID,                         0, 0, 15, 1, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* BEDROCK        */ { "bedrock",        ALL6(quantum::TILE_BEDROCK),      BUILD_SOLID,                                 0, 0, 255, 0, 0, 0,     1, { FULL_BLOCK },             255, 255, 255 },
    /* SAND           */ { "sand",           ALL6(quantum::TILE_SAND),         BUILD_SOLID | FLAG_GRAVITY,                  0, 0, 3,  0, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* SANDSTONE      */ { "sandstone",      TOP_BOTTOM(quantum::TILE_SANDSTONE_TOP, quantum::TILE_SANDSTONE_TOP, quantum::TILE_SANDSTONE_SIDE), BUILD_SOLID, 0, 0, 12, 1, 0, 0xffff, 1, { FULL_BLOCK }, 255, 255, 255 },
    /* GRAVEL         */ { "gravel",         ALL6(quantum::TILE_GRAVEL),       BUILD_SOLID | FLAG_GRAVITY,                  0, 0, 3,  0, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* CLAY           */ { "clay",           ALL6(quantum::TILE_CLAY),         BUILD_SOLID,                                 0, 0, 3,  0, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* LOG_OAK        */ { "log_oak",        TOP_BOTTOM(quantum::TILE_LOG_OAK_TOP, quantum::TILE_LOG_OAK_TOP, quantum::TILE_LOG_OAK_SIDE), BUILD_SOLID, 0, 0, 6, 2, 0, 0xffff, 1, { FULL_BLOCK }, 255, 255, 255 },
    /* LOG_BIRCH      */ { "log_birch",      TOP_BOTTOM(quantum::TILE_LOG_BIRCH_TOP, quantum::TILE_LOG_BIRCH_TOP, quantum::TILE_LOG_BIRCH_SIDE), BUILD_SOLID, 0, 0, 6, 2, 0, 0xffff, 1, { FULL_BLOCK }, 255, 255, 255 },
    /* LOG_SPRUCE     */ { "log_spruce",     TOP_BOTTOM(quantum::TILE_LOG_SPRUCE_TOP, quantum::TILE_LOG_SPRUCE_TOP, quantum::TILE_LOG_SPRUCE_SIDE), BUILD_SOLID, 0, 0, 6, 2, 0, 0xffff, 1, { FULL_BLOCK }, 255, 255, 255 },
    /* LEAVES_OAK     */ { "leaves_oak",     ALL6(quantum::TILE_LEAVES_OAK),   FLAG_SOLID | FLAG_CUTOUT | FLAG_TRANSLUCENT, 0, 2, 2,  4, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* LEAVES_BIRCH   */ { "leaves_birch",   ALL6(quantum::TILE_LEAVES_BIRCH), FLAG_SOLID | FLAG_CUTOUT | FLAG_TRANSLUCENT, 0, 2, 2,  4, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* PLANKS_OAK     */ { "planks_oak",     ALL6(quantum::TILE_PLANKS_OAK),   BUILD_SOLID,                                 0, 0, 8,  2, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* PLANKS_BIRCH   */ { "planks_birch",   ALL6(quantum::TILE_PLANKS_BIRCH), BUILD_SOLID,                                 0, 0, 8,  2, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* PLANKS_SPRUCE  */ { "planks_spruce",  ALL6(quantum::TILE_PLANKS_SPRUCE),BUILD_SOLID,                                 0, 0, 8,  2, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* BRICKS         */ { "bricks",         ALL6(quantum::TILE_BRICK),        BUILD_SOLID,                                 0, 0, 12, 1, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* STONE_BRICKS   */ { "stone_bricks",   ALL6(quantum::TILE_STONE_BRICKS), BUILD_SOLID,                                 0, 0, 12, 1, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* GLASS          */ { "glass",          ALL6(quantum::TILE_GLASS),        BUILD_TRANSPARENT,                           0, 0, 2,  0, 0, 0,      1, { FULL_BLOCK },             255, 255, 255 },
    /* WATER          */ { "water",          ALL6(quantum::TILE_WATER),        BUILD_LIQUID,                                0, 3, 0,  0, 0, 0,      0, { },                        232, 244, 255 },
    /* ICE            */ { "ice",            ALL6(quantum::TILE_ICE),          BUILD_TRANSPARENT,                           0, 1, 8,  1, 0, 0,      1, { FULL_BLOCK },             240, 250, 255 },
    /* PACKED_ICE     */ { "packed_ice",     ALL6(quantum::TILE_PACKED_ICE),   BUILD_SOLID,                                 0, 0, 8,  1, 0, 0xffff, 1, { FULL_BLOCK },             236, 248, 255 },
    /* SNOW           */ { "snow",           ALL6(quantum::TILE_SNOW),         BUILD_SOLID,                                 0, 0, 2,  3, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* COAL_ORE       */ { "coal_ore",       ALL6(quantum::TILE_COAL_ORE),     BUILD_SOLID,                                 0, 0, 15, 1, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* IRON_ORE       */ { "iron_ore",       ALL6(quantum::TILE_IRON_ORE),     BUILD_SOLID,                                 0, 0, 15, 1, 1, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* GOLD_ORE       */ { "gold_ore",       ALL6(quantum::TILE_GOLD_ORE),     BUILD_SOLID,                                 0, 0, 15, 1, 2, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* DIAMOND_ORE    */ { "diamond_ore",    ALL6(quantum::TILE_DIAMOND_ORE),  BUILD_SOLID,                                 0, 0, 15, 1, 2, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* REDSTONE_ORE   */ { "redstone_ore",   ALL6(quantum::TILE_REDSTONE_ORE), BUILD_SOLID | FLAG_EMISSIVE,                 4, 0, 15, 1, 2, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* LAPIS_ORE      */ { "lapis_ore",      ALL6(quantum::TILE_LAPIS_ORE),    BUILD_SOLID,                                 0, 0, 15, 1, 1, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* EMERALD_ORE    */ { "emerald_ore",    ALL6(quantum::TILE_EMERALD_ORE),  BUILD_SOLID,                                 0, 0, 15, 1, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* TNT            */ { "tnt",            TOP_BOTTOM(quantum::TILE_TNT_TOP, quantum::TILE_TNT_TOP, quantum::TILE_TNT_SIDE), BUILD_SOLID,    0, 0, 4,  0, 0, 0xffff, 1, { FULL_BLOCK }, 255, 255, 255 },
    /* GLOWSTONE      */ { "glowstone",      ALL6(quantum::TILE_GLOWSTONE),    BUILD_SOLID | FLAG_EMISSIVE,                 15, 0, 4, 0, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* OBSIDIAN       */ { "obsidian",       ALL6(quantum::TILE_OBSIDIAN),     BUILD_SOLID,                                 0, 0, 50, 1, 3, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* NETHERRACK     */ { "netherrack",     ALL6(quantum::TILE_NETHERRACK),   BUILD_SOLID,                                 0, 0, 4,  1, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* CRAFTING_TABLE */ { "crafting_table", TOP_BOTTOM(quantum::TILE_CRAFTING_TABLE_TOP, quantum::TILE_PLANKS_OAK, quantum::TILE_CRAFTING_TABLE_SIDE), BUILD_SOLID, 0, 0, 6, 2, 0, 0xffff, 1, { FULL_BLOCK }, 255, 255, 255 },
    /* FURNACE        */ { "furnace",        { quantum::TILE_FURNACE_SIDE, quantum::TILE_FURNACE_SIDE, quantum::TILE_FURNACE_TOP, quantum::TILE_FURNACE_TOP, quantum::TILE_FURNACE_FRONT, quantum::TILE_FURNACE_SIDE }, BUILD_SOLID, 0, 0, 12, 1, 0, 0xffff, 1, { FULL_BLOCK }, 255, 255, 255 },
    /* BOOKSHELF      */ { "bookshelf",      TOP_BOTTOM(quantum::TILE_PLANKS_OAK, quantum::TILE_PLANKS_OAK, quantum::TILE_BOOKSHELF), BUILD_SOLID, 0, 0, 6, 2, 0, 0xffff, 1, { FULL_BLOCK }, 255, 255, 255 },
    /* WOOL_WHITE     */ { "wool_white",     ALL6(quantum::TILE_WOOL_WHITE),   BUILD_SOLID,                                 0, 0, 4,  0, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* WOOL_RED       */ { "wool_red",       ALL6(quantum::TILE_WOOL_RED),     BUILD_SOLID,                                 0, 0, 4,  0, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* WOOL_GREEN     */ { "wool_green",     ALL6(quantum::TILE_WOOL_GREEN),   BUILD_SOLID,                                 0, 0, 4,  0, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* WOOL_BLUE      */ { "wool_blue",      ALL6(quantum::TILE_WOOL_BLUE),    BUILD_SOLID,                                 0, 0, 4,  0, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* WOOL_YELLOW    */ { "wool_yellow",    ALL6(quantum::TILE_WOOL_YELLOW),  BUILD_SOLID,                                 0, 0, 4,  0, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* WOOL_BLACK     */ { "wool_black",     ALL6(quantum::TILE_WOOL_BLACK),   BUILD_SOLID,                                 0, 0, 4,  0, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* POPPY          */ { "poppy",          ALL6(quantum::TILE_FLOWER_POPPY), BUILD_PLANT,                                 0, 0, 0,  0, 0, 0xffff, 0, { },                        255, 255, 255 },
    /* DANDELION      */ { "dandelion",      ALL6(quantum::TILE_FLOWER_DANDELION), BUILD_PLANT,                             0, 0, 0,  0, 0, 0xffff, 0, { },                        255, 255, 255 },
    /* TALL_GRASS     */ { "tall_grass",     ALL6(quantum::TILE_TALL_GRASS),   BUILD_PLANT,                                 0, 0, 0,  0, 0, 0,      0, { },                        255, 255, 255 },
    /* DEAD_BUSH      */ { "dead_bush",      ALL6(quantum::TILE_DEAD_BUSH),    BUILD_PLANT,                                 0, 0, 0,  0, 0, 0,      0, { },                        255, 255, 255 },
    /* CACTUS         */ { "cactus",         TOP_BOTTOM(quantum::TILE_CACTUS_TOP, quantum::TILE_CACTUS_TOP, quantum::TILE_CACTUS_SIDE), BUILD_SOLID, 0, 0, 4, 0, 0, 0xffff, 1, { { 2, 0, 2, 14, 16, 14 } }, 255, 255, 255 },
    /* MELON          */ { "melon",          TOP_BOTTOM(quantum::TILE_MELON_TOP, quantum::TILE_MELON_TOP, quantum::TILE_MELON_SIDE), BUILD_SOLID, 0, 0, 4, 0, 0, 0xffff, 1, { FULL_BLOCK }, 255, 255, 255 },
    /* PUMPKIN        */ { "pumpkin",        TOP_BOTTOM(quantum::TILE_PUMPKIN_TOP, quantum::TILE_PUMPKIN_TOP, quantum::TILE_PUMPKIN_SIDE), BUILD_SOLID, 0, 0, 4, 0, 0, 0xffff, 1, { FULL_BLOCK }, 255, 255, 255 },
    /* LAVA           */ { "lava",           ALL6(quantum::TILE_LAVA),         BUILD_LIQUID | FLAG_EMISSIVE,                15, 0, 0, 0, 0, 0,     0, { },                         255, 255, 255 },
    /* MUD            */ { "mud",            ALL6(quantum::TILE_MUD),          BUILD_SOLID | FLAG_GRAVITY,                  0, 0, 3,  0, 0, 0xffff, 1, { FULL_BLOCK },             255, 255, 255 },
    /* SLAB_STONE     */ { "stone_slab",     ALL6(quantum::TILE_SLAB_STONE),   BUILD_SOLID,                                 0, 0, 12, 1, 0, 0xffff, 1, { LOWER_SLAB },             255, 255, 255 },
    /* FENCE          */ { "fence",          ALL6(quantum::TILE_PLANKS_OAK),   FLAG_SOLID | FLAG_TRANSLUCENT,               0, 0, 8,  2, 0, 0xffff, 1, { FENCE_POST },             255, 255, 255 },
    /* TORCH          */ { "torch",          ALL6(quantum::TILE_TORCH),    FLAG_CUTOUT | FLAG_EMISSIVE | FLAG_TRANSLUCENT, 14, 0, 0, 0, 0, 0xffff, 0, { },                    255, 255, 255 },
};

const BlockDef& block(uint16_t type) {
    if (type >= BLOCK_COUNT) return g_blocks[BLOCK_STONE];
    return g_blocks[type];
}

uint32_t blockFlags(uint16_t type) { return block(type).flags; }
bool isOpaque(uint16_t type) { return (block(type).flags & FLAG_OPAQUE) != 0; }
bool isSolid(uint16_t type) { return (block(type).flags & FLAG_SOLID) != 0; }
bool isReplaceable(uint16_t type) { return type == BLOCK_AIR || (block(type).flags & FLAG_REPLACEABLE) != 0; }

BlockBox blockBox(uint16_t id) {
    const BlockDef& def = block(stateType(id));
    if (def.shapeCount == 0) { const BlockBox full = { 0, 0, 0, 1, 1, 1 }; return full; }
    return shapeBox(def.shapes[0], id);
}

}  // namespace voxel
