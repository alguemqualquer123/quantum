// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include "VoxelConfig.h"

#include <cstdint>
#include <string>

namespace voxel {

class VoxelWorld;
struct Probe;

// Survival state: health, food, water and air, with the rules that move them.
//
// This module is arithmetic on a report. It never reads the world, the player or the engine loop:
// the host fills a VoxelBody each frame and hands it over with the seconds that passed. That is what
// makes every rate below testable headless - a probe can spend exactly one hundred seconds standing
// still, which a module that sampled the world by itself could only do by waiting for a real player
// to walk them through it.

// Twenty units per bar, so a heart is two health, a shank two food and a drop two water. The scale
// is not 100 because the HUD row is an integer number of icons tall, and the icons are half a bar.
const float VITAL_MAX = 20.0f;
// Ten seconds with the head under: long enough to cross a pond, short enough that a lake is a
// decision. Surfacing refills four times faster than holding a breath empties it.
const float BREATH_MAX = 10.0f;
// A respawn does not hand back the whole day: the bars come back partly spent, so getting up is not
// a free refill and the first thing a revived player does is look for food.
const float RESPAWN_HUNGER = 12.0f;
const float RESPAWN_THIRST = 12.0f;
// One drink from a water block, in water units: a third of the bar. A sip does not take the column
// apart - the world is not emptied - so the bar is the only thing the act costs anything.
const float DRINK_PER_SIP = 6.0f;

// ---- the rates ----
// Food and water burn per second of game time. Idle is the floor; walking and sprinting multiply
// it, and a jump pays on top, because the bars are the price of moving rather than a clock.
const float HUNGER_IDLE_PER_SECOND = 0.05f;
const float THIRST_IDLE_PER_SECOND = 0.08f;
const float ACTIVITY_WALK = 2.0f;
const float ACTIVITY_SPRINT = 3.0f;
const float JUMP_HUNGER_COST = 0.06f;

// The first three blocks of a drop are nothing and every block past them costs one unit - half a
// heart, since a heart is two units. A ten-block drop therefore takes seven units off.
const float FALL_SAFE_BLOCKS = 3.0f;
const float FALL_DAMAGE_PER_BLOCK = 1.0f;

// Continuous harm, all per second, so the damage of a stay is the rate times the time in it.
const float LAVA_DAMAGE_PER_SECOND = 6.0f;      // twenty frames of lava is a death
const float HAZARD_DAMAGE_PER_SECOND = 1.0f;    // cactus: a prick, survivable, annoying
const float DROWN_DAMAGE_PER_SECOND = 2.0f;     // once the air is gone
const float STARVE_DAMAGE_PER_SECOND = 0.3f;
const float THIRST_DAMAGE_PER_SECOND = 0.5f;    // thirst is the faster way to die
const float BREATH_REFILL_PER_SECOND = 4.0f;

// Healing is bought with food: the bar has to be near full and the water bar above a quarter, and
// the health that comes back spends food at REGEN_HUNGER_PER_SECOND - more than idling costs, so
// sitting still at full health is the only free state in the module.
const float REGEN_HEALTH_PER_SECOND = 0.4f;
const float REGEN_HUNGER_PER_SECOND = 0.09f;
const float REGEN_HUNGER_GATE = 16.0f;
const float REGEN_THIRST_GATE = 6.0f;

// Below this the world has given up on the body (the controller respawns it on the surface out of
// the same void), so what is left of the player is not a player.
const float VOID_FEET_Y = -16.0f;

enum VoxelDamageCause : int {
    DAMAGE_FALL = 0,
    DAMAGE_DROWN,
    DAMAGE_LAVA,
    DAMAGE_HAZARD,     // cactus and whatever else pricks
    DAMAGE_STARVE,
    DAMAGE_THIRST,
    DAMAGE_VOID,       // fell out of the world
    DAMAGE_DIRECT,     // something outside this module hurt the body
    DAMAGE_ATTACK,     // a mob struck the body - appended, because a death count is keyed by cause
    DAMAGE_COUNT
};

// The name the log, the overlay and the probe print. Order must match the enum.
const char* voxelDamageCauseName(int cause);

// What the body did this frame, as reported by the host. `moved` is horizontal and already capped
// for a teleport: a reposition is not a stride, and a respawn that counted as one would pay for the
// distance the world moved the player.
struct VoxelBody {
    float feetX = 0.0f, feetY = 0.0f, feetZ = 0.0f;
    float moved = 0.0f;
    bool grounded = true;
    bool jumped = false;      // edge: true on the frame the body left the ground under its own power
    bool flying = false;      // a fall from up here is not a fall
    bool sprinting = false;
    bool inWater = false;
    bool headUnder = false;   // the eye is inside a liquid, so no breath
    bool inLava = false;
    bool touchingHazard = false;
};

struct VoxelVitals {
    float health = VITAL_MAX;
    float hunger = VITAL_MAX;
    float thirst = VITAL_MAX;
    float breath = BREATH_MAX;
    bool alive = true;
};

// The whole state, readable and writable, so a probe can put back exactly what it found and the HUD
// can draw a bar without the module owning any UI.
VoxelVitals voxelVitals();
void setVoxelVitals(const VoxelVitals& v);

float healthValue();
float hungerValue();
float thirstValue();
float breathValue();
bool voxelAlive();

// Where a respawn puts the body. The host sets it once the world knows the spawn column; the module
// only remembers it, because only the controller can actually place a body.
void setVoxelSpawn(float x, float y, float z);
void voxelSpawn(float& x, float& y, float& z);

// Which rule set the bars are under. The controller owns the answer and the host hands it over each
// frame; the module remembers it, in the same shape as the spawn, because "creative does not decay or
// take damage" is a rule about the vitals and a rule that only holds while the caller remembers to
// skip the call is not a rule - a mob bite in a later phase reaches `damageVoxelLife` directly.
// Outside survival the bars are kept, not reset: switching back is the way it was, which is what makes
// a creative stretch a break rather than a fresh character. Health is the one bar a switch fills,
// because a mode that cannot hurt you has no reason to be holding a corpse - and no way to stand one
// up, since the decay that would heal it is the gate being closed.
void setVoxelMode(int mode);
int voxelMode();

// The one entry point: the seconds since the last call and what the body did in them. Everything the
// module does is a rate times those seconds or an edge taken from the report, so a caller that hands
// it 0.5f twice gets the same answer as one that hands it 1.0f once.
void updateVoxelLife(const VoxelBody& body, float seconds);

// Harm and help from outside the module - a mob bite, a falling block, the inventory's food. A
// damage on a dead body does nothing: one death is one death, and a death count that climbed while
// the player lay there would be a lie about how many times the game ended. A damage outside survival
// also does nothing, and returns false: the caller does not have to know the mode to be right about
// it.
bool damageVoxelLife(float amount, int cause);
void healVoxelLife(float amount);

// Returns false when the bar was already full, so a caller can keep the item it spent rather than
// eat it for nothing.
bool eatVoxelLife(float nutrition);
bool drinkVoxelLife(float water);

// What a block is worth as food: 0 for anything that is not worth eating. The table is here rather
// than in a BlockDef field because g_blocks is 59 positional aggregates and a new column there would
// have to be written out 59 times, with a row that forgot it silently inheriting the row above.
float voxelFoodNutrition(uint16_t type);

// Brings the body back with the bars partly spent, and forgets the fall it died in - `atFeetY` is
// where the respawn put it, without which the peak of the drop that killed it would be charged
// again the moment it landed.
void reviveVoxelLife(float atFeetY);

// The death edge, consumed by the first caller: the host asks once per frame, and only the host has
// a controller to teleport, a log to write and a respawn point to honour.
bool voxelJustDied(int& cause);

// The hurt edge, same shape and the same reason. Every harm the body took goes through this module -
// the fall it just landed, the lava it is standing in, the mob that struck it from a hook the host
// installed - and the HUD is the only thing that can answer any of them with a flash. It is an edge
// rather than a level because a plate that fades while the cause keeps hurting has to be re-armed by
// the hit, not by the clock: `amount` is what that one hit took off.
bool voxelJustHurt(int& cause, float& amount);
// The same two numbers after the edge has been consumed, so an overlay can still name the last hit.
float voxelLastHurtAmount();
int voxelLastHurtCause();

int voxelDeaths();
int voxelDeathsByCause(int cause);
// The cause that ended the body, valid after a death and until the next one.
int voxelLastDeathCause();

// "Life: survival  17 hp  20 food  12 water  air 9.0  dead 0" for the debug overlay - the only way to
// see the bars move in a headless capture, and the line the probe reads back by walking its numbers in
// order. Each value is labelled rather than written as a fraction for exactly that reason: a "17/20"
// would hand the parser the maximum as the next number it finds. The mode leads because it is what
// makes the numbers mean anything: a frozen bar in creative and a frozen bar after a death are the
// same digits. When the body is dead the line names the cause and the death count instead, because a
// row of zeros would read as a bug.
std::string lifeStats();

// The claim this module makes, tested headless: the bars burn at the rates named, movement makes
// them burn faster, a drop charges the damage and water and flight cancel it, the air runs out and
// then the health does, an empty bar kills and a full one heals at the price of food, the death is
// one event with a cause, a revive puts the body back without charging it for the way it died, and
// outside survival nothing burns, nothing hurts and the bars are kept for the way back.
void probeVoxelLife(VoxelWorld& world, Probe& probe);

}  // namespace voxel
