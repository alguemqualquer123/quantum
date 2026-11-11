// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include "VoxelBlocks.h"
#include "VoxelConfig.h"

#include <cstdint>

namespace quantum { class Scene; }

namespace voxel {

class VoxelWorld;
struct Probe;

// Breaking is progressive: the block keeps drawing its own faces and a crack grows over them.
// Ten stages, the same number the atlas carries, so a stage is an atlas slot and nothing else.
const int BREAK_STAGE_COUNT = 10;

// How often a mining puff is released while the button stays held. A break lasts seconds; one
// burst at the end reads as a pop, so the chips have to fall the whole time.
const float BREAK_CHIP_SECONDS = 0.28f;

// Which of the ten tiles a 0..1 mining progress is on. -1 means "nothing is being broken":
// progress 0 is the frame before the first tick, and drawing stage 0 there would flash a crack
// the instant the crosshair lands on a block.
int breakStage(float progress);

// The atlas slot a stage draws, or -1 for an out-of-range stage. Stage tiles are appended at the
// end of the atlas by scripts/generate_voxel_atlas.py, so the slot number is a layout fact and this
// function is the only place that knows it.
int breakStageTile(int stage);

// The crack over the targeted block: one entity, 24 vertices, and a stage change writes four
// floats. The cube's UVs cover 0..1 and the submesh's textureRect uniform (mesh.vert's
// HAS_TEXTURERECT path) maps that range onto the stage tile, so ten stages are ten rects and the
// geometry is uploaded exactly once. The cube is inflated by a fraction of a block so it sits in
// front of the face it cracks instead of inside it.
class BreakOverlay {
public:
    // Creates the entity. Safe to call every frame: it only builds once per scene.
    bool attach(quantum::Scene* scene);

    // Put the crack on a block. stage < 0 hides it, and the same stage twice in a row does nothing
    // at all - the overlay only writes the uniform when the stage actually moves.
    void show(int x, int y, int z, int stage);
    void hide();

    bool isShowing() const;
    int stage() const;
    int blockX() const { return bx; }
    int blockY() const { return by; }
    int blockZ() const { return bz; }

    // Diagnostics the probe reads: how many uniform rewrites happened, and whether the entity is
    // still alive after a scene teardown.
    uint64_t stageWrites() const { return writes; }
    uint64_t moves() const { return movesMade; }

    // Whether the render system has taken the cube's buffers yet. A mesh that never loads stays
    // invisible without saying so, so the capture log reads the component's own flag. The shader
    // bits are the variant the renderer picked: if they do not carry Uv1+Txr the rect was never
    // compiled into the vertex shader and the crack samples the atlas corner.
    bool geometryLoaded() const;
    unsigned int geometryVertices() const;
    unsigned long long geometryShaderBits() const;

private:
    quantum::Scene* scenePtr = nullptr;
    int64_t entity = -1;          // quantum::Entity is a small int; -1 = never built
    int bx = 0, by = 0, bz = 0;
    int currentStage = -1;
    bool showing = false;
    uint64_t writes = 0;
    uint64_t movesMade = 0;
};

BreakOverlay& breakOverlay();

// Chips flying off a block. One ring of pooled emitters, each a world-space point emitter over the
// block's own atlas tile: the debris is a square of the texture it came from, which is what makes
// sand and stone look different as they fall. Ring-rotating means a burst never waits for the
// previous one to die, and no entity is created after the first call.
void spawnBreakDebris(quantum::Scene* scene, int x, int y, int z, uint16_t type, int count);

// Diagnostics for the probe: pool size, how many bursts were fired, and how many live points the
// most recent burst queued.
int breakDebrisPoolSize();
uint64_t breakDebrisSpawns();
int breakDebrisLastPoints();

// The claim this module makes, tested headless: a progress maps monotonically onto the ten stages
// and cancels to -1, the overlay follows a target and hides on cancel, and a break queues debris.
void probeVoxelBreak(VoxelWorld& world, Probe& probe);

}  // namespace voxel
