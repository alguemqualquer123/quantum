// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include "ScriptBase.h"
#include "ScriptProperty.h"
#include "Vector3.h"
#include "VoxelWorld.h"

#include <cstdint>
#include <string>
#include <vector>

namespace quantum {

class Scene;
class Text;
class Image;
class Fog;
class Light;
class VoxelPlayerScript;

// The scene's voxel game host. It owns the VoxelWorld, installs whichever voxel modules are
// compiled in (generation, lighting, liquids, saving, networking, player, mobs), drives the
// streaming from the player's position, runs the day/night pair (sun + fog + baked skylight) and
// draws the HUD. Everything else in the project is a module that hangs off the world's hooks.
class VoxelGame : public ScriptBase {
public:
    DPROPERTY("Seed")
    int seedValue = 1337;

    DPROPERTY("View Radius")
    int viewRadius = 4;

    DPROPERTY("Keep Radius")
    int keepRadius = 6;

    DPROPERTY("Save To Disk")
    bool saveToDisk = true;

    DPROPERTY("Save Folder")
    std::string saveFolder = "world";

    DPROPERTY("Day Length (seconds)")
    float dayLength = 600.0f;

    DPROPERTY("Time Of Day")
    float startDaylight = 0.35f;

    DPROPERTY("Mobs")
    int mobCount = 12;

    DPROPERTY("Multiplayer")
    std::string netMode = "offline";

    DPROPERTY("Port")
    int netPort = 25600;

    DPROPERTY("Server Address")
    std::string netAddress = "127.0.0.1";

    DPROPERTY("Show Debug Overlay")
    bool showDebug = true;

    DPROPERTY("Place Player On Surface")
    bool snapToSurface = true;

    VoxelGame(Scene* scene, Entity entity);
    ~VoxelGame();

    void onUpdate();

    voxel::VoxelWorld* getWorld() { return &world; }

    // The one live instance: the HUD, the player script and the console commands all reach the
    // world through this instead of hunting for the entity.
    static VoxelGame* current();

    // The controller for the hooks a plain function pointer installs - those have no `this` and
    // cannot reach the private accessor below. The same answer as playerScript(), and nullptr in a
    // build with no player module, which is what makes a collector that asks the hand safe to write.
    VoxelPlayerScript* getPlayer() { return playerScript(); }

    float daylight() const { return dayLight; }
    float timeOfDay() const { return dayTime; }

private:
    void initWorld();
    void rebuildWorld();
    void updateDayNight(float dt);
    void updateBreakFx(float dt);
    // VOXEL_HAVE_SELECT: the frame around the voxel the crosshair is on, drawn from the block's own
    // collision boxes. Takes no dt - it reads a target the player module already resolved this frame.
    void updateSelectFx();
    // VOXEL_HAVE_AUDIO: the ear the sound module gets this frame - where the feet are, how far they
    // moved, whether the eye is under water, and how bright the sky is.
    void updateAudioFx();
    // VOXEL_HAVE_LIFE: the same report for the vitals module, plus the two keys that relieve it and
    // the respawn a death asks for. Takes dt because the module's whole vocabulary is a rate times
    // the seconds the frame lasted.
    void updateLifeFx(float dt);
    void rebakeDaylight(float daylight);
    void buildHud();
    void updateHud();
    void handleKeys();
    void setupPlayerCamera();
    bool playerPosition(float& x, float& y, float& z) const;
    bool lookDirection(float& dx, float& dy, float& dz) const;

    // VOXEL_PROBE: run every compiled-in module's behavioural probe against the live world and
    // print one total. Each module restores what it touched, so the run leaves the world as it was.
    void runProbes();

    // VOXEL_DROP=<count>: throw <count> palette blocks into the scene in front of the eye. Only a
    // headless capture needs it - the item is otherwise put on the ground by a mined block, and
    // nothing in an automated run presses the button. It uses the same spawn path the game does,
    // so it exists only in a build that compiled the drop module in.
    void seedDrops();

    // The controller, found through the scene's script table. The HUD shows the inventory the
    // player actually holds rather than the host's creative palette, and a build without the
    // player module simply gets nullptr and keeps the palette.
    VoxelPlayerScript* playerScript();

    // The creative palette the hotbar shows. The player module owns break/place; this is only what
    // slot N contains, so a build without the player still has a HUD that means something.
    uint16_t selectedBlock() const {
        if (selectedSlot < 0 || selectedSlot >= 9) return paletteIds[0];
        return paletteIds[selectedSlot];
    }

    voxel::VoxelWorld world;
    bool ready = false;
    uint64_t worldSeed = 0;

    float dayTime = 0.0f;        // 0..1, 0 = midnight
    float dayLight = 1.0f;       // what the shader/bake currently sees
    float bakedDaylight = -1.0f; // the daylight the loaded meshes were baked at
    double mobTimer = 0.0;
    double chipTimer = 0.0;       // VOXEL_HAVE_BREAK: time since the last mining puff
    // VOXEL_HAVE_AUDIO: a step is owed by distance rather than by time, so the host keeps the walk
    // it saw last frame; and entering the water is an edge, not a level, so the last state too.
    float audioPrevX = 0.0f, audioPrevZ = 0.0f;
    bool audioPrevValid = false;
    bool audioPrevInWater = false;
    // VOXEL_HAVE_LIFE: the stride the vitals are charged for, measured the same way the footstep is.
    bool lifePrevValid = false;
    float lifePrevX = 0.0f, lifePrevZ = 0.0f;
    // The two relief keys are edges: a held G must not eat a whole stack in one second.
    bool lifePrevEat = false, lifePrevDrink = false;
    // The spawn column is only known once the world has streamed it, and a death can happen before
    // that - so the host remembers the place it was asked for and keeps trying until the body fits.
    bool lifeSpawnSet = false;
    bool lifeAwaitingRespawn = false;
    float lifeRespawnX = 0.0f, lifeRespawnZ = 0.0f;
    // The mode the bars were under last frame. -1 so the first frame reports it: which rule set a run
    // started in is the first thing a log has to answer.
    int lifeLastMode = -1;
    int mobSpawned = 0;
    bool surfacePlaced = false;
    VoxelPlayerScript* player = nullptr;    // resolved once, see playerScript()

    // The host owns the two environment entities. A scene does not have to carry a Sun or a fog
    // block for the voxel world to have a day/night cycle, and an authored one would fight the bake
    // for the same colour.
    Light* sun = nullptr;
    Fog* fog = nullptr;
    Vector3 sunDayColor = Vector3(1.0f, 0.98f, 0.92f);
    Vector3 sunDuskColor = Vector3(1.0f, 0.62f, 0.32f);

    // A UI entity inside the world scene is a quad in the perspective camera's space, not a screen
    // widget: anchors would position it in blocks. The HUD therefore lives in its own Scene added
    // with Engine::addSceneLayer, which gets the default CAMERA_UI orthographic camera (y-down,
    // one unit per pixel) and does not clear the color buffer, so it draws over the world.
    Scene* hudScene = nullptr;
    // UI has no z-order knob: the render pass walks the entities and draws them in that order, so a
    // backing has to be created before the widget it is meant to sit behind.
    Image* overlayBg = nullptr;      // the dark plate under the debug block
    std::vector<Image*> slotCells;   // the hotbar frames, one per slot
    std::vector<Text*> slots;        // the slot digits
    Text* overlay = nullptr;      // debug lines, top-left
    Text* crosshair = nullptr;    // the plus in the middle
    Text* title = nullptr;        // what you are looking at
    uint16_t paletteIds[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    int selectedSlot = 0;
    float reach = 5.0f;           // how far a break/place ray may travel, in blocks
    std::string overlayText;
    std::string lastLookText;
    bool overlayDirty = true;
    int hudFrame = 0;
    int captureCountdown = -1;
    int probeFrame = 0;         // VOXEL_PROBE: the frame on which the controller runs its self-test
    int dropFrame = 0;          // VOXEL_DROP: the frame the host throws the seeded items on the ground
    int dropSeed = 0;           // how many items VOXEL_DROP asked for
    int quitCountdown = 0;      // frames left before the process closes, once the capture landed
    std::string capturePath;

    // VOXEL_NETEDIT="dx dy dz id" writes one block relative to the player's feet, on purpose, from
    // a headless run. Without it the protocol can only be proven by a human pressing keys on two
    // machines; with it a script arms both sides and reads the peer's log for the effect.
    // The offset is relative rather than absolute so the column is guaranteed to be streamed on
    // both ends: each instance spawns from the same seed at the same place.
    int netEditOffset[3] = {0, 2, 0};
    uint16_t netEditId = 0;
    bool netEditArmed = false;
    bool netEditDone = false;

    static VoxelGame* s_current;
};

}  // namespace quantum
