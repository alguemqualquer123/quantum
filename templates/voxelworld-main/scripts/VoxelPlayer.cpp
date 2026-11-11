// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelPlayer.h"

#include "VoxelCollide.h"
#include "VoxelGen.h"
#include "VoxelWorld.h"
#include "VoxelBlocks.h"
#include "VoxelProbe.h"

#include "Engine.h"
#include "Input.h"
#include "Log.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "math/Quaternion.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace voxel {

// The handover points between the integrator and the script. Internal linkage for the world
// pointer: one world, one player, and nothing outside this file needs it.
static VoxelWorld* s_world = nullptr;

void setPlayerWorld(VoxelWorld* world) { s_world = world; }

// Null until the water module is wired in; the integrator assigns it.
void (*liquidSourcePlacer)(VoxelWorld&, int, int, int, bool, int) = nullptr;

// The break report. Installed by the host, which owns the effects; a build with no FX module
// leaves it null and the controller keeps editing the world exactly as quietly as before. The place
// report is its mirror and has one more reason to exist: a block written into the grid is the only
// way the host learns a sound should play where nothing was before.
void (*blockBrokenFn)(int, int, int, uint16_t) = nullptr;
void (*blockPlacedFn)(int, int, int, uint16_t) = nullptr;

// The mined block's route out of the world and into the hand. Installed by the host when the drop
// module is compiled in; null otherwise, and a break then writes the count straight into the hotbar
// the way it always did.
void (*itemDropSpawner)(VoxelWorld&, float, float, float, uint16_t, int) = nullptr;

}  // namespace voxel

namespace quantum {

using namespace voxel;

// 0.6 x 1.8 x 0.6 body with the eye 1.62 above the feet: the numbers the block shapes are
// authored against, so a 1-block doorway fits and a 1-block gap does not.
static const float BODY_HALF = 0.3f;
static const float BODY_HEIGHT = 1.8f;
static const float EYE_OFFSET = 1.62f;
static const float SNEAK_EYE_OFFSET = 1.42f;
static const float FLY_EYE_OFFSET = 1.70f;

#ifdef VOXEL_HAVE_MODEL
// How far behind the head the camera sits when the body is being looked at instead of looked from.
// Less than two blocks and the head fills the frame; more than four and the world streams out of
// view behind the near clip.
static const float THIRD_PERSON_BACK = 3.2f;
// Blocks per full stride cycle, the same number the mob pool uses: a step is one half-cycle, so this
// is roughly the length of a leg swing and it is what keeps the feet from sliding over the ground.
static const float PLAYER_STRIDE = 1.6f;
// A path, never in-memory pixels - see the note in VoxelEntity.cpp about TextureData owning its
// buffer. Whoever wants their own skin drops a 64x64 PNG here (see the skin module, roadmap row 23).
static const char* const PLAYER_SKIN_PATH = "textures/skins/default.png";
#endif

static const float TERMINAL_VELOCITY = 56.0f;   // blocks/s
static const float GROUND_ACCEL = 14.0f;
static const float AIR_ACCEL = 3.0f;
static const float WATER_GRAVITY = 7.0f;
static const float WATER_SINK = 3.0f;
static const float WATER_SWIM_UP = 4.2f;
static const float LAVA_GRAVITY = 4.0f;
static const float LAVA_SINK = 1.1f;
static const float MOUSE_SENSITIVITY = 0.16f;   // degrees per pixel
static const float PITCH_LIMIT = 89.0f;
static const float DOUBLE_TAP_SECONDS = 0.35f;
static const float MAX_FRAME = 0.1f;            // a 1s hitch must not teleport a body through a wall
static const float FIT_EPS = 0.001f;            // "it fits exactly" tolerance for placement
// Twice the flight speed. A spectator is shown around a world rather than commuting through it, and at
// the flying rate a tour of the streaming radius takes longer than the interest of anyone watching a
// capture.
static const float SPECTATOR_SPEED_MULT = 4.4f;

static const char* const TOOL_NAMES[5] = { "", "pickaxe", "axe", "shovel", "shears" };
static const char* const TIER_NAMES[6] = { "", "wooden", "stone", "iron", "diamond", "netherite" };

// How far the shore search looks for water before giving up and using the authored column. The
// continents of this generator are ~870 blocks across and 28% of the world is under water, so a
// coast is normally inside a few rings; 320 is the point where "no water anywhere near here" is a
// real answer about the seed rather than a search that stopped too early. Cost matters: the ring walk
// reads the height field, not the world, so this runs before anything has streamed.
static const int SHORE_SEARCH_RADIUS = 320;

static inline float clampedF(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// VOXEL_MINE=<frames> holds the left button for that many updates with no mouse in sight. The crack
// and the chips are a picture claim, and the only picture this repo is allowed to take is a headless
// capture, so the capture harness has to be able to mine. Same knob class as VOXEL_NETEDIT.
// VOXEL_MINE_DELAY=<frames> waits that long before arming, so the captured frame can sit in the
// middle of one break instead of after twenty. VOXEL_MINE_SLOW=<n> advances the mining clock at
// 1/n: a dirt block dies in four updates, which is no stage to photograph, and ten stages need to
// be on screen long enough for the shutter to land inside one.
static int s_mineHarnessFrames = -1;
static int s_mineHarnessDelay = 0;
static int s_mineHarnessSlow = 1;
static int s_mineHarnessTick = 0;
static bool s_mineHarnessArmed = false;

static int envInt(const char* name, int fallback) {
#ifdef _MSC_VER
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) return fallback;
    const int v = (int)std::strtol(buffer, nullptr, 10);
    free(buffer);
    return v;
#else
    const char* v = std::getenv(name);
    return v ? (int)std::strtol(v, nullptr, 10) : fallback;
#endif
}

// VOXEL_MODE=<survival|creative|spectator> owns the rule set for every controller in the process. The
// knob exists because the modes change what a run can even show: a capture of a world from the inside
// wants flight, and a probe of the survival rules wants them on. An unset or empty value returns -1
// and leaves the answer to the scene row; a name this build does not know becomes survival, which is
// the strictest of the three and so the safe reading of a typo.
static int envMode(const char* name) {
#ifdef _MSC_VER
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) return -1;
    const int mode = (*buffer == '\0') ? -1 : voxel::voxelGameModeFromName(buffer);
    free(buffer);
    return mode;
#else
    const char* v = std::getenv(name);
    if (!v || *v == '\0') return -1;
    return voxel::voxelGameModeFromName(v);
#endif
}

// VOXEL_SPAWN=authored puts the body back on the column the scene was saved with. The default is the
// coast, because the saved camera sits inland and an inland start has never once had a drop of the
// quarter of this world under water in frame. Only that exact word turns it off, so a typo costs a
// view of the sea rather than silently losing it.
static bool envSpawnIsAuthored() {
#ifdef _MSC_VER
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, "VOXEL_SPAWN") != 0 || buffer == nullptr) return false;
    const bool authored = std::strcmp(buffer, "authored") == 0;
    free(buffer);
    return authored;
#else
    const char* v = std::getenv("VOXEL_SPAWN");
    return v != nullptr && std::strcmp(v, "authored") == 0;
#endif
}

// VOXEL_LOOKAT="x y z" pins the crosshair on one voxel and presses no button. Which shape an outline
// draws is only evidence when a frame shows that shape, and a headless run has no pointer to aim
// with: this is how a capture gets pointed at the poppy the seed grew beside the coast instead of at
// the sand under the player's own feet.
static int s_lookPin[3];
static bool envLookPin() {
    static const bool pinned = [] {
#ifdef _MSC_VER
        char* buffer = nullptr;
        size_t length = 0;
        if (_dupenv_s(&buffer, &length, "VOXEL_LOOKAT") != 0 || buffer == nullptr) return false;
        const int read = std::sscanf(buffer, "%d %d %d", s_lookPin, s_lookPin + 1, s_lookPin + 2);
        free(buffer);
        return read == 3;
#else
        const char* v = std::getenv("VOXEL_LOOKAT");
        return v != nullptr && std::sscanf(v, "%d %d %d", s_lookPin, s_lookPin + 1, s_lookPin + 2) == 3;
#endif
    }();
    return pinned;
}

// VOXEL_PLANT=<count> puts that many poppies on the ground beside the body and prints each one. A
// capture has to show the shape an outline draws, and the shipped seed's nearest flower is twenty
// columns off - further than the five blocks a crosshair reaches - so the alternative to planting is
// a screenshot of sand. It is idempotent: a column that already holds the flower is no longer air,
// so the next frame moves on. VOXEL_LOOKAT then aims the frame at a printed coordinate.
static void placeHarnessPlants(VoxelWorld& world, float x, float z) {
    static int remaining = envInt("VOXEL_PLANT", 0);
    if (remaining <= 0) return;
    // Outward ring in block steps, so the flowers land where the body can see them and not inside it.
    static const int ring[12][2] = { {2, 0}, {-2, 0}, {0, 2}, {0, -2}, {2, 2}, {2, -2},
                                     {-2, 2}, {-2, -2}, {3, 1}, {-3, 1}, {1, -3}, {-1, -3} };
    const int bx = (int)std::floor(x), bz = (int)std::floor(z);
    for (int i = 0; i < 12 && remaining > 0; i++) {
        const int wx = bx + ring[i][0], wz = bz + ring[i][1];
        const int top = world.getTop(wx, wz);
        if (top < 0 || top + 1 >= WORLD_HEIGHT) continue;
        if (stateType(world.get(wx, top + 1, wz)) != BLOCK_AIR) continue;
        world.setBlock(wx, top + 1, wz, makeState(BLOCK_POPPY, 0));
        Log::print("VoxelPlant: poppy on %d,%d,%d", wx, top + 1, wz);
        remaining--;
    }
}

VoxelPlayerScript::VoxelPlayerScript(quantum::Scene* scene, quantum::Entity entity)
    : ScriptBase(scene, entity) {
    REGISTER_ENGINE_EVENT(onUpdate);
    focusPin = envInt("VOXEL_FOCUS", -1);
}

VoxelPlayerScript::~VoxelPlayerScript() {
    UNREGISTER_ENGINE_EVENT(onUpdate);
    // Never leave the user's cursor stolen by a script that has stopped existing.
    if (mouseGrabbed) Engine::setMouseMode(MouseMode::NORMAL);
}

void VoxelPlayerScript::grabMouse(bool grab) {
    if (grab == mouseGrabbed) return;
    mouseGrabbed = grab;
    Engine::setMouseMode(grab ? MouseMode::CAPTURED : MouseMode::NORMAL);
    if (grab) {
        everGrabbed = true;
        const Vector2 center((float)(Engine::getCanvasWidth() / 2),
                             (float)(Engine::getCanvasHeight() / 2));
        Engine::setMousePosition(center.x, center.y);
        // The point the pointer is pinned to is the point the next delta is read against, so the
        // grab that makes the pin has to move the baseline with it.
        lastMouse = center;
    }
}

bool VoxelPlayerScript::windowFocused() const {
    if (focusPin >= 0) return focusPin != 0;
    return Engine::hasFocus();
}

// The cursor is one per desktop, not one per window. A grabbed look re-centres it every frame, so a
// window that went to the background would keep dragging the pointer out from under whatever the
// player clicked on. Releasing here hands it back and lets the arrow show again; the re-take on the
// way back is the same step the Linux backend takes on FocusIn, so returning to the game resumes
// turning instead of leaving the player to hunt for the key that released it. A release the player
// pressed is never resurrected - that one means "leave my cursor alone" - and the launch takes the
// pointer only from a window that is already in the foreground.
void VoxelPlayerScript::applyFocusPolicy() {
    if (!windowFocused()) {
        if (mouseGrabbed) {
            grabMouse(false);
            resumeLookOnFocus = true;
        }
        return;
    }
    if (resumeLookOnFocus) {
        resumeLookOnFocus = false;
        grabMouse(true);
        return;
    }
    if (!everGrabbed) grabMouse(true);
}

voxel::VoxelBox VoxelPlayerScript::bodyBox(float atFeetY) const {
    VoxelBox b;
    b.minX = feetX - BODY_HALF; b.maxX = feetX + BODY_HALF;
    b.minY = atFeetY; b.maxY = atFeetY + BODY_HEIGHT;
    b.minZ = feetZ - BODY_HALF; b.maxZ = feetZ + BODY_HALF;
    return b;
}

void VoxelPlayerScript::spawnOnSurface(VoxelWorld& world) {
    const int sx = (int)std::floor(feetX);
    const int sz = (int)std::floor(feetZ);
    const int top = world.getTop(sx, sz);
    if (top < 0) return;                     // column not generated yet: stay put and ask again
    feetY = (float)(top + 1);
    if (feetY < 1.0f) feetY = 1.0f;
    velX = 0.0f; velY = 0.0f; velZ = 0.0f;
    spawned = true;
}

bool VoxelPlayerScript::applyShoreSpawn(VoxelWorld& world) {
    int sx = 0, sz = 0;
    float tx = 0.0f, tz = 0.0f;
    // From the origin, not from the feet. The camera column the body was read out of is wherever the
    // editor left it, and a saved world left it in a different place on each launch, so a start taken
    // from it answers a different coast every run - two captures of one build became two different
    // pictures and the A/B measured the walk instead of the change. From (0,0) the answer is the
    // seed's alone: same world, same beach, every time.
    if (!findShoreColumn(world.seed(), 0, 0, SHORE_SEARCH_RADIUS, sx, sz, tx, tz)) {
        // A miss says the origin's whole radius is dry or open ocean, and the authored column is then
        // no better - the run stands where the scene said and the start probe reports it as such.
        return false;
    }
    // The middle of the column, so the body box never straddles two voxels at the start.
    feetX = (float)sx + 0.5f;
    feetZ = (float)sz + 0.5f;
    // A YXZ rotation turns the camera by yaw from -Z towards -X, so the angle that looks along
    // (tx, tz) is atan2(-dx, -dz). Facing the water is the whole point: a coast behind the head is a
    // coast nobody sees, and the capture harness has no mouse to turn around with.
    yaw = std::atan2(-tx, -tz) * (180.0f / 3.14159265358979323846f);
    // Printed because every claim about the start is a claim about this column, and a capture log that
    // does not carry it cannot tell a moved body from a search that never ran.
    Log::print("VoxelShore: seed %llu coast %d,%d, aim (%.2f,%.2f), yaw %.1f",
               (unsigned long long)world.seed(), sx, sz, (double)tx, (double)tz, (double)yaw);
    return true;
}

bool VoxelPlayerScript::respawnAt(VoxelWorld& world, float x, float z) {
    const int sx = (int)std::floor(x);
    const int sz = (int)std::floor(z);
    const int top = world.getTop(sx, sz);
    // Asked before writing: a body moved into a column that has not streamed would drag the
    // streaming focus there too, and the world would start generating 500 blocks from the player.
    if (top < 0) return false;
    feetX = x; feetZ = z;
    feetY = (float)(top + 1);
    if (feetY < 1.0f) feetY = 1.0f;
    velX = 0.0f; velY = 0.0f; velZ = 0.0f;
    // Standing on the block the respawn just found: without this the first frame after getting up is
    // airborne, and the drop that killed the player is measured again from the new feet.
    grounded = true;
    jumped = false;
    flying = false;
    sneaking = false;
    spawned = true;
    // The published report moves with the body: the host reads footX/footY/footZ after this call to
    // know where the respawn actually put the player, and an update has not run in between.
    footX = feetX; footY = feetY; footZ = feetZ;
    footBlock = stateType(world.get((int)std::floor(feetX), (int)std::floor(feetY - 0.05f),
                                   (int)std::floor(feetZ)));
    inWater = eyeInWater = false;
    setEyeFromBody();
    return true;
}

void VoxelPlayerScript::applyLook() {
    const Vector2 mp = Input::getMousePosition();
    float dx = 0.0f, dy = 0.0f;
    if (mouseGrabbed) {
        dx = mp.x - lastMouse.x;
        dy = mp.y - lastMouse.y;
        yaw -= dx * MOUSE_SENSITIVITY;
        pitch -= dy * MOUSE_SENSITIVITY;
        pitch = clampedF(pitch, -PITCH_LIMIT, PITCH_LIMIT);
        if (yaw > 360.0f) yaw -= 360.0f;
        if (yaw < -360.0f) yaw += 360.0f;
        const Vector2 center((float)(Engine::getCanvasWidth() / 2),
                             (float)(Engine::getCanvasHeight() / 2));
        // Re-centring is what makes a full spin possible: the deltas come from the pointer's
        // travel, and without a warp it hits the window edge and the yaw stalls mid-turn.
        Engine::setMousePosition(center.x, center.y);
        // And the baseline travels with the warp. The engine reports an absolute position that the
        // line above overwrites, so reading the next frame against this frame's own reading
        // subtracts the warp from the travel as well: two frames of equal motion cancel, and a
        // steady sweep - which is exactly what turning sideways is - never moved the view at all.
        lastMouse = center;
    } else {
        // Released, the pointer is free and its position is its own: holding the centre here would
        // charge the cursor's trip back to the window to the aim on the frame it is grabbed again.
        lastMouse = mp;
    }
    // The pin runs after the pointer so it wins over whatever the cursor did this frame, and it uses
    // the same convention the coast search aims with: a YXZ rotation turns the camera from -Z towards
    // -X, so the angle that looks along (dx, dz) is atan2(-dx, -dz) and the pitch is the arcsine of
    // the vertical component.
    if (envLookPin()) {
        const Vector3 to((float)s_lookPin[0] + 0.5f - feetX,
                         (float)s_lookPin[1] + 0.5f - (feetY + EYE_OFFSET),
                         (float)s_lookPin[2] + 0.5f - feetZ);
        const float len = to.length();
        if (len > 0.05f) {
            const float rad2deg = 180.0f / 3.14159265358979323846f;
            pitch = clampedF(std::asin(to.y / len) * rad2deg, -PITCH_LIMIT, PITCH_LIMIT);
            yaw = std::atan2(-to.x, -to.z) * rad2deg;
        }
    }
    // VOXEL_LOOK=<frame period> prints what the pointer layer actually reports. "The view will not
    // turn sideways" is a claim that spans the platform code and this script, and only the numbers
    // say on which side of that boundary the motion is being lost.
    static const int lookPeriod = envInt("VOXEL_LOOK", 0);
    if (lookPeriod > 0 && (lookTick++ % lookPeriod) == 0) {
        Log::print("VoxelLook: grab %d mode %d mp %.1f,%.1f base %.1f,%.1f d %.1f,%.1f yaw %.1f pitch %.1f",
                   mouseGrabbed ? 1 : 0, (int)Engine::getMouseMode(),
                   (double)mp.x, (double)mp.y, (double)(mp.x - dx), (double)(mp.y - dy),
                   (double)dx, (double)dy, (double)yaw, (double)pitch);
    }
}

int VoxelPlayerScript::gameMode() const {
    // Read once: an environment variable cannot change under a running game, and this is called from
    // the movement code, the edit code, the overlay and the host in the same frame.
    static const int env = envMode("VOXEL_MODE");
    if (env >= 0) return env;
    return voxel::voxelGameModeFromName(gameModeName.c_str());
}

bool VoxelPlayerScript::modeIsPinned() const {
    return envMode("VOXEL_MODE") >= 0;
}

bool VoxelPlayerScript::isAirborne() const {
    // A spectator always is; creative is whenever the toggle says so; survival never is - the flag is
    // ignored rather than cleared, so a player who comes back from creative keeps the toggle they left
    // on without getting the air.
    const int mode = gameMode();
    if (mode == VOXEL_MODE_SPECTATOR) return true;
    return mode == VOXEL_MODE_CREATIVE && flying;
}

bool VoxelPlayerScript::canEdit() const {
    // The gate is here rather than only where the buttons are read, for the same reason the vitals
    // module refuses damage outside survival: a caller that forgets to ask - the mining harness, a
    // later phase's entity highlight, the probe below - must not be able to hand a mode with no hands
    // the power to rewrite the world.
    return gameMode() != VOXEL_MODE_SPECTATOR;
}

// survival -> creative -> spectator -> survival. The order is the order the scene row documents, so
// the key walks the same list a person reads in the inspector.
void VoxelPlayerScript::cycleGameMode() {
    if (modeIsPinned()) {
        // The environment owns the answer, so the key cannot do the one thing it promises. Silence
        // here would read as a broken key binding.
        Log::print("VoxelMode: [M] ignored - VOXEL_MODE pins this process to %s",
                   voxelGameModeName(gameMode()));
        return;
    }
    int next = gameMode() + 1;
    if (next >= VOXEL_MODE_COUNT) next = 0;
    gameModeName = voxelGameModeName(next);
    Log::print("VoxelMode: %s", gameModeName.c_str());
}

float VoxelPlayerScript::miningSeconds(const BlockDef& def) const {
    if (def.hardness >= 255) return -1.0f;                 // bedrock, and world.breakBlock agrees
    if (isCreative()) return 0.0f;
    const bool rightTool = (def.tool == 0) || (heldTool == (int)def.tool);
    float speed = rightTool ? (1.0f + 1.5f * (float)heldToolLevel) : 0.35f;
    // The tier gate costs yield rather than time, but a pick that cannot cut the rock is slower too.
    if (def.tool != 0 && heldToolLevel < (int)def.minLevel) speed *= 0.3f;
    float seconds = (float)def.hardness / (20.0f * speed) + 0.05f;
    if (inWater) seconds *= 2.0f;
    if (eyeInWater) seconds *= 1.5f;
    return seconds;
}

void VoxelPlayerScript::refreshToolName() {
    if (heldTool <= 0 || heldTool > 4) { heldToolName = "hand"; return; }
    const int tier = (int)clampedF((float)heldToolLevel, 1.0f, 5.0f);
    std::string name = TIER_NAMES[tier];
    name += " ";
    name += TOOL_NAMES[heldTool];
    heldToolName = name;
}

void VoxelPlayerScript::cycleTool(bool level) {
    if (level) {
        heldToolLevel++;
        if (heldToolLevel > 5) heldToolLevel = 1;
    } else {
        heldTool++;
        if (heldTool > 4) heldTool = 0;
    }
    refreshToolName();
}

void VoxelPlayerScript::cycleHotbar(int dir) {
    int next = selectedSlot + dir;
    if (next < 0) next = HOTBAR_SLOTS - 1;
    if (next >= HOTBAR_SLOTS) next = 0;
    selectedSlot = next;
}

bool VoxelPlayerScript::addToHotbar(uint16_t type, int count) {
    if (type == 0 || count <= 0) return false;
    int left = count;

    // A stack that already holds the block takes what it has room for, then an empty slot takes the
    // rest, then the next empty slot takes what that one could not: one block type never spreads over
    // nine slots while a stack of it sits at 60.
    for (int i = 0; i < HOTBAR_SLOTS && left > 0; i++) {
        if (hotbar[i].type != type || hotbar[i].count <= 0) continue;
        if (hotbar[i].count >= HOTBAR_MAX_STACK) continue;
        const int take = left < HOTBAR_MAX_STACK - hotbar[i].count ? left : HOTBAR_MAX_STACK - hotbar[i].count;
        hotbar[i].count += take;
        left -= take;
    }
    for (int i = 0; i < HOTBAR_SLOTS && left > 0; i++) {
        if (hotbar[i].type != 0 && hotbar[i].count > 0) continue;
        const int take = left < HOTBAR_MAX_STACK ? left : HOTBAR_MAX_STACK;
        hotbar[i].type = type;
        // Creative publishes 64 because the HUD reads the count and an empty-looking stack in the
        // mode that never runs out would be a lie about a thing the player can still do.
        hotbar[i].count = isCreative() ? HOTBAR_MAX_STACK : take;
        left -= take;
    }
    // Creative ignores counts entirely, so the hand always closes. Refusing there would drop the item
    // back on the floor of a world whose whole point is that it does not litter.
    if (isCreative()) return true;
    return left == 0;
}

bool VoxelPlayerScript::dropHeldItem(VoxelWorld& world) {
    if (!itemDropSpawner || isSpectator()) return false;
    Slot& sel = hotbar[selectedSlot];
    if (sel.type == 0 || (!isCreative() && sel.count <= 0)) return false;

    // In front of the feet and a little up. The distance is the drop module's magnet plus room: one
    // block ahead it would land inside the pull and come straight back to the body that just threw it,
    // which makes the key look broken rather than empty. The direction is the flat forward - the same
    // (0,0,-1) the controller walks with, without the pitch, so looking at the floor does not bury the
    // item in it.
    const float dx = -std::sin(yaw), dz = -std::cos(yaw);
    const uint16_t type = sel.type;
    if (!isCreative() && --sel.count <= 0) sel.type = 0;
    itemDropSpawner(world, feetX + dx * 2.2f, feetY + 0.6f, feetZ + dz * 2.2f, type, 1);
    return true;
}

void VoxelPlayerScript::givePalette(const uint16_t* ids, int count) {
    if (!ids || count <= 0) return;
    for (int i = 0; i < count && i < HOTBAR_SLOTS; i++) {
        if (!ids[i] || hotbar[i].type) continue;
        hotbar[i].type = ids[i];
        hotbar[i].count = isCreative() ? 64 : 1;
    }
}

void VoxelPlayerScript::pickTarget(const VoxelHit& hit) {
    if (!hit.hit || !canEdit()) return;
    const uint16_t type = stateType(hit.id);
    if (!isCreative()) {
        // Survival only selects the slot that already holds the block; copying one would mint it.
        for (int i = 0; i < HOTBAR_SLOTS; i++) {
            if (hotbar[i].type == type && hotbar[i].count > 0) { selectedSlot = i; return; }
        }
        return;
    }
    hotbar[selectedSlot].type = type;
    hotbar[selectedSlot].count = 64;
}

void VoxelPlayerScript::updateMining(VoxelWorld& world, float dt, const VoxelHit& hit, bool mining) {
    if (!mining || !hit.hit || !canEdit()) {
        // A spectator that holds the button down still gets no crack: the progress is what draws it,
        // and clearing it here is what keeps the outline honest about a block no one can break.
        miningElapsed = 0.0f;
        targetProgress = 0.0f;
        return;
    }

    const uint16_t type = stateType(hit.id);
    const BlockDef& def = block(type);
    const float needed = miningSeconds(def);
    if (needed < 0.0f) { targetProgress = 0.0f; return; }

    if (hit.x != miningX || hit.y != miningY || hit.z != miningZ) {
        miningX = hit.x; miningY = hit.y; miningZ = hit.z;
        miningElapsed = 0.0f;
    }

    if (isCreative() || needed == 0.0f) {
        const uint16_t previous = world.breakBlock(hit.x, hit.y, hit.z);
        if (blockBrokenFn && stateType(previous) != BLOCK_AIR)
            blockBrokenFn(hit.x, hit.y, hit.z, stateType(previous));
        miningElapsed = 0.0f;
        targetProgress = 0.0f;
        return;
    }

    miningElapsed += dt;
    if (miningElapsed >= needed) {
        const uint16_t previous = world.breakBlock(hit.x, hit.y, hit.z);
        miningElapsed = 0.0f;
        targetProgress = 0.0f;
        miningX = -1; miningY = -1; miningZ = -1;
        const uint16_t broke = stateType(previous);
        if (broke != BLOCK_AIR && broke == type) {
            if (blockBrokenFn) blockBrokenFn(hit.x, hit.y, hit.z, broke);
            const BlockDef& brokeDef = block(broke);
            uint16_t drop = brokeDef.drop == 0xffff ? broke : brokeDef.drop;
            // The tier gate is what makes diamond ore with a stone pick yield nothing at all.
            if (brokeDef.tool != 0 && heldToolLevel < (int)brokeDef.minLevel) drop = 0;
            drop = (uint16_t)(drop & BLOCK_TYPE_MASK);
            if (drop != 0 && itemDropSpawner) {
                // The block becomes a thing in the world rather than a number in a hand: it falls,
                // turns slowly, and comes back only when the body is close enough to ask for it. The
                // centre of the voxel it came from, so a drop that lands in the sea floats up rather
                // than being buried by it.
                itemDropSpawner(world, (float)hit.x + 0.5f, (float)hit.y + 0.5f, (float)hit.z + 0.5f,
                                drop, 1);
            } else if (drop != 0) {
                addToHotbar(drop, 1);   // the build with no drop module: the old straight hand-off
            }
        }
    } else {
        targetProgress = clampedF(miningElapsed / needed, 0.0f, 1.0f);
    }
}

void VoxelPlayerScript::tryPlace(VoxelWorld& world, const VoxelHit& hit) {
    if (!hit.hit || !canEdit()) return;
    const Slot& sel = hotbar[selectedSlot];
    if (sel.type == 0 || (!isCreative() && sel.count <= 0)) return;

    const int px = hit.x + hit.nx;
    const int py = hit.y + hit.ny;
    const int pz = hit.z + hit.nz;
    if (py < 0 || py >= WORLD_HEIGHT) return;

    const uint16_t type = sel.type;
    const BlockDef& def = block(type);
    const bool liquid = (def.flags & FLAG_LIQUID) != 0;

    // A bucket is used while sneaking. Without this branch the only water you can place is a dead
    // source block, and the flow lives behind a function pointer so this file still builds when
    // the liquid module is not compiled in.
    if (liquid && sneaking) {
        if (liquidSourcePlacer) liquidSourcePlacer(world, px, py, pz, type == BLOCK_LAVA, 15);
        else world.placeBlock(px, py, pz, makeState(type, 15));
        if (blockPlacedFn) blockPlacedFn(px, py, pz, type);
        if (!isCreative()) {
            hotbar[selectedSlot].count--;
            if (hotbar[selectedSlot].count <= 0) hotbar[selectedSlot].type = 0;
        }
        return;
    }

    // Refuse to build inside the player. The test uses the block's own collision shape, not its
    // cube: the half of a slab you could stand in is free space, so a slab goes where a full block
    // would not - which is exactly the per-block collision the rest of the game obeys.
    const VoxelBox body = bodyBox(feetY);
    if (def.flags & FLAG_SOLID) {
        VoxelBox shapes[4];
        const int n = blockShapeBoxes(makeState(type, 0), px, py, pz, shapes);
        if (n == 0) {
            shapes[0] = { (float)px, (float)py, (float)pz,
                          (float)(px + 1), (float)(py + 1), (float)(pz + 1) };
        }
        for (int i = 0; i < n; i++) {
            if (shapes[i].minX + FIT_EPS < body.maxX && shapes[i].maxX - FIT_EPS > body.minX &&
                shapes[i].minY + FIT_EPS < body.maxY && shapes[i].maxY - FIT_EPS > body.minY &&
                shapes[i].minZ + FIT_EPS < body.maxZ && shapes[i].maxZ - FIT_EPS > body.minZ) {
                return;
            }
        }
    }

    if (!world.placeBlock(px, py, pz, makeState(type, 0))) return;
    if (blockPlacedFn) blockPlacedFn(px, py, pz, type);
    if (!isCreative()) {
        hotbar[selectedSlot].count--;
        if (hotbar[selectedSlot].count <= 0) hotbar[selectedSlot].type = 0;
    }
}

void VoxelPlayerScript::setEyeFromBody() {
    Scene* sc = getScene();
    if (!sc) return;
    quantum::Transform* tr = sc->findComponent<quantum::Transform>(bodyEntity);
    if (!tr) return;

    // The eye sits higher in the air than on the ground, and a spectator is always in the air - the
    // flag alone is not the answer, because survival ignores it.
    // The mode's own answer to "is this body in the air", and the reason the eye sits higher.
    const bool airborne = isAirborne();
    const float eyeOffset = airborne ? FLY_EYE_OFFSET : (sneaking ? SNEAK_EYE_OFFSET : EYE_OFFSET);
    tr->position = Vector3(feetX, feetY + eyeOffset, feetZ);

    Quaternion rot;
    rot.fromEulerAngles(pitch, yaw, 0.0f, RotationOrder::YXZ);
    tr->rotation = rot;
    tr->needUpdate = true;

    // The head is the reference point for everything the rest of the game asks about the player -
    // what block the eye is on, whether it is underwater, where a fall started - so it is published
    // before the camera is moved away from it, and never after.
    posX = tr->position.x;
    posY = tr->position.y;
    posZ = tr->position.z;

#ifdef VOXEL_HAVE_MODEL
    if (thirdPerson && !isSpectator()) {
        // Behind the head, along the look direction, and stopped by the first block on the way: a
        // camera that ends up inside a wall shows the inside of that wall instead of the player it is
        // following, which is the one thing a third-person view must not do.
        const Vector3 back = rot * Vector3(0.0f, 0.0f, 1.0f);
        float dist = THIRD_PERSON_BACK;
        const VoxelWorld* world = s_world;
        if (world) {
            const VoxelHit wall = raycastVoxel(*world, tr->position.x, tr->position.y, tr->position.z,
                                               back.x, back.y, back.z, dist + 0.35f);
            if (wall.hit && wall.dist - 0.35f < dist) dist = wall.dist > 0.95f ? wall.dist - 0.35f : 0.6f;
        }
        tr->position = tr->position + back * dist;
        tr->needUpdate = true;
    }
#endif

    CameraComponent* cam = sc->findComponent<CameraComponent>(camEntity);
    if (cam) {
        // Rotation-driven, not target-driven: while a target is set the view is a lookAt and the
        // pitch written into the rotation above has no effect at all.
        cam->useTarget = false;
        if (cam->type != CameraType::CAMERA_PERSPECTIVE) {
            // A camera a script made is a UI camera with a 10-unit far clip; a voxel world needs
            // more than ten blocks of it or the horizon is a wall.
            cam->type = CameraType::CAMERA_PERSPECTIVE;
            cam->nearClip = 0.05f;
            cam->farClip = 300.0f;
            cam->yfov = 1.2217f;   // 70 degrees
        }
        cam->needUpdate = true;
    }
}

#ifdef VOXEL_HAVE_MODEL
// Six transforms, written from the numbers the controller already keeps. The body turns with the look
// direction rather than with the strafe, because that is what a third-person view of a first-person
// controller owes: a body that rotated sideways every time the player tapped A is a crab.
void VoxelPlayerScript::updateAvatar() {
    Scene* sc = getScene();
    if (!sc) return;
    if (avatarScene != sc) {
        // A different scene took the old rig down with it; the parts are its entities, not ours.
        destroyVoxelModelRig(sc, avatarRig);
        avatarScene = sc;
        avatarLastValid = false;
        avatarPhase = 0.0f;
    }
    if (!avatarRig.built) {
        createVoxelModelRig(sc, avatarRig, 0, PLAYER_SKIN_PATH, 1.0f, 1.0f, 1.0f);
    }

    const float moved = avatarLastValid ? std::sqrt((feetX - avatarLastX) * (feetX - avatarLastX) +
                                                    (feetZ - avatarLastZ) * (feetZ - avatarLastZ))
                                       : 0.0f;
    avatarLastX = feetX;
    avatarLastZ = feetZ;
    avatarLastValid = true;

    // Only a step on the ground turns the cycle: a jump, a fall and a swim all freeze the legs where
    // they were, so a body lands in the middle of the stride it left in rather than skipping a step.
    const bool walking = grounded && !inWater && !isFlying();
    if (walking) avatarPhase = voxelModelWalkPhase(avatarPhase, moved, PLAYER_STRIDE);

    const float speed = std::sqrt(velX * velX + velZ * velZ);
    VoxelModelPose pose;
    pose.x = feetX;
    pose.y = feetY;
    pose.z = feetZ;
    // The engine's camera looks down -Z and the model's front face is its +Z, so the body wears the
    // look direction with a half turn on it; and a positive head pitch tips the face down, which is
    // the negation of the camera's own pitch. Both are measured, not assumed - see selfTest.
    pose.yaw = yaw + 180.0f;
    pose.headPitch = -pitch;
    pose.walkPhase = avatarPhase;
    pose.swing = walking ? (sneaking ? voxelModelSwing(speed) * 0.35f : voxelModelSwing(speed)) : 0.0f;
    pose.scale = voxelModelScaleForHeight(VOXEL_MODEL_PLAYER_HEIGHT);
    pose.visible = thirdPerson && !isSpectator();
    poseVoxelModelRig(sc, avatarRig, pose);
}
#endif

void VoxelPlayerScript::onUpdate() {
    float dt = Engine::getDeltatime();
    if (!(dt > 0.0f)) return;
    if (dt > MAX_FRAME) dt = MAX_FRAME;
    clock += dt;

    VoxelWorld* world = voxel::s_world;
    Scene* sc = getScene();
    if (!world || !sc) return;

    // Who owns the pointer, decided before this frame reads or moves it: a window in the background
    // has no business warping the cursor the desktop is using.
    applyFocusPolicy();

    // Cleared every frame and set only where the impulse is applied, so a body that left the ground
    // because the block under it was mined reports no jump.
    jumped = false;

    // What to drive: the player entity is the camera when it carries one, otherwise the scene
    // camera is the body. findComponent rather than getComponent because a missing component logs
    // and rethrows, and this build has exceptions off.
    const quantum::Entity self = getEntity();
    if (camEntity == NULL_ENTITY || !sc->isEntityCreated(camEntity)) {
        camEntity = sc->findComponent<CameraComponent>(self) ? self : sc->getCamera();
        if (camEntity == NULL_ENTITY || !sc->isEntityCreated(camEntity)) camEntity = self;
        bodyEntity = camEntity;
        quantum::Transform* start = sc->findComponent<quantum::Transform>(bodyEntity);
        if (start) {
            feetX = start->position.x;
            feetY = start->position.y - EYE_OFFSET;
            feetZ = start->position.z;
        }
        if (spawnByWater && !envSpawnIsAuthored()) {
            // The saved column is wherever the editor left the camera, and this world puts its water
            // in basins, so an inland start is a first frame without a single drop in it. The search
            // reads the height field only, so it answers before the coast has streamed - which is the
            // only reason the streaming focus can start walking that way.
            applyShoreSpawn(*world);
        }
        // Written here, once, because this is the only moment the feet still hold the *start*. After
        // this line the current, gravity and every respawn move the body, and the probes that claim to
        // measure where the run began have nothing else to read.
        spawnX = feetX;
        spawnZ = feetZ;
        spawnYaw = yaw;
        spawnRecorded = true;
        if (!hotbar[0].type) refreshToolName();
#ifdef VOXEL_HAVE_MODEL
        // VOXEL_THIRD=1 starts the run already wearing the body. There is no [V] to press headlessly
        // and the only picture this project is allowed to take is a capture, so the capture harness is
        // the one place the avatar has to be in frame without a keyboard.
        thirdPerson = envInt("VOXEL_THIRD", 0) != 0;
#endif
    }
    quantum::Transform* tr = sc->findComponent<quantum::Transform>(bodyEntity);
    if (!tr) return;

    // ---- whose rules this frame is under ----
    const bool mDown = Input::isKeyPressed(D_KEY_M);
    if (mDown && !prevM) cycleGameMode();
    prevM = mDown;

    const int mode = gameMode();
    const bool spectator = mode == VOXEL_MODE_SPECTATOR;
    // Flight is a creative trick, and survival ignores the flag rather than clearing it: a player who
    // comes back from creative keeps the toggle they set but does not get the air. A spectator needs no
    // toggle - nothing in the world can hold one up or let one fall - and refusing it here is what keeps
    // the flag meaning the one thing it claims to mean, because the space key up there is a thruster.
    const bool canFly = mode == VOXEL_MODE_CREATIVE;
    const bool airborne = isAirborne();

    if (!spawned || (feetY < -16.0f && mode == VOXEL_MODE_SURVIVAL)) {
        // Fell out of the world, or the column under the authored position has not streamed in. Only
        // survival is rescued from the void: it is the one mode that dies there, so a body below the
        // floor has to come back. Creative can fly out and a spectator is down here on purpose - the
        // underside of a world is the reason to load it.
        spawnOnSurface(*world);
        setEyeFromBody();
        return;
    }

    applyLook();

    Quaternion rot;
    rot.fromEulerAngles(pitch, yaw, 0.0f, RotationOrder::YXZ);
    const Vector3 fwd = rot * Vector3(0.0f, 0.0f, -1.0f);
    const float eyeOffset = airborne ? FLY_EYE_OFFSET : (sneaking ? SNEAK_EYE_OFFSET : EYE_OFFSET);
    const Vector3 eye(feetX, feetY + eyeOffset, feetZ);

    // Planted after the body has settled and before the frame reads its target, so the first
    // capture frame already has a flower in reach instead of a world that grew elsewhere.
    placeHarnessPlants(*world, feetX, feetZ);

    // ---- what the eye is on: plants are targetable (they can be picked), water is not ----
    const VoxelHit hit = raycastVoxel(*world, eye.x, eye.y, eye.z,
                                      fwd.x, fwd.y, fwd.z, reach, false, true);
    targetValid = hit.hit;
    valid = hit.hit;
    if (hit.hit) {
        targetX = hit.x; targetY = hit.y; targetZ = hit.z;
        targetBox = { (float)hit.x, (float)hit.y, (float)hit.z,
                      (float)(hit.x + 1), (float)(hit.y + 1), (float)(hit.z + 1) };
        const uint16_t targetType = stateType(hit.id);
        const char* name = block(targetType).name;
        if (targetName != name) targetName = name;
    } else {
        targetX = 0; targetY = 0; targetZ = 0;
        if (!targetName.empty()) targetName.clear();
        targetProgress = 0.0f;
        miningX = -1; miningY = -1; miningZ = -1;
    }

    // ---- where the feet are ----
    VoxelBox body = bodyBox(feetY);
    const int cx = (int)std::floor(feetX);
    const int cz = (int)std::floor(feetZ);
    const uint16_t footType = stateType(world->get(cx, (int)std::floor(feetY + 0.1f), cz));
    const uint16_t midType = stateType(world->get(cx, (int)std::floor(feetY + BODY_HEIGHT * 0.5f), cz));
    const uint16_t eyeType = stateType(world->get(cx, (int)std::floor(eye.y), cz));
    inWater = (blockFlags(footType) & FLAG_LIQUID) != 0 || (blockFlags(midType) & FLAG_LIQUID) != 0;
    eyeInWater = (blockFlags(eyeType) & FLAG_LIQUID) != 0;
    // The step the audio module strikes and the block it sounds like. Reported rather than used:
    // the controller knows which voxel the feet are inside, the host is what turns that into a sound.
    // The surface is sampled just *below* the body's bottom, not at the feet themselves: standing on
    // a block puts the feet in the air voxel above it, which would make every floor sound like air.
    footX = feetX; footY = feetY; footZ = feetZ;
    footBlock = stateType(world->get(cx, (int)std::floor(feetY - 0.05f), cz));
    const bool inLava = footType == BLOCK_LAVA || midType == BLOCK_LAVA;
    // Lava is not "water that hurts": it is thick, so everything slows down and sinks slowly.
    const float drag = inLava ? 0.35f : 1.0f;

    // ---- input intent ----
    const bool keyW = Input::isKeyPressed(D_KEY_W) || Input::isKeyPressed(D_KEY_UP);
    const bool keyS = Input::isKeyPressed(D_KEY_S) || Input::isKeyPressed(D_KEY_DOWN);
    const bool keyA = Input::isKeyPressed(D_KEY_A) || Input::isKeyPressed(D_KEY_LEFT);
    const bool keyD = Input::isKeyPressed(D_KEY_D) || Input::isKeyPressed(D_KEY_RIGHT);
    const bool space = Input::isKeyPressed(D_KEY_SPACE);
    const bool ctrl = Input::isKeyPressed(D_KEY_LEFT_CONTROL) || Input::isKeyPressed(D_KEY_RIGHT_CONTROL);
    const bool shift = Input::isKeyPressed(D_KEY_LEFT_SHIFT) || Input::isKeyPressed(D_KEY_RIGHT_SHIFT);

    sneaking = shift && !airborne;
    sprinting = ctrl && keyW && !sneaking && !airborne;

    // Fly toggle: double-tap jump (the familiar one) or F (the one you can find without reading). Both
    // are refused outright in survival, where the flag does nothing anyway - the answer has to be the
    // same for the two keys, and a toggle that flips a flag the physics ignores is a flag that lies.
    const bool spaceEdge = space && !prevSpace;
    if (spaceEdge) {
        const bool doubled = clock - doubleTapClock < DOUBLE_TAP_SECONDS;
        if (doubled && canFly) {
            flying = !flying;
            velY = 0.0f;
            doubleTapClock = -10.0f;
        } else {
            doubleTapClock = clock;
        }
    }
    prevSpace = space;
    const bool fDown = Input::isKeyPressed(D_KEY_F);
    if (fDown && !prevF && canFly) { flying = !flying; velY = 0.0f; }
    prevF = fDown;

#ifdef VOXEL_HAVE_MODEL
    const bool vDown = Input::isKeyPressed(D_KEY_V);
    if (vDown && !prevV) thirdPerson = !thirdPerson;
    prevV = vDown;
#endif

    Vector3 flatFwd(fwd.x, 0.0f, fwd.z);
    Vector3 flatRight(rot * Vector3(1.0f, 0.0f, 0.0f));
    flatRight.y = 0.0f;
    if (flatFwd.length() > 0.001f) flatFwd.normalize();
    if (flatRight.length() > 0.001f) flatRight.normalize();

    float speed = moveSpeed;
    if (spectator) {
        // A spectator is touring, not walking: this multiplier is on top of nothing, and neither the
        // lava nor the water drag applies, because a body that goes through a block is not in it.
        speed *= SPECTATOR_SPEED_MULT;
    } else {
        if (sprinting) speed *= sprintMultiplier;
        if (sneaking) speed *= 0.32f;
        if (airborne) speed *= 2.2f;
        speed *= drag;
        if (inWater && !airborne) speed *= 0.62f;
    }

    float dx = 0.0f, dy = 0.0f, dz = 0.0f;

    if (spectator) {
        // The look is the steering wheel, normalised in three dimensions rather than as a stride plus
        // a lift: pointing at the floor and holding W goes *into* the floor at the same speed as level
        // flight, which is the whole reason the mode has a pitch. No gravity, no ground, no block.
        float vx = 0.0f, vy = 0.0f, vz = 0.0f;
        if (keyW) { vx += fwd.x; vy += fwd.y; vz += fwd.z; }
        if (keyS) { vx -= fwd.x; vy -= fwd.y; vz -= fwd.z; }
        if (keyD) { vx += flatRight.x; vz += flatRight.z; }
        if (keyA) { vx -= flatRight.x; vz -= flatRight.z; }
        if (space) vy += 1.0f;
        if (ctrl) vy -= 1.0f;
        const float vl = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (vl > 0.001f) { vx /= vl; vy /= vl; vz /= vl; }
        velX = vx * speed;
        velY = vy * speed;
        velZ = vz * speed;
        dx = velX * dt; dy = velY * dt; dz = velZ * dt;
        grounded = false;
        feetX += dx; feetY += dy; feetZ += dz;
    } else {
        float mx = 0.0f, mz = 0.0f;
        if (keyW) { mx += flatFwd.x; mz += flatFwd.z; }
        if (keyS) { mx -= flatFwd.x; mz -= flatFwd.z; }
        if (keyD) { mx += flatRight.x; mz += flatRight.z; }
        if (keyA) { mx -= flatRight.x; mz -= flatRight.z; }
        const float ml = std::sqrt(mx * mx + mz * mz);
        if (ml > 0.001f) { mx /= ml; mz /= ml; }

        const float accel = (grounded || airborne || inWater) ? GROUND_ACCEL : AIR_ACCEL;
        const float blend = clampedF(accel * dt, 0.0f, 1.0f);
        velX += (mx * speed - velX) * blend;
        velZ += (mz * speed - velZ) * blend;

        if (airborne) {
            float up = 0.0f;
            if (space) up += 1.0f;
            if (ctrl) up -= 1.0f;
            velY = up * speed;
        } else if (inWater) {
            if (inLava) {
                velY -= LAVA_GRAVITY * dt;
                velY = clampedF(velY, -LAVA_SINK, 1.2f);
                if (space) velY = 1.2f;
            } else {
                velY -= WATER_GRAVITY * dt;
                if (velY < -WATER_SINK) velY = -WATER_SINK;
                if (space) velY = WATER_SWIM_UP;
            }
            grounded = false;
        } else {
            // Simple grounding, no coyote time: the jump only exists where the ground flag says so.
            if (space && grounded && !sneaking) {
                velY = jumpPower;
                grounded = false;
                jumped = true;
            }
            velY -= gravity * dt;
            if (velY < -TERMINAL_VELOCITY) velY = -TERMINAL_VELOCITY;
        }
        dy = velY * dt;
        dx = velX * dt;
        dz = velZ * dt;

        // Sneaking stops the walk before the walk stops the player: try the horizontal move on its own
        // and throw it away if nothing solid would be left under the feet afterwards.
        if (sneaking && grounded && !airborne && !inWater) {
            VoxelBox probe = body;
            moveBoxVoxel(*world, probe, dx, 0.0f, dz);
            if (!boxSupported(*world, probe)) {
                dx = 0.0f; dz = 0.0f;
                velX = 0.0f; velZ = 0.0f;
            }
        }

        const int flags = moveBoxVoxel(*world, body, dx, dy, dz);
        if (flags & VOXEL_HIT_GROUND) {
            if (dy < 0.0f && velY < 0.0f) velY = 0.0f;
            grounded = true;
        } else if (!airborne && !inWater) {
            grounded = false;
        }
        if ((flags & VOXEL_HIT_CEIL) && velY > 0.0f) velY = 0.0f;
        if (flags & VOXEL_IN_LIQUID) inWater = true;

        feetX = (body.minX + body.maxX) * 0.5f;
        feetY = body.minY;
        feetZ = (body.minZ + body.maxZ) * 0.5f;
    }

    setEyeFromBody();

#ifdef VOXEL_HAVE_MODEL
    // After the eye, because the camera may have just moved away from the head and the body has to
    // be posed where the feet actually ended up, not where they were when the frame started.
    updateAvatar();
#endif

    // ---- buttons: the mouse must be grabbed before it means "hit the world" ----
    bool lmb = Input::isMousePressed(D_MOUSE_BUTTON_LEFT);
    bool rmb = Input::isMousePressed(D_MOUSE_BUTTON_RIGHT);
    bool mmb = Input::isMousePressed(D_MOUSE_BUTTON_MIDDLE);

    const bool escDown = Input::isKeyPressed(D_KEY_ESCAPE);
    if (escDown && !prevEscape) grabMouse(!mouseGrabbed);
    prevEscape = escDown;

    if (!mouseGrabbed) {
        // A released cursor means the click was for the desktop, not for the world: the player is
        // doing something else and the editor has to stay usable. Taking the pointer is the focus
        // policy's decision, made at the top of the frame, not a side effect of reading buttons.
        lmb = false; rmb = false; mmb = false;
    }

    // The mining harness outranks the cursor gate: a headless run has no cursor to grab, and it is
    // the only way to see a mid-break frame at all. Arming also tips the view steeply down, because
    // the block one step ahead of the feet is near enough that a nine-pixel crack is still pixels
    // on screen, and a crack over nothing would prove nothing either way.
    float mineDt = dt;
    if (s_mineHarnessFrames < 0) {
        s_mineHarnessFrames = envInt("VOXEL_MINE", 0);
        s_mineHarnessDelay = envInt("VOXEL_MINE_DELAY", 0);
        s_mineHarnessSlow = envInt("VOXEL_MINE_SLOW", 1);
        if (s_mineHarnessSlow < 1) s_mineHarnessSlow = 1;
    }
    if (s_mineHarnessDelay > 0) {
        s_mineHarnessDelay--;
    } else if (s_mineHarnessFrames > 0) {
        if (!s_mineHarnessArmed) {
            s_mineHarnessArmed = true;
            pitch = -58.0f;
        }
        s_mineHarnessFrames--;
        lmb = true;
        mineDt = dt / (float)s_mineHarnessSlow;
        // Steepen until the ray lands on something, and let it fire *after* the arm. The gate this
        // replaces compared the pitch against -30 while arming had already set -58, so the fallback
        // could never run: on a spawn whose neighbouring columns drop away - the beach the seed
        // anchors to -58 passes over the terrain, answers "no target", and every capture made with
        // this harness shows neither crack nor outline while printing nothing about why.
        if (!hit.hit && pitch > -80.0f) pitch -= 6.0f;
    }

    updateMining(*world, mineDt, hit, lmb);
    // A harness that cannot say what the ray answered is a harness that cannot be debugged: the
    // crack overlay only appears when the controller has a target, so the capture run has to print
    // the target it did or did not find. The screen pair is the same claim in pixels - the overlay
    // is placed on the block the ray hit through the crosshair, so a capture that draws the crack
    // anywhere else has a placement fault, not a texture fault, and this line says which.
    if (s_mineHarnessArmed && (s_mineHarnessTick++ % 30) == 0) {
        int sx = -1, sy = -1;
        CameraComponent* cam = sc ? sc->findComponent<CameraComponent>(camEntity) : nullptr;
        if (cam && hit.hit) {
            const Vector4 clip = cam->viewProjectionMatrix *
                Vector4((float)hit.x + 0.5f, (float)hit.y + 0.5f, (float)hit.z + 0.5f, 1.0f);
            if (clip.w > 0.0f) {
                const Rect view = Engine::getViewRect();
                sx = (int)(((clip.x / clip.w) * 0.5f + 0.5f) * view.getWidth());
                sy = (int)((1.0f - ((clip.y / clip.w) * 0.5f + 0.5f)) * view.getHeight());
            }
        }
        Log::print("VoxelMine: pitch %.0f yaw %.0f hit %d at %d,%d,%d dist %.2f progress %.3f "
                   "slow %d block centre on screen %d,%d",
                   pitch, yaw, hit.hit ? 1 : 0, hit.x, hit.y, hit.z, hit.dist, targetProgress,
                   s_mineHarnessSlow, sx, sy);
    }
    if (rmb && !prevRight) tryPlace(*world, hit);
    if (mmb && !prevMiddle) pickTarget(hit);
    prevRight = rmb;
    prevMiddle = mmb;

    // ---- hotbar (1..9, wheel) and tools (Q type, R tier) ----
    for (int i = 0; i < HOTBAR_SLOTS; i++) {
        if (Input::isKeyPressed(D_KEY_1 + i)) selectedSlot = i;
    }
    const Vector2 scroll = Input::getMouseScroll();
    if (scroll.y != lastScroll) {
        cycleHotbar(scroll.y > 0.0f ? 1 : -1);
        lastScroll = scroll.y;
    }

    const bool qDown = Input::isKeyPressed(D_KEY_Q);
    if (qDown && !prevQ) cycleTool(false);
    prevQ = qDown;
    const bool rDown = Input::isKeyPressed(D_KEY_R);
    if (rDown && !prevR) cycleTool(true);
    prevR = rDown;

    // [J]: one of the held stack goes back out into the world. The letter is "jogar", and it is the
    // one key left that does not already move, mine, place or change the mode.
    const bool jDown = Input::isKeyPressed(D_KEY_J);
    if (jDown && !prevJ && canEdit()) dropHeldItem(*world);
    prevJ = jDown;
}

// ---------------------------------------------------------------------------
// Headless self-test. Every check reads the world back through VoxelWorld::get, so it proves the
// write happened and not just that a function returned true. The body is teleported clear of the
// terrain first: a placement target that is also the voxel the feet occupy is the one case the
// controller refuses, and the probe has to be able to tell that failure apart from that rule.
void VoxelPlayerScript::selfTest(VoxelWorld& world, voxel::Probe& probe) {
    ProbeModule mod(probe, "player");
    char detail[192];

    // ---- the vocabulary, before any of it is pointed at a body ----
    // The scene row, the overlay, the [M] key and VOXEL_MODE all spell the same three answers, so the
    // claim is that the table round-trips: name -> mode -> name. No world and no mode of the player's
    // own is involved, which is why it is made before the streaming guard below.
    bool namesRoundTrip = true;
    for (int i = 0; i < VOXEL_MODE_COUNT; i++) {
        if (voxelGameModeFromName(voxelGameModeName(i)) != i) namesRoundTrip = false;
    }
    probe.check("every mode name names the mode it was read from", namesRoundTrip, "%s / %s / %s",
                voxelGameModeName(VOXEL_MODE_SURVIVAL), voxelGameModeName(VOXEL_MODE_CREATIVE),
                voxelGameModeName(VOXEL_MODE_SPECTATOR));
    probe.check("a mode name this build does not know is survival, the strictest of the three",
                voxelGameModeFromName("adventure") == VOXEL_MODE_SURVIVAL &&
                    voxelGameModeFromName("Creative") == VOXEL_MODE_SURVIVAL &&
                    voxelGameModeFromName("") == VOXEL_MODE_SURVIVAL,
                "adventure %d, wrong case %d, empty %d, survival is %d",
                voxelGameModeFromName("adventure"), voxelGameModeFromName("Creative"),
                voxelGameModeFromName(""), VOXEL_MODE_SURVIVAL);

    const int colX = (int)std::floor(feetX);
    const int colZ = (int)std::floor(feetZ);
    const int ground = world.getTop(colX, colZ);
    if (ground < 0) {
        std::snprintf(detail, sizeof(detail), "column %d,%d has not streamed", colX, colZ);
        probe.skip(detail);
        return;   // before any state is touched, so a skip cannot leave the player in another mode
    }

    // ---- where the run put the body ----
    // An inland start is a first frame with no water in it, and the only proof this project is allowed
    // that the sea is anywhere to be seen is a capture of it. The claims below are read over the same
    // pure height field the start searched, so they measure the ground and not the streaming queue.
    {
        const bool shoreOn = spawnByWater && !envSpawnIsAuthored();
        // The start, not the feet: read from the column recorded on the frame the body was placed, so
        // the answer is the seed's and not the current's. Same origin as the spawn search ran from, so
        // the coast found here is the coast the run was pointed at.
        const int startColX = (int)std::floor(spawnRecorded ? spawnX : feetX);
        const int startColZ = (int)std::floor(spawnRecorded ? spawnZ : feetZ);
        const int hHere = terrainHeight(world.seed(), startColX, startColZ);
        int underHere = 0;
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dz == 0) continue;
                if (terrainHeight(world.seed(), startColX + dx, startColZ + dz) < SEA_LEVEL) underHere++;
            }
        }
        std::snprintf(detail, sizeof(detail), "start %d,%d: ground %d, sea %d, %d of 8 under, by water %d",
                      startColX, startColZ, hHere, SEA_LEVEL, underHere, shoreOn ? 1 : 0);
        probe.check("the run starts on dry ground at the waterline, or where the scene said to",
                    !shoreOn || (hHere >= SEA_LEVEL && hHere <= SEA_LEVEL + 3 && underHere >= 5),
                    "%s", detail);

        // Asked from the origin the spawn searched from, which is the world's own (0,0) and not the
        // camera's saved column. Two calls must agree, and the coast they agree on must be the column
        // the body stands on: that is the whole determinism claim, because a coast that moves between
        // launches makes every capture of it a picture of a different place.
        int sx = 0, sz = 0;
        float tx = 0.0f, tz = 0.0f;
        const bool found = findShoreColumn(world.seed(), 0, 0, SHORE_SEARCH_RADIUS, sx, sz, tx, tz);
        int sx2 = 0, sz2 = 0;
        float tx2 = 0.0f, tz2 = 0.0f;
        const bool again = findShoreColumn(world.seed(), 0, 0, SHORE_SEARCH_RADIUS, sx2, sz2, tx2, tz2);
        const int hThere = found ? terrainHeight(world.seed(), sx, sz) : -1;
        int underThere = 0;
        if (found) {
            for (int dz = -1; dz <= 1; dz++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dz == 0) continue;
                    if (terrainHeight(world.seed(), sx + dx, sz + dz) < SEA_LEVEL) underThere++;
                }
            }
        }
        const float len = std::sqrt(tx * tx + tz * tz);
        const int lookX = sx + (len > 0.0f ? (int)std::lroundf(tx / len * 3.0f) : 0);
        const int lookZ = sz + (len > 0.0f ? (int)std::lroundf(tz / len * 3.0f) : 0);
        const int hLook = found ? terrainHeight(world.seed(), lookX, lookZ) : -1;
        std::snprintf(detail, sizeof(detail), "coast %d,%d ground %d, %d of 8 under, look %d,%d ground %d,"
                      " body stands on %d,%d", sx, sz, hThere, underThere, lookX, lookZ, hLook,
                      startColX, startColZ);
        probe.check("the nearest coast is found over the height field and is the same coast twice",
                    found && again && sx == sx2 && sz == sz2 && tx == tx2 && tz == tz2, "%s", detail);
        probe.check("the run stands on the coast its seed names",
                    !shoreOn || !found || (sx == startColX && sz == startColZ), "%s", detail);
        probe.check("the coast it names is dry ground with water around most of it",
                    hThere >= SEA_LEVEL && hThere <= SEA_LEVEL + 3 && underThere >= 5, "%s", detail);
        probe.check("the direction it hands back points down into the water",
                    found && hLook < SEA_LEVEL && hLook < hThere, "%s", detail);

        // The height field is a promise; the chunk is what the eye sees. A coast that exists only in
        // the pure function would put the run beside a dry hole, so the water the start is facing is
        // looked for in the streamed block rather than in the number the generator would write there.
        int waterY = -1;
        for (int y = SEA_LEVEL + 3; y >= 0 && waterY < 0; y--) {
            if ((blockFlags(stateType(world.get(lookX, y, lookZ))) & FLAG_LIQUID) != 0) waterY = y;
        }
        std::snprintf(detail, sizeof(detail), "facing %d,%d: water block %d (surface %d), feet %d, "
                      "opaque top there %d", lookX, lookZ, waterY, waterY + 1,
                      (int)std::floor(feetY), world.getTop(lookX, lookZ));
        probe.check("the water the start faces is standing in the streamed chunks",
                    !shoreOn || waterY >= 0, "%s", detail);

        // And the eye has to be pointed at it. Measured against the yaw the start wrote, not the live
        // camera: a run whose pointer got captured does have a mouse — the user's real one, moving over
        // the window — and once the look works the head turns, so reading the live yaw here made the
        // claim flip between launches. The live direction is still printed, as context.
        Scene* sc = getScene();
        const quantum::Transform* ct = sc ? sc->findComponent<quantum::Transform>(camEntity) : nullptr;
        const float dlen = std::sqrt(tx * tx + tz * tz);
        if (dlen > 0.0f) {
            // A YXZ yaw turns (0,0,-1) towards -X, so the ground projection of the start's aim is
            // (-sin yaw, -cos yaw) - the same algebra applyShoreSpawn inverted to pick the angle.
            const float aimX = -std::sin(spawnYaw * (3.14159265358979323846f / 180.0f));
            const float aimZ = -std::cos(spawnYaw * (3.14159265358979323846f / 180.0f));
            const float dot = (aimX * tx + aimZ * tz) / dlen;
            float liveDot = 0.0f;
            quantum::Vector3 fwd;
            if (ct) {
                fwd = ct->rotation * quantum::Vector3(0, 0, -1);
                liveDot = (fwd.x * tx + fwd.z * tz) / dlen;
            }
            std::snprintf(detail, sizeof(detail), "start aims (%.2f,%.2f), coast lies (%.2f,%.2f), "
                          "aligned %.2f, spawn yaw %.1f, live camera %.2f (yaw now %.1f)",
                          (double)aimX, (double)aimZ, (double)(tx / dlen), (double)(tz / dlen),
                          (double)dot, (double)spawnYaw, (double)liveDot, (double)yaw);
            probe.check("the eye is turned toward the coast the start chose",
                        !shoreOn || dot > 0.9f, "%s", detail);
        } else {
            probe.skip("the coast it names has no direction to aim at");
        }
    }

    // Everything below is a claim about what a player does *to* the world, and a spectator does none of
    // it. VOXEL_MODE=spectator therefore leaves this module with nothing to measure; saying so in one
    // line is the honest answer, where failing eleven checks would only be noise.
    if (!canEdit()) {
        std::snprintf(detail, sizeof(detail), "VOXEL_MODE=%s gives this module no hands to test",
                      voxelGameModeName(gameMode()));
        probe.skip(detail);
        return;
    }

    const bool envOwnsMode = envMode("VOXEL_MODE") >= 0;
    const float savedX = feetX, savedY = feetY, savedZ = feetZ;
    const std::string savedModeName = gameModeName;
    const Slot savedSlot = hotbar[selectedSlot];
    const bool savedGrounded = grounded;
    const bool savedFlying = flying;

    // Stand four blocks over the surface and look straight down: the column below is then entirely
    // air, and the block the ray reaches is the one the terrain actually ends at.
    feetX = (float)colX + 0.5f;
    feetZ = (float)colZ + 0.5f;
    feetY = (float)(ground + 4);
    const float eyeY = feetY + EYE_OFFSET;

    VoxelHit h = raycastVoxel(world, feetX, eyeY, feetZ, 0.0f, -1.0f, 0.0f, 8.0f, false, true);
    const uint16_t before = h.hit ? world.get(h.x, h.y, h.z) : (uint16_t)BLOCK_AIR;
    std::snprintf(detail, sizeof(detail), "hit %d,%d,%d n=%d top=%d id=%s",
                  h.x, h.y, h.z, h.ny, ground, block(stateType(h.id)).name);
    probe.check("raycast down reports the column it is over",
                h.hit && h.y == ground && h.ny == 1 && stateType(h.id) == stateType(before), "%s", detail);

    const int bx = h.x, by = h.y, bz = h.z;

    // ---- what each mode is allowed to do to a body and to the world ----
    // Written into the scene row, because the row is what a person sets and [M] walks. An environment
    // pin outranks it, and then the rows cannot be taken anywhere: one skip says which claims are
    // missing and why, rather than letting them be read as failures of the modes themselves.
    if (envOwnsMode) {
        std::snprintf(detail, sizeof(detail),
                      "VOXEL_MODE pins the process to %s, so the rows cannot be taken to the other two",
                      voxelGameModeName(gameMode()));
        probe.skip(detail);
    } else {
        flying = true;
        gameModeName = "survival";
        const bool survivalAir = isAirborne();
        probe.check("survival ignores the flight toggle rather than clearing it",
                    !survivalAir && flying && !isCreative() && !isSpectator() && canEdit(),
                    "airborne %d, toggle still %d", survivalAir ? 1 : 0, flying ? 1 : 0);

        gameModeName = "creative";
        flying = false;
        const bool creativeGrounded = isAirborne();
        flying = true;
        const bool creativeAirborne = isAirborne();
        probe.check("creative is in the air exactly when the toggle says so",
                    !creativeGrounded && creativeAirborne && isCreative(),
                    "toggle off %d, toggle on %d", creativeGrounded ? 1 : 0,
                    creativeAirborne ? 1 : 0);

        gameModeName = "spectator";
        flying = false;
        const bool spectatorAir = isAirborne();
        const uint16_t heldType = stateType(before) == BLOCK_STONE ? BLOCK_DIRT : BLOCK_STONE;
        hotbar[selectedSlot].type = heldType;
        hotbar[selectedSlot].count = 3;
        // The clicks: the mode is refused inside the three functions that write, so a caller that
        // forgets to ask - the harness above, a later phase, this probe - still cannot break anything.
        updateMining(world, 1.0f, h, true);
        tryPlace(world, h);
        pickTarget(h);
        std::snprintf(detail, sizeof(detail), "%d,%d,%d still %s, progress %.2f, hand %s x%d",
                      bx, by, bz, block(stateType(world.get(bx, by, bz))).name, targetProgress,
                      block(hotbar[selectedSlot].type).name, hotbar[selectedSlot].count);
        probe.check("a spectator is in the air with no toggle, and its clicks change nothing",
                    spectatorAir && !flying && isSpectator() && !canEdit() &&
                        stateType(world.get(bx, by, bz)) == stateType(before) &&
                        targetProgress == 0.0f && hotbar[selectedSlot].type == heldType &&
                        hotbar[selectedSlot].count == 3, "%s", detail);

        gameModeName = "creative";
        flying = savedFlying;
    }

    // ---- break, place, the body's own shape ----
    if (isCreative()) {
        updateMining(world, 0.016f, h, true);
        std::snprintf(detail, sizeof(detail), "%s -> %s at %d,%d,%d", block(stateType(before)).name,
                      block(stateType(world.get(bx, by, bz))).name, bx, by, bz);
        probe.check("creative mining empties the voxel",
                    stateType(world.get(bx, by, bz)) == BLOCK_AIR, "%s", detail);
    } else {
        std::snprintf(detail, sizeof(detail),
                      "VOXEL_MODE=%s prices mining by hardness, so this run cannot show an instant break",
                      voxelGameModeName(gameMode()));
        probe.skip(detail);
    }

    hotbar[selectedSlot].type = BLOCK_STONE;
    hotbar[selectedSlot].count = 64;
    VoxelHit h2 = raycastVoxel(world, feetX, eyeY, feetZ, 0.0f, -1.0f, 0.0f, 8.0f, false, true);
    const int px = h2.x + h2.nx, py = h2.y + h2.ny, pz = h2.z + h2.nz;
    tryPlace(world, h2);
    std::snprintf(detail, sizeof(detail), "placed %s at %d,%d,%d (face of %d,%d,%d), %d left in hand",
                  block(stateType(world.get(px, py, pz))).name, px, py, pz, h2.x, h2.y, h2.z,
                  hotbar[selectedSlot].count);
    probe.check("placing writes the held block one voxel out along the face",
                h2.hit && stateType(world.get(px, py, pz)) == BLOCK_STONE, "%s", detail);
    // The stack is the other half of the mode: creative builds for free, survival pays one block per
    // block placed. The expectation follows the mode in force, so the claim holds in either run.
    probe.check("a creative place spends nothing from the stack and a survival one spends a block",
                hotbar[selectedSlot].count == (isCreative() ? 64 : 63), "%d left, creative %d",
                hotbar[selectedSlot].count, isCreative() ? 1 : 0);

    // Now stand on what was just placed and ask for the same voxel the body occupies. The block under
    // the feet is not replaceable, so tryPlace is the only thing that can write there - and it must
    // not, because the body is in the way.
    world.setBlock(px, py + 1, pz, BLOCK_AIR);
    feetX = (float)px + 0.5f;
    feetZ = (float)pz + 0.5f;
    feetY = (float)(py + 1);
    VoxelHit h4 = raycastVoxel(world, feetX, feetY + EYE_OFFSET, feetZ, 0.0f, -1.0f, 0.0f,
                               reach, false, true);
    const int ty = h4.y + h4.ny;
    tryPlace(world, h4);
    std::snprintf(detail, sizeof(detail), "target %d,%d,%d holds %s, feet at %.2f",
                  h4.x, ty, h4.z, block(stateType(world.get(h4.x, ty, h4.z))).name, feetY);
    probe.check("a block that would intersect the body is refused",
                h4.hit && ty == (int)feetY && stateType(world.get(h4.x, ty, h4.z)) == BLOCK_AIR, "%s", detail);

    // The controller's own step rule, measured in the player's column instead of in a carved arena.
    // `probeCollide` owns the shape claims (a cube, a fence post, a cactus inset); this one says the
    // rule the player actually moves by still lifts the body onto a half-block shelf and leaves it
    // standing there. The push is only 0.6 blocks because a long one is not a step test - at 3 blocks
    // the raised body sails clean over the slab and lands wherever the terrain happens to be.
    const int wx = (int)std::floor(feetX) + 1;
    const int wz = (int)std::floor(feetZ);
    world.setBlock(wx, (int)feetY, wz, makeState(BLOCK_SLAB_STONE, 0));
    VoxelBox climbed = bodyBox(feetY);
    const int climbFlags = moveBoxVoxel(world, climbed, 0.6f, 0.0f, 0.0f);
    std::snprintf(detail, sizeof(detail), "feet %.3f -> %.3f (slab top %.3f), minX %.2f on %.2f, flags=%d",
                  (double)feetY, (double)climbed.minY, (double)feetY + 0.5, (double)climbed.minX,
                  (double)feetX, climbFlags);
    probe.check("the controller walks up a half-block shelf and lands on it",
                std::fabs(climbed.minY - (feetY + 0.5f)) < 0.05f && climbed.minX > feetX &&
                (climbFlags & VOXEL_HIT_GROUND) != 0, "%s", detail);

    world.setBlock(wx, (int)feetY, wz, BLOCK_AIR);
    world.setBlock(px, py, pz, BLOCK_AIR);
    world.setBlock(bx, by, bz, before);

    // Putting the body back on the surface is the respawn the life module asks for, and the two ways
    // it can go wrong are both silent: a missing velocity zeroing makes the player land on the spawn
    // and bounce, and an answer of "true" for a column that has not streamed leaves them inside stone.
    feetY = (float)(ground + 30);   // anywhere but there: the point is that the call moves the body
    velX = 4.0f; velY = -9.0f; velZ = 4.0f;
    flying = true;
    const bool put = respawnAt(world, (float)colX + 0.25f, (float)colZ + 0.75f);
    std::snprintf(detail, sizeof(detail), "respawn %d -> feet %.2f (top %d), vel %.2f %.2f %.2f, "
                  "grounded %d, flying %d, published %.2f %.2f %.2f",
                  put ? 1 : 0, (double)feetY, ground, (double)velX, (double)velY, (double)velZ,
                  grounded ? 1 : 0, flying ? 1 : 0, (double)footX, (double)footY, (double)footZ);
    probe.check("respawnAt stands the body on the surface of the column and forgets the velocity",
                put && feetY == (float)(ground + 1) && velX == 0.0f && velY == 0.0f && velZ == 0.0f &&
                    grounded && !flying && footY == feetY && footX > (float)colX &&
                    footZ > (float)colZ, "%s", detail);

    // Far outside the streamed radius on any view radius this project ships (16 chunks wide, and the
    // columns asked for are 512 away).
    const float keptX = feetX, keptY = feetY, keptZ = feetZ;
    const int farX = colX + 512, farZ = colZ + 512;
    const bool far = respawnAt(world, (float)farX + 0.5f, (float)farZ + 0.5f);
    std::snprintf(detail, sizeof(detail), "unstreamed column %d,%d answered %d, top %d, body %.2f "
                  "%.2f %.2f -> %.2f %.2f %.2f", farX, farZ, far ? 1 : 0, world.getTop(farX, farZ),
                  (double)keptX, (double)keptY, (double)keptZ, (double)feetX, (double)feetY,
                  (double)feetZ);
    probe.check("respawnAt refuses a column with no surface and leaves the body where it was",
                !far && world.getTop(farX, farZ) < 0 && feetX == keptX && feetY == keptY &&
                    feetZ == keptZ, "%s", detail);

    // ---- the pointer, delivered the way the platform delivers it ----
    // Turning sideways is a steady sweep: the pointer travels the same number of pixels every frame.
    // The look code warps the reported position back to the centre each frame, so the baseline the
    // delta is read against has to be that centre - read against the previous frame's own reading, the
    // warp is subtracted from the travel too, two equal frames cancel, and a hand that never stops
    // moving never moves the view. This is reproduced rather than trusted because no headless run can
    // wiggle a mouse, and because the failure it describes is invisible from the platform side.
    {
        const float savedYawLook = yaw, savedPitchLook = pitch;
        const bool savedGrab = mouseGrabbed;
        const Vector2 savedBase = lastMouse;
        const Vector2 center((float)(Engine::getCanvasWidth() / 2),
                             (float)(Engine::getCanvasHeight() / 2));
        const float TRAVEL = 12.0f;   // pixels per frame, and the same every frame

        mouseGrabbed = true;
        Engine::setMousePosition(center.x, center.y);
        lastMouse = center;
        yaw = 0.0f;
        pitch = 0.0f;
        for (int f = 0; f < 4; f++) {
            Engine::setMousePosition(center.x + TRAVEL, center.y);
            applyLook();
        }
        const float turned = -yaw;    // a right sweep decreases yaw: the camera turns from -Z to -X
        std::snprintf(detail, sizeof(detail), "4 frames of %.0f px: yaw %.3f degrees instead of %.3f",
                      TRAVEL, (double)yaw, (double)(-4.0f * TRAVEL * MOUSE_SENSITIVITY));
        probe.check("a steady horizontal sweep turns the view by all of its travel",
                    std::fabs(turned - 4.0f * TRAVEL * MOUSE_SENSITIVITY) < 1e-3f, "%s", detail);

        // A hand that stops must leave the view where it got to. Measured against the reading instead
        // of the warp point, the frame after a sweep is the negation of the last one and the view
        // drifts back by exactly the travel it just earned.
        Engine::setMousePosition(center.x, center.y);
        applyLook();
        std::snprintf(detail, sizeof(detail), "stopped: yaw %.3f, still %.3f", (double)yaw,
                      (double)turned);
        probe.check("a pointer that stops leaves the view where it was",
                    std::fabs(yaw + turned) < 1e-3f, "%s", detail);

        // The same travel on the other axis, because the two share one baseline and one bug.
        Engine::setMousePosition(center.x, center.y);
        lastMouse = center;
        pitch = 0.0f;
        for (int f = 0; f < 3; f++) {
            Engine::setMousePosition(center.x, center.y - TRAVEL);
            applyLook();
        }
        std::snprintf(detail, sizeof(detail), "3 frames of %.0f px up: pitch %.3f instead of %.3f",
                      TRAVEL, (double)pitch, (double)(3.0f * TRAVEL * MOUSE_SENSITIVITY));
        probe.check("the same steady travel tips the view up by all of its travel",
                    std::fabs(pitch - 3.0f * TRAVEL * MOUSE_SENSITIVITY) < 1e-3f, "%s", detail);

        mouseGrabbed = savedGrab;
        lastMouse = savedBase;
        yaw = savedYawLook;
        pitch = savedPitchLook;
    }

    // ---- the pointer while the window is in the background ----
    // The bug this checks is one the player feels on the desktop rather than in the game: a grabbed
    // look re-centres the cursor every frame, and a window that is not foreground has no right to move
    // the pointer the user is working with. The focus answer is pinned (VOXEL_FOCUS=0/1) because a
    // headless run cannot arrange for another window to be foreground - and because the platform half
    // of the fix, the warp refusing to reach SetCursorPos while unfocused, is not observable from here.
    {
        const bool savedGrab = mouseGrabbed;
        const bool savedResume = resumeLookOnFocus;
        const int savedPin = focusPin;
        const MouseMode savedMode = Engine::getMouseMode();

        // A focused window keeps the pointer it was holding.
        focusPin = 1;
        resumeLookOnFocus = false;
        mouseGrabbed = false;
        grabMouse(true);
        applyFocusPolicy();
        probe.check("a focused window keeps the pointer it grabbed",
                    mouseGrabbed && Engine::getMouseMode() == MouseMode::CAPTURED,
                    "grab %d mode %d", mouseGrabbed ? 1 : 0, (int)Engine::getMouseMode());

        // Losing focus hands it back: released, and the mode that hides and clips it is off, which is
        // what lets the arrow show again over whatever the player clicked on.
        focusPin = 0;
        applyFocusPolicy();
        const bool blurred = !mouseGrabbed && Engine::getMouseMode() == MouseMode::NORMAL;
        probe.check("losing focus releases the pointer instead of warping it",
                    blurred, "grab %d mode %d", mouseGrabbed ? 1 : 0,
                    (int)Engine::getMouseMode());

        // Coming back re-takes it, the way the Linux backend re-applies CAPTURED on FocusIn: the
        // player resumes turning without hunting for the key that released the first time.
        focusPin = 1;
        applyFocusPolicy();
        probe.check("regaining focus re-takes a grab the blur interrupted",
                    mouseGrabbed && Engine::getMouseMode() == MouseMode::CAPTURED &&
                        !resumeLookOnFocus,
                    "grab %d mode %d pending %d", mouseGrabbed ? 1 : 0,
                    (int)Engine::getMouseMode(), resumeLookOnFocus ? 1 : 0);

        // A release the player pressed is a request, not an accident: it must not come back on its own.
        grabMouse(false);
        focusPin = 0;
        applyFocusPolicy();
        focusPin = 1;
        applyFocusPolicy();
        probe.check("a grab the player released by hand is not re-taken by focus",
                    !mouseGrabbed && !resumeLookOnFocus, "grab %d pending %d",
                    mouseGrabbed ? 1 : 0, resumeLookOnFocus ? 1 : 0);

        // The launch is the same decision taken by the same function the update calls: a run started
        // from behind another window does not seize its cursor, and the first frame in the foreground does.
        const bool savedEver = everGrabbed;
        everGrabbed = false;
        mouseGrabbed = false;
        focusPin = 0;
        applyFocusPolicy();
        const bool idleLaunch = !mouseGrabbed && !everGrabbed;
        focusPin = 1;
        applyFocusPolicy();
        probe.check("a background launch waits for the foreground before taking the pointer",
                    idleLaunch && mouseGrabbed && everGrabbed,
                    "launch grab %d, after arriving grab %d", idleLaunch ? 1 : 0,
                    mouseGrabbed ? 1 : 0);
        everGrabbed = savedEver;

        focusPin = savedPin;
        resumeLookOnFocus = savedResume;
        mouseGrabbed = savedGrab;
        Engine::setMouseMode(savedMode);
    }

#ifdef VOXEL_HAVE_MODEL
    // The body the player is wearing only earns its keep if it points where they look. Two signs are
    // easy to get backwards - the camera looks down its own -Z while a box's front face is its +Z, and
    // a positive camera pitch tips the view up while a positive head pitch tips the face down - so this
    // measures the two quaternions the engine is holding instead of repeating this file's algebra: the
    // head's front and the camera's forward have to be the same vector.
    //
    // Under the survival row with the crouch released, because the eye the claims compare against is
    // only feet + EYE_OFFSET while neither the sneaking nor the flying offset is in force - and a
    // spectator has no body to wear, so VOXEL_MODE pinning one is answered with a skip, not a fail.
    const float savedYaw = yaw, savedPitch = pitch;
    const bool savedThird = thirdPerson;
    const bool savedSneak = sneaking;
    const std::string savedRow = gameModeName;
    gameModeName = "survival";
    sneaking = false;
    thirdPerson = true;
    yaw = 37.0f;
    pitch = -14.0f;
    // Four blocks over the surface, the vantage the raycast claim above uses: the column below the
    // head is then all air, so a camera pulled up and behind has air to sit in.
    feetX = (float)colX + 0.5f;
    feetY = (float)(ground + 4);
    feetZ = (float)colZ + 0.5f;

    Scene* sc = getScene();
    Transform* camTr = sc ? sc->findComponent<Transform>(camEntity) : nullptr;
    Transform* headTr = nullptr;
    if (sc && avatarRig.parts[VOXEL_PART_HEAD] != 0) {
        const Entity headEntity = (Entity)avatarRig.parts[VOXEL_PART_HEAD];
        if (sc->isEntityCreated(headEntity)) headTr = sc->findComponent<Transform>(headEntity);
    }
    if (!camTr || !headTr) {
        probe.skip("no camera transform or avatar head to measure");
    } else if (isSpectator()) {
        probe.skip("VOXEL_MODE pins a spectator, which has no body to wear");
    } else {
        setEyeFromBody();
        updateAvatar();

        const Vector3 look = camTr->rotation * Vector3(0.0f, 0.0f, -1.0f);
        const Vector3 face = headTr->rotation * Vector3(0.0f, 0.0f, 1.0f);
        const float dot = look.x * face.x + look.y * face.y + look.z * face.z;
        std::snprintf(detail, sizeof(detail), "yaw %.0f pitch %.0f: look %.3f %.3f %.3f, face %.3f "
                      "%.3f %.3f, dot %.5f", yaw, pitch, look.x, look.y, look.z,
                      face.x, face.y, face.z, dot);
        probe.check("the head the player wears looks exactly where the camera looks",
                    dot > 0.9999f, "%s", detail);

        // Pulling the camera out of the head must not move the head: every other part of the game -
        // what block the eye is on, whether the player is drowning, where a fall started - reads the
        // published eye, and a third-person view that dragged it backwards would change reach, breath
        // and fall damage for the cost of a camera angle.
        const float dx = camTr->position.x - posX, dy = camTr->position.y - posY,
                    dz = camTr->position.z - posZ;
        const float back = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float along = dx * look.x + dy * look.y + dz * look.z;
        std::snprintf(detail, sizeof(detail), "eye %.2f %.2f %.2f (feet %.2f %.2f %.2f), camera %.2f "
                      "away and %.2f along the look, %.2f asked", posX, posY, posZ, feetX, feetY,
                      feetZ, back, along, THIRD_PERSON_BACK);
        probe.check("the eye stays the head and only the camera moves for the third-person view",
                    posX == feetX && posY == feetY + EYE_OFFSET && posZ == feetZ &&
                        std::fabs(back - THIRD_PERSON_BACK) < 1e-3f && along < 0.0f,
                    "%s", detail);

        // The model has to be as tall as the collider and no taller: a crown above the box's top is a
        // head that clips through a ceiling the body fitted under, and one below it is a player
        // standing on their own feet. Measured with the head unrotated, because a tilted box's top
        // corner is not its height.
        yaw = 0.0f; pitch = 0.0f;
        setEyeFromBody();
        updateAvatar();
        const VoxelModelInfo& hi = voxelModelInfo(VOXEL_PART_HEAD);
        const float crown = headTr->position.y + hi.h * headTr->scale.x;
        std::snprintf(detail, sizeof(detail), "neck %.3f scale %.3f head %.3f -> crown %.3f, collider "
                      "top %.3f", headTr->position.y, headTr->scale.x, hi.h, crown,
                      feetY + BODY_HEIGHT);
        probe.check("the model is exactly as tall as the body it stands inside",
                    std::fabs(crown - (feetY + BODY_HEIGHT)) < 1e-3f, "%s", detail);

        // A camera buried in a wall shows the inside of that wall, which is the one thing this view
        // must not do. The voxel it had just landed in is written solid and the answer asked again: the
        // camera has to come back in front of that block rather than stay inside it.
        yaw = 37.0f; pitch = -14.0f;
        setEyeFromBody();
        const int wallX = (int)std::floor(camTr->position.x);
        const int wallY = (int)std::floor(camTr->position.y);
        const int wallZ = (int)std::floor(camTr->position.z);
        const uint16_t wasHere = world.get(wallX, wallY, wallZ);
        world.setBlock(wallX, wallY, wallZ, makeState(BLOCK_STONE, 0));
        setEyeFromBody();
        const int vx = (int)std::floor(camTr->position.x);
        const int vy = (int)std::floor(camTr->position.y);
        const int vz = (int)std::floor(camTr->position.z);
        const float ddx = camTr->position.x - posX, ddy = camTr->position.y - posY,
                    ddz = camTr->position.z - posZ;
        const float shrunk = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
        std::snprintf(detail, sizeof(detail), "wall at %d,%d,%d: camera now %d,%d,%d holding %s, %.2f "
                      "from the eye instead of %.2f", wallX, wallY, wallZ, vx, vy, vz,
                      block(stateType(world.get(vx, vy, vz))).name, shrunk, THIRD_PERSON_BACK);
        probe.check("a third-person camera is stopped by a wall instead of buried in it",
                    (vx != wallX || vy != wallY || vz != wallZ) && shrunk < THIRD_PERSON_BACK - 0.2f,
                    "%s", detail);
        world.setBlock(wallX, wallY, wallZ, wasHere);
    }

    // Back as the player left it, and the camera brought home to the head with it: the rig stays built
    // (it is the player's body, not a probe prop), so the pose it ends on is the one the next frame
    // will read. The stride clock forgets the teleport the measurement made.
    gameModeName = savedRow;
    sneaking = savedSneak;
    thirdPerson = savedThird;
    yaw = savedYaw;
    pitch = savedPitch;
    feetX = savedX; feetY = savedY; feetZ = savedZ;
    avatarLastValid = false;
    setEyeFromBody();
    updateAvatar();
#endif

    feetX = savedX; feetY = savedY; feetZ = savedZ;
    grounded = savedGrounded;
    flying = savedFlying;
    hotbar[selectedSlot] = savedSlot;
    gameModeName = savedModeName;
    // The row and not the resolved number is what goes back: an environment pin is not this function's
    // to restore, and a run that left with the mode it arrived in cannot have changed the game the
    // player was playing by probing it.
    std::snprintf(detail, sizeof(detail), "%s -> %s (%d), pinned by VOXEL_MODE %d",
                  savedModeName.c_str(), gameModeName.c_str(), gameMode(), envOwnsMode ? 1 : 0);
    probe.check("the probe leaves the mode, the body, the hand and the world as it found them",
                gameModeName == savedModeName && feetX == savedX && feetY == savedY &&
                    feetZ == savedZ && flying == savedFlying && grounded == savedGrounded &&
                    hotbar[selectedSlot].type == savedSlot.type &&
                    hotbar[selectedSlot].count == savedSlot.count &&
                    stateType(world.get(bx, by, bz)) == stateType(before), "%s", detail);
}

}  // namespace quantum
