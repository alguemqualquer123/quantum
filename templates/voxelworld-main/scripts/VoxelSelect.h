// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace quantum { class Scene; }

namespace voxel {

class VoxelWorld;
struct Probe;

// One edge of the outline, in block-local units: 0 is the voxel's own corner and 1 the far side of
// the cube, so the geometry depends only on the block *type* and the Transform moves it to the voxel
// the crosshair is on. That is what lets the overlay re-upload only when the type changes.
struct SelectSegment {
    float ax, ay, az, bx, by, bz;
};

// A cube is 12 edges, the crossed quads of a plant are 16, and the collision table caps at 4 sub-boxes.
const int SELECT_MAX_SEGMENTS = 48;

// The edges the crosshair draws for a block, in the voxel's local space.
//
// Three sources, in this order, and the point of the order is that the first one is the table the
// player's body already collides with:
//  - a block with collision shapes (blockShapeBoxes, i.e. BlockDef::shapes) outlines those boxes, so
//    a slab shows a half-high shelf, a fence a thin post and a cactus an inset column - the outline
//    *is* the collision, which is the claim this module exists to make visible;
//  - a plant - crossed quads with no collision at all - outlines the quads the mesher actually
//    draws, because outlining the empty cube around them would claim a shape the block does not have;
//  - anything else that has neither (a block added without a shape) falls back to the full cube.
// Air and liquids return 0: nothing is outlined where there is nothing to break.
int selectionSegments(uint16_t id, SelectSegment out[], int cap);

// The outline itself: one entity, rebuilt only when the targeted block type changes, hidden when the
// crosshair leaves the world. Drawn as triangles because the engine's LINES shader type has no
// compiled variant in this project's bundle and the runtime never compiles GLSL; every edge becomes a
// thin bar, and each bar is two crossed quads with both windings so it reads from any angle and from
// either side (the same trick the mesher uses to make a plant double-sided).
//
// Two strokes per bar - a dark band and a lighter core nested inside it - so the line stays legible
// over snow and over obsidian without a per-block colour.
class SelectOverlay {
public:
    // Creates the entity. Safe to call every frame: it only builds once per scene.
    bool attach(quantum::Scene* scene);

    // Outline the block `id` at this voxel. An id with no edges (air, water) hides the overlay.
    void show(int x, int y, int z, uint16_t id);
    void hide();

    bool isShowing() const { return showing; }
    int blockX() const { return bx; }
    int blockY() const { return by; }
    int blockZ() const { return bz; }
    int blockId() const { return drawnId; }

    // Diagnostics the probe reads: how many edges and boxes the current outline has, how many times
    // the buffers were rebuilt (it should track block *types*, not frames), and how many moves.
    int segments() const { return segmentsMade; }
    int boxes() const { return boxesMade; }
    uint64_t uploads() const { return uploadsMade; }
    uint64_t moves() const { return movesMade; }

    // Whether the render system has taken the bars' buffers yet, and which shader variant it picked.
    // The bits matter: this overlay must land on the variant the liquid mesh already uses, or nothing
    // is drawn and a mesh that never loads says so only here.
    bool geometryLoaded() const;
    unsigned int geometryVertices() const;
    unsigned long long geometryShaderBits() const;

    // Whether the last rebuild is still waiting for the render system. The engine reloads a mesh from
    // its draw path on the loadCalled flag, not on needReload, so a rebuild that clears only
    // needReload writes buffers nobody will read and the outline keeps drawing the shape it was
    // built with. This is the number that says which of the two happened.
    bool geometryReloadQueued() const;

    // How many vertices the last build handed to the renderer. Separated from geometryVertices(),
    // which is what the component says it holds: the two numbers answer different questions, and a
    // difference between them is a buffer the engine refused rather than a bar that was never built.
    unsigned int builtVertices() const { return vertsMade; }

private:
    quantum::Scene* scenePtr = nullptr;
    int64_t entity = -1;
    int bx = 0, by = 0, bz = 0;
    int drawnId = -1;         // the block id the buffers currently hold, -1 before the first build
    unsigned long long geomKey = 0;   // what those buffers actually draw, so two blocks of one shape share them
    bool showing = false;
    int segmentsMade = 0;
    int boxesMade = 0;
    unsigned int vertsMade = 0;
    uint64_t uploadsMade = 0;
    uint64_t movesMade = 0;
};

SelectOverlay& selectOverlay();

// Diagnostics for the probe: the same numbers, for a build where the overlay is not attached.
int selectStrokeCount();          // how many strokes each bar is drawn with
float selectDarkThickness();
float selectCoreThickness();

// The claim this module makes, tested headless: the outline of a solid block is its collision table
// and not its cube, a plant outlines the quads it draws instead of the empty cube around it, water
// and air outline nothing, and the overlay follows a target without rebuilding geometry per frame.
void probeVoxelSelect(VoxelWorld& world, Probe& probe);

}  // namespace voxel
