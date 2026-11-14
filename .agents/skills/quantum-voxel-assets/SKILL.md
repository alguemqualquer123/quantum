---
name: quantum-voxel-assets
description: The local Mojang asset dump (templates/voxelworld-main/minecraft-assets-26.3) - its measured layout, the file names that are not obvious, how the voxel project stitches it into its own atlases at build time, and the licence boundary that keeps every byte of it out of git. Use when working with real Minecraft textures, sounds, recipes, loot tables, lang names or shaders as reference in the voxel project.
---

# Minecraft asset pack (26.3)

## Licence boundary - read this before touching a file

The pack is Mojang's copyrighted work, dropped in by the user at
`templates/voxelworld-main/minecraft-assets-26.3/`. Three rules follow from that and none is
negotiable:

1. **It is gitignored** (`templates/voxelworld-main/.gitignore`, `/minecraft-assets-26.3/`), and
   so is everything whose pixels come from it: `textures/voxel/atlas_minecraft.png|.txt`,
   `textures/gui/`, `textures/entity/`. A derived atlas is the same copyright, not a clean one.
2. **Never copy a pack file into the repository** - not into `assets/`, not into `engine/`, not
   into a commit. Read it from the pack path at build time or at runtime, and write the result to
   an ignored path.
3. **The project's own generated art stays.** `scripts/generate_voxel_atlas.py` paints every tile
   pixel by pixel from a seeded hash and is the licence-clean source of truth; the pack is an
   *optional input*. Any tile the pack does not have falls back to the painter, so a machine
   without the pack still builds and still runs.

The repo is MIT and the engine is distributed; the pack is not. If a task would put a pack byte
under version control, stop and say so.

## What is actually in there (measured, 28 111 files)

`version.json` says `id 26.3`, release `2026-09-15`; `game-version.json` says world version 5023,
protocol 777, resource pack format 97.1. Layout is the Mojang asset dump, not a zip:

| Path | Count | What it gives the voxel project |
|---|---|---|
| `assets/minecraft/textures/block/` | 1300 `.png`, 105 `.mcmeta` | every block face, `destroy_stage_0..9`, `water_still/flow`, `glass`, `ice` |
| `assets/minecraft/textures/item/` | 832 | non-block icons - food, tools, ingots, spawn eggs |
| `assets/minecraft/textures/entity/<mob>/` | 117 dirs (creeper, cow, chicken, pig, zombie, ...) | real mob skins |
| `assets/minecraft/textures/colormap/` | `grass.png`, `foliage.png`, `dry_foliage.png` | the biome tint the block textures are multiplied by |
| `assets/minecraft/textures/gui/sprites/` | 24 categories (`hud`, `container`, `widget`, `tooltip`, `pause_menu`, `recipe_book`, ...) | hearts, hunger, air, slots, buttons - the whole modern UI set |
| `assets/minecraft/textures/particle/` | 117 | rain, splash, dust |
| `assets/minecraft/lang/` | 144 locales | `en_us.json` = display name per block/item |
| `assets/minecraft/sounds/` + `sounds.json` | 5698 `.ogg` | `dig.stone`, `random.eat`, `mob.cow.ambient` as event → file |
| `assets/minecraft/shaders/core/` + `include/` + `post/` | 96 | `block.fsh`, `lightmap.fsh`, `clouds.fsh`, `oit_*.fsh` - **reference only** |
| `assets/minecraft/atlases/` | `blocks.json`, `items.json`, `gui.json`, ... | how Mojang stitches its own atlas - the recipe for the stitcher |
| `data/minecraft/recipe/` | 1764 | crafting recipes in the current JSON shape |
| `data/minecraft/loot_table/blocks/` | 1203 | authoritative "this block drops that item" |
| `data/minecraft/tags/block/` | 299 | sets (which blocks are ores, logs, mineable) |

Every directory carries a `_list.json` (`{"directories":[...],"files":[...]}`) - use it instead of
walking 28k files, a full recursive `find` on this tree takes minutes.

## Names that are not what you would guess

- Logs are split: `oak_log.png` is the **side**, `oak_log_top.png` the rings. The project's tiles
  are `log_oak_side` / `log_oak_top`, so the mapping is `<wood>_log` → side, `<wood>_log_top` → top.
- `grass_block_top.png` / `grass_block_side.png` (project: `grass_top` / `grass_side`), plus
  `grass_block_side_overlay.png` and `grass_block_snow.png` as separate layers.
- `short_grass.png` is the project's `tall_grass`; `poppy.png`, `dandelion.png` match.
- Leaves per tree: `oak_leaves.png`, `birch_leaves.png` - and they are **untinted grey**, the green
  comes from multiplying by `colormap/foliage.png`. Same for `grass_block_top.png`.
- Animated textures have a sibling `.mcmeta` with `{"animation": {"frametime": 2}}` (lowercase
  `frametime`, and `frames: [...]` when the order is not sequential). Water, lava, fire, portals,
  poppy and glass all animate.
- Cracks exist twice: `destroy_stage_N.png` and `destroy_oit_stage_N.png` (the order-independent
  transparency variant). Take the plain one.
- GUI sprites are many small PNGs, not one sheet - `gui/sprites/hud/heart/container.png`,
  `.../hud/heart/absorbing_full_blinking.png`, `.../hud/air.png`, `gui/sprites/container/slot.png`.
  Stitch them; do not try to load 400 files at runtime.
- **Player skins are in the pack**: `textures/entity/player/{slim,wide}/*.png`, nine official
  skins (steve, alex, ari, efe, kai, makena, noor, sunny, zuri) each 64x64 - exactly the layout
  `VoxelModel` already UVs against. `slim` vs `wide` is the arm width, so the model variant has to
  pick the folder. A user's own PNG still wins over all of them.
- **Most block textures are not coloured.** `block/grass_block_top.png` opens as mode `L` - it is a
  grayscale heightmap to be multiplied by `colormap/grass.png`, which is **256x256** indexed by
  (temperature, humidity), not a 1D ramp. Load it untinted and the whole world goes grey. Leaves
  and redstone follow the same rule (`colormap/foliage.png`).
- Animated textures are **vertical strips**, not sprite sheets: `block/water_still.png` is 16x512 =
  32 frames of 16x16, mode `P`. The `.mcmeta` `{"animation": {"frametime": 2}}` gives frames per
  tick, and `frames: [...]` reorders them when they are not sequential.
- `misc/shadow.png` is a 64x64 `LA` ellipse - the entity ground shadow, which is what a dropped
  item needs under it. `misc/vignette.png`, `misc/pumpkinblur.png`, `misc/nausea.png` and
  `misc/enchanted_glint_{item,armor}.png` are the other full-screen overlays.

## How the voxel project consumes it

The mesher reads one texture and one enum, so the whole integration is in the atlas name:

- `scripts/generate_voxel_atlas.py:34` `TILES` is the slot order; `:870` writes
  `textures/voxel/atlas.txt`, `:881` writes `scripts/VoxelTiles.h`. `CUTOUT` `:54`, `LIQUID` `:57`,
  `OVERLAY` `:60` decide how a tile is meshed. **This stays the shipped art** - the generated atlas
  is what a machine without the pack runs on.
- `scripts/build_minecraft_atlas.py` — **repo-level** `scripts/`, not the project's `scripts/`
  (task #202) — stitches the pack into `textures/voxel/atlas_minecraft.png` + `.txt` in
  **the same slot order**, append-only: a saved world stores slot numbers, not names
  (`VoxelTiles.h:13`, block enum `VoxelBlocks.h:63`). Four
  pack-specific facts the stitcher had to learn:
  - Grass/foliage tiles are **grayscale** and are multiplied by `colormap/grass.png` /
    `foliage.png`, which are **256×256 temperature×humidity**, not a palette. The temperate-plain
    coordinate is measured at `:126`; sampling the wrong corner tints the whole world.
  - `tintindex` in `models/block/*.json` says *which* faces get tinted - `grass_block` tints the top
    and the side overlay but **not** the plain side, which already ships coloured. Tint everything
    and the dirt band turns green.
  - Animated tiles are one vertical strip (`water.png` 16×256, `lava.png` 32×1024) with a `.mcmeta`
    `{"animation":{"frametime":N}}`; frame 0 is the top square (`load_frame0`, `:133`).
  - `destroy_stage_N.png` is white RGB with a **soft alpha**, while this project draws the break
    overlay as an alpha-tested cutout (`MASK`): a fade carried by alpha pops to solid black, so the
    stitcher thresholds it (`CRACK_CUTOFF` `:130`, printed at `:307`).
- Runtime picks the source in `voxelAtlasPath()` (`scripts/VoxelMesh.cpp:31-40`): the pack atlas if
  it exists, else the generated one. There is **no env override** - the two names
  `VOXEL_ATLAS_GENERATED` / `VOXEL_ATLAS_FROM_PACK` (`:28-29`) are C++ constants, not variables - and
  the choice is a function-local `static`, so it is resolved once per process: to see the other
  atlas, delete/rename the file and relaunch. Nothing in `VoxelMesh`/`VoxelChunk`/HUD changes,
  because every UV comes from the same `voxelTileRect()` (`VoxelMesh.cpp:163`, probe `:477`).
- **`FileData::getSystemPath()` is mandatory for a presence test.** The engine's VFS resolves the
  resource root; a bare relative `std::filesystem::exists("textures/voxel/atlas_minecraft.png")`
  tests the *process cwd*, which is not the project dir once the standalone launcher is involved -
  so the pack atlas is silently ignored and no error prints (`VoxelMesh.cpp:37`, `:409`, `:437`).
  Same for reading the `.txt` manifest.
- Textures are loaded by the engine's own loader (png via stb). There is no shader compilation at
  runtime, so `shaders/core/*.fsh` is a **specification to read**, never a file to include; porting
  one means forking the engine's GLSL and rebuilding the `.sdat` (`quantum-shaders`).

## What the pack does not give you

- No block models usable as geometry: `models/block/*.json` is Minecraft's own item-transform /
  element format, and the voxel mesher builds cubes from the block table. Only non-cube blocks
  (torch, fence, slab) would need a real port.
- No water *colour* map - `colormap/` has grass, foliage and dry_foliage only. Water tint is authored
  per biome in `data/minecraft/worldgen` (the `water_color` of a dimension), which is data, not a
  texture.
- No lightmap texture either: `find textures -iname "*lightmap*"` is empty. Minecraft computes the
  blocklight x skylight gradient at runtime, so `shaders/core/lightmap.fsh` is the formula to port,
  and the project's `VoxelLight` already holds the two channels per column.
- No shadow map: shading is face normal + that lightmap. Projected shadows stay the engine's job
  (task #208), with `misc/shadow.png` as the blob.
- No mob geometry: the pack gives skins only; the cube rig stays the project's (`VoxelModel`).

## Verifying a slice that used the pack

A capture is the only proof a texture landed: `VOXEL_SHOT=<path> VOXEL_FRAMES=200` (plus
`VOXEL_THIRD=1` to see the body). Keep the probe suite honest about it - a check that the pack is
absent must `Probe::skip`, not fail, because CI and every other machine have no pack. Never stage
`minecraft-assets-26.3/`, `world/`, or a stitched atlas: `git status` after staging must show no
pack path.
