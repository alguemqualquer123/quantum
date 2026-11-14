// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include "ScriptBase.h"
#include "ScriptProperty.h"
#include "math/Vector2.h"

#include "VoxelCollide.h"
#include "VoxelConfig.h"

// The avatar is this script's one optional dependency, and the include is in the header rather than
// the source because the rig is a member: a build of the project without VoxelModel.h gets a
// controller that moves the same body and draws nothing, which is the same rule every other voxel
// module follows.
#if defined(__has_include)
#  if __has_include("VoxelModel.h")
#    include "VoxelModel.h"
#    define VOXEL_HAVE_MODEL 1
#  endif
#endif

#include <cstdint>
#include <string>

namespace voxel {
class VoxelWorld;
struct Probe;

// The world the player lives in, handed over by the integrator. One active world per process is
// enough for a single-player game, and it keeps the script's entity free of owning a 100 KB map.
void setPlayerWorld(voxel::VoxelWorld* world);

// Set by the integrator when the liquid module is compiled in, so placing water or lava runs the
// real source/spread code instead of dropping a dead block. Left null the player falls back to
// writing a level-15 source. Arguments: world, voxel, true = lava, source level 0..15.
extern void (*liquidSourcePlacer)(voxel::VoxelWorld&, int, int, int, bool, int);

// The edit this controller makes, reported rather than drawn: the player script never builds an
// effect, it says what happened and the host decides whether that means chips or a sound. Null when
// the module is not compiled in, which is the same optional-module rule the rest of the voxel code
// follows. Arguments: voxel coordinates and the block type that went away.
extern void (*blockBrokenFn)(int, int, int, uint16_t);

// The same report for a block arriving, with the voxel it was written into and the type it holds.
// Placing is the mirror image of mining and the host has to know about both: a break sparks debris,
// a place only makes the noise that says the world changed.
extern void (*blockPlacedFn)(int, int, int, uint16_t);

// The one edit this controller cannot do by itself: a mined block has to become a thing lying in the
// world rather than a number added to a hand. Setting it asks the host for that, so a broken block
// falls, turns and waits to be picked up. Left null - the build without the drop module - the player
// keeps the old direct hand-off and the game still plays, which is the same optional-module rule
// every other voxel file follows. Arguments: world, the centre of the cube in world units, type, count.
extern void (*itemDropSpawner)(voxel::VoxelWorld&, float, float, float, uint16_t, int);
}

namespace quantum {

// First-person voxel controller: mouse look, WASD against the body box, mining and placing.
// The voxel grid is the collider - moveBoxVoxel() resolves the body against the block shapes, so
// walls slide, one-block steps are climbed and a slab stops you at half height, with no physics
// engine component anywhere in sight.
//
// Everything the overlay needs is published as a plain public field below; this script draws
// nothing, it only reports.
class VoxelPlayerScript : public quantum::ScriptBase {
    DPROPERTY("Move Speed")
    float moveSpeed = 4.37f;

    DPROPERTY("Sprint Multiplier")
    float sprintMultiplier = 1.3f;

    DPROPERTY("Jump Power")
    float jumpPower = 8.0f;

    DPROPERTY("Gravity")
    float gravity = 28.0f;

    DPROPERTY("Reach")
    float reach = 5.0f;

    DPROPERTY("Flying")
    bool flying = false;

    // Which rule set the player is under: "survival", "creative" or "spectator". A name rather than a
    // bool because there are three of them and the scene row is read by a person; anything the parser
    // does not recognise (including an empty row) is survival, so a typo costs a game rather than
    // handing out a free pass. VOXEL_MODE in the environment outranks this row for the whole process -
    // see gameMode().
    DPROPERTY("Game Mode")
    std::string gameModeName = "survival";

    // The integrator reads Game Mode/World Seed when it builds the VoxelWorld; the script itself only
    // needs the mode, which turns mining instant, the hotbar infinite and flight allowed.
    DPROPERTY("World Seed")
    uint64_t seed = 1337;

    // Which column the body starts on: the nearest coast, or the one the scene was saved with. The
    // search walks the pure height field, so the answer exists on the first frame before a single
    // chunk has streamed, and the direction it returns is the way the water lies. Off is a scene row
    // for the person who wants the inland start; VOXEL_SPAWN=authored is the same switch for a run
    // with no inspector to change it in.
    DPROPERTY("Spawn By Water")
    bool spawnByWater = true;

public:
    // A stack is 64 and the hand is nine of them, because the drop module merges ground piles to the
    // same cap: two numbers for one rule is how an item ends up with nowhere to go.
    enum { HOTBAR_SLOTS = 9, HOTBAR_MAX_STACK = 64 };

    struct Slot {
        uint16_t type;   // a BlockType id; 0 = empty hand
        int count;       // ignored while creative, where a stack never runs out
    };

    // ---- read by the HUD/overlay, written only here ----
    int targetX = 0, targetY = 0, targetZ = 0;
    bool targetValid = false;
    bool valid = false;                                  // targetValid under the name the overlay reads
    std::string targetName;                              // BlockDef::name, "" when nothing is targeted
    voxel::VoxelBox targetBox = { 0, 0, 0, 0, 0, 0 };    // the targeted voxel's full cube, for an outline
    float targetProgress = 0.0f;                         // 0..1 through the current mining operation

    Slot hotbar[HOTBAR_SLOTS] = { { 0, 0 } };
    int selectedSlot = 0;

    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;   // the head: where a ray starts and what water drowns
    float footX = 0.0f, footY = 0.0f, footZ = 0.0f;  // the body's bottom, i.e. where a step is struck
    uint16_t footBlock = 0;                          // the block the feet stand on, for its sound
    bool sneaking = false;
    bool sprinting = false;
    bool inWater = false;
    bool eyeInWater = false;
    bool grounded = false;
    // [V]: the camera behind the head instead of in it, which is the only way this body is ever seen
    // by the person wearing it. The eye stays the eye - mining, drowning and the fall are all measured
    // from the head, and only the transform moves.
    bool thirdPerson = false;
    // Edge, not a state: true only on the frame the jump impulse was applied. The life module prices
    // a jump once, and a caller that read `grounded == false` instead would charge the body for every
    // airborne frame of the arc - and for swimming up to the surface, which is the same key.
    bool jumped = false;

    int heldTool = 1;              // 0 hand, 1 pickaxe, 2 axe, 3 shovel, 4 shears
    int heldToolLevel = 1;         // 1 wood, 2 stone, 3 iron, 4 diamond
    std::string heldToolName = "wooden pickaxe";

    // Which rule set is in force, resolved when asked rather than mirrored into a bool: the name is
    // what the scene row, the editor and VOXEL_MODE write, and a second field holding the same answer
    // is a second place to be wrong. The environment outranks the row for the whole process, because a
    // headless run has no inspector to change a scene with.
    int gameMode() const;
    bool isCreative() const { return gameMode() == voxel::VOXEL_MODE_CREATIVE; }
    bool isSpectator() const { return gameMode() == voxel::VOXEL_MODE_SPECTATOR; }

    // True when VOXEL_MODE owns the answer for this process, so [M] can only refuse. The overlay
    // advertises keys, and a key the environment overrules is a key that does not work.
    bool modeIsPinned() const;

    // Whether the body is actually off the ground under its own power - the mode's answer, not the
    // toggle the player left switched on in another mode. The vitals module prices a fall from here,
    // and a flight flag that survived a trip through creative would otherwise cancel a drop that the
    // controller is still measuring in full.
    bool isFlying() const { return isAirborne(); }

    VoxelPlayerScript(quantum::Scene* scene, quantum::Entity entity);
    ~VoxelPlayerScript();

    void onUpdate();

    // The game host owns the creative loadout and hands it over once, to a controller that spawned
    // with an empty hand. Slots the player already filled are left alone.
    void givePalette(const uint16_t* ids, int count);

    // Closes the hand around `count` of `type`: top up a stack that already holds it, else an empty
    // slot, capped at HOTBAR_MAX_STACK per slot. Public because the drop module's collector asks the
    // hand to close and only takes the item out of the world when the answer is yes - which is what
    // makes "the inventory is full" a thing you can see lying on the floor rather than a silent loss.
    // Creative always answers yes: its counts are ignored, so a world in that mode is never littered.
    bool addToHotbar(uint16_t type, int count);

    // The other side of the same door: one of the held stack goes back out into the world as a
    // spinning cube. [J]. Returns false when the hand holds nothing or the module is not compiled in.
    bool dropHeldItem(voxel::VoxelWorld& world);

    // Puts the body on the surface of the column at x,z, zeroes the velocity and writes the camera
    // back. The life module can name a spawn but cannot move a body - only the controller owns the
    // feet, the velocity and the eye - so the host calls this when the body dies. Returns false when
    // that column has not streamed, leaving the player where they were rather than dropping them into
    // ungenerated stone; the caller then asks again on a later frame.
    bool respawnAt(voxel::VoxelWorld& world, float x, float z);

    // Headless proof of what this controller claims: the DDA reports the block the eye is on, mining
    // empties it, placing writes it back one voxel out along the entered face, a block that would
    // intersect the body is refused, and the body is stopped (or climbed) by the block's own shape
    // rather than its cube. The mode rows are part of the claim: the names resolve to the rule sets
    // they name, a name this build does not know is survival, and the rules that follow from a mode -
    // instant mining, no flight in survival, a spectator that changes nothing - are measured rather
    // than assumed. Edits the world and puts it back. Called by the host on VOXEL_PROBE, which hands
    // one Probe to every module so the run ends in a single total.
    void selfTest(voxel::VoxelWorld& world, voxel::Probe& probe);

private:
    voxel::VoxelBox bodyBox(float atFeetY) const;

#ifdef VOXEL_HAVE_MODEL
    // The body the world sees: six posed boxes hung on the feet this controller moves. Built once per
    // scene, posed every frame, and only drawn in third person - a first-person body would spend the
    // whole game filling the camera with the inside of its own skull.
    void updateAvatar();

    voxel::VoxelModelRig avatarRig;
    quantum::Scene* avatarScene = nullptr;
    float avatarPhase = 0.0f;
    float avatarLastX = 0.0f, avatarLastZ = 0.0f;
    bool avatarLastValid = false;
#endif

    // What the mode allows, as opposed to what the player toggled: see gameMode().
    bool isAirborne() const;
    // Whether this frame is allowed to change the world at all - a spectator only looks.
    bool canEdit() const;
    // M: the next mode in the list, written back into the scene row the mode is read from.
    void cycleGameMode();
    void setEyeFromBody();
    void applyLook();
    void updateMining(voxel::VoxelWorld& world, float dt, const voxel::VoxelHit& hit, bool mining);
    void tryPlace(voxel::VoxelWorld& world, const voxel::VoxelHit& hit);
    void pickTarget(const voxel::VoxelHit& hit);
    void cycleHotbar(int dir);
    void cycleTool(bool level);
    float miningSeconds(const voxel::BlockDef& def) const;
    void grabMouse(bool grab);
    // Whether the desktop is sending input to this window. VOXEL_FOCUS pins the answer so the
    // policy below can be tested by a run that cannot arrange for another window to be foreground.
    bool windowFocused() const;
    // Gives the pointer back to the desktop while the window is in the background, and takes it
    // back when the player returns to a look the blur had interrupted.
    void applyFocusPolicy();
    void refreshToolName();
    void spawnOnSurface(voxel::VoxelWorld& world);
    // Moves the feet onto the nearest coast and turns the head to face the water. False when the seed
    // has no coast inside the search radius, which leaves the authored column alone.
    bool applyShoreSpawn(voxel::VoxelWorld& world);

    quantum::Entity camEntity = NULL_ENTITY;
    quantum::Entity bodyEntity = NULL_ENTITY;

    float feetX = 0.0f, feetY = 0.0f, feetZ = 0.0f;
    float velX = 0.0f, velY = 0.0f, velZ = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
    // The column the run began in, kept apart from the feet because the feet move: a body standing at
    // the waterline is pushed by the current, and a check that reads "where the run starts" off a mid
    // -run frame then answers a different coast every launch.
    float spawnX = 0.0f, spawnZ = 0.0f;
    float spawnYaw = 0.0f;
    bool spawnRecorded = false;
    quantum::Vector2 lastMouse;
    bool mouseGrabbed = false;
    bool everGrabbed = false;
    // Set only when a blur released a grab the player had not released themselves.
    bool resumeLookOnFocus = false;
    int focusPin = -1;
    int lookTick = 0;

    bool spawned = false;
    float clock = 0.0f;
    float lastScroll = 0.0f;
    float doubleTapClock = -10.0f;
    bool prevSpace = false;
    bool prevF = false;
    bool prevEscape = false;
    bool prevRight = false;
    bool prevMiddle = false;
    bool prevQ = false;
    bool prevR = false;
    bool prevJ = false;
    bool prevM = false;
#ifdef VOXEL_HAVE_MODEL
    bool prevV = false;
#endif

    int miningX = -1, miningY = -1, miningZ = -1;
    float miningElapsed = 0.0f;
};

}  // namespace quantum
