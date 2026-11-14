// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelGame.h"

#include "Engine.h"
#include "Input.h"
#include "Scene.h"
#include "System.h"
#include "Log.h"
#include "Vector2.h"
#include "Vector3.h"
#include "component/CameraComponent.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/ScriptComponent.h"
#include "component/Transform.h"
#include "object/Light.h"
#include "object/environment/Fog.h"
#include "object/ui/Text.h"
#include "object/ui/Image.h"

#include "VoxelBlocks.h"
#include "VoxelMesh.h"
#include "VoxelProbe.h"
#include "VoxelTiles.h"

// The voxel modules are optional by design: each one hangs off a std::function on VoxelWorld, so a
// build that does not compile a module simply does not have that feature. The include test is what
// lets the game host land before the modules do.
#if defined(__has_include)
#  if __has_include("VoxelGen.h")
#    include "VoxelGen.h"
#    define VOXEL_HAVE_GEN 1
#  endif
#  if __has_include("VoxelLight.h")
#    include "VoxelLight.h"
#    define VOXEL_HAVE_LIGHT 1
#  endif
#  if __has_include("VoxelWater.h")
#    include "VoxelWater.h"
#    define VOXEL_HAVE_WATER 1
#  endif
#  if __has_include("VoxelSave.h")
#    include "VoxelSave.h"
#    define VOXEL_HAVE_SAVE 1
#  endif
#  if __has_include("VoxelNet.h")
#    include "VoxelNet.h"
#    define VOXEL_HAVE_NET 1
#  endif
#  if __has_include("VoxelCollide.h")
#    include "VoxelCollide.h"
#    define VOXEL_HAVE_COLLIDE 1
#  endif
#  if __has_include("VoxelPlayer.h")
#    include "VoxelPlayer.h"
#    define VOXEL_HAVE_PLAYER 1
#  endif
#  if __has_include("VoxelEntity.h")
#    include "VoxelEntity.h"
#    define VOXEL_HAVE_ENTITY 1
#  endif
#  if __has_include("VoxelModel.h")
#    include "VoxelModel.h"
#    define VOXEL_HAVE_MODEL 1
#  endif
#  if __has_include("VoxelBreak.h")
#    include "VoxelBreak.h"
#    define VOXEL_HAVE_BREAK 1
#  endif
#  if __has_include("VoxelSelect.h")
#    include "VoxelSelect.h"
#    define VOXEL_HAVE_SELECT 1
#  endif
#  if __has_include("VoxelAudio.h")
#    include "VoxelAudio.h"
#    define VOXEL_HAVE_AUDIO 1
#  endif
#  if __has_include("VoxelLife.h")
#    include "VoxelLife.h"
#    define VOXEL_HAVE_LIFE 1
#  endif
#  if __has_include("VoxelDrop.h")
#    include "VoxelDrop.h"
#    define VOXEL_HAVE_DROP 1
#  endif
#  if __has_include("VoxelUi.h")
#    include "VoxelUi.h"
#    define VOXEL_HAVE_UI 1
#  endif
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace quantum {

using namespace voxel;

VoxelGame* VoxelGame::s_current = nullptr;

VoxelGame* VoxelGame::current() { return s_current; }

static const float VOXEL_PI = 3.14159265358979f;

// std::getenv is a deprecation warning on MSVC, and this repo builds without warnings.
static bool envValue(const char* name, std::string& out) {
#ifdef _MSC_VER
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) return false;
    out.assign(buffer);
    free(buffer);
    return true;
#else
    const char* v = std::getenv(name);
    if (!v) return false;
    out.assign(v);
    return true;
#endif
}

static uint16_t blockIdByName(const char* name) {
    for (int t = 0; t < BLOCK_COUNT; t++) {
        if (block((uint16_t)t).name && std::strcmp(block((uint16_t)t).name, name) == 0) return (uint16_t)t;
    }
    return BLOCK_STONE;
}

// "dx dy dz id" into four integers, without sscanf: MSVC deprecates it (C4996) and this repo builds
// warning-free. Accepts spaces, tabs or commas between fields and negative numbers. Guarded with the
// net module because it is the only reader, and an unused static function is a warning.
#ifdef VOXEL_HAVE_NET
static int parseFourInts(const std::string& text, int out[4]) {
    int n = 0;
    const char* p = text.c_str();
    while (*p && n < 4) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p) break;
        out[n++] = (int)v;
        p = end;
    }
    return n;
}
#endif

VoxelGame::VoxelGame(Scene* scene, Entity entity) : ScriptBase(scene, entity) {
    REGISTER_ENGINE_EVENT(onUpdate);
}

VoxelGame::~VoxelGame() {
    UNREGISTER_ENGINE_EVENT(onUpdate);

#ifdef VOXEL_HAVE_SAVE
    if (ready) flushVoxelSave(world);
#endif
#ifdef VOXEL_HAVE_NET
    stopVoxelNet();
#endif
    for (Image* im : slotCells) delete im;
    slotCells.clear();
    for (Text* t : slots) delete t;
    slots.clear();
    delete overlay; delete crosshair; delete title;
    delete overlayBg;
    overlay = nullptr; crosshair = nullptr; title = nullptr;
    overlayBg = nullptr;
    if (hudScene) {
        Engine::removeScene(hudScene);
        delete hudScene;
        hudScene = nullptr;
    }
    delete sun; sun = nullptr;
    delete fog; fog = nullptr;
    if (s_current == this) s_current = nullptr;
}

void VoxelGame::initWorld() {
    if (!scene) return;

    worldSeed = (uint64_t)(uint32_t)seedValue;
    world.viewRadius = viewRadius > 0 ? viewRadius : 4;
    world.keepRadius = keepRadius > viewRadius ? keepRadius : viewRadius + 2;

    // Environment overrides, so that one project directory can run both sides of a multiplayer
    // test. The scene file cannot carry them: it is shared, and two instances pointed at one save
    // folder fight over its chunk index. VOXEL_SAVEDIR="" means "do not save at all", which is what
    // a throwaway test peer wants.
    std::string env;
#ifdef VOXEL_HAVE_NET
    if (envValue("VOXEL_NET", env) && !env.empty()) netMode = env;
    if (envValue("VOXEL_PORT", env) && !env.empty()) netPort = std::atoi(env.c_str());
    if (envValue("VOXEL_ADDR", env) && !env.empty()) netAddress = env;
#endif
    bool saveDirChosen = false;
    if (envValue("VOXEL_SAVEDIR", env)) {
        saveToDisk = !env.empty();
        saveFolder = env;
        saveDirChosen = true;
    }
    // A probe run must measure the world the seed describes, not the one the last run mined. Every
    // check that reads the ground (where the body starts, whether the coast it faces is still water)
    // is answered from the streamed chunks, and a chunk loaded off disk carries the edits a previous
    // process wrote into it - so the same command on the same binary gave two different totals here.
    // The fix is a cold directory, not no directory: probeVoxelSave installs over the world's own
    // save dir and skips entirely when the module is absent, so simply stopping saving cost the suite
    // its 15 save checks. An explicit VOXEL_SAVEDIR still wins - a deliberate save test owns its path.
    if (!saveDirChosen && envValue("VOXEL_PROBE", env) && !env.empty() && std::atoi(env.c_str()) != 0) {
        std::error_code ec;
        const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
        if (!tmp.empty() && !ec) {
            std::string dir = (tmp / "quantum-voxel-probe-world").string();
            for (char& ch : dir) if (ch == '\\') ch = '/';
            std::filesystem::remove_all(dir, ec);   // wipe the previous run's debris, then save into it
            saveFolder = dir;
            Log::print("VoxelWorld: VOXEL_PROBE saves to a scratch dir, so the world is the seed: %s",
                       saveFolder.c_str());
        } else {
            saveToDisk = false;
            saveFolder.clear();
            Log::warn("VoxelWorld: the OS temp dir is unreachable, so this probe run does not save");
        }
    }
#ifdef VOXEL_HAVE_NET
    std::string spec;
    if (envValue("VOXEL_NETEDIT", spec) && !spec.empty()) {
        int fields[4] = {0, 0, 0, 0};
        if (parseFourInts(spec, fields) == 4) {
            netEditOffset[0] = fields[0];
            netEditOffset[1] = fields[1];
            netEditOffset[2] = fields[2];
            netEditId = (uint16_t)fields[3];
            netEditArmed = true;
            Log::print("VoxelNet: scripted edit armed at %d %d %d -> id %d (%s)",
                       netEditOffset[0], netEditOffset[1], netEditOffset[2], (int)netEditId,
                       block(stateType(netEditId)).name ? block(stateType(netEditId)).name : "unknown");
        } else {
            Log::warn("VoxelNet: VOXEL_NETEDIT wants four numbers (dx dy dz id), got '%s'", spec.c_str());
        }
    }
#endif

#ifdef VOXEL_HAVE_GEN
    installVoxelGen(world);
#endif
#ifdef VOXEL_HAVE_LIGHT
    installVoxelLight(world);
#endif
#ifdef VOXEL_HAVE_WATER
    installVoxelWater(world);
#endif
    if (saveToDisk && !saveFolder.empty()) {
#ifdef VOXEL_HAVE_SAVE
        installVoxelSave(world, saveFolder);
        world.saveDir = saveFolder;
#else
        Log::warn("VoxelWorld: saving asked for but the save module is not compiled in");
#endif
    }

    world.init(scene, worldSeed);
    s_current = this;

    // The environment belongs to the world, not to the scene file: the sun's direction and the
    // fog's colour are both a function of the same dayTime the chunk bake reads, so authoring a
    // second sun in the editor would only fight it.
    if (!sun) {
        sun = new Light(scene);
        sun->setType(LightType::DIRECTIONAL);
        sun->setShadows(true);
    }
    if (!fog) {
        fog = new Fog(scene);
        fog->setType(FogType::LINEAR);
    }

    // Multiplayer is switched on here rather than in a menu: the mode is a scene property, so a
    // host and a client are the same project with a different seed of settings.
#ifdef VOXEL_HAVE_NET
    if (netMode == "host" || netMode == "server") {
        if (!startVoxelNet(world, NetMode::HOST, netPort, ""))
            Log::warn("VoxelNet: host failed to start (%s)", netError());
    } else if (netMode == "client") {
        if (!startVoxelNet(world, NetMode::CLIENT, netPort, netAddress))
            Log::warn("VoxelNet: connect failed to %s (%s)", netAddress.c_str(), netError());
    }
#endif

#ifdef VOXEL_HAVE_PLAYER
    setPlayerWorld(&world);
#endif
#if defined(VOXEL_HAVE_PLAYER) && defined(VOXEL_HAVE_WATER)
    // The controller places a liquid through this hook so that pouring a bucket runs the real
    // source/spread solver. Left unassigned it would drop a level-15 block that never flows. The
    // solver answers with the number of cells it wetted, which the player has no use for.
    liquidSourcePlacer = [](VoxelWorld& w, int x, int y, int z, bool lava, int amount) {
        placeLiquidSource(w, x, y, z, lava, amount);
    };
#endif
#if defined(VOXEL_HAVE_PLAYER) && (defined(VOXEL_HAVE_BREAK) || defined(VOXEL_HAVE_AUDIO))
    // The completion of a mine is the one moment the controller cannot draw: it knows a block went
    // away, the host knows which entity is allowed to spark chips and which voice is allowed to make
    // the noise. Both halves are compiled separately, so a build with FX but no sound - or the other
    // way round - still reports the break, and a build with neither leaves the hook null.
    blockBrokenFn = [](int x, int y, int z, uint16_t type) {
#ifdef VOXEL_HAVE_BREAK
        // Scoped inside the guard: a sound-only build would otherwise carry an unused handle to a
        // scene it never touches.
        VoxelGame* g = VoxelGame::current();
        Scene* fxScene = g ? g->getWorld()->scene() : nullptr;
        if (fxScene) spawnBreakDebris(fxScene, x, y, z, type, 12);
#endif
#ifdef VOXEL_HAVE_AUDIO
        playVoxelBreak(voxelSoundMaterial(type), (float)x, (float)y, (float)z);
#endif
    };
#endif
#if defined(VOXEL_HAVE_PLAYER) && defined(VOXEL_HAVE_AUDIO)
    // Placing is the mirror image: the controller says a voxel changed, the host decides that it
    // changed loudly enough to hear. A bucket is the exception - water arriving is a splash, not a
    // block arriving, and the file it plays has no material in it at all.
    blockPlacedFn = [](int x, int y, int z, uint16_t type) {
        if (type == BLOCK_WATER || type == BLOCK_LAVA)
            playVoxelSplash((float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f);
        else
            playVoxelPlace(voxelSoundMaterial(type), (float)x, (float)y, (float)z);
    };
#endif
#if defined(VOXEL_HAVE_PLAYER) && defined(VOXEL_HAVE_DROP)
    // A mined block leaves the world through this hook and comes back through the collector under
    // it: the item is a thing on the ground for a second, not a number in a hand. Neither closure
    // captures anything, because both are handed to a file-scope function pointer that outlives the
    // world they were installed for - the host is reached by name, and a rebuilt world finds the
    // drop pool rebound to its own scene on the first call.
    itemDropSpawner = [](VoxelWorld& w, float x, float y, float z, uint16_t type, int count) {
        spawnVoxelDrop(w, x, y, z, type, count);
    };
    // The hand answers for the inventory, and a false answer is the item staying where it lies. That
    // is the whole reason the two are separated: "the hotbar is full" becomes a thing you can look at
    // instead of a block that quietly disappeared at the moment you mined it.
    dropCollectFn = [](uint16_t type, int count, float, float, float) {
        VoxelGame* g = VoxelGame::current();
        VoxelPlayerScript* p = g ? g->getPlayer() : nullptr;
        return p && p->addToHotbar(type, count);
    };
#endif
#ifdef VOXEL_HAVE_AUDIO
    attachVoxelAudio(scene);
#endif
#ifdef VOXEL_HAVE_LIFE
    // A new world is a new body. The spawn column belongs to the seed that made it, the stride has
    // to be re-anchored against the position the player is teleported to, and a player who died in
    // the previous world must not be left waiting for a respawn into a column that no longer exists.
    // The bars come back full because surviving a world reset is not a thing this game pretends.
    lifePrevValid = false;
    lifeSpawnSet = false;
    lifeAwaitingRespawn = false;
    setVoxelVitals(VoxelVitals{});
#endif

    // A chunk mesh is only as bright as the sun it was baked under, so the bake has to know what
    // the sky is doing before the first column is built.
#ifdef VOXEL_HAVE_LIGHT
    dayTime = startDaylight;
    dayLight = daylightFactor(dayTime);
#endif
    setMeshDaylight(dayLight);
    bakedDaylight = dayLight;

    buildHud();
    ready = true;
}

void VoxelGame::rebuildWorld() {
    if (!ready) return;
    world.shutdown();
#ifdef VOXEL_HAVE_NET
    stopVoxelNet();
#endif
    ready = false;
    seedValue = (int)(Engine::getSystemTime() * 1000.0);
    initWorld();
    Log::print("VoxelWorld: regenerated with seed %d", seedValue);
}

// A preset rewrites the four anchor offsets on every layout pass, so a widget that has to keep a
// distance from an edge cannot use one: points and offsets are set together here, and the offsets
// carry the box itself, because the layout derives width/height from them.
static void anchorBox(UILayout* t, float pl, float pt, float pr, float pb, int left, int top, int right,
                      int bottom) {
    t->setAnchorPoints(pl, pt, pr, pb);
    t->setAnchorOffsets(left, top, right, bottom);
}

// setPivotCentered() is not in-box centring: it shifts the run left by half the layout box, which is
// whatever the offsets said, not the text. So a label is centred by measuring the advance the font
// reports for its own characters and re-anchoring the box around the wanted centre. Block names are
// ASCII, so one codepoint per byte here.
static void anchorCentered(Text* t, int xmid, int top, int bottom, const std::string& s) {
    float advance = 0.0f;
    for (size_t i = 0; i < s.size(); i++) advance += t->getCharWidth((uint32_t)(unsigned char)s[i]);

    const int half = (int)(advance * 0.5f) + 1;
    anchorBox(t, 0.5f, 1.0f, 0.5f, 1.0f, xmid - half, top, xmid + half, bottom);
}

void VoxelGame::buildHud() {
    if (hudScene || overlay) return;

    hudScene = new Scene();
    // The layer has no clickable widget, and its camera is a UI camera, which is what makes a
    // scene claim pointer events by default. The world scene must keep them.
    hudScene->setEnableUIEvents(UIEventState::DISABLED);
    Engine::addSceneLayer(hudScene);

    Scene* hud = hudScene;

    // The debug block sits over sky, leaves and stone in turn and a 16px glyph has no outline here,
    // so the same text is unreadable against a bright backdrop half the time. The panel texture is a
    // solid white square and the widget multiplies it by its own colour, which makes a translucent
    // plate cost one 4x4 PNG instead of a shipped GUI atlas.
    static const char* const HUD_PANEL = "textures/voxel/panel.png";

    overlayBg = new Image(hud);
    overlayBg->setTexture(HUD_PANEL);
    overlayBg->setColor(0.0f, 0.0f, 0.0f, 0.45f);
    anchorBox(overlayBg, 0.0f, 0.0f, 0.0f, 0.0f, 4, 4, 540, 276);

    // One plate per hotbar cell, drawn before the digits so the frames sit behind them. The cells are
    // bottom-centred as a group: 9 x 44px, so the strip runs from -198 to +198 around the centre.
    slotCells.resize(9);
    for (int i = 0; i < 9; i++) {
        Image* cell = new Image(hud);
        cell->setTexture(HUD_PANEL);
        cell->setColor(0.0f, 0.0f, 0.0f, 0.42f);
        const int left = -198 + i * 44;
        anchorBox(cell, 0.5f, 1.0f, 0.5f, 1.0f, left, -92, left + 42, -44);
        slotCells[i] = cell;
    }

    overlay = new Text(hud);
    overlay->setFontSize(16);
    overlay->setColor(1.0f, 1.0f, 1.0f, 1.0f);
    overlay->setMultiline(true);
    // The debug block is rebuilt every frame and is well past the component's 100-char default; the
    // growth path logs a warning and reallocs the text buffer each time it has to raise the cap.
    overlay->setMaxTextSize(1024);
    anchorBox(overlay, 0.0f, 0.0f, 0.0f, 0.0f, 12, 12, 532, 272);

    crosshair = new Text(hud);
    crosshair->setFontSize(24);
    crosshair->setColor(1.0f, 1.0f, 1.0f, 0.85f);
    crosshair->setAnchorPreset(AnchorPreset::CENTER);
    crosshair->setText("+");
    crosshair->createText();

    title = new Text(hud);
    title->setFontSize(18);
    title->setColor(1.0f, 1.0f, 1.0f, 0.95f);
    title->setMultiline(false);
    anchorCentered(title, 0, -124, -96, "");

    // Creative palette: the hotbar the player script has nothing to fill until it owns one, so the
    // game host picks the starter set and the selected slot is what placeBlock gets.
    static const char* kPalette[9] = {
        "grass", "dirt", "stone", "planks_oak", "log_oak",
        "glass", "torch", "glowstone", "stone_slab"
    };
    slots.resize(9);
    for (int i = 0; i < 9; i++) {
        Text* slot = new Text(hud);
        slot->setFontSize(14);
        slot->setColor(0.85f, 0.85f, 0.85f, 0.9f);
        paletteIds[i] = blockIdByName(kPalette[i]);
        slot->setText(std::to_string(i + 1));
        slot->createText();
        // The engine never sizes a layout box to the text run, so the digit is centred inside its
        // own cell by re-anchoring from the advance the font actually reported.
        anchorCentered(slot, -198 + i * 44 + 21, -88, -48, std::to_string(i + 1));
        slots[i] = slot;
    }

#ifdef VOXEL_HAVE_UI
    // The pack's own art replaces the plates this layer was authored from: the vitals row is new,
    // the nine hotbar cells are the same boxes re-bound to `container/slot.png`, and the modal
    // widgets are built now so opening a menu later is one call and no allocation. Every one of
    // these answers "no" when the pack is not on disk, and the plates above stay exactly as written.
    uiAttach(hud);
    uiBuildVitals();
    uiBindHotbar(slotCells.data(), (int)slotCells.size(), nullptr, -1);
#endif
}

void VoxelGame::updateHud() {
    if (!overlay) return;

    char line[256];
    std::string text;

    std::snprintf(line, sizeof(line), "VoxelWorld  seed %d", seedValue);
    text += line;
    std::snprintf(line, sizeof(line), "\nfps %.0f   frame %.1f ms", (double)Engine::getFramerate(),
                  (double)Engine::getDeltatime() * 1000.0);
    text += line;
    std::snprintf(line, sizeof(line), "\nchunks %zu   gen %u/%.1fms   mesh %u/%.1fms",
                  world.loadedCount(), world.chunksGeneratedThisFrame, world.lastGenMs,
                  world.chunksMeshedThisFrame, world.lastMeshMs);
    text += line;
    std::snprintf(line, sizeof(line), "\ntriangles %llu", (unsigned long long)world.totalTriangles());
    text += line;

    float px = 0.0f, py = 0.0f, pz = 0.0f;
    if (playerPosition(px, py, pz)) {
        std::snprintf(line, sizeof(line), "\npos %.1f %.1f %.1f   biome %s", px, py, pz,
                      biomeName(world.getBiome((int)px, (int)pz)));
        text += line;
    }

    std::snprintf(line, sizeof(line), "\nsun %.2f   daylight %.2f (baked %.2f)",
                  dayTime, dayLight, bakedDaylight);
    text += line;

#ifdef VOXEL_HAVE_ENTITY
    std::snprintf(line, sizeof(line), "\n%s", mobStats(world).c_str());
    text += line;
#endif
#ifdef VOXEL_HAVE_WATER
    std::snprintf(line, sizeof(line), "\n%s", waterStats(world).c_str());
    text += line;
#endif
#ifdef VOXEL_HAVE_SAVE
    std::snprintf(line, sizeof(line), "\n%s", saveStats(world).c_str());
    text += line;
#endif
#ifdef VOXEL_HAVE_NET
    std::snprintf(line, sizeof(line), "\n%s", netStats().c_str());
    text += line;
#endif
#ifdef VOXEL_HAVE_AUDIO
    // A sound module cannot be checked by looking at a frame, so the overlay reports what it is
    // doing: whether it is off, whether it ever attached, and whether the pack it names is on disk.
    std::snprintf(line, sizeof(line), "\n%s", audioStats().c_str());
    text += line;
#endif
#ifdef VOXEL_HAVE_LIFE
    // The numbers behind the vitals row, printed as well: a sprite that cannot be read on a headless
    // run and a plate that can are two different proofs, and food falling, water falling faster and a
    // drop costing health have to be visible in the log for the row above it to be trusted.
    std::snprintf(line, sizeof(line), "\n%s", lifeStats().c_str());
    text += line;
#endif
#ifdef VOXEL_HAVE_DROP
    // What the ground holds and what happened to it. The four totals are the only way to see the
    // module working without mining forty blocks and counting cubes by eye - and "lost" is the one
    // that says the pool filled up rather than the items being taken.
    std::snprintf(line, sizeof(line), "\n%s", dropStats().c_str());
    text += line;
#endif

    // The controller's inventory wins: the number keys and the wheel move its slots, so a hotbar
    // that kept drawing the host's palette would lie about what is in hand.
    uint16_t shown[9];
    int shownSelected = selectedSlot;
    for (int i = 0; i < 9; i++) shown[i] = paletteIds[i];
    // Without the player module the host owns the body and there is exactly one rule set, the one
    // where the world hurts - which is also the only one its [G]/[H] keys mean anything in.
    int viewMode = voxel::VOXEL_MODE_SURVIVAL;
    const char* modeKeys = "";
#ifdef VOXEL_HAVE_PLAYER
    if (VoxelPlayerScript* p = playerScript()) {
        for (int i = 0; i < 9; i++) shown[i] = p->hotbar[i].type;
        shownSelected = p->selectedSlot;
        viewMode = p->gameMode();
        // A run pinned by VOXEL_MODE prints a refusal when [M] is pressed, so the manual must not
        // promise a switch the environment overrules.
        modeKeys = p->modeIsPinned() ? "" : "  [M] mode";
    }
#endif
    if (shownSelected < 0 || shownSelected >= 9) shownSelected = 0;
    const bool spectatorView = viewMode == voxel::VOXEL_MODE_SPECTATOR;

    // The keys named here are the ones the compiled modules read this frame: R is the controller's
    // tool tier when the player is in the build and a new world only when the host still owns input
    // (handleKeys guards the same #ifdef), and a spectator steers with the look plus two lift keys.
    if (spectatorView)
        std::snprintf(line, sizeof(line),
                      "\nspectator   [F3] overlay  [mouse] look  [space] up  [ctrl] down%s", modeKeys);
    else {
#ifdef VOXEL_HAVE_PLAYER
        const char* rKey = "[R] tool";
#else
        const char* rKey = "[R] new world";
#endif
        std::snprintf(line, sizeof(line), "\nheld: %s   [F3] overlay  %s  [mouse] look  [1-9]%s",
                      block(shown[shownSelected]).name ? block(shown[shownSelected]).name : "?",
                      rKey, modeKeys);
    }
    text += line;
#ifdef VOXEL_HAVE_LIFE
    // A key this build does not implement must not be advertised: the plate is the whole manual the
    // player gets, and it is written from the modules that were actually compiled in. The same rule
    // covers a key the mode refuses - outside survival the vitals module takes no food and no water.
    if (viewMode == voxel::VOXEL_MODE_SURVIVAL) text += "  [G] eat  [H] drink";
#endif
#if defined(VOXEL_HAVE_PLAYER) && defined(VOXEL_HAVE_DROP)
    // The key belongs to the controller and the item to the drop module: a build without either has
    // no [J] to promise. A spectator has no hand to empty, so it does not get the row either.
    if (!spectatorView) text += "  [J] drop";
#endif

    if (text != overlayText) {
        overlayText = text;
        overlay->setText(overlayText);
        overlay->createText();

        // The plate is measured from the block it backs. The debug lines come and go with the modules
        // compiled in and the seed/pos lines change width as the player walks, so a fixed box is
        // either a grey band across the world or too short for the line it has to make readable.
        int lines = 1;
        float row = 0.0f, widest = 0.0f;
        for (size_t i = 0; i < overlayText.size(); i++) {
            const char c = overlayText[i];
            if (c == '\n') {
                if (row > widest) widest = row;
                row = 0.0f;
                lines++;
                continue;
            }
            // The advance is fractional at this size: truncating per glyph loses about a third of a
            // pixel a character, which is a whole line's worth of under-measured width by the end.
            row += overlay->getCharWidth((uint32_t)(unsigned char)c);
        }
        if (row > widest) widest = row;
        const int w = (int)(widest + 0.5f);

        int lineHeight = overlay->getLineHeight();
        if (lineHeight <= 0) lineHeight = (int)overlay->getFontSize() + 1;
        anchorBox(overlayBg, 0.0f, 0.0f, 0.0f, 0.0f, 4, 4, 12 + w + 8, 12 + lines * lineHeight + 6);
    }

    for (size_t i = 0; i < slots.size(); i++) {
        if (!slots[i]) continue;
        if (spectatorView) {
            // A spectator holds nothing, so the strip would be a row of blocks the mode cannot place
            // and a highlight that cannot move. Alpha rather than a teardown: the frame that leaves
            // spectator repaints the same widgets, with the same labels, straight back.
            slots[i]->setAlpha(0.0f);
            if (i < slotCells.size() && slotCells[i]) slotCells[i]->setAlpha(0.0f);
            continue;
        }
        const bool selected = ((int)i == shownSelected);
        std::string label = std::to_string((int)i + 1);
        if (selected && block(shown[i]).name) label += "\n";
        slots[i]->setText(label);
        // 0.75 rather than a mid grey: the unselected digits have to stay behind the selected one and
        // still be legible over a plate that is itself only 42 % opaque.
        slots[i]->setColor(selected ? 1.0f : 0.75f, selected ? 1.0f : 0.75f, selected ? 1.0f : 0.75f, 1.0f);
        slots[i]->createText();
        // The frame is the only selection cue the HUD has: the digit alone does not survive the
        // brighter text colour at 14px, and a warm plate reads as "this one" at a glance. Once the
        // cell carries the pack's sprite the same cue has to be a shade of the sprite itself,
        // because a colour multiply on real art is a tinted icon, not a highlight.
        if (i < slotCells.size() && slotCells[i]) {
#ifdef VOXEL_HAVE_UI
            if (uiHotbarBound())
                slotCells[i]->setColor(selected ? 1.0f : 0.62f, selected ? 1.0f : 0.62f,
                                       selected ? 1.0f : 0.62f, 1.0f);
            else
#endif
            if (selected) slotCells[i]->setColor(0.82f, 0.62f, 0.24f, 0.72f);
            else slotCells[i]->setColor(0.0f, 0.0f, 0.0f, 0.42f);
        }
    }

#ifdef VOXEL_HAVE_UI
    // The vitals row reads the same numbers the text plate prints, and hides the air column when
    // the head is out of liquid - the row is the underwater answer, not a fourth bar. A spectator
    // has no body to hurt, so the whole row goes away the way the hotbar strip does.
    //
    // The row needs the vitals module: a build with the art and no life rules has nothing to draw.
#if defined(VOXEL_HAVE_LIFE)
    {
        const bool vitalsVisible = !spectatorView;
        const VoxelVitals v = voxelVitals();
        // The bubble column is the underwater answer and the eye is the only thing that knows it: the
        // controller tests that same flag every frame for the breath rules, so the row reads it
        // instead of guessing from a breath value that is still climbing after a surfacing.
        bool headUnder = v.breath < voxel::BREATH_MAX;
#ifdef VOXEL_HAVE_PLAYER
        if (VoxelPlayerScript* p = playerScript()) headUnder = p->eyeInWater;
#endif
        uiSetVitals(vitalsVisible ? v.health : 0.0f, vitalsVisible ? v.hunger : 0.0f,
                    vitalsVisible ? v.breath : 0.0f, vitalsVisible && headUnder);
    }
#endif
#endif

    // The plus marks the voxel an edit would land on. A spectator has no edit, so the reticle would
    // be promising an action the mode refuses - the same lie as the strip above it.
    if (crosshair) crosshair->setAlpha(spectatorView ? 0.0f : 0.85f);

    if (title) {
        std::string look;
#ifdef VOXEL_HAVE_COLLIDE
        float dx = 0, dy = 0, dz = 0;
        if (lookDirection(dx, dy, dz)) {
            VoxelHit h = raycastVoxel(world, px, py, pz, dx, dy, dz, reach, false);
            if (h.hit) look = block(h.id).name ? block(h.id).name : "block";
        }
#endif
#ifdef VOXEL_HAVE_PLAYER
        // The controller raycasts every frame for break/place; its answer is the one that matches
        // what a click would do, including the blocks the item cannot target.
        if (VoxelPlayerScript* p = playerScript()) {
            if (p->targetValid) look = p->targetName;
        }
#endif
        if (look != lastLookText) {
            lastLookText = look;
            title->setText(look);
            title->createText();
            anchorCentered(title, 0, -124, -96, look);
        }
    }
}

void VoxelGame::setupPlayerCamera() {
    if (!scene) return;
    Entity cam = scene->getCamera();
    if (cam == NULL_ENTITY) return;

    Transform& t = scene->getComponent<Transform>(cam);
    if (!snapToSurface) return;

    // Drop the camera just above the highest loaded column, so the first frame is not underwater
    // or inside a mountain on a seed nobody has walked yet.
    int wx = (int)std::floor(t.position.x);
    int wz = (int)std::floor(t.position.z);
    int top = world.getTop(wx, wz);
    if (top >= 0) t.position = Vector3((float)wx + 0.5f, (float)(top + 2), (float)wz + 0.5f);
}

// The engine keeps every C++ script instance on the entity's ScriptEntry::instance, so the HUD can
// read the controller's inventory without the controller having to publish a singleton.
VoxelPlayerScript* VoxelGame::playerScript() {
#ifdef VOXEL_HAVE_PLAYER
    if (player) return player;
    if (!scene) return nullptr;
    for (Entity e : scene->getEntityList()) {
        ScriptComponent* sc = scene->findComponent<ScriptComponent>(e);
        if (!sc) continue;
        for (const ScriptEntry& entry : sc->scripts) {
            if (entry.className == "VoxelPlayerScript" && entry.instance) {
                player = reinterpret_cast<VoxelPlayerScript*>(entry.instance);
                // A controller that spawned with an empty hand has nothing to place, and placing is
                // what the whole hotbar is for; the host's palette is the creative loadout.
                if (paletteIds[0]) player->givePalette(paletteIds, 9);
                return player;
            }
        }
    }
#endif
    return nullptr;
}

bool VoxelGame::playerPosition(float& x, float& y, float& z) const {
    if (!scene) return false;
    Entity cam = scene->getCamera();
    if (cam == NULL_ENTITY) return false;
    const Transform& t = scene->getComponent<Transform>(cam);
    x = t.position.x; y = t.position.y; z = t.position.z;
    return true;
}

bool VoxelGame::lookDirection(float& dx, float& dy, float& dz) const {
    if (!scene) return false;
    Entity cam = scene->getCamera();
    if (cam == NULL_ENTITY) return false;
    const CameraComponent& cc = scene->getComponent<CameraComponent>(cam);
    // The engine fills this field from the third column of the view matrix, which is the camera's own
    // +Z axis - the direction the frame is looked *down*, and what the shaders want for a view vector.
    // A ray from the eye has to go the other way, so it comes back negated: without that the block
    // named above the crosshair is the one behind the body.
    Vector3 d = -cc.worldDirection;
    dx = d.x; dy = d.y; dz = d.z;
    return (dx * dx + dy * dy + dz * dz) > 0.0001f;
}

void VoxelGame::updateDayNight(float dt) {
    if (dayLength > 1.0f) {
        dayTime += dt / dayLength;
        if (dayTime >= 1.0f) dayTime -= 1.0f;
        if (dayTime < 0.0f) dayTime += 1.0f;
    }

#ifdef VOXEL_HAVE_LIGHT
    dayLight = daylightFactor(dayTime);
#else
    float elev = -std::cos(dayTime * 2.0f * VOXEL_PI);
    dayLight = elev < 0.0f ? 0.06f : 0.06f + 0.94f * elev;
#endif

    if (!scene) return;

    // One arc for both bodies: below the horizon the same direction becomes moonlight, which is why
    // the intensity keeps a floor instead of going to zero at midnight.
    float a = (dayTime - 0.25f) * 2.0f * VOXEL_PI;
    float y = std::sin(a);
    Vector3 sunPos(std::cos(a) * 0.75f, std::fabs(y) + 0.08f, 0.28f);
    sunPos = sunPos.normalized();

    if (sun) {
        sun->setDirection(sunPos * -1.0f);
        float warm = 1.0f - std::fabs(y);
        Vector3 c(sunDayColor.x + (sunDuskColor.x - sunDayColor.x) * warm,
                  sunDayColor.y + (sunDuskColor.y - sunDayColor.y) * warm,
                  sunDayColor.z + (sunDuskColor.z - sunDayColor.z) * warm);
        if (y < 0.0f) c = Vector3(0.45f, 0.52f, 0.72f);   // moonlight, blue and faint
        sun->setColor(c);
        sun->setIntensity(0.10f + dayLight * dayLight * 2.4f);
    }

    if (fog) {
        // The fog is the horizon curtain: it ends where the view radius runs out, so a chunk
        // appearing at the edge of the world is already fogged when it is drawn.
        float far = (float)(world.viewRadius * CHUNK_SX) * 1.35f;
        Vector3 day(0.62f, 0.78f, 0.95f);
        Vector3 night(0.035f, 0.05f, 0.09f);
        Vector3 dusk(0.85f, 0.5f, 0.3f);
        float warm = 1.0f - std::fabs(y);
        Vector3 c(day.x + (dusk.x - day.x) * warm * 0.7f,
                  day.y + (dusk.y - day.y) * warm * 0.7f,
                  day.z + (dusk.z - day.z) * warm * 0.7f);
        c = c * dayLight + night * (1.0f - dayLight);
        fog->setColor(c);
        fog->setLinearStartEnd(far * 0.45f, far);
    }

    rebakeDaylight(dayLight);
}

void VoxelGame::rebakeDaylight(float daylight) {
    // Voxel light is baked into vertex colour, so day/night cannot be a uniform. Re-baking every
    // chunk every frame would eat the whole mesh budget; re-baking only when the sun really moved
    // keeps the cost to the transition windows, and the streaming budget turns it into a rolling
    // wave of light instead of one long frame.
    if (bakedDaylight >= 0.0f && std::fabs(daylight - bakedDaylight) < 0.08f) return;
    bakedDaylight = daylight;
    setMeshDaylight(daylight);

    for (int dz = -world.viewRadius; dz <= world.viewRadius; dz++)
        for (int dx = -world.viewRadius; dx <= world.viewRadius; dx++)
            world.requestRemesh(world.focusChunkX() + dx, world.focusChunkZ() + dz);
}

void VoxelGame::handleKeys() {
    static bool prevF3 = false;
    bool f3 = Input::isKeyPressed(D_KEY_F3);
    if (f3 && !prevF3) {
        showDebug = !showDebug;
        if (overlay) overlay->setAlpha(showDebug ? 1.0f : 0.0f);
    }
    prevF3 = f3;

#ifndef VOXEL_HAVE_PLAYER
    // Without the player module the host has to own whatever input its HUD still shows. With it,
    // these keys belong to the controller: R is its tool cycle, 1-9 and the wheel its hotbar.
    static bool prevR = false;
    bool r = Input::isKeyPressed(D_KEY_R);
    if (r && !prevR) rebuildWorld();
    prevR = r;

    static bool prevDigits[9] = {false,false,false,false,false,false,false,false,false};
    for (int i = 0; i < 9; i++) {
        bool down = Input::isKeyPressed(D_KEY_1 + i);
        if (down && !prevDigits[i]) selectedSlot = i;
        prevDigits[i] = down;
    }

    static bool prevScroll = false;
    Vector2 scroll = Input::getMouseScroll();
    bool scrolled = scroll.y != 0.0f;
    if (scrolled && !prevScroll) {
        selectedSlot += (scroll.y > 0.0f ? 1 : -1);
        if (selectedSlot < 0) selectedSlot += 9;
        if (selectedSlot > 8) selectedSlot -= 9;
    }
    prevScroll = scrolled;
#endif
}

// A headless run has to prove what the GPU holds, not what the mesher counted: the triangle number
// the world reports comes from the local accumulator, so a chunk whose upload silently did nothing
// would still be counted. This reads the mesh components back off the entities.
static void logChunkGpuState(Scene* scene, VoxelWorld& world) {
    if (!scene) return;
    int withGeo = 0, empty = 0, hidden = 0, unloaded = 0;
    unsigned long long verts = 0;
    std::string sample;
    // The two meshes are separate entities because one is opaque and the other blends, so their
    // health has to be counted apart: a frame of pure sand is either "the blocks never meshed" or
    // "the liquid never reached the GPU", and only the second number tells them apart.
    int liq = 0, liqGeo = 0, liqHidden = 0, liqUnloaded = 0;
    unsigned long long liqVerts = 0;
    std::string liqSample;
    for (Entity e : scene->getEntityList()) {
        const std::string name = scene->getEntityName(e);
        const bool liquidMesh = name.compare(0, 10, "VoxelWater") == 0;
        if (!liquidMesh && name.compare(0, 10, "VoxelChunk") != 0) continue;
        MeshComponent* mc = scene->findComponent<MeshComponent>(e);
        if (!mc) { if (!liquidMesh) empty++; continue; }
        if (mc->vertexCount == 0) empty++;
        else { withGeo++; verts += mc->vertexCount; }
        if (!mc->loaded) unloaded++;
        Transform* t = scene->findComponent<Transform>(e);
        if (t && !t->visible) hidden++;
        if (sample.empty() && mc->vertexCount)
            sample = name + " " + std::to_string(mc->vertexCount) + "v";
        if (liquidMesh) {
            liq++;
            if (mc->vertexCount) {
                liqGeo++;
                liqVerts += mc->vertexCount;
                if (liqSample.empty()) liqSample = name + " " + std::to_string(mc->vertexCount) + "v";
            }
            if (t && !t->visible) liqHidden++;
            if (!mc->loaded) liqUnloaded++;
        }
    }
    Log::print("VoxelWorld: gpu geo %d empty %d hidden %d unloaded %d verts %llu (e.g. %s)",
               withGeo, empty, hidden, unloaded, verts, sample.c_str());
    Log::print("VoxelWorld: liquid meshes %d withGeo %d hidden %d unloaded %d verts %llu (e.g. %s)",
               liq, liqGeo, liqHidden, liqUnloaded, liqVerts, liqSample.c_str());

    // The picture alone cannot say whether an empty frame is "no geometry" or "geometry out of
    // view", so the capture also states where the camera points and how high the ground under it
    // is. A pitch that is not what the controller wrote is the difference between the two.
    if (scene) {
        const Entity cam = scene->getCamera();
        if (cam != NULL_ENTITY) {
            const Transform& t = scene->getComponent<Transform>(cam);
            const int cx = (int)std::floor(t.position.x), cz = (int)std::floor(t.position.z);
            Log::print("VoxelWorld: cam %.1f %.1f %.1f rot %.3f %.3f %.3f %.3f  top(%d,%d)=%d "
                       "neighbours %d %d %d %d",
                       (double)t.position.x, (double)t.position.y, (double)t.position.z,
                       (double)t.rotation.w, (double)t.rotation.x, (double)t.rotation.y,
                       (double)t.rotation.z, cx, cz, world.getTop(cx, cz),
                       world.getTop(cx + 4, cz), world.getTop(cx - 4, cz),
                       world.getTop(cx, cz + 4), world.getTop(cx, cz - 4));

            // The column heights said water, the picture said sand, so the only way to settle it is
            // to print the blocks themselves: a vertical slice of the world straight ahead, one row
            // per height, '~' for a liquid and the block's own initial for everything solid.
            const Vector3 fwd = t.rotation * Vector3(0, 0, -1);
            const int stepX = (int)std::lroundf(fwd.x), stepZ = (int)std::lroundf(fwd.z);
            auto glyph = [](uint16_t id) {
                const uint16_t type = stateType(id);
                if (type == BLOCK_AIR) return '.';
                const BlockDef& d = block(type);
                // A liquid's drawn height is its level, so the slice has to say which one it holds:
                // '~' is a full source (surface at 15/16 of the voxel), '=' is falling and '-' is the
                // 1/8 sheet a level-0 id paints. A sea written as bare ids reads as a row of '-' and
                // is invisible behind its own shore, which is exactly what a flat picture cannot show.
                if (d.flags & FLAG_LIQUID) {
                    const int level = state::waterLevel(id);
                    return level >= 15 ? '~' : (level > 0 ? '=' : '-');
                }
                if (!(d.flags & FLAG_SOLID)) return ',';
                return (char)(d.name[0] >= 'a' ? d.name[0] - 32 : d.name[0]);
            };
            Log::print("VoxelWorld: slice ahead of %d,%d (rows are heights, '.' air, '~' full water, "
                       "'=' falling, '-' level-0 sheet):", cx, cz);
            for (int y = 46; y >= 38; y--) {
                char row[32];
                int used = std::snprintf(row, sizeof(row), "  y%2d ", y);
                for (int d = 0; d < 14 && used < (int)sizeof(row) - 2; d++)
                    used += std::snprintf(row + used, sizeof(row) - used, "%c",
                                          glyph(world.get(cx + stepX * d, y, cz + stepZ * d)));
                Log::print("%s", row);
            }

            // The mesher lowers a liquid by its level, so the number that decides whether the sea is
            // seen or buried is the drawn surface against the shore's own top. Printed next to the
            // slice because a picture that shows only sand cannot tell "not drawn" from "drawn under
            // the lip of the basin it sits in".
            int firstY = -1, firstD = -1;
            uint16_t firstId = BLOCK_AIR;
            for (int d = 0; d < 14 && firstY < 0; d++) {
                for (int y = 46; y >= 38; y--) {
                    const uint16_t id = world.get(cx + stepX * d, y, cz + stepZ * d);
                    if (!(block(stateType(id)).flags & FLAG_LIQUID)) continue;
                    firstY = y; firstD = d; firstId = id;
                    break;
                }
            }
            if (firstY >= 0) {
                const int level = state::waterLevel(firstId);
                const float surface = (float)firstY + 0.125f + 0.8125f * ((float)level / 15.0f);
                Log::print("VoxelWorld: nearest liquid at %d,%d,%d id 0x%04X level %d -> drawn surface y%.2f; "
                           "eye y%.2f, ground here %d, ground there %d",
                           cx + stepX * firstD, firstY, cz + stepZ * firstD, (unsigned)firstId, level,
                           (double)surface, (double)t.position.y, world.getTop(cx, cz),
                           world.getTop(cx + stepX * firstD, cz + stepZ * firstD));
            } else {
                Log::print("VoxelWorld: no liquid in the 14 columns ahead");
            }
        }
    }
}

#ifdef VOXEL_HAVE_BREAK
// The crack and the chips. The controller only publishes a 0..1 progress and the coordinates of the
// block it is chewing on, so the host is the one that turns that number into a stage tile and into
// debris - which is also what lets a build without the FX module keep mining silently.
void VoxelGame::updateBreakFx(float dt) {
    Scene* fxScene = world.scene();
    if (!fxScene) return;
    if (!breakOverlay().attach(fxScene)) return;

    VoxelPlayerScript* p = playerScript();
    const int stage = (p && p->targetValid) ? breakStage(p->targetProgress) : -1;
    if (stage < 0) {
        breakOverlay().hide();
        chipTimer = 0.0;
        return;
    }
    breakOverlay().show(p->targetX, p->targetY, p->targetZ, stage);

    // A block that takes seconds to break throws chips the whole time, not one pop at the end.
    chipTimer += dt;
    if (chipTimer < BREAK_CHIP_SECONDS) return;
    chipTimer = 0.0;
    const uint16_t type = stateType(world.get(p->targetX, p->targetY, p->targetZ));
    if (type != BLOCK_AIR) spawnBreakDebris(fxScene, p->targetX, p->targetY, p->targetZ, type, 4);
#ifdef VOXEL_HAVE_AUDIO
    // The mining noise rides the chip puff rather than keeping its own clock: the sound of a pick
    // and the chips it throws are the same swing, and two timers would drift apart within a break.
    // A build without this module cannot show a crack, so it has nothing to time the dig to either.
    playVoxelDig(voxelSoundMaterial(type), (float)p->targetX, (float)p->targetY, (float)p->targetZ);
#endif
}
#endif

#ifdef VOXEL_HAVE_SELECT
// The frame around the block the crosshair is on. It reads the target the mining path already
// resolved and the shape table the player's body already collides with, so what is outlined is what
// would stop you - and a build without this module simply aims at a block with no frame on it.
void VoxelGame::updateSelectFx() {
    Scene* fxScene = world.scene();
    if (!fxScene) return;
    if (!selectOverlay().attach(fxScene)) return;

    VoxelPlayerScript* p = playerScript();
    if (!p || !p->targetValid) { selectOverlay().hide(); return; }
    selectOverlay().show(p->targetX, p->targetY, p->targetZ,
                         world.get(p->targetX, p->targetY, p->targetZ));
}
#endif

#ifdef VOXEL_HAVE_AUDIO
// The ear, once per frame, after the world and the player have both moved. attach() is idempotent
// and cheap, and calling it here rather than only in initWorld is what lets a scene teardown and a
// rebuild take the voices with them and get them back without a restart.
void VoxelGame::updateAudioFx() {
    if (!attachVoxelAudio(scene)) return;

    VoxelPlayerScript* p = playerScript();

    VoxelEar ear;
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    if (p) {
        ear.x = p->footX; ear.y = p->footY; ear.z = p->footZ;
        ear.grounded = p->grounded;
        ear.underwater = p->eyeInWater;
        ear.footMaterial = voxelSoundMaterial(p->footBlock);
        px = p->footX; pz = p->footZ;
    } else if (playerPosition(px, py, pz)) {
        // No controller: the camera is the only body in the world, so a step can only be struck
        // where the eye is. Without the player module nothing walks, so this is a position for the
        // one-shots to fall back on rather than a footstep source.
        ear.x = px; ear.y = py; ear.z = pz;
    }

    // Horizontal only: a drop of twenty blocks must not pay for twenty footsteps on the way down,
    // and the first frame after a spawn has no previous position to be measured against.
    if (audioPrevValid) {
        const float dx = px - audioPrevX;
        const float dz = pz - audioPrevZ;
        const float moved = std::sqrt(dx * dx + dz * dz);
        ear.moved = moved > 1.0f ? 1.0f : moved;    // a teleport is not a long stride
    }
    audioPrevX = px; audioPrevZ = pz; audioPrevValid = true;

    // Going into the water is an edge, not a level. A held dip should splash once, and climbing out
    // should not splash at all - the sound is the entry, not the boundary.
    const bool inWater = p && p->inWater;
    if (inWater && !audioPrevInWater) playVoxelSplash(ear.x, ear.y + 0.5f, ear.z);
    audioPrevInWater = inWater;

    ear.daylight = dayLight;
    updateVoxelAudio(ear);
}
#endif

#ifdef VOXEL_HAVE_LIFE
// A cactus is the only block in the table that hurts you for standing next to it, and the list is a
// list rather than a BlockDef flag because g_blocks is 59 positional aggregates: a new column has to
// be written out 59 times and a row that forgets it silently inherits the row above. Same reason the
// sound material and the food value live in module tables.
static bool isVoxelHazard(uint16_t type) {
    return type == BLOCK_CACTUS;
}

// The vitals move on the body the controller reports, once per frame after the world and the player
// have both moved. The module is arithmetic and never reads the world, so the host's whole job is the
// sample: where the feet are, what the eye is inside, which neighbour would hurt, and how many
// seconds the frame lasted.
void VoxelGame::updateLifeFx(float dt) {
    VoxelPlayerScript* p = playerScript();

    // ---- whose rules the bars are under ----
    // The controller owns the answer and the module is told, because "creative does not decay" is a
    // rule about the vitals: the module gates its own rates, harm and death on it, so a caller that
    // forgets - a later phase's mob bite, the eat key two screens down - cannot route around it.
    const int mode = p ? p->gameMode() : voxel::VOXEL_MODE_SURVIVAL;
    if (mode != lifeLastMode) {
        Log::print("VoxelMode: %s - %s", voxel::voxelGameModeName(mode),
                   mode == VOXEL_MODE_SURVIVAL   ? "the bars decay and the world hurts"
                 : mode == VOXEL_MODE_CREATIVE   ? "no decay, no harm, instant mining, free stacks"
                                                 : "no decay, no harm, no edits, no collision");
        // Switching out of survival stands a dead body up, so a respawn this host was waiting for has
        // nothing left to wait for. Asking the module rather than the flag is the point: the body came
        // back on the switch, and a pending flag for a living player is a lie about the state.
        if (lifeAwaitingRespawn && voxelAlive()) {
            lifeAwaitingRespawn = false;
            lifePrevValid = false;
            Log::print("VoxelLife: respawn dropped, because %s has nothing to recover from",
                       voxel::voxelGameModeName(mode));
        }
        lifeLastMode = mode;
    }
    setVoxelMode(mode);

    // A spectator has no feet in the world: the sample below would read the blocks a body that is not
    // there is standing in, and the "fall" it measures is a camera move. The bars keep what survival
    // left them at, which is the whole job until the mode changes back.
    if (mode == VOXEL_MODE_SPECTATOR) {
        lifePrevValid = false;
        return;
    }

    VoxelBody body;
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    if (p) {
        body.feetX = p->footX; body.feetY = p->footY; body.feetZ = p->footZ;
        px = p->footX; py = p->footY; pz = p->footZ;
        body.grounded = p->grounded;
        body.jumped = p->jumped;
        body.sprinting = p->sprinting;
        body.flying = p->isFlying();
        body.inWater = p->inWater;
        body.headUnder = p->eyeInWater;
    } else if (playerPosition(px, py, pz)) {
        // No controller: the camera is the only body in the world. It cannot walk, jump or swim, so
        // all the module gets from it is a position and whatever liquid the eye happens to be in.
        body.feetX = px; body.feetY = py; body.feetZ = pz;
    } else {
        // Nothing to move: a scene with no player has no vitals, and pretending otherwise would be a
        // clock counting down for a body that does not exist.
        return;
    }

    // The respawn column is the player's own, read once the world has streamed it. Asking the world
    // rather than the scene is the point: the authored transform is a position in a chunk that may
    // not exist yet, and a respawn aimed there would find no surface to stand on.
    if (!lifeSpawnSet) {
        const int sx = (int)std::floor(px);
        const int sz = (int)std::floor(pz);
        const int top = world.getTop(sx, sz);
        if (top >= 0) {
            setVoxelSpawn((float)sx + 0.5f, (float)(top + 1), (float)sz + 0.5f);
            lifeSpawnSet = true;
        }
    }

    // Horizontal only, and capped: a respawn moves the body the whole way back to the spawn column,
    // and that distance is not a stride worth of food.
    if (lifePrevValid) {
        const float dx = px - lifePrevX;
        const float dz = pz - lifePrevZ;
        const float moved = std::sqrt(dx * dx + dz * dz);
        body.moved = moved > 1.0f ? 1.0f : moved;
    }
    lifePrevX = px; lifePrevZ = pz; lifePrevValid = true;

    // What hurts: the liquids the controller already reports, plus the cells a body occupies and the
    // ring around them. A cactus you are standing next to pricks you; a lava you are knee-deep in
    // burns, which is why the feet cell is sampled and not only the chest.
    const int fx = (int)std::floor(body.feetX);
    const int fy = (int)std::floor(body.feetY);
    const int fz = (int)std::floor(body.feetZ);
    for (int oz = -1; oz <= 1; oz++) {
        for (int ox = -1; ox <= 1; ox++) {
            const uint16_t low = stateType(world.get(fx + ox, fy, fz + oz));
            const uint16_t high = stateType(world.get(fx + ox, fy + 1, fz + oz));
            if (low == BLOCK_LAVA || high == BLOCK_LAVA) body.inLava = true;
            if (isVoxelHazard(low) || isVoxelHazard(high)) body.touchingHazard = true;
        }
    }

    // [G] eats the block in hand, [H] drinks from the water in front of you. Edges, because a held
    // key would otherwise empty a stack - or a bar - in a second.
    const bool eatDown = Input::isKeyPressed(D_KEY_G);
    if (eatDown && !lifePrevEat && p && voxelAlive()) {
        VoxelPlayerScript::Slot& slot = p->hotbar[p->selectedSlot];
        const uint16_t foodType = stateType(slot.type);
        const char* name = block(foodType).name ? block(foodType).name : "?";
        const float nutrition = voxelFoodNutrition(foodType);
        const float before = hungerValue();
        if (nutrition <= 0.0f) {
            Log::print("VoxelLife: %s is not food", name);
        } else if (eatVoxelLife(nutrition)) {
            // The type is snapshotted before the stack is spent: a last bite clears the slot, and a
            // log that re-reads it afterwards would report the player eating air.
            if (!p->isCreative() && --slot.count <= 0) slot.type = 0;
            Log::print("VoxelLife: ate %s, food %.1f -> %.1f", name, before, hungerValue());
        } else {
            Log::print("VoxelLife: already full, %s kept", name);
        }
    }
    lifePrevEat = eatDown;

    const bool drinkDown = Input::isKeyPressed(D_KEY_H);
    if (drinkDown && !lifePrevDrink && voxelAlive()) {
        const bool targetIsWater = p && p->targetValid &&
            stateType(world.get(p->targetX, p->targetY, p->targetZ)) == BLOCK_WATER;
        if (body.headUnder || body.inWater || targetIsWater) {
            const float before = thirstValue();
            if (drinkVoxelLife(DRINK_PER_SIP))
                Log::print("VoxelLife: drank, water %.1f -> %.1f", before, thirstValue());
        } else {
            Log::print("VoxelLife: nothing to drink here");
        }
    }
    lifePrevDrink = drinkDown;

    updateVoxelLife(body, dt);

    int cause = DAMAGE_DIRECT;
    if (voxelJustDied(cause)) {
        float sx = px, sy = py, sz = pz;
        voxelSpawn(sx, sy, sz);
        Log::print("VoxelLife: died of %s at %.1f %.1f %.1f (food %.1f water %.1f), death %d - "
                   "respawning at %.1f %.1f %.1f", voxelDamageCauseName(cause), body.feetX,
                   body.feetY, body.feetZ, hungerValue(), thirstValue(), voxelDeaths(), sx, sy, sz);
        lifeRespawnX = sx; lifeRespawnZ = sz;
        lifeAwaitingRespawn = true;
        // The stride across the world is not a walk, and the fall the body died in is over: without
        // this the respawn pays for the drop on the way back up.
        lifePrevValid = false;
    }

    // A column that has not streamed has nowhere to stand, so the host asks again every frame rather
    // than dropping the body into ungenerated stone and calling it a respawn.
    if (lifeAwaitingRespawn && p && p->respawnAt(world, lifeRespawnX, lifeRespawnZ)) {
        reviveVoxelLife(p->footY);
        lifeAwaitingRespawn = false;
        lifePrevValid = false;
        Log::print("VoxelLife: up at %.1f %.1f %.1f with food %.1f and water %.1f", p->footX,
                   p->footY, p->footZ, hungerValue(), thirstValue());
    }
}
#endif

void VoxelGame::onUpdate() {
    if (!ready) initWorld();
    if (!ready) return;

    float dt = Engine::getDeltatime();

    handleKeys();

    float px = 0.0f, py = 0.0f, pz = 0.0f;
    bool havePlayer = playerPosition(px, py, pz);
    if (!havePlayer) { px = 0.0f; pz = 0.0f; }

    world.update(px, pz);

#ifndef VOXEL_HAVE_PLAYER
    // The controller spawns itself on the surface; the host only has to place a bare camera.
    if (!surfacePlaced && hudFrame == 2) {
        setupPlayerCamera();
        surfacePlaced = true;
    }
#endif

    updateDayNight(dt);

#ifdef VOXEL_HAVE_BREAK
    updateBreakFx(dt);
#endif
#ifdef VOXEL_HAVE_SELECT
    updateSelectFx();
#endif
#ifdef VOXEL_HAVE_AUDIO
    updateAudioFx();
#endif
#ifdef VOXEL_HAVE_LIFE
    // After the audio: the footstep and the vitals read the same frame's body, and the life module is
    // the one that may move it back to the spawn.
    updateLifeFx(dt);
#endif

#ifdef VOXEL_HAVE_ENTITY
    // A mob steps on the frame, not on a timer: one 1-second step would teleport it through the
    // ground probe and out of the water it was swimming in. Only the spawn retry is throttled,
    // because a column that has not streamed in yet has nowhere to stand.
    float mobX = px, mobZ = pz;
    updateVoxelMobs(world, dt, mobX, mobZ, (float)(world.viewRadius * CHUNK_SX + 16));
    updateFallingBlocks(world, dt);

    mobTimer += dt;
    if (mobTimer > 1.0f) {
        mobTimer = 0.0;
        int alive = aliveMobCount(world);
        if (alive < mobCount) {
            int want = mobCount - alive;
            if (want > 4) want = 4;
            mobSpawned += spawnVoxelMobs(world, mobX, mobZ, 40, want);
        }
    }
#endif

#ifdef VOXEL_HAVE_DROP
    // Seeded before the step below, so the frame the item appears is already the first one it falls.
    if (dropFrame > 0 && hudFrame == dropFrame) {
        dropFrame = 0;
        seedDrops();
    }
    // The same eye the mobs are measured from, and the reason the drops cannot simply take it: while
    // no body is in the world nothing may be pulled or collected. The magnet and the pickup test are a
    // distance to the chest, the despawn test is horizontal only, so a chest a kilometre under the
    // real one silences the first two and leaves the third alone - the items fall, turn, merge and
    // expire until somebody arrives to want them.
    updateVoxelDrops(world, dt, px, havePlayer ? py : py - 1000.0f, pz,
                     (float)(world.viewRadius * CHUNK_SX + 32));
#endif

#ifdef VOXEL_HAVE_NET
    float yaw = 0.0f;
    // VOXEL_NETEDIT: the one scripted block write, and the reason the protocol is testable without
    // two humans pressing keys. It waits for netReady() because the client's journal is not drained
    // before the handshake, so an edit written earlier simply never reaches the wire and a test would
    // measure a dropped packet instead of a working link. It also waits for a remote player to be
    // visible: a host that writes before anyone joins broadcasts to nobody, and the joiner then reads
    // that column from its own generation instead of from the wire. Finally it waits for the column to
    // exist here, which is what setBlockQuiet() answers "air, unchanged" for.
    if (netEditArmed && !netEditDone && havePlayer && hudFrame > 30 && netReady() &&
        !remotePlayers().empty()) {
        const int ex = (int)std::floor(px) + netEditOffset[0];
        const int ey = (int)std::floor(py) + netEditOffset[1];
        const int ez = (int)std::floor(pz) + netEditOffset[2];
        if (world.getTop(ex, ez) >= 0) {
            const uint16_t was = world.setBlock(ex, ey, ez, netEditId);
            netEditDone = true;
            Log::print("NET LOCAL EDIT %d %d %d: %d -> %d%s", ex, ey, ez, (int)was, (int)netEditId,
                       was == netEditId ? " (already there: nothing journaled)" : "");
        }
    }
    tickVoxelNet(world, dt, px, py, pz, yaw);
#endif

    // The HUD rebuilds text at 6 Hz: a per-frame rebuild of a multiline STB text run is real cost
    // and the numbers it shows do not change faster than that.
    hudFrame++;
    if (showDebug && (hudFrame % 10) == 0) updateHud();
#ifdef VOXEL_HAVE_BREAK
    // A picture of a crack is only evidence if the log says which stage it is holding: the same run
    // with the overlay on stage 3 and the overlay hidden produce images that differ by a few pixels.
    // The geometry fields settle "never loaded" apart from "loaded and drawn wrong": a mesh the
    // renderer refused to take stays invisible without printing anything at all.
    if (breakOverlay().isShowing() && (hudFrame % 10) == 0) {
        Log::print("VoxelBreak: stage %d on %d,%d,%d (writes %llu, debris bursts %llu, last points %d, "
                   "loaded %d verts %u bits 0x%08llX)",
                   breakOverlay().stage(), breakOverlay().blockX(), breakOverlay().blockY(),
                   breakOverlay().blockZ(), (unsigned long long)breakOverlay().stageWrites(),
                   (unsigned long long)breakDebrisSpawns(), breakDebrisLastPoints(),
                   breakOverlay().geometryLoaded() ? 1 : 0, breakOverlay().geometryVertices(),
                   breakOverlay().geometryShaderBits());
    }
#endif
#ifdef VOXEL_HAVE_SELECT
    // Which shape is on screen is a property of the block, not of the frame, so the log prints the
    // edge and box counts alongside the coordinates: a capture of a slab with 12 edges at half height
    // and a capture of a cube with 12 edges at full height are otherwise the same evidence.
    if (selectOverlay().isShowing() && (hudFrame % 10) == 0) {
        Log::print("VoxelSelect: %d edges of %d box on %d,%d,%d block %d (uploads %llu, moves %llu, "
                   "loaded %d verts %u bits 0x%08llX)",
                   selectOverlay().segments(), selectOverlay().boxes(), selectOverlay().blockX(),
                   selectOverlay().blockY(), selectOverlay().blockZ(), selectOverlay().blockId(),
                   (unsigned long long)selectOverlay().uploads(),
                   (unsigned long long)selectOverlay().moves(),
                   selectOverlay().geometryLoaded() ? 1 : 0, selectOverlay().geometryVertices(),
                   selectOverlay().geometryShaderBits());
    }
#endif
    if (captureCountdown < 0 && hudFrame == 1) {
        int frames = 0;
        std::string f;
        if (envValue("VOXEL_FRAMES", f) && !f.empty()) frames = std::atoi(f.c_str());
        std::string shot;
        if (envValue("VOXEL_SHOT", shot) && !shot.empty()) {
            capturePath = shot;
            captureCountdown = frames > 0 ? frames : 120;
            Log::print("VoxelWorld: capturing %s after %d frames", capturePath.c_str(), captureCountdown);
        } else if (frames > 0) {
            // VOXEL_FRAMES has to close the run on its own. A frame budget that only quits when a
            // picture was also asked for leaves every probe-only invocation burning CPU forever,
            // and the log then reads as a hang rather than as a harness that forgot to stop.
            captureCountdown = frames;
            Log::print("VoxelWorld: quitting after %d frames", captureCountdown);
        }
        std::string probe;
        if (envValue("VOXEL_PROBE", probe) && !probe.empty()) {
            probeFrame = std::atoi(probe.c_str());
            if (probeFrame < 2) probeFrame = 90;   // "1" means "run it", not "run it before the world exists"
        }
#ifdef VOXEL_HAVE_DROP
        // VOXEL_DROP=<count> puts items on the ground for the shutter to catch. The throw is timed
        // against the capture rather than against frame 1 because an item that was just dropped is
        // still mid-hop: ninety frames is one and a half seconds, enough to have landed and turned
        // a quarter of the way round.
        std::string drops;
        if (envValue("VOXEL_DROP", drops) && !drops.empty()) {
            dropSeed = std::atoi(drops.c_str());
            if (dropSeed < 1) dropSeed = 1;
            int lead = captureCountdown > 0 ? captureCountdown - 90 : 30;
            if (lead < 1) lead = 1;
            dropFrame = hudFrame + lead;
            Log::print("VoxelWorld: VOXEL_DROP will throw %d item(s) on frame %d", dropSeed, dropFrame);
        }
#endif
    }
    if (probeFrame > 0 && hudFrame == probeFrame) {
        probeFrame = 0;
        runProbes();
    }
    if (captureCountdown > 0) {
        captureCountdown--;
        if (captureCountdown == 0) {
            // A headless run has to prove its numbers, not just its picture: the same text the
            // overlay carries goes to the log, so triangles and frame time land in the capture log.
            updateHud();
            Log::print("%s", overlayText.c_str());
            logChunkGpuState(getScene(), world);
            // VOXEL_AERIAL=1 lifts the capture camera 40 blocks up and points it straight down. A
            // frame with terrain there but not at eye level says frustum/culling; empty in both says
            // the chunks are never drawn. Only a headless run can answer that here.
            if (!capturePath.empty()) {
                std::string aerial;
                if (envValue("VOXEL_AERIAL", aerial) && !aerial.empty() && getScene()) {
                    const Entity cam = getScene()->getCamera();
                    if (cam != NULL_ENTITY) {
                        Transform& t = getScene()->getComponent<Transform>(cam);
                        Quaternion q;
                        q.fromEulerAngles(-90.0f, 0.0f, 0.0f, RotationOrder::YXZ);
                        t.position = Vector3(t.position.x, t.position.y + 40.0f, t.position.z);
                        t.rotation = q;
                        Log::print("VoxelWorld: aerial capture from %.1f %.1f %.1f",
                                   (double)t.position.x, (double)t.position.y, (double)t.position.z);
                    }
                }
                // VOXEL_SHOTCAM="x y z yaw pitch" pins the capture eye. The start is no longer the thing
                // that moves between runs - the shore search now runs from the world origin, so one seed
                // stands on one beach every time - but a pinned eye still takes the player module out of
                // the frame, and a diff of two runs that stand in the same column answers "did this
                // change paint pixels" and nothing else. The position is the part that takes: the player
                // script writes the camera transform from its own look every update, so yaw and pitch
                // only land when there is no player module in the build.
                std::string shotcam;
                if (envValue("VOXEL_SHOTCAM", shotcam) && !shotcam.empty() && getScene()) {
                    float cx = 0, cy = 0, cz = 0, yaw = 0, pitch = 0;
                    std::istringstream in(shotcam);
                    if (in >> cx >> cy >> cz >> yaw >> pitch) {
                        const Entity cam = getScene()->getCamera();
                        if (cam != NULL_ENTITY) {
                            Transform& t = getScene()->getComponent<Transform>(cam);
                            t.position = Vector3(cx, cy, cz);
                            Quaternion q;
                            q.fromEulerAngles(pitch, yaw, 0.0f, RotationOrder::YXZ);
                            t.rotation = q;
                            Log::print("VoxelWorld: capture camera pinned to %.1f %.1f %.1f yaw %.1f pitch %.1f",
                                       (double)cx, (double)cy, (double)cz, (double)yaw, (double)pitch);
                        }
                    } else {
                        Log::warn("VoxelWorld: VOXEL_SHOTCAM wants five numbers (x y z yaw pitch), got '%s'",
                                  shotcam.c_str());
                    }
                }
                // VOXEL_NOWATER=1 hides every liquid mesh before the picture is taken. Diffing that
                // PNG against the same run without the knob says whether the water was in the frame
                // at all - the same question the eye cannot answer when the sea and the beach are
                // both pale and the mesh is one block wide.
#ifdef VOXEL_HAVE_DROP
                // Where the ground holds what. A capture of an item nobody can locate is not evidence,
                // so the live cubes' positions and their spin go in the log beside the camera's own.
                DropSnapshot snap;
                int shownDrops = 0;
                for (int i = 0; i < MAX_DROPS && shownDrops < 8; i++) {
                    if (!dropSnapshot(i, snap)) continue;
                    shownDrops++;
                    Log::print("VoxelWorld: drop %d at %.2f %.2f %.2f spin %.0f deg bob %.2f %s x%d %s",
                               i, (double)snap.x, (double)snap.y, (double)snap.z, (double)snap.spinDeg,
                               (double)snap.bobY, block(snap.type).name ? block(snap.type).name : "?",
                               snap.amount, snap.grounded ? "resting" : "falling");
                }
#endif
                std::string nowater;
                if (envValue("VOXEL_NOWATER", nowater) && !nowater.empty() && getScene()) {
                    int hidden = 0;
                    for (Entity e : getScene()->getEntityList()) {
                        const std::string name = getScene()->getEntityName(e);
                        if (name.compare(0, 10, "VoxelWater") != 0) continue;
                        Transform* wt = getScene()->findComponent<Transform>(e);
                        if (wt && wt->visible) { wt->visible = false; hidden++; }
                    }
                    Log::print("VoxelWorld: hid %d liquid meshes for the capture", hidden);
                }
                Engine::screenshotAfter(capturePath, 2);
                Log::print("VoxelWorld: screenshot queued -> %s", capturePath.c_str());
                // The read happens on the frame the buffer is submitted, so the quit must come after it.
                quitCountdown = 5;
            } else {
                quitCountdown = 1;
            }
        }
    }
    if (quitCountdown > 0 && --quitCountdown == 0) {
        if (!capturePath.empty()) Log::print("VoxelWorld: captured %s", capturePath.c_str());
        else Log::print("VoxelWorld: frame budget reached, quitting");
        System::instance().quit();
    }
}

#ifdef VOXEL_HAVE_DROP
// VOXEL_DROP: the items a headless capture gets to look at. Nothing presses the button in an
// automated run, and the module's own proof is a picture of a cube lying on the ground turning - so
// the host hands the same spawnVoxelDrop a mined block uses, laid out in a row across the view.
// Every kind of block comes from the palette the hotbar already shows, so the picture says "the drop
// is the block" for as many tiles as it can carry.
void VoxelGame::seedDrops() {
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    if (!playerPosition(px, py, pz)) {
        Log::print("VoxelWorld: VOXEL_DROP asked for %d item(s) but there is no body to drop them", dropSeed);
        return;
    }
    float dx = 0.0f, dy = 0.0f, dz = -1.0f;
    if (!lookDirection(dx, dy, dz)) { dz = -1.0f; }
    // Flat, because the row is laid across the view and a look straight down has no horizontal answer.
    float fx = dx, fz = dz;
    const float n = std::sqrt(fx * fx + fz * fz);
    if (n < 0.0001f) { fx = 0.0f; fz = -1.0f; } else { fx /= n; fz /= n; }
    const float rx = -fz, rz = fx;
    // Five and a half blocks ahead: far enough to clear the lip of ground the body stands on, close
    // enough that a cube a third of a block wide is still readable, and three times past the magnet
    // so nothing walks back into the hand before the shutter opens.
    float cx = px + fx * 5.5f, cy = py + 0.4f, cz = pz + fz * 5.5f;
#ifdef VOXEL_HAVE_COLLIDE
    // The block under the crosshair is the one spot a headless run can be sure the camera is looking
    // at, so the row is aimed there instead of at a guess. A nearer target is refused: an item three
    // blocks from the eye is inside the magnet's reach, and the picture would come out of an empty
    // ground because the body walked its own drop into its hand.
    const VoxelHit h = raycastVoxel(world, px, py, pz, dx, dy, dz, reach, false);
    if (h.hit && h.dist >= 3.0f) {
        cx = (float)h.x + 0.5f + (float)h.nx * 0.6f;
        cy = (float)h.y + (h.ny > 0 ? 1.6f : (h.ny < 0 ? -0.4f : 0.6f));
        cz = (float)h.z + 0.5f + (float)h.nz * 0.6f;
    }
#endif
    const int wanted = dropSeed > 9 ? 9 : dropSeed;   // one item per palette slot, and there are nine
    int placed = 0;
    for (int i = 0; i < 9 && placed < wanted; i++) {
        if (paletteIds[i] == 0) continue;
        const float off = ((float)placed - (float)(wanted - 1) * 0.5f) * 0.5f;
        spawnVoxelDrop(world, cx + rx * off, cy, cz + rz * off, paletteIds[i], 1);
        placed++;
    }
    Log::print("VoxelWorld: VOXEL_DROP threw %d item(s) around %.1f %.1f %.1f", placed,
               (double)cx, (double)cy, (double)cz);
}
#endif  // VOXEL_HAVE_DROP

// VOXEL_PROBE: every compiled-in module reports its own behaviour against the live world, and the
// sum of them lands on the last line, so one grep says whether the build does what its headers
// claim. Order matters: the shape probes measure a world the liquid and light probes have not
// rearranged, and the player's own probe (which edits real terrain) runs last. Each module puts
// back what it touched, so a probe run leaves no scaffolding in the save or on the wire.
void VoxelGame::runProbes() {
    Probe probe;
    // First and unconditional: the mesher is the base module every other one draws through, and this
    // one reads files rather than the world, so it answers even when the terrain has not streamed.
    probeVoxelAtlas(probe);
    // Right after the atlas and before every module that edits terrain: this one rebuilds the focus
    // column's mesh twice and reads the vertices back, so it wants the world as the streaming left it.
    probeVoxelMesh(world, probe);
#ifdef VOXEL_HAVE_COLLIDE
    probeCollide(world, probe);
#endif
#ifdef VOXEL_HAVE_LIGHT
    probeLight(world, probe);
#endif
#ifdef VOXEL_HAVE_WATER
    probeWater(world, probe);
#endif
#ifdef VOXEL_HAVE_PLAYER
    if (VoxelPlayerScript* p = playerScript()) p->selfTest(world, probe);
    else Log::print("PROBE SKIP  player  no player script in this scene");
#endif
#ifdef VOXEL_HAVE_LIFE
    // The vitals probe is arithmetic and only reads one column (to check the spawn has a surface),
    // so it neither needs the world edits the later modules make nor leaves any behind.
    probeVoxelLife(world, probe);
#endif
#ifdef VOXEL_HAVE_BREAK
    probeVoxelBreak(world, probe);
#endif
#ifdef VOXEL_HAVE_SELECT
    // After the crack: both modules want the arena the collision probes carved, and neither edits
    // terrain - this one only reads the shape table and builds its own outline buffers.
    probeVoxelSelect(world, probe);
#endif
#ifdef VOXEL_HAVE_AUDIO
    probeVoxelAudio(world, probe);
#endif
#ifdef VOXEL_HAVE_ENTITY
    // Before the save probe: this one is the only module whose writes come through the loud
    // setBlock path, and the save probe measures a journal it did not write.
    probeVoxelEntities(world, probe);
#endif
#ifdef VOXEL_HAVE_DROP
    // After the mobs: the drop probe borrows the arena the collision probes carve, installs its own
    // fake inventory over the collector the host just hung there, and puts both back before it
    // returns - so it must not run before a module that reads the real hotbar.
    probeVoxelDrops(world, probe);
#endif
#ifdef VOXEL_HAVE_MODEL
    // After the entity probe, which measures a mob pool it built itself: this one creates and
    // destroys six entities of its own and leaves nothing behind, so it cannot change what the
    // earlier modules counted but can be counted by nothing after it either.
    probeVoxelModel(getScene(), probe);
#endif
#ifdef VOXEL_HAVE_UI
    // The UI probe is the only one that answers about *art*: it reads the pack's own PNGs, re-measures
    // each against the table, and presses a button no pointer can reach. It runs after the world probes
    // have finished editing terrain and before the save probe moves the world directory, because it
    // builds widgets into the HUD scene the plates above already own and leaves them standing.
    probeVoxelUi(probe);
#endif
#ifdef VOXEL_HAVE_SAVE
    // Last, because it is the only probe that moves the world's save directory: it runs against a
    // scratch folder under the OS temp path and puts the real one back before it returns.
    probeVoxelSave(world, probe);
#endif
    probe.finish();
}

}  // namespace quantum
