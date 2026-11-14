// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include "VoxelConfig.h"

#include <cstdint>
#include <string>

namespace voxel {

class VoxelWorld;
struct Probe;

// Dropped items: a broken block does not teleport into the inventory, it lands in the world as a
// small cube of its own atlas tile, spins slowly, bobs, and is taken back when someone walks over
// it. The cube is the block rather than a drawn icon because the project has no item atlas and the
// tile already says what the thing is.
//
// Like every other entity here there is no Body3D: the voxel grid is the collider and the item is
// moved through it with the same moveBoxVoxel() the player and the mobs use, so an item slides into
// a corner, rides up a step and floats in water for exactly the reasons a body does.

const int MAX_DROPS = 96;                  // pool cap: what may lie on the ground at once

const float DROP_CUBE = 0.30f;             // one third of a block, measured edge to edge
const float DROP_SPIN_DEG_PER_SEC = 90.0f; // a quarter turn a second: slow enough to read the tile
const float DROP_BOB = 0.05f;              // how far the resting cube floats above its shadow
const float DROP_BOB_RATE = 2.4f;          // radians of the sine per second
const float DROP_GRAVITY = 24.0f;          // lighter than the player's 28: an item feels tossed
const float DROP_SWIM_UP = 6.0f;           // buoyancy in a liquid, against that gravity
const float DROP_THROW_UP = 3.2f;          // the hop a block gets when it leaves the ground
const float DROP_MAX_FRAME = 0.1f;         // a hitch must not tunnel an item through a floor

const float DROP_DELAY = 0.6f;             // a fresh drop cannot be sucked straight back
const float DROP_CHEST_BELOW_EYE = 0.9f;   // the eye is what the host has; the magnet aims lower
// Inside this, the item starts coming to you. Kept smaller than the distance [J] throws, because a
// thing you pressed a key to get rid of must not be back in your hand two thirds of a second later -
// the magnet closes the last step, it does not fetch across the screen.
const float DROP_MAGNET_RADIUS = 1.8f;
const float DROP_MAGNET_SPEED = 5.5f;      // blocks/s of that pull
const float DROP_PICKUP_RADIUS = 0.85f;    // ... and inside this it is gone from the world
const float DROP_ASK_SECONDS = 0.25f;      // a refused item asks again this often, not every frame
const float DROP_MERGE_RADIUS = 0.75f;     // two same-type cubes this close become one stack
const float DROP_MERGE_SECONDS = 0.3f;     // the merge pass does not need to run every frame
const float DROP_LIFETIME = 180.0f;        // abandoned items expire instead of eating the pool
const int DROP_MAX_STACK = 64;             // one drop's ceiling, and the hotbar's

// The inventory is not this module's business: a drop that reached a body asks, and whoever answers
// says whether the item fitted. Null, or a false answer, leaves the item in the world - which is the
// whole point of dropping into the world instead of straight into the hand. Arguments: block type,
// amount, and where the item was when it was taken (a sound, or a particle, wants the place).
extern bool (*dropCollectFn)(uint16_t type, int count, float x, float y, float z);

// Puts `count` of `type` in the world at (x,y,z) with the usual little hop. It first tries to join a
// neighbouring stack of the same block, so breaking forty stone makes one pile and not forty
// entities. False when the pool is full and the item had nowhere to go.
bool spawnVoxelDrop(VoxelWorld& world, float x, float y, float z, uint16_t type, int count);

// Fall, spin, bob, merge, get pulled in, get taken, expire, and despawn beyond `despawnRadius` of the
// eye at (eyeX,eyeY,eyeZ). Call it every frame next to updateVoxelMobs().
void updateVoxelDrops(VoxelWorld& world, float dt, float eyeX, float eyeY, float eyeZ,
                      float despawnRadius);

// The pool's own bookkeeping, for the overlay and the probe.
int liveDropCount();
std::string dropStats();
uint64_t dropsSpawned();
uint64_t dropsPickedUp();
uint64_t dropsMerged();
uint64_t dropsExpired();
uint64_t dropsLost();
// Empties the pool without asking anyone to collect: the probe uses it to leave nothing behind.
void clearVoxelDrops();

// What one slot of the pool is currently doing, by index rather than by handle, so a headless run can
// read the sim without a renderer. False when the slot is empty or out of range.
struct DropSnapshot {
    uint16_t type = 0;
    int amount = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float spinDeg = 0.0f;   // the yaw the cube is drawn at
    float bobY = 0.0f;      // ... and the offset the bob adds to its centre
    bool grounded = false;
};
bool dropSnapshot(int slot, DropSnapshot& out);

// The animation is a function of age, not an accumulator: a drop that was never drawn still spins,
// and a probe can measure the turn without a scene. Both are deg / unit, wrapping into 0..360.
float dropSpinDeg(float age);
float dropBobY(float age);

// The measured proof of what this file claims: an item appears in the world instead of the hand,
// falls until the block under it stops it, turns at the advertised rate and bobs on the ground, two
// same-type piles join into one stack, the magnet takes an item that is not yet walked over and the
// collector's answer decides whether it leaves the world. Runs in an arena carved in the focus column
// and puts the pool and every voxel back. Called by the host on VOXEL_PROBE.
void probeVoxelDrops(VoxelWorld& world, voxel::Probe& probe);

}  // namespace voxel
