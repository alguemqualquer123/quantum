// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelLife.h"

#include "VoxelBlocks.h"
#include "VoxelProbe.h"
#include "VoxelWorld.h"

#include <cmath>
#include <cstdio>

namespace voxel {

namespace {

struct LifeState {
    VoxelVitals v;
    float spawnX = 0.0f, spawnY = 0.0f, spawnZ = 0.0f;

    // Which rule set the bars are under. Survival is the only one that moves them; see setVoxelMode.
    int mode = VOXEL_MODE_SURVIVAL;

    // The fall the body is in: the highest the feet went since they last touched something. Measured
    // from the peak rather than from the block the player jumped off, so a jump that lands on a ledge
    // costs the drop it actually made.
    float airPeakY = 0.0f;
    bool wasAirborne = false;
    bool wasGrounded = true;

    int deaths = 0;
    int deathsByCause[DAMAGE_COUNT] = {0};
    int lastCause = DAMAGE_DIRECT;
    bool deathEdge = false;

    // The hit the HUD is asked to flash for. `amount` is what that one hit took off, not the bar, so
    // the plate can be brighter for the bite that hurt than for the cactus that pricked.
    bool hurtEdge = false;
    float hurtAmount = 0.0f;
    int hurtCause = DAMAGE_DIRECT;
};

LifeState g;

float barDown(float v) { return v < 0.0f ? 0.0f : v; }
float barUp(float v, float max) { return v > max ? max : v; }

// One death, one record. The bars stay where they are so the HUD can still say what the player was
// out of when they died, and the counter only moves on the transition - a death count that climbed
// while the body lay there would be counting frames, not deaths.
void kill(LifeState& s, int cause) {
    s.v.health = 0.0f;
    s.v.alive = false;
    s.deaths++;
    if (cause >= 0 && cause < DAMAGE_COUNT) s.deathsByCause[cause]++;
    s.lastCause = cause;
    s.deathEdge = true;
}

// The harm one frame did, split by cause so the killing frame can name the worst of them: a body that
// starves while standing in lava dies of lava, and a death logged as "starve" there would send
// whoever reads it looking for food instead of looking at where they were standing.
struct Harm {
    float total = 0.0f;
    int lead = DAMAGE_DIRECT;
    float leadAmount = 0.0f;

    void add(int cause, float amount) {
        if (amount <= 0.0f) return;
        total += amount;
        if (amount > leadAmount) { leadAmount = amount; lead = cause; }
    }
};

const char* const kCauseNames[DAMAGE_COUNT] = {
    "fall", "drown", "lava", "cactus", "starve", "thirst", "void", "damage", "attack"
};

// One hit, recorded once. Every harm the body takes passes here - the landing of a fall, the second
// of lava, the bite a mob dealt through the host's hook - because the flash is a reaction to the hit
// and a hit that skipped this line would leave the screen calm while a bar dropped.
void hurt(LifeState& s, int cause, float amount) {
    if (amount <= 0.0f) return;
    s.hurtEdge = true;
    s.hurtAmount = amount;
    s.hurtCause = cause;
}

}  // namespace

const char* voxelDamageCauseName(int cause) {
    if (cause < 0 || cause >= DAMAGE_COUNT) return "unknown";
    return kCauseNames[cause];
}

VoxelVitals voxelVitals() { return g.v; }

void setVoxelVitals(const VoxelVitals& v) {
    g.v.health = barUp(barDown(v.health), VITAL_MAX);
    g.v.hunger = barUp(barDown(v.hunger), VITAL_MAX);
    g.v.thirst = barUp(barDown(v.thirst), VITAL_MAX);
    g.v.breath = barUp(barDown(v.breath), BREATH_MAX);
    g.v.alive = v.alive;
    // A bar written from outside has not moved the body, so the fall bookkeeping restarts: the peak
    // is re-anchored by the next frame the feet are on something, and a state handed back by a probe
    // cannot land as a drop the player never fell.
    g.wasAirborne = false;
    g.deathEdge = false;
    // A bar handed back from outside is not a hit: only the code that takes health off arms the flash.
    g.hurtEdge = false;
}

float healthValue() { return g.v.health; }
float hungerValue() { return g.v.hunger; }
float thirstValue() { return g.v.thirst; }
float breathValue() { return g.v.breath; }
bool voxelAlive() { return g.v.alive; }

void setVoxelSpawn(float x, float y, float z) {
    g.spawnX = x; g.spawnY = y; g.spawnZ = z;
}

void voxelSpawn(float& x, float& y, float& z) {
    x = g.spawnX; y = g.spawnY; z = g.spawnZ;
}

void setVoxelMode(int mode) {
    if (mode < 0 || mode >= VOXEL_MODE_COUNT) mode = VOXEL_MODE_SURVIVAL;
    if (mode == g.mode) return;

    g.mode = mode;
    // The fall is forgotten at the switch, and only at the switch: a body that spent a minute flying
    // in creative and is handed back to survival must not pay for the height it reached while it
    // could not be hurt, and a caller that sets the same mode every frame must not reset the drop the
    // player is in the middle of.
    g.wasAirborne = false;
    g.wasGrounded = true;
    g.airPeakY = 0.0f;

    // A mode where nothing can hurt you does not keep a corpse. The bars are the other story: food
    // and water are kept exactly as they were, because they are the debt the player comes back to,
    // and only health - the bar a death is made of - is filled. Without this, a body that died with
    // the respawn column unstreamed and was then switched out of survival would stay dead inside a mode
    // that has no way to stand it up, and the switch back would put it on the ground at zero food with
    // nobody left to revive it. The death counters keep the death that happened; this is not a second
    // one.
    if (mode != VOXEL_MODE_SURVIVAL && !g.v.alive) {
        g.v.health = VITAL_MAX;
        g.v.alive = true;
        g.deathEdge = false;
    }
}

int voxelMode() { return g.mode; }

void updateVoxelLife(const VoxelBody& body, float seconds) {
    LifeState& s = g;
    // Outside survival the bars do not move at all - not the burn, not the harm, not the death. The
    // host could skip this call instead, and does for a spectator, but the gate has to live here as
    // well: the damage a later phase deals from outside (a mob bite, a falling block) reaches
    // damageVoxelLife directly, and a rule that only holds while every caller remembers it is not a
    // rule.
    if (s.mode != VOXEL_MODE_SURVIVAL) return;
    if (!s.v.alive) return;   // the dead do not decay
    if (seconds <= 0.0f) { s.wasGrounded = body.grounded; return; }

    // Which bars were already empty when this interval began. The harm terms read these rather than
    // the values they are about to compute, because the interval that *finishes* a bar is one the
    // body spent having air (or food) in it: charging it anyway gives a half-step of drowning at the
    // moment the last bubble pops, which is a frame-rate-dependent answer to "how long is ten
    // seconds of breath".
    const bool wasAirless = s.v.breath <= 0.0f;
    const bool wasStarving = s.v.hunger <= 0.0f;
    const bool wasParched = s.v.thirst <= 0.0f;

    // 1. Moving is what costs. The idle rate is the floor and everything the body does on top of
    // standing still multiplies it, so the bars are the price of the walk rather than a clock.
    float burn = 1.0f;
    if (body.sprinting) burn = ACTIVITY_SPRINT;
    else if (body.moved > 0.001f) burn = ACTIVITY_WALK;
    s.v.hunger -= HUNGER_IDLE_PER_SECOND * burn * seconds;
    s.v.thirst -= THIRST_IDLE_PER_SECOND * burn * seconds;

    // Only on the frame the body leaves the ground, and only from the ground: swimming up and flying
    // take the same key without paying for a jump.
    if (body.jumped && s.wasGrounded && !body.flying && !body.inWater) s.v.hunger -= JUMP_HUNGER_COST;
    s.wasGrounded = body.grounded;

    // 2. The fall, and the moment it ends.
    Harm landing;
    if (body.flying) {
        // Flight cancels the debt rather than pausing it: a body that lands after flying down thirty
        // blocks did not fall, so the peak from before the take-off has to go.
        s.wasAirborne = false;
        s.airPeakY = body.feetY;
    } else if (!body.grounded) {
        s.wasAirborne = true;
        if (body.feetY > s.airPeakY) s.airPeakY = body.feetY;
    } else if (s.wasAirborne) {
        const float drop = s.airPeakY - body.feetY;
        // Water is the soft landing: the drop is measured the same way and then thrown away. A body
        // lifted by the world has a drop of its own sign, which is not a fall at all.
        if (drop > FALL_SAFE_BLOCKS && !body.inWater)
            landing.add(DAMAGE_FALL, (drop - FALL_SAFE_BLOCKS) * FALL_DAMAGE_PER_BLOCK);
        s.wasAirborne = false;
        s.airPeakY = body.feetY;
    } else {
        s.airPeakY = body.feetY;   // standing: the peak follows the ground under the feet
    }
    if (landing.total > 0.0f) {
        hurt(s, landing.lead, landing.total);
        s.v.health = barDown(s.v.health - landing.total);
        if (s.v.health <= 0.0f) { kill(s, landing.lead); return; }
    }

    // 3. Air. The bar empties one second per second and refills four times as fast, so a body that
    // breaks the surface for one frame gets back what it just spent.
    if (body.headUnder) s.v.breath = barDown(s.v.breath - seconds);
    else s.v.breath = barUp(s.v.breath + BREATH_REFILL_PER_SECOND * seconds, BREATH_MAX);

    // 4. Everything that hurts, gathered by cause and applied as one number.
    Harm harm;
    if (body.headUnder && wasAirless)
        harm.add(DAMAGE_DROWN, DROWN_DAMAGE_PER_SECOND * seconds);
    if (body.inLava) harm.add(DAMAGE_LAVA, LAVA_DAMAGE_PER_SECOND * seconds);
    if (body.touchingHazard) harm.add(DAMAGE_HAZARD, HAZARD_DAMAGE_PER_SECOND * seconds);
    if (wasStarving) harm.add(DAMAGE_STARVE, STARVE_DAMAGE_PER_SECOND * seconds);
    if (wasParched) harm.add(DAMAGE_THIRST, THIRST_DAMAGE_PER_SECOND * seconds);

    s.v.hunger = barDown(s.v.hunger);
    s.v.thirst = barDown(s.v.thirst);

    // The controller gives up on a body below this height and puts it back on the surface; what is
    // down here is not a player with a health bar, it is a body outside the world.
    if (body.feetY < VOID_FEET_Y) { kill(s, DAMAGE_VOID); return; }

    if (harm.total > 0.0f) {
        hurt(s, harm.lead, harm.total);
        s.v.health = barDown(s.v.health - harm.total);
        if (s.v.health <= 0.0f) { kill(s, harm.lead); return; }
    }

    // 5. Healing is bought with food, and only by a body nothing is currently hurting: a player in
    // lava regenerating while they burn is not a healing rule, it is two rules fighting.
    if (harm.total <= 0.0f && landing.total <= 0.0f && s.v.health < VITAL_MAX &&
        s.v.hunger >= REGEN_HUNGER_GATE && s.v.thirst >= REGEN_THIRST_GATE) {
        s.v.health = barUp(s.v.health + REGEN_HEALTH_PER_SECOND * seconds, VITAL_MAX);
        s.v.hunger = barDown(s.v.hunger - REGEN_HUNGER_PER_SECOND * seconds);
    }
}

bool damageVoxelLife(float amount, int cause) {
    // The refusal is here, not only in the caller, for the same reason the decay gate is: whoever
    // deals the damage in a later phase (a mob, a falling block) should not have to know which mode
    // the player is in to be wrong about it. Creative players are not invulnerable because someone
    // remembered to ask; they are invulnerable because this function says no.
    if (g.mode != VOXEL_MODE_SURVIVAL) return false;
    if (!g.v.alive || amount <= 0.0f) return false;
    hurt(g, cause, amount);
    g.v.health = barDown(g.v.health - amount);
    if (g.v.health <= 0.0f) kill(g, cause);
    return true;
}

void healVoxelLife(float amount) {
    if (!g.v.alive || amount <= 0.0f) return;
    g.v.health = barUp(g.v.health + amount, VITAL_MAX);
}

bool eatVoxelLife(float nutrition) {
    if (nutrition <= 0.0f || g.v.hunger >= VITAL_MAX) return false;
    g.v.hunger = barUp(g.v.hunger + nutrition, VITAL_MAX);
    return true;
}

bool drinkVoxelLife(float water) {
    if (water <= 0.0f || g.v.thirst >= VITAL_MAX) return false;
    g.v.thirst = barUp(g.v.thirst + water, VITAL_MAX);
    return true;
}

float voxelFoodNutrition(uint16_t type) {
    // Two blocks feed a survival player, and both are already in the table, breakable and growable.
    // Everything else answers 0, which is how the host knows not to spend the item. The table lives
    // here rather than in a BlockDef column because g_blocks is 59 positional aggregates, and a new
    // column has to be written out 59 times - a row that forgets it silently keeps the row above.
    switch (type) {
        case BLOCK_MELON:   return 6.0f;   // a third of the bar: melon is the meal
        case BLOCK_PUMPKIN: return 4.0f;
        default:            return 0.0f;
    }
}

void reviveVoxelLife(float atFeetY) {
    g.v.health = VITAL_MAX;
    g.v.hunger = RESPAWN_HUNGER;
    g.v.thirst = RESPAWN_THIRST;
    g.v.breath = BREATH_MAX;
    g.v.alive = true;
    g.deathEdge = false;
    // The hit that ended the body is as forgotten as the fall that caused it: a respawned player
    // whose line still says "last hit -12.0 attack" would read as still bleeding.
    g.hurtEdge = false;
    g.hurtAmount = 0.0f;
    // The peak becomes the ground the body was just put on: without this, standing up at the spawn
    // pays for the drop that killed the player and the second death is instant.
    g.wasAirborne = false;
    g.wasGrounded = true;
    g.airPeakY = atFeetY;
}

bool voxelJustDied(int& cause) {
    if (!g.deathEdge) return false;
    g.deathEdge = false;
    cause = g.lastCause;
    return true;
}

bool voxelJustHurt(int& cause, float& amount) {
    if (!g.hurtEdge) return false;
    g.hurtEdge = false;
    cause = g.hurtCause;
    amount = g.hurtAmount;
    return true;
}

float voxelLastHurtAmount() { return g.hurtAmount; }

int voxelLastHurtCause() { return g.hurtCause; }

int voxelDeaths() { return g.deaths; }

int voxelDeathsByCause(int cause) {
    if (cause < 0 || cause >= DAMAGE_COUNT) return 0;
    return g.deathsByCause[cause];
}

int voxelLastDeathCause() { return g.lastCause; }

std::string lifeStats() {
    char buf[160];
    // The mode leads the line because a frozen bar and an empty one look the same from the outside:
    // "food 4" during a creative session reads as a bug unless the line says whose rules it is under.
    // The name carries no digits, so the probe's number walk is unaffected.
    if (!g.v.alive) {
        snprintf(buf, sizeof(buf), "Life: %s - dead (%s), %d deaths, waiting for a respawn",
                 voxelGameModeName(g.mode), voxelDamageCauseName(g.lastCause), g.deaths);
        return std::string(buf);
    }
    // Each bar is labelled rather than written as a fraction, because the probe reads this line back
    // by walking its numbers in order and a "17/20" would hand it the maximum as the next value.
    snprintf(buf, sizeof(buf), "Life: %s  %.0f hp  %.0f food  %.0f water  air %.1f  dead %d",
             voxelGameModeName(g.mode), g.v.health, g.v.hunger, g.v.thirst, g.v.breath, g.deaths);
    std::string line(buf);
    // The last hit is appended, never inserted: the number walk above reads the bars in order from
    // the front of the line, and a value added at the end is the only one it cannot disturb. It is a
    // level rather than the edge, because the edge is consumed by whoever flashes the screen and the
    // log has to keep saying what the last hit was after that flash went away.
    if (g.hurtAmount > 0.0f) {
        snprintf(buf, sizeof(buf), "  last hit -%.1f %s", g.hurtAmount,
                 voxelDamageCauseName(g.hurtCause));
        line += buf;
    }
    return line;
}

namespace {

// A body that stood still on a block, so a check that measures decay can name what it changed.
VoxelBody standing(float feetY) {
    VoxelBody b;
    b.feetX = 4.0f; b.feetY = feetY; b.feetZ = 4.0f;
    b.grounded = true;
    return b;
}

// Game time spent in fixed steps: the same wall of seconds a real run spends, in a number a check
// can state. The count is rounded rather than accumulated so the last step is not a remainder.
void spend(const VoxelBody& b, float seconds, float step) {
    const int n = (int)(seconds / step + 0.5f);
    for (int i = 0; i < n; i++) updateVoxelLife(b, step);
}

// The next run of digits in the overlay line. Hand-scanned rather than sscanf: the CRT calls sscanf
// deprecated in this build, and the line is short enough that reading the digits it is known to hold
// is the whole job.
float nextNumber(const std::string& text, size_t& cursor) {
    while (cursor < text.size() && !(text[cursor] >= '0' && text[cursor] <= '9')) cursor++;
    float value = 0.0f;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9')
        value = value * 10.0f + (text[cursor++] - '0');
    if (cursor < text.size() && text[cursor] == '.') {
        cursor++;
        float scale = 0.1f;
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
            value += (text[cursor++] - '0') * scale;
            scale *= 0.1f;
        }
    }
    return value;
}

}  // namespace

void probeVoxelLife(VoxelWorld& world, Probe& probe) {
    ProbeModule mod(probe, "life");

    // A probe leaves the body it tested on exactly as it found it: the bars, both counters, the death
    // edge, the hit edge it may have been mid-flash on, the fall it may have been in the middle of,
    // and the spawn the host had chosen.
    const VoxelVitals found = voxelVitals();
    const int deathsFound = g.deaths;
    int causesFound[DAMAGE_COUNT];
    for (int i = 0; i < DAMAGE_COUNT; i++) causesFound[i] = g.deathsByCause[i];
    const int lastCauseFound = g.lastCause;
    const bool edgeFound = g.deathEdge;
    const bool hurtEdgeFound = g.hurtEdge;
    const float hurtAmountFound = g.hurtAmount;
    const int hurtCauseFound = g.hurtCause;
    const float peakFound = g.airPeakY;
    const bool airborneFound = g.wasAirborne;
    const bool groundedFound = g.wasGrounded;
    const float spawnFoundX = g.spawnX, spawnFoundY = g.spawnY, spawnFoundZ = g.spawnZ;
    const int modeFound = g.mode;

    // Every rate below is a survival rate, so the probe starts by taking the rules it measures. A run
    // launched with VOXEL_MODE=creative still has to be able to prove what creative does.
    setVoxelMode(VOXEL_MODE_SURVIVAL);

    const float y = 70.0f;

    // 1. The idle burn, in the units the header names.
    setVoxelVitals(VoxelVitals{});
    spend(standing(y), 100.0f, 1.0f);
    const float idleFood = VITAL_MAX - hungerValue();
    const float idleWater = VITAL_MAX - thirstValue();
    probe.check("standing burns food and water at the rate the header names",
                std::fabs(idleFood - HUNGER_IDLE_PER_SECOND * 100.0f) < 1e-3f &&
                    std::fabs(idleWater - THIRST_IDLE_PER_SECOND * 100.0f) < 1e-3f &&
                    healthValue() == VITAL_MAX,
                "%.4f food and %.4f water in 100 s, health %.1f", idleFood, idleWater,
                healthValue());

    // 2. Walking and running are multiples of that same floor, not separate clocks.
    VoxelBody walk = standing(y);
    walk.moved = 0.05f;
    setVoxelVitals(VoxelVitals{});
    spend(walk, 100.0f, 1.0f);
    const float walkFood = VITAL_MAX - hungerValue();
    VoxelBody run = walk;
    run.sprinting = true;
    setVoxelVitals(VoxelVitals{});
    spend(run, 100.0f, 1.0f);
    const float runFood = VITAL_MAX - hungerValue();
    probe.check("walking pays the idle rate times its multiplier and running pays more",
                std::fabs(walkFood - idleFood * ACTIVITY_WALK) < 1e-3f &&
                    std::fabs(runFood - idleFood * ACTIVITY_SPRINT) < 1e-3f,
                "idle %.3f, walk %.3f (%.2fx), run %.3f (%.2fx)", idleFood, walkFood,
                idleFood > 0.0f ? walkFood / idleFood : 0.0f, runFood,
                idleFood > 0.0f ? runFood / idleFood : 0.0f);

    // 3. A jump is an edge: it charges once, on the frame it leaves the ground.
    setVoxelVitals(VoxelVitals{});
    VoxelBody hop = standing(y);
    hop.jumped = true;
    updateVoxelLife(hop, 1.0f);
    const float jumped = VITAL_MAX - hungerValue();
    probe.check("one jump costs the food the header names, on top of the second it rode",
                std::fabs(jumped - (HUNGER_IDLE_PER_SECOND + JUMP_HUNGER_COST)) < 1e-3f,
                "%.4f food for one jumped second", jumped);
    setVoxelVitals(VoxelVitals{});
    VoxelBody swim = hop;
    swim.inWater = true;
    updateVoxelLife(swim, 1.0f);
    probe.check("swimming up is the same key and not the same price",
                std::fabs((VITAL_MAX - hungerValue()) - HUNGER_IDLE_PER_SECOND) < 1e-3f,
                "%.4f food for one jumped second in water", VITAL_MAX - hungerValue());

    // 4. Drops: one airborne frame sets the peak, the frame that lands closes it. Each sequence starts
    // by standing the body on the ground at y, because a peak left over from the live player - or
    // from the check above - would be charged to a fall that never happened.
    setVoxelVitals(VoxelVitals{});
    updateVoxelLife(standing(y), 0.1f);
    VoxelBody air = standing(y);
    air.grounded = false;
    air.feetY = y + 3.0f;
    updateVoxelLife(air, 0.1f);            // exactly the free drop
    VoxelBody land = standing(y);
    updateVoxelLife(land, 0.1f);           // and the frame that ends it
    probe.check("three blocks is exactly the free drop", healthValue() == VITAL_MAX,
                "health %.2f after a 3.0 block fall", healthValue());

    setVoxelVitals(VoxelVitals{});
    updateVoxelLife(standing(y), 0.1f);
    air.feetY = y + 10.0f;
    updateVoxelLife(air, 0.1f);
    land.feetY = y;
    updateVoxelLife(land, 0.1f);
    probe.check("ten blocks take off one unit per block over the safe three",
                std::fabs(healthValue() - (VITAL_MAX - (10.0f - FALL_SAFE_BLOCKS) *
                                           FALL_DAMAGE_PER_BLOCK)) < 1e-3f,
                "health %.3f, expected %.3f", healthValue(),
                VITAL_MAX - (10.0f - FALL_SAFE_BLOCKS) * FALL_DAMAGE_PER_BLOCK);

    setVoxelVitals(VoxelVitals{});
    updateVoxelLife(standing(y), 0.1f);
    air.feetY = y + 30.0f;
    updateVoxelLife(air, 0.1f);
    VoxelBody intoWater = standing(y);
    intoWater.inWater = true;
    updateVoxelLife(intoWater, 0.1f);
    probe.check("water takes the fall out of a thirty block drop", healthValue() == VITAL_MAX,
                "health %.2f after 30 blocks into water", healthValue());

    // A body carried upward by the world - or put back on the surface after dying - has a drop of the
    // other sign, and the peak has to follow it or the lift becomes damage on the next landing.
    setVoxelVitals(VoxelVitals{});
    updateVoxelLife(standing(y), 0.1f);
    air.feetY = y + 30.0f;
    updateVoxelLife(air, 0.1f);
    VoxelBody atTop = standing(y + 30.0f);
    updateVoxelLife(atTop, 0.1f);
    probe.check("being lifted thirty blocks owes no fall when the feet stop",
                healthValue() == VITAL_MAX, "health %.2f after a lift and a stand", healthValue());

    setVoxelVitals(VoxelVitals{});
    updateVoxelLife(standing(y), 0.1f);
    VoxelBody fly = standing(y + 40.0f);
    fly.flying = true;
    fly.grounded = false;
    updateVoxelLife(fly, 0.1f);
    fly.feetY = y;
    updateVoxelLife(fly, 0.1f);
    updateVoxelLife(standing(y), 0.1f);
    probe.check("a landing out of flight is not a fall", healthValue() == VITAL_MAX,
                "health %.2f after landing from 40 blocks of flight", healthValue());

    // 5. Air, and then drowning in it.
    setVoxelVitals(VoxelVitals{});
    VoxelBody under = standing(y);
    under.headUnder = true;
    spend(under, 9.0f, 0.5f);
    probe.check("nine seconds under water leaves one second of air and no wound",
                breathValue() > 0.9f && breathValue() < 1.1f && healthValue() == VITAL_MAX,
                "air %.2f, health %.2f", breathValue(), healthValue());
    spend(under, 4.0f, 0.5f);
    probe.check("once the air is gone the health goes at the drown rate",
                breathValue() == 0.0f &&
                    std::fabs(healthValue() - (VITAL_MAX - 3.0f * DROWN_DAMAGE_PER_SECOND)) < 1e-3f,
                "air %.2f, health %.2f after 3 s past empty", breathValue(), healthValue());
    VoxelBody surfaced = standing(y);
    updateVoxelLife(surfaced, 1.0f);
    probe.check("one second at the surface brings back four times what it cost",
                std::fabs(breathValue() - BREATH_REFILL_PER_SECOND) < 1e-3f,
                "air %.2f after 1 s up", breathValue());

    // 6. Contact harm.
    setVoxelVitals(VoxelVitals{});
    VoxelBody lava = standing(y);
    lava.inLava = true;
    spend(lava, 2.0f, 0.5f);
    probe.check("lava burns at six units a second",
                std::fabs(healthValue() - (VITAL_MAX - 2.0f * LAVA_DAMAGE_PER_SECOND)) < 1e-3f,
                "health %.2f after 2 s in it", healthValue());
    setVoxelVitals(VoxelVitals{});
    VoxelBody prick = standing(y);
    prick.touchingHazard = true;
    spend(prick, 3.0f, 0.5f);
    probe.check("a cactus pricks at one unit a second",
                std::fabs(healthValue() - (VITAL_MAX - 3.0f * HAZARD_DAMAGE_PER_SECOND)) < 1e-3f,
                "health %.2f after 3 s against one", healthValue());

    // 7. The two bars that kill, and which of them kills faster.
    setVoxelVitals(VoxelVitals{VITAL_MAX, 0.0f, VITAL_MAX, BREATH_MAX, true});
    spend(standing(y), 10.0f, 1.0f);
    const float starved = VITAL_MAX - healthValue();
    setVoxelVitals(VoxelVitals{VITAL_MAX, VITAL_MAX, 0.0f, BREATH_MAX, true});
    spend(standing(y), 10.0f, 1.0f);
    const float parched = VITAL_MAX - healthValue();
    probe.check("an empty bar hurts, and thirst hurts faster than hunger",
                std::fabs(starved - 10.0f * STARVE_DAMAGE_PER_SECOND) < 1e-3f &&
                    std::fabs(parched - 10.0f * THIRST_DAMAGE_PER_SECOND) < 1e-3f &&
                    parched > starved,
                "starve %.2f and thirst %.2f over 10 s", starved, parched);

    // 8. Healing, and what it costs.
    setVoxelVitals(VoxelVitals{10.0f, VITAL_MAX, VITAL_MAX, BREATH_MAX, true});
    const float foodBefore = hungerValue();
    spend(standing(y), 10.0f, 1.0f);
    probe.check("a nearly full food bar buys health at the price the header names",
                std::fabs(healthValue() - (10.0f + 10.0f * REGEN_HEALTH_PER_SECOND)) < 1e-3f &&
                    std::fabs((foodBefore - hungerValue()) -
                              10.0f * (REGEN_HUNGER_PER_SECOND + HUNGER_IDLE_PER_SECOND)) < 1e-3f,
                "health %.2f, %.2f food spent for it", healthValue(), foodBefore - hungerValue());
    setVoxelVitals(VoxelVitals{10.0f, VITAL_MAX, VITAL_MAX, BREATH_MAX, true});
    spend(lava, 1.0f, 1.0f);
    probe.check("nothing heals while it hurts", healthValue() < 10.0f,
                "health %.2f after a second in lava with a full bar", healthValue());
    setVoxelVitals(VoxelVitals{10.0f, 12.0f, VITAL_MAX, BREATH_MAX, true});
    spend(standing(y), 5.0f, 1.0f);
    probe.check("a bar below the gate feeds nothing", healthValue() < 10.1f,
                "health %.2f after 5 s at %.1f food", healthValue(), hungerValue());

    // 9. Dying: one event, named by the worst cause of the frame that ended it.
    const int deathsBefore = g.deaths;
    setVoxelVitals(VoxelVitals{1.0f, VITAL_MAX, VITAL_MAX, BREATH_MAX, true});
    VoxelBody both = standing(y);
    both.touchingHazard = true;
    both.inLava = true;
    updateVoxelLife(both, 1.0f);
    int cause = -1;
    const bool edge = voxelJustDied(cause);
    int ignoredCause = -1;
    const bool again = voxelJustDied(ignoredCause);
    probe.check("the death is reported once and names the cause that did the most harm",
                edge && cause == DAMAGE_LAVA && !again && g.deaths == deathsBefore + 1 &&
                    voxelDeathsByCause(DAMAGE_LAVA) >= 1,
                "edge %d cause %s, read again %d, deaths %d -> %d", edge ? 1 : 0,
                voxelDamageCauseName(cause), again ? 1 : 0, deathsBefore, g.deaths);

    // Snapshot before the call: comparing a count read after the damage against the same count read
    // after the damage proves nothing, and a death counter that silently climbed would still pass it.
    const int deathsBeforeDeadDamage = g.deaths;
    const bool tookMore = damageVoxelLife(10.0f, DAMAGE_DIRECT);
    probe.check("a dead body takes no more damage and no more deaths",
                healthValue() == 0.0f && !tookMore && g.deaths == deathsBeforeDeadDamage,
                "health %.2f, damage accepted %d, deaths %d -> %d", healthValue(), tookMore ? 1 : 0,
                deathsBeforeDeadDamage, g.deaths);

    const float foodAtDeath = hungerValue();
    VoxelBody drift = standing(y);
    drift.moved = 1.0f;
    spend(drift, 100.0f, 1.0f);
    probe.check("the dead do not decay: a hundred seconds of walking changes nothing",
                hungerValue() == foodAtDeath && healthValue() == 0.0f,
                "food %.2f -> %.2f, health %.2f", foodAtDeath, hungerValue(), healthValue());

    // 10. Standing up. The drop that killed the body must not be charged again on the way to the
    // spawn, and the bars come back spent rather than free.
    setVoxelVitals(VoxelVitals{1.0f, VITAL_MAX, VITAL_MAX, BREATH_MAX, true});
    air.grounded = false;
    air.feetY = y + 25.0f;
    updateVoxelLife(air, 0.1f);
    const int deathsBeforeRevive = g.deaths;
    reviveVoxelLife(y);
    // Then let the body actually stand a frame where the spawn put it. The drop that killed it happened
    // at a peak this column does not share, so a module that forgot to re-anchor pays for the same fall
    // twice and the second death arrives before the player can move. One idle frame spends under a
    // hundredth of each bar, which the tolerance below allows.
    updateVoxelLife(standing(y), 0.1f);
    int causeAfterRevive = -1;
    probe.check("reviving fills health, leaves the bars partly spent and forgets the drop",
                voxelAlive() && healthValue() == VITAL_MAX &&
                    std::fabs(hungerValue() - RESPAWN_HUNGER) < 0.01f &&
                    std::fabs(thirstValue() - RESPAWN_THIRST) < 0.01f &&
                    breathValue() == BREATH_MAX && !voxelJustDied(causeAfterRevive) &&
                    g.deaths == deathsBeforeRevive,
                "health %.3f food %.3f water %.3f air %.3f, deaths %d", healthValue(),
                hungerValue(), thirstValue(), breathValue(), g.deaths);

    // 11. Food: the blocks this module feeds on have to be blocks a survival player can actually pick
    // up, and a block id with a state nibble on it has to still be the same food.
    const float melon = voxelFoodNutrition(BLOCK_MELON);
    probe.check("what the module calls food is a breakable block, and stone is not food",
                melon > 0.0f && voxelFoodNutrition(BLOCK_PUMPKIN) > 0.0f &&
                    block(BLOCK_MELON).name != nullptr && block(BLOCK_MELON).hardness < 255 &&
                    voxelFoodNutrition(BLOCK_STONE) == 0.0f &&
                    voxelFoodNutrition(stateType(makeState(BLOCK_MELON, 3))) == melon,
                "melon %.1f (%s, hardness %d), pumpkin %.1f, stone %.1f", melon,
                block(BLOCK_MELON).name ? block(BLOCK_MELON).name : "?",
                (int)block(BLOCK_MELON).hardness, voxelFoodNutrition(BLOCK_PUMPKIN),
                voxelFoodNutrition(BLOCK_STONE));

    setVoxelVitals(VoxelVitals{10.0f, 10.0f, 10.0f, BREATH_MAX, true});
    const bool firstBite = eatVoxelLife(melon);
    const bool drank = drinkVoxelLife(6.0f);
    const bool secondBite = eatVoxelLife(melon);
    const bool thirdBite = eatVoxelLife(melon);
    probe.check("eating fills the bar it feeds and a full bar refuses the food",
                firstBite && drank && secondBite && !thirdBite && hungerValue() == VITAL_MAX &&
                    thirstValue() == 16.0f,
                "bites %d/%d/%d, food %.1f, water %.1f", firstBite ? 1 : 0, secondBite ? 1 : 0,
                thirdBite ? 1 : 0, hungerValue(), thirstValue());

    // 12. The overlay line is the only visible sign this module has, so it has to say what the state
    // actually is rather than what the format string was written when the code was shorter.
    setVoxelVitals(VoxelVitals{17.0f, 20.0f, 12.0f, 9.0f, true});
    const std::string line = lifeStats();
    size_t cursor = 0;
    const float shownHealth = nextNumber(line, cursor);
    const float shownFood = nextNumber(line, cursor);
    const float shownWater = nextNumber(line, cursor);
    const float shownAir = nextNumber(line, cursor);
    probe.check("the overlay's life line reports the bars it is holding",
                shownHealth == 17.0f && shownFood == 20.0f && shownWater == 12.0f &&
                    std::fabs(shownAir - 9.0f) < 1e-3f, "%s", line.c_str());

    // 13. The step size must not matter: a probe that hands one second at a time and a run that hands
    // a fiftieth of a second both add up to the same ten seconds.
    setVoxelVitals(VoxelVitals{});
    spend(standing(y), 10.0f, 1.0f);
    const float byWhole = VITAL_MAX - hungerValue();
    setVoxelVitals(VoxelVitals{});
    spend(standing(y), 10.0f, 0.02f);
    const float byFrames = VITAL_MAX - hungerValue();
    probe.check("ten seconds in one step or in five hundred frames are the same ten seconds",
                std::fabs(byWhole - byFrames) < 1e-3f, "%.4f vs %.4f food", byWhole, byFrames);

    // 14. The host sets the spawn from the world's own surface, so the place a respawn aims at has to
    // be a column this build has a block to stand on - a respawn into an unstreamed column is how a
    // dead player would stay dead forever.
    const int cx = world.focusChunkX() * CHUNK_SX + 4;
    const int cz = world.focusChunkZ() * CHUNK_SZ + 4;
    const int top = world.getTop(cx, cz);
    if (top >= 0) {
        setVoxelSpawn((float)cx + 0.5f, (float)(top + 1), (float)cz + 0.5f);
        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        voxelSpawn(sx, sy, sz);
        probe.check("the spawn a host sets from the world has a surface under it",
                    (int)std::floor(sx) == cx && (int)std::floor(sz) == cz &&
                        sy == (float)(top + 1) && stateType(world.get(cx, top, cz)) != BLOCK_AIR,
                    "spawn %.1f %.1f %.1f, column top %d holds %s", sx, sy, sz, top,
                    block(stateType(world.get(cx, top, cz))).name);
    } else {
        probe.check("the spawn a host sets from the world has a surface under it", false,
                    "the focus column %d,%d has not streamed", cx, cz);
    }

    // 15. The modes. The rule the other three sections rest on is that these rates are survival
    // rates, so the checks here are that outside survival nothing moves, nothing hurts, and the bars
    // are kept rather than reset - the player who comes back is the same player who left.
    setVoxelVitals(VoxelVitals{10.0f, 10.0f, 10.0f, 5.0f, true});
    setVoxelMode(VOXEL_MODE_CREATIVE);
    VoxelBody punishment = standing(y);
    punishment.moved = 1.0f;
    punishment.sprinting = true;
    punishment.jumped = true;
    punishment.grounded = false;
    punishment.feetY = y + 40.0f;
    punishment.headUnder = true;
    punishment.inLava = true;
    punishment.touchingHazard = true;
    spend(punishment, 100.0f, 0.5f);
    probe.check("creative spends a hundred seconds of running, drowning and lava and nothing moves",
                healthValue() == 10.0f && hungerValue() == 10.0f && thirstValue() == 10.0f &&
                    breathValue() == 5.0f && voxelAlive() && voxelMode() == VOXEL_MODE_CREATIVE,
                "health %.2f food %.2f water %.2f air %.2f", healthValue(), hungerValue(),
                thirstValue(), breathValue());

    const int deathsBeforeCreativeDamage = g.deaths;
    const bool creativeTook = damageVoxelLife(1000.0f, DAMAGE_LAVA);
    int creativeDeathCause = -1;
    const bool creativeDied = voxelJustDied(creativeDeathCause);
    probe.check("creative refuses damage, so a mob that bites here does nothing",
                !creativeTook && healthValue() == 10.0f && voxelAlive() && !creativeDied &&
                    g.deaths == deathsBeforeCreativeDamage,
                "damage accepted %d, health %.2f, died %d, deaths %d -> %d", creativeTook ? 1 : 0,
                healthValue(), creativeDied ? 1 : 0, deathsBeforeCreativeDamage, g.deaths);

    setVoxelMode(VOXEL_MODE_SPECTATOR);
    spend(punishment, 100.0f, 0.5f);
    const bool spectatorTook = damageVoxelLife(1000.0f, DAMAGE_VOID);
    probe.check("a spectator burns nothing and refuses the same thousand units of damage",
                healthValue() == 10.0f && hungerValue() == 10.0f && thirstValue() == 10.0f &&
                    breathValue() == 5.0f && !spectatorTook && voxelAlive(),
                "health %.2f food %.2f air %.2f, damage accepted %d", healthValue(), hungerValue(),
                breathValue(), spectatorTook ? 1 : 0);

    // Back to survival: the same numbers, and the very next second spends from them. Water at 10 is
    // above the healing gate and food at 10 is under it, so health is the one bar that stays put.
    setVoxelMode(VOXEL_MODE_SURVIVAL);
    const bool heldAcrossTheSwitch = healthValue() == 10.0f && hungerValue() == 10.0f &&
        thirstValue() == 10.0f && breathValue() == 5.0f;
    updateVoxelLife(standing(y), 1.0f);
    probe.check("a switch back to survival resumes from the bars the other mode kept",
                heldAcrossTheSwitch &&
                    std::fabs(hungerValue() - (10.0f - HUNGER_IDLE_PER_SECOND)) < 1e-4f &&
                    std::fabs(thirstValue() - (10.0f - THIRST_IDLE_PER_SECOND)) < 1e-4f &&
                    std::fabs(breathValue() - (5.0f + BREATH_REFILL_PER_SECOND)) < 1e-4f &&
                    healthValue() == 10.0f,
                "held %d, food %.3f water %.3f air %.3f health %.2f", heldAcrossTheSwitch ? 1 : 0,
                hungerValue(), thirstValue(), breathValue(), healthValue());

    // A drop that was in progress when the switch took over is the one thing that must not survive
    // it: the height was reached under rules that could not charge for it.
    setVoxelVitals(VoxelVitals{});
    updateVoxelLife(standing(y), 0.1f);
    air.feetY = y + 30.0f;
    air.grounded = false;
    updateVoxelLife(air, 0.1f);            // survival: thirty blocks of fall in progress
    setVoxelMode(VOXEL_MODE_CREATIVE);     // and the player decides to stop paying for it
    VoxelBody creativeLand = standing(y);
    updateVoxelLife(creativeLand, 0.1f);
    setVoxelMode(VOXEL_MODE_SURVIVAL);
    updateVoxelLife(standing(y), 0.1f);
    probe.check("the fall in progress when creative took over is not charged on the way back",
                healthValue() == VITAL_MAX, "health %.2f after the switch and a landing",
                healthValue());

    // ... and the same mode set again, which is what a host does every frame, must not quietly cancel
    // a fall the player is still in.
    setVoxelMode(VOXEL_MODE_SURVIVAL);
    updateVoxelLife(standing(y), 0.1f);
    air.feetY = y + 20.0f;
    updateVoxelLife(air, 0.1f);
    setVoxelMode(VOXEL_MODE_SURVIVAL);
    setVoxelMode(VOXEL_MODE_SURVIVAL);
    updateVoxelLife(standing(y), 0.1f);
    probe.check("a host that sets the same mode every frame still charges the fall it is in",
                std::fabs(healthValue() - (VITAL_MAX - (20.0f - FALL_SAFE_BLOCKS) *
                                           FALL_DAMAGE_PER_BLOCK)) < 1e-3f,
                "health %.2f, expected %.2f", healthValue(),
                VITAL_MAX - (20.0f - FALL_SAFE_BLOCKS) * FALL_DAMAGE_PER_BLOCK);

    // A body that died and is switched out of survival is stood up: a mode that cannot hurt has no
    // reason to hold a corpse, and the death counters keep the death that already happened.
    setVoxelVitals(VoxelVitals{1.0f, 4.0f, 4.0f, 0.0f, true});
    const int deathsBeforeTheKill = g.deaths;
    const bool killedIt = damageVoxelLife(1.0f, DAMAGE_DIRECT);
    int killCause = -1;
    const bool edgeFromKill = voxelJustDied(killCause);
    setVoxelMode(VOXEL_MODE_CREATIVE);
    const bool stoodUp = voxelAlive() && healthValue() == VITAL_MAX && !voxelJustDied(killCause);
    spend(punishment, 10.0f, 1.0f);
    probe.check("switching out of survival stands a dead body up without inventing a second death",
                killedIt && edgeFromKill && killCause == DAMAGE_DIRECT &&
                    g.deaths == deathsBeforeTheKill + 1 && stoodUp &&
                    hungerValue() == 4.0f && thirstValue() == 4.0f && breathValue() == 0.0f &&
                    voxelDeaths() == deathsBeforeTheKill + 1,
                "alive %d health %.1f food %.1f water %.1f air %.1f, deaths %d -> %d",
                stoodUp ? 1 : 0, healthValue(), hungerValue(), thirstValue(), breathValue(),
                deathsBeforeTheKill, g.deaths);

    setVoxelMode(VOXEL_MODE_SURVIVAL);
    setVoxelMode(99);
    const int clampedHigh = voxelMode();
    setVoxelMode(-3);
    const int clampedLow = voxelMode();
    setVoxelMode(VOXEL_MODE_SURVIVAL);
    probe.check("a mode number this build does not know becomes survival",
                clampedHigh == VOXEL_MODE_SURVIVAL && clampedLow == VOXEL_MODE_SURVIVAL,
                "%d -> %d, %d -> %d", 99, clampedHigh, -3, clampedLow);

    // The overlay is the only place a player can see which rules they are under, so the line has to
    // say it - and the name has to stay out of the probe's way when it walks the numbers.
    setVoxelVitals(VoxelVitals{17.0f, 20.0f, 12.0f, 9.0f, true});
    setVoxelMode(VOXEL_MODE_CREATIVE);
    const std::string creativeLine = lifeStats();
    size_t creativeCursor = 0;
    const float creativeShownHealth = nextNumber(creativeLine, creativeCursor);
    const float creativeShownFood = nextNumber(creativeLine, creativeCursor);
    setVoxelMode(VOXEL_MODE_SPECTATOR);
    const std::string spectatorLine = lifeStats();
    setVoxelMode(VOXEL_MODE_SURVIVAL);
    const std::string survivalLine = lifeStats();
    probe.check("the overlay names the mode whose rules the bars are under",
                creativeLine.find("creative") != std::string::npos &&
                    creativeShownHealth == 17.0f && creativeShownFood == 20.0f &&
                    spectatorLine.find("spectator") != std::string::npos &&
                    survivalLine.find("survival") != std::string::npos,
                "%s | %s", creativeLine.c_str(), spectatorLine.c_str());

    // 16. The hit edge. A mob strike reaches this module through damageVoxelLife from the host, and
    // the only thing the screen can react to is the hit itself - so every harm has to arm one edge,
    // name its cause and be taken exactly once. A flash that stayed lit until someone asked twice
    // would pulse at whatever rate the overlay happens to run at.
    setVoxelMode(VOXEL_MODE_SURVIVAL);
    setVoxelVitals(VoxelVitals{});
    const bool bitten = damageVoxelLife(3.0f, DAMAGE_ATTACK);
    int hurtCause = -1;
    float hurtAmount = 0.0f;
    const bool firstFlash = voxelJustHurt(hurtCause, hurtAmount);
    const bool secondFlash = voxelJustHurt(hurtCause, hurtAmount);
    probe.check("a strike through the module's door arms one flash and names what dealt it",
                bitten && firstFlash && !secondFlash && hurtCause == DAMAGE_ATTACK &&
                    std::fabs(hurtAmount - 3.0f) < 1e-3f &&
                    std::fabs(healthValue() - (VITAL_MAX - 3.0f)) < 1e-3f,
                "flash %d taken twice %d cause %s amount %.1f health %.1f", firstFlash ? 1 : 0,
                secondFlash ? 1 : 0, voxelDamageCauseName(hurtCause), hurtAmount, healthValue());

    // The fall is the module's own harm, not a caller's, and the player still has to see it.
    setVoxelVitals(VoxelVitals{});
    VoxelBody aloft = standing(80.0f);
    aloft.grounded = false;
    updateVoxelLife(aloft, 0.1f);
    int droppedCause = -1;
    float droppedAmount = 0.0f;
    updateVoxelLife(standing(70.0f), 0.1f);
    const bool landedFlash = voxelJustHurt(droppedCause, droppedAmount);
    probe.check("the landing of a drop arms the same flash with the fall as its cause",
                landedFlash && droppedCause == DAMAGE_FALL &&
                    std::fabs(droppedAmount - (10.0f - FALL_SAFE_BLOCKS) * FALL_DAMAGE_PER_BLOCK) <
                        1e-3f,
                "flash %d cause %s amount %.2f", landedFlash ? 1 : 0,
                voxelDamageCauseName(droppedCause), droppedAmount);

    // Creative refuses the harm, and a refusal must not leave a flash behind: the screen would pulse
    // on a hit the body never took. The bars are emptied first because the landing above paid for its
    // drop, and an unreset health would read as creative having damaged the player.
    setVoxelVitals(VoxelVitals{});
    setVoxelMode(VOXEL_MODE_CREATIVE);
    int pinnedCause = -1;
    float pinnedAmount = 0.0f;
    const bool creativeBit = damageVoxelLife(3.0f, DAMAGE_ATTACK);
    const bool creativeFlash = voxelJustHurt(pinnedCause, pinnedAmount);
    setVoxelMode(VOXEL_MODE_SURVIVAL);
    probe.check("a mode that takes no damage arms no flash",
                !creativeBit && !creativeFlash && healthValue() == VITAL_MAX,
                "damage %d flash %d health %.1f", creativeBit ? 1 : 0, creativeFlash ? 1 : 0,
                healthValue());

    // The cause a strike ends the body with has to be the strike, so the death line sends whoever
    // reads it at the mob rather than at the nearest cactus.
    setVoxelVitals(VoxelVitals{2.0f, VITAL_MAX, VITAL_MAX, BREATH_MAX, true});
    const int attackDeathsBefore = g.deathsByCause[DAMAGE_ATTACK];
    damageVoxelLife(3.0f, DAMAGE_ATTACK);
    int bittenDeath = -1;
    const bool diedBitten = voxelJustDied(bittenDeath);
    probe.check("a body that ends under a strike dies of the strike",
                diedBitten && bittenDeath == DAMAGE_ATTACK &&
                    g.deathsByCause[DAMAGE_ATTACK] == attackDeathsBefore + 1,
                "cause %s, attack deaths %d -> %d", voxelDamageCauseName(bittenDeath),
                attackDeathsBefore, g.deathsByCause[DAMAGE_ATTACK]);

    // The edge is taken by whoever flashes; the line keeps the answer for the log, which is the only
    // way a headless run sees the bite at all once the flash has been consumed.
    setVoxelVitals(VoxelVitals{});
    damageVoxelLife(4.0f, DAMAGE_ATTACK);
    int spentCause = -1;
    float spentAmount = 0.0f;
    voxelJustHurt(spentCause, spentAmount);
    const std::string hitLine = lifeStats();
    probe.check("the life line still names the last hit after the flash was taken",
                hitLine.find("last hit") != std::string::npos &&
                    hitLine.find("attack") != std::string::npos,
                "%s", hitLine.c_str());
    reviveVoxelLife(70.0f);

    // Everything back, counters included: a probe that leaves the player dead, or leaves its own
    // deaths in the log, makes the run's other numbers unreadable.
    setVoxelMode(modeFound);
    for (int i = 0; i < DAMAGE_COUNT; i++) g.deathsByCause[i] = causesFound[i];
    g.lastCause = lastCauseFound;
    g.deathEdge = edgeFound;
    g.airPeakY = peakFound;
    g.wasAirborne = airborneFound;
    g.wasGrounded = groundedFound;
    g.spawnX = spawnFoundX; g.spawnY = spawnFoundY; g.spawnZ = spawnFoundZ;
    g.deaths = deathsFound;
    setVoxelVitals(found);
    // Restored after the bars, because writing the bars is how a caller says "this is the state" and
    // that drops any flash in flight.
    g.hurtEdge = hurtEdgeFound;
    g.hurtAmount = hurtAmountFound;
    g.hurtCause = hurtCauseFound;
    const VoxelVitals left = voxelVitals();
    probe.check("the probe leaves the vitals, the mode and the death count as it found them",
                left.health == found.health && left.hunger == found.hunger &&
                    left.thirst == found.thirst && left.breath == found.breath &&
                    left.alive == found.alive && g.deaths == deathsFound &&
                    voxelMode() == modeFound &&
                    g.deathsByCause[DAMAGE_LAVA] == causesFound[DAMAGE_LAVA],
                "health %.2f/%.2f food %.2f/%.2f deaths %d/%d mode %d/%d", left.health, found.health,
                left.hunger, found.hunger, g.deaths, deathsFound, voxelMode(), modeFound);
    probe.check("the probe leaves the hit edge and the last hit as it found them",
                g.hurtEdge == hurtEdgeFound && g.hurtAmount == hurtAmountFound &&
                    g.hurtCause == hurtCauseFound,
                "edge %d/%d amount %.2f/%.2f", g.hurtEdge ? 1 : 0, hurtEdgeFound ? 1 : 0,
                g.hurtAmount, hurtAmountFound);
}

}  // namespace voxel
