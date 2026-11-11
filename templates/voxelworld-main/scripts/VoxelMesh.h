// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "VoxelWorld.h"

namespace quantum { struct MeshComponent; }

namespace voxel {

// The two atlases that can serve the block grid, project-relative. One texture for every block, so
// a chunk is one draw call.
//
// atlas.png is painted by scripts/generate_voxel_atlas.py and ships with the project.
// atlas_minecraft.png is stitched by scripts/build_minecraft_atlas.py out of the user's own Mojang
// dump and is never committed - a derivative of Mojang's pixels carries Mojang's copyright. Both are
// written in the slot order of VoxelTiles.h, so one voxelTileRect() indexes either and swapping the
// file cannot repaint a world whose chunks store slot numbers.
extern const char* const VOXEL_ATLAS_GENERATED;
extern const char* const VOXEL_ATLAS_FROM_PACK;

// The atlas to bind: the stitched one while it is on disk, the generated one otherwise. Resolved on
// the first call and cached - a run either has the pack installed or it does not, and this answers
// through the engine's own path resolver, so it asks about the same file the Texture will open.
const char* voxelAtlasPath();

// Fills (or creates) the two mesh entities of a chunk column: the alpha-tested opaque/cutout mesh
// and the blended liquid mesh. Runs on the main thread: it writes GPU-facing buffers and touches
// the ECS. The geometry itself is computed into local vectors first, so the culling/AO pass can be
// moved off-thread later without changing this signature.
// The render slot is taken from the world here rather than passed in: rebuilding drops the old
// entities, which erases the map node a caller-held ChunkRender& would point into.
void buildChunkMesh(VoxelWorld& world, Chunk& chunk);

// Applies the atlas material and the render flags a voxel mesh needs. Exposed for the preview
// brush and any other code that builds voxel geometry outside a chunk.
void setupVoxelMaterial(quantum::MeshComponent& mesh, bool liquid);

// Number of blocks that produced faces in the last build - cheap diagnostic for the headless probe.
uint32_t lastMeshedBlockCount();

// Quads emitted for a liquid since boot, and quads that came out with no area. A liquid has no
// collision shape, so its draw box is authored in the mesher; if that box is ever read back as zero
// the faces collapse onto a line, the mesh still uploads its vertices and the sea paints nothing.
// These two numbers are what separates that from "the water is out of view".
unsigned long long liquidQuadCount();
unsigned long long flatQuadCount();

// Reports which atlas the mesher resolved, that the file it chose is the file the engine will open,
// and that the two candidates agree on the slot layout. The only probe that needs no world: it
// measures files on disk and a material built off-thread-safe, so it runs even in a build with every
// optional module compiled out.
struct Probe;
void probeVoxelAtlas(Probe& probe);

// The measured proof of what the mesher claims about *shapes*: a stone slab reaches the GPU as a
// half-height shelf, a cactus as a box inset 2/16 on every side, a fence as a post, a torch as
// crossed quads, and none of them arrives black. Reads the vertices back out of the buffer the mesh
// entity was handed, because "the mesher ran" and "the renderer has something to draw" are two
// different claims and only the second one is what the player sees.
void probeVoxelMesh(VoxelWorld& world, Probe& probe);

// Sun strength baked into the next chunk meshes (0..1). Voxel light is vertex colour, so a day/night
// cycle cannot be a uniform: the game script sets this and asks for a re-bake when it moves enough,
// and the streaming budget turns the re-bake into a rolling update instead of one long frame.
void  setMeshDaylight(float factor);
float meshDaylight();

}  // namespace voxel
