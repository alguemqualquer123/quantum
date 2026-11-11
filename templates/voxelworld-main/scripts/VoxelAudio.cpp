// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "VoxelAudio.h"

#include "VoxelProbe.h"
#include "VoxelWorld.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "Vector3.h"
#include "component/CameraComponent.h"
#include "component/SoundComponent.h"
#include "component/Transform.h"
#include "io/Data.h"
#include "math/Quaternion.h"
#include "subsystem/AudioSystem.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace voxel {

using namespace quantum;

namespace {

// The pack's file names. scripts/generate_voxel_sounds.py writes dig_<mat>_<v>, break_<mat>,
// place_<mat>, step_<mat>_<v>, splash, underwater, ambience_day, ambience_night; this table is the
// other half of that contract, and the first thing the probe checks.
const char* const SAMPLE_DIR = "sounds/voxel/";

enum SampleKind : int { SAMPLE_DIG, SAMPLE_BREAK, SAMPLE_PLACE, SAMPLE_STEP, SAMPLE_SPLASH };
const int KIND_COUNT = SAMPLE_SPLASH + 1;

const int DIG_VARIANTS[SOUND_MATERIAL_COUNT] = { 3, 3, 2, 2, 1 };
const int STEP_VARIANTS = 2;

// Rough lengths, used for the frames before the engine has filled `length` from the file. The voice
// budget is re-cut from the real length as soon as the load lands, so these only have to be close.
double sampleSeconds(SampleKind kind) {
    switch (kind) {
    case SAMPLE_DIG: return 0.13;
    case SAMPLE_BREAK: return 0.34;
    case SAMPLE_PLACE: return 0.11;
    case SAMPLE_STEP: return 0.16;
    default: return 0.55;
    }
}

// The level each event is mixed at. A break is the loudest thing in the game because it is the
// reward; a footstep is the quietest because it is the one that repeats twice a second, and any
// level above a whisper turns a walk into a machine gun.
float sampleGain(SampleKind kind) {
    switch (kind) {
    case SAMPLE_DIG: return 0.55f;
    case SAMPLE_BREAK: return 0.9f;
    case SAMPLE_PLACE: return 0.7f;
    case SAMPLE_STEP: return 0.35f;
    default: return 0.8f;
    }
}

const int ONE_SHOT_POOL = 12;
const int AMBIENCE_VOICES = 3;          // day bed, night bed, underwater bed
const float AMBIENCE_LEVEL = 0.34f;
const float UNDERWATER_LEVEL = 0.5f;
// Water muffles everything above the surface. The engine has no per-voice equaliser, so the module
// takes the gain down instead; two thirds is enough to hear the difference and still place a break.
const float UNDERWATER_DUCK = 0.62f;
// The beds lose more than a one-shot does: a strike still has its transient to be found by, while a
// continuous bed under water is exactly the sound that must stop competing with the splash.
const float AMBIENCE_DUCK = 0.35f;
// One step per two blocks: at the default 4.37 blocks/s walk that is the two-steps-a-second
// cadence, and it slows down with the player instead of running on a timer.
const float STRIDE_BLOCKS = 2.0f;

std::string samplePath(SampleKind kind, int material, int variant);

int variantCount(SampleKind kind, int material) {
    if (kind == SAMPLE_DIG) return DIG_VARIANTS[material];
    if (kind == SAMPLE_STEP) return STEP_VARIANTS;
    return 1;
}

// The name the pack holds for one event. The rule is per event, not per count: a swing is always
// indexed (dig_stone_0, dig_glass_0 - even the surface with a single grain keeps its _0, because a
// second grain added later must not rename the first) and a single impact never is (break_stone,
// place_stone). This is exactly what generate_voxel_sounds.py writes, and the probe opens every
// path here against the real files, so a drift in either direction fails a check instead of logging
// a missing sample once per frame.
std::string samplePath(SampleKind kind, int material, int variant) {
    if (kind == SAMPLE_SPLASH) return std::string(SAMPLE_DIR) + "splash.wav";
    const char* prefix = kind == SAMPLE_DIG ? "dig" :
                         kind == SAMPLE_BREAK ? "break" :
                         kind == SAMPLE_PLACE ? "place" : "step";
    const bool indexed = kind == SAMPLE_DIG || kind == SAMPLE_STEP;
    char buf[96];
    if (indexed) {
        snprintf(buf, sizeof(buf), "%s%s_%s_%d.wav", SAMPLE_DIR, prefix,
                 voxelSoundMaterialName(material), variant);
    } else {
        snprintf(buf, sizeof(buf), "%s%s_%s.wav", SAMPLE_DIR, prefix,
                 voxelSoundMaterialName(material));
    }
    return std::string(buf);
}

struct Voice {
    Entity target = NULL_ENTITY;
    double endsAt = 0.0;      // Engine::getSystemTime() at which this sample is over
    double startedAt = 0.0;
    bool refined = false;     // the budget has been re-cut from the file's own length
    bool live = false;
    std::string file;         // what the voice is holding, for the drift report
};

std::vector<Voice> s_voices;   // the stealable one-shot ring
std::vector<Voice> s_loops;    // the beds, never stolen
Scene* s_scene = nullptr;
bool s_3d = true;
bool s_underwater = false;
float s_masterVolume = 1.0f;
bool s_muted = false;
float s_stride = 0.0f;

uint64_t s_plays = 0;
uint64_t s_steals = 0;
uint64_t s_missing = 0;
char s_lastFile[128] = "";

// The fire order, per material and kind, so a held mining button walks through the three stone
// grains instead of replaying one transient. Deterministic: two runs of the same session sound
// identical, which is what lets a capture be compared against itself.
int s_variantNext[SOUND_MATERIAL_COUNT][KIND_COUNT];

// A few percent of detune is the difference between five digs and one dig played five times. Bounded
// tight, because a voice a tenth off pitch is a different material, not the same one twice.
uint64_t s_seed = 0x9E3779B97F4A7C15ull;

double nextRandom() {
    // xorshift64*, the generator the water system uses for its rain drops: no <random>, no rand().
    s_seed ^= s_seed >> 12;
    s_seed ^= s_seed << 25;
    s_seed ^= s_seed >> 27;
    return (double)((s_seed * 0x2545F4914F6CDD1Dull) >> 40) / 16777216.0;
}

bool soundSwitchOn() { return voxelEnvInt("VOXEL_NO_SOUND", 0) == 0; }

// A voice the engine still holds. AudioSystem never moves `state` back to Stopped when a sound
// finishes - it only stops writing - so `handle != 0` is a claim that stays true forever after the
// first play. This is the same test Sound::isPlaying() makes, and from outside the subsystem it is
// the only honest one.
bool voiceIsLive(const SoundComponent& a) {
    return a.state == SoundState::Playing && a.length > 0.0 &&
           a.playingTime > 0.0 && a.playingTime < a.length;
}

// True when nothing was ever read for this voice: no sample, no handle, not loaded. Only such a
// voice may have its filename cleared, because the audio system treats an empty filename as a
// destroy - and destroy evicts the pooled sample every other voice is still sharing.
bool voiceHoldsNothing(const SoundComponent& a) {
    return !a.loaded && a.handle == 0 && !a.sample;
}

void buildVoice(Voice& v, Scene* scene, const char* name, bool threeD, bool loop) {
    Entity e = scene->createEntity();
    scene->setEntityName(e, name);

    SoundComponent a;
    a.state = SoundState::Stopped;
    a.volume = loop ? 0.0 : 1.0;
    a.needUpdate = true;
    if (threeD) {
        a.minDistance = 1.0f;
        // A break thirty blocks away is a rumour and past that it is gone. The engine's own default
        // is 1000, which in a world of one-metre blocks is "every sound is everywhere".
        a.maxDistance = 32.0f;
        a.attenuationModel = SoundAttenuation::INVERSE_DISTANCE;
        a.attenuationRolloffFactor = 1.0f;
    }
    // A bed that goes quiet is not a bed that stopped. With the default behaviour SoLoud drops an
    // inaudible voice, and a crossfade back up would restart the loop at its beginning - the night
    // wind would resume mid-air every dawn.
    a.inaudibleBehaviorMustTick = loop;
    a.inaudibleBehaviorKill = !loop;
    a.looping = loop;
    scene->addComponent<SoundComponent>(e, std::move(a));

    if (threeD) {
        Transform t;
        t.position = Vector3(0.0f, 0.0f, 0.0f);
        t.rotation = Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
        t.scale = Vector3(1.0f, 1.0f, 1.0f);
        scene->addComponent<Transform>(e, t);
    }
    v.target = e;
    v.live = false;
    v.endsAt = 0.0;
    v.refined = false;
    v.file.clear();
}

// Switching a voice's file through Sound::loadSound is a trap: it calls AudioSystem::destroySound
// whenever the name changes, and destroySound runs SoundPool::remove(id), which evicts the shared
// sample the rest of the pool is still using. A ring that alternates between 35 files would then
// re-read one from disk on every fire. Writing the component directly leaves the cache alone - and
// loadSoundSample() only consults the pool when the shared_ptr is empty, so the reset below is what
// makes the new name load at all.
void armVoice(Voice& v, Scene* scene, const std::string& path, float gain, float pitch, double now,
              const Vector3& at, bool threeD, double estimate) {
    SoundComponent& a = scene->getComponent<SoundComponent>(v.target);

    // Stopping a handle SoLoud has already recycled silences whatever took its index - which here is
    // another one of these voices. A handle inside its own budget is the only one that can still
    // genuinely be up.
    if (v.live && now < v.endsAt && a.handle != 0 && voiceIsLive(a)) a.stopTrigger = true;

    a.filename = path;
    a.sample.reset();
    a.loaded = false;
    a.loadFailed = false;
    a.handle = 0;
    a.length = 0.0;
    a.playingTime = 0.0;
    a.state = SoundState::Stopped;
    a.volume = gain;
    a.speed = pitch;
    a.pan = 0.0f;
    a.looping = false;
    a.enableClocked = false;
    a.protectVoice = false;
    a.startTrigger = true;
    a.pauseTrigger = false;
    a.needUpdate = true;

    if (threeD) {
        Transform& t = scene->getComponent<Transform>(v.target);
        // worldPosition as well as position: the audio system reads the world one, and the transform
        // pass that normally derives it is not guaranteed to have seen this write yet. A voice with
        // no parent holds the same number in both fields anyway.
        t.position = at;
        t.worldPosition = at;
        t.needUpdate = true;
    }

    v.file = path;
    v.startedAt = now;
    v.endsAt = now + estimate + 0.12;
    v.refined = false;
    v.live = true;
}

// A fixed pool cannot grow, so a fire past its capacity takes a voice from another sound. Free first
// (budget spent or engine says the voice is gone), then the one that ends soonest.
Voice* takeVoice(double now) {
    Voice* best = nullptr;
    for (Voice& v : s_voices) {
        const bool busy = v.live && now < v.endsAt;
        if (!busy) return &v;
        if (!best || v.endsAt < best->endsAt) best = &v;
    }
    if (best) s_steals++;
    return best;
}

void fire(SampleKind kind, int material, const Vector3& at) {
    if (!s_scene || s_voices.empty() || !soundSwitchOn()) return;
    if (material < 0 || material >= SOUND_MATERIAL_COUNT) return;

    const int variants = variantCount(kind, material);
    int variant = 0;
    if (variants > 1) {
        int& next = s_variantNext[material][kind];
        next = (next + 1) % variants;
        variant = next;
    }
    const std::string path = samplePath(kind, material, variant);

    Voice* v = takeVoice(Engine::getSystemTime());
    if (!v || !s_scene->isEntityCreated(v->target)) return;

    const float pitch = 0.97f + (float)nextRandom() * 0.06f;
    const float gain = sampleGain(kind) * (s_underwater ? UNDERWATER_DUCK : 1.0f);
    armVoice(*v, s_scene, path, gain, pitch, Engine::getSystemTime(), at, s_3d, sampleSeconds(kind));

    snprintf(s_lastFile, sizeof(s_lastFile), "%s", path.c_str());
    s_plays++;
}

// One bed voice: start it once, then only move its gain. A bed that has never been asked for keeps
// its filename empty, which is what makes the first non-zero level the frame it loads.
void setBed(size_t index, const char* name, float gain) {
    if (index >= s_loops.size() || !s_scene) return;
    Voice& v = s_loops[index];
    if (!s_scene->isEntityCreated(v.target)) return;
    SoundComponent& a = s_scene->getComponent<SoundComponent>(v.target);

    if (gain <= 0.001f) {
        v.live = false;
        if (a.volume > 0.001f) {
            a.volume = 0.0;
            a.needUpdate = true;
        }
        return;
    }
    v.live = true;
    if (a.filename.empty()) {
        const std::string path = std::string(SAMPLE_DIR) + name + ".wav";
        a.filename = path;
        a.sample.reset();
        a.loaded = false;
        a.handle = 0;
        a.length = 0.0;
        a.state = SoundState::Stopped;
        a.looping = true;
        a.inaudibleBehaviorMustTick = true;
        a.inaudibleBehaviorKill = false;
        a.volume = gain;
        a.speed = 1.0f;
        a.enableClocked = false;
        // A bed must survive the walk: it has no source position, and SoLoud would otherwise be
        // given the listener's own, which is inside it.
        a.attenuationModel = SoundAttenuation::NO_ATTENUATION;
        a.startTrigger = true;
        a.pauseTrigger = false;
        a.needUpdate = true;
        v.file = path;
        snprintf(s_lastFile, sizeof(s_lastFile), "%s", path.c_str());
        s_plays++;
        return;
    }
    // The crossfade runs every frame; a needUpdate costs a full SoLoud parameter push for a number
    // no ear tells apart, so only a real move is written.
    if (std::fabs((double)a.volume - (double)gain) > 0.015) {
        a.volume = gain;
        a.needUpdate = true;
    }
}

}  // namespace

// ---- the block table ----

// Id ranges, in the order VoxelBlocks.h appends them. That list is append-only by its own rule, so a
// new block id falls outside every range here and answers SOUND_DIRT - the dull, safe guess - rather
// than reading as stone.
//
// The mapping is here instead of in BlockDef because g_blocks is 59 positional aggregates: a new
// field there has to be written out 59 times, and a row that forgets it silently inherits the
// previous block's value.
int voxelSoundMaterial(uint16_t type) {
    const uint16_t id = stateType(type);
    switch (id) {
    case BLOCK_AIR:
        return -1;
    case BLOCK_WATER:
    case BLOCK_LAVA:
        // You do not hit a liquid, you fall into it, and that is the splash.
        return -1;
    case BLOCK_STONE:
    case BLOCK_COBBLESTONE:
    case BLOCK_MOSSY_COBBLESTONE:
    case BLOCK_SANDSTONE:
    case BLOCK_CLAY:
    case BLOCK_BRICKS:
    case BLOCK_STONE_BRICKS:
    case BLOCK_COAL_ORE:
    case BLOCK_IRON_ORE:
    case BLOCK_GOLD_ORE:
    case BLOCK_DIAMOND_ORE:
    case BLOCK_REDSTONE_ORE:
    case BLOCK_LAPIS_ORE:
    case BLOCK_EMERALD_ORE:
    case BLOCK_BEDROCK:
    case BLOCK_OBSIDIAN:
    case BLOCK_NETHERRACK:
    case BLOCK_FURNACE:
    case BLOCK_SLAB_STONE:
        return SOUND_STONE;
    case BLOCK_LOG_OAK:
    case BLOCK_LOG_BIRCH:
    case BLOCK_LOG_SPRUCE:
    case BLOCK_PLANKS_OAK:
    case BLOCK_PLANKS_BIRCH:
    case BLOCK_PLANKS_SPRUCE:
    case BLOCK_CRAFTING_TABLE:
    case BLOCK_BOOKSHELF:
    case BLOCK_FENCE:
        return SOUND_WOOD;
    case BLOCK_SAND:
    case BLOCK_GRAVEL:
    case BLOCK_MUD:
        return SOUND_SAND;
    case BLOCK_GLASS:
    case BLOCK_ICE:
    case BLOCK_PACKED_ICE:
        return SOUND_GLASS;
    default:
        return SOUND_DIRT;
    }
}

const char* voxelSoundMaterialName(int material) {
    static const char* const names[SOUND_MATERIAL_COUNT] = { "stone", "dirt", "wood", "sand", "glass" };
    if (material < 0 || material >= SOUND_MATERIAL_COUNT) return "none";
    return names[material];
}

// ---- attach ----

bool attachVoxelAudio(Scene* scene) {
    if (!scene || !soundSwitchOn()) return false;
    const bool sameScene = s_scene == scene && (int)s_voices.size() == ONE_SHOT_POOL &&
                           (int)s_loops.size() == AMBIENCE_VOICES;
    if (sameScene) {
        // A scene teardown takes the entities with it while the host keeps calling every frame, so
        // the alive check is what makes attach() safe to call once per frame like every other one.
        bool allAlive = true;
        for (const Voice& v : s_voices)
            if (!scene->isEntityCreated(v.target)) allAlive = false;
        for (const Voice& v : s_loops)
            if (!scene->isEntityCreated(v.target)) allAlive = false;
        if (allAlive) return true;
    }

    s_scene = scene;
    s_voices.clear();
    s_loops.clear();
    s_stride = 0.0f;
    s_underwater = false;

    for (int i = 0; i < ONE_SHOT_POOL; i++) {
        Voice v;
        char name[40];
        snprintf(name, sizeof(name), "VoxelAudioVoice_%d", i);
        buildVoice(v, scene, name, true, false);
        s_voices.push_back(v);
    }
    static const char* const loopNames[AMBIENCE_VOICES] = {
        "VoxelAudioAmbienceDay", "VoxelAudioAmbienceNight", "VoxelAudioUnderwater"
    };
    for (int i = 0; i < AMBIENCE_VOICES; i++) {
        Voice v;
        buildVoice(v, scene, loopNames[i], false, true);
        s_loops.push_back(v);
    }

    // A 3D source is only 3D if the scene has a camera to act as the listener - AudioSystem's own
    // test. Without one every voice plays flat and nothing says so, so say it here.
    s_3d = scene->findComponent<CameraComponent>(scene->getCamera()) != nullptr &&
           scene->findComponent<Transform>(scene->getCamera()) != nullptr;
    if (!s_3d) Log::warn("VoxelAudio: no main camera in this scene, voices play flat");

    // Report the pack against the files this module will actually ask for. A missing one would
    // otherwise surface as one engine error per fire, forever, and a renamed variant would surface
    // as silence.
    int missing = 0;
    for (int m = 0; m < SOUND_MATERIAL_COUNT; m++) {
        for (int k = 0; k <= SAMPLE_STEP; k++) {
            for (int v = 0; v < variantCount((SampleKind)k, m); v++) {
                Data d;
                if (d.open(samplePath((SampleKind)k, m, v).c_str()) != FileErrors::FILEDATA_OK) missing++;
            }
        }
    }
    static const char* const beds[AMBIENCE_VOICES] = { "ambience_day", "ambience_night", "underwater" };
    for (int i = 0; i < AMBIENCE_VOICES; i++) {
        Data d;
        const std::string path = std::string(SAMPLE_DIR) + beds[i] + ".wav";
        if (d.open(path.c_str()) != FileErrors::FILEDATA_OK) missing++;
    }
    s_missing = (uint64_t)missing;
    Log::print("VoxelAudio: %d voices + %d beds, %d samples missing, 3d %d",
               ONE_SHOT_POOL, AMBIENCE_VOICES, missing, s_3d ? 1 : 0);
    return true;
}

bool voxelAudioEnabled() { return soundSwitchOn(); }

bool voxelAudioUses3D() { return s_3d; }

// ---- one-shots ----

void playVoxelDig(int material, float x, float y, float z) {
    fire(SAMPLE_DIG, material, Vector3(x + 0.5f, y + 0.5f, z + 0.5f));
}

void playVoxelBreak(int material, float x, float y, float z) {
    fire(SAMPLE_BREAK, material, Vector3(x + 0.5f, y + 0.5f, z + 0.5f));
}

void playVoxelPlace(int material, float x, float y, float z) {
    fire(SAMPLE_PLACE, material, Vector3(x + 0.5f, y + 0.5f, z + 0.5f));
}

void playVoxelSplash(float x, float y, float z) {
    // The splash is the one event that does not care what it hits: the kind selects the file before
    // the material does, so any in-range surface reaches the same water.
    fire(SAMPLE_SPLASH, SOUND_STONE, Vector3(x, y, z));
}

// ---- the frame ----

void updateVoxelAudio(const VoxelEar& ear) {
    if (!s_scene || s_voices.empty() || !soundSwitchOn()) return;
    s_underwater = ear.underwater;

    const double now = Engine::getSystemTime();

    // 1. The moving listener. A one-shot's source sits where the block was, so without a re-arm the
    // engine pushes its 3D parameters exactly once and the volume never falls as the player walks
    // away: needUpdate is the only thing that makes AudioSystem call update3dAudio at all.
    for (Voice& v : s_voices) {
        if (!s_scene->isEntityCreated(v.target)) { v.live = false; continue; }
        SoundComponent& a = s_scene->getComponent<SoundComponent>(v.target);

        if (!v.live || now >= v.endsAt) {
            if (!v.live && a.filename.empty()) continue;
            v.live = false;
            // A voice that never got a sample would otherwise keep its startTrigger set, because the
            // audio system only clears a trigger on a voice it managed to load. Handing it an empty
            // filename stops the retry, and is only safe while it holds nothing to evict.
            if (voiceHoldsNothing(a) && !a.filename.empty()) {
                a.filename.clear();
                a.startTrigger = false;
                a.state = SoundState::Stopped;
                v.file.clear();
            }
            continue;
        }

        if (!v.refined && a.loaded && a.length > 0.0) {
            // The file's own length is the truth; the estimate is only what the first frames had.
            const double rate = a.speed > 0.0f ? (double)a.speed : 1.0;
            v.endsAt = v.startedAt + a.length / rate + 0.12;
            v.refined = true;
        }
        Transform& t = s_scene->getComponent<Transform>(v.target);
        t.needUpdate = true;
    }

    // 2. Footsteps, on distance rather than time.
    if (ear.grounded && ear.moved > 0.0f) {
        s_stride += ear.moved;
        if (s_stride >= STRIDE_BLOCKS) {
            s_stride -= STRIDE_BLOCKS;
            const int m = ear.footMaterial >= 0 && ear.footMaterial < SOUND_MATERIAL_COUNT ?
                ear.footMaterial : SOUND_DIRT;
            // At the feet, not at the origin: an unattenuated step would be quieter the closer the
            // player spawns to the world centre, which is the opposite of a footstep.
            fire(SAMPLE_STEP, m, Vector3(ear.x, ear.y, ear.z));
        }
    } else if (!ear.grounded && s_stride > STRIDE_BLOCKS * 2.0f) {
        // An airborne frame owes nobody a step, but a half-stride is kept: landing after a one-block
        // hop continues the walk instead of starting a new one. A long fall drops it.
        s_stride = 0.0f;
    }

    // 3. The beds. Day and night crossfade on the daylight the host already computes; going under
    // the surface turns the world bed down and the water bed up, which is the whole trick and costs
    // nothing here rather than in a shader.
    float d = ear.daylight;
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    const bool ambience = voxelEnvInt("VOXEL_NO_AMBIENCE", 0) == 0;
    const float duck = ear.underwater ? AMBIENCE_DUCK : 1.0f;
    setBed(0, "ambience_day", ambience ? AMBIENCE_LEVEL * d * duck : 0.0f);
    setBed(1, "ambience_night", ambience ? AMBIENCE_LEVEL * (1.0f - d) * duck : 0.0f);
    setBed(2, "underwater", ear.underwater ? UNDERWATER_LEVEL : 0.0f);
}

// ---- master controls ----

void setVoxelMasterVolume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    s_masterVolume = volume;
    AudioSystem::setGlobalVolume(volume);
}

float voxelMasterVolume() { return s_masterVolume; }

void setVoxelMuted(bool muted) {
    s_muted = muted;
    // The platform mute rather than a zero volume: a player who turns the volume down and back up
    // should not find the game silent because a stored 0 came back as a master gain.
    AudioSystem::setMuted(muted);
}

bool voxelIsMuted() { return s_muted; }

// ---- diagnostics ----

int voxelAudioVoiceCount() { return (int)s_voices.size(); }
uint64_t voxelAudioPlays() { return s_plays; }
uint64_t voxelAudioSteals() { return s_steals; }
uint64_t voxelAudioLoadFailures() { return s_missing; }
const char* voxelAudioLastFile() { return s_lastFile; }

int voxelAudioActiveVoices() {
    const double now = Engine::getSystemTime();
    int n = 0;
    for (const Voice& v : s_voices)
        if (v.live && now < v.endsAt) n++;
    return n;
}

static float bedGain(size_t index) {
    if (index >= s_loops.size() || !s_scene) return 0.0f;
    if (!s_scene->isEntityCreated(s_loops[index].target)) return 0.0f;
    return (float)s_scene->getComponent<SoundComponent>(s_loops[index].target).volume;
}

float voxelAudioAmbienceDay() { return bedGain(0); }
float voxelAudioAmbienceNight() { return bedGain(1); }
float voxelAudioUnderwaterGain() { return bedGain(2); }

std::string audioStats() {
    // The two quiet answers say which of the two silent failure modes this is: switched off by the
    // environment, or never attached to a scene at all. Both look like a mute to a player.
    if (!soundSwitchOn()) return std::string("Sound: off (VOXEL_NO_SOUND)");
    if (!s_scene) return std::string("Sound: silent (no voices built)");
    char buf[128];
    snprintf(buf, sizeof(buf), "Sound: %d/%d voices (%llu plays, %llu steals, %llu missing)",
             voxelAudioActiveVoices(), ONE_SHOT_POOL,
             (unsigned long long)voxelAudioPlays(), (unsigned long long)voxelAudioSteals(),
             (unsigned long long)voxelAudioLoadFailures());
    return std::string(buf);
}

// ---- the probe ----

void probeVoxelAudio(VoxelWorld& world, Probe& probe) {
    ProbeModule mod(probe, "audio");

    // 1. The block table: air and liquids are silent and everything a pickaxe hits is not.
    probe.check("air and lava make no sound",
                voxelSoundMaterial(BLOCK_AIR) == -1 && voxelSoundMaterial(BLOCK_LAVA) == -1,
                "air %d lava %d", voxelSoundMaterial(BLOCK_AIR), voxelSoundMaterial(BLOCK_LAVA));
    probe.check("stone, wood, sand, glass and grass land on five different surfaces",
                voxelSoundMaterial(BLOCK_STONE) == SOUND_STONE &&
                    voxelSoundMaterial(BLOCK_PLANKS_OAK) == SOUND_WOOD &&
                    voxelSoundMaterial(BLOCK_GLASS) == SOUND_GLASS &&
                    voxelSoundMaterial(BLOCK_SAND) == SOUND_SAND &&
                    voxelSoundMaterial(BLOCK_GRASS) == SOUND_DIRT,
                "stone %d wood %d glass %d sand %d grass %d",
                voxelSoundMaterial(BLOCK_STONE), voxelSoundMaterial(BLOCK_PLANKS_OAK),
                voxelSoundMaterial(BLOCK_GLASS), voxelSoundMaterial(BLOCK_SAND),
                voxelSoundMaterial(BLOCK_GRASS));
    int unmapped = 0;
    for (int id = 0; id < BLOCK_COUNT; id++) {
        const int m = voxelSoundMaterial((uint16_t)id);
        if (m < -1 || m >= SOUND_MATERIAL_COUNT) unmapped++;
    }
    probe.check("every block in the registry answers with a surface or silence", unmapped == 0,
                "%d of %d ids outside the table", unmapped, BLOCK_COUNT);
    probe.check("a block with state still sounds like its type",
                voxelSoundMaterial(makeState(BLOCK_SLAB_STONE, state::SLAB_TOP_BIT)) == SOUND_STONE &&
                    voxelSoundMaterial(makeState(BLOCK_LOG_OAK,
                                                  (uint16_t)(1 << state::LOG_AXIS_SHIFT))) == SOUND_WOOD,
                "top slab %d, sideways log %d",
                voxelSoundMaterial(makeState(BLOCK_SLAB_STONE, state::SLAB_TOP_BIT)),
                voxelSoundMaterial(makeState(BLOCK_LOG_OAK, (uint16_t)(1 << state::LOG_AXIS_SHIFT))));

    // 2. Every file this module will ask for. Data is the loader's own resolver, so a check here
    // fails for exactly the reason a play would.
    int missing = 0, checked = 0;
    for (int m = 0; m < SOUND_MATERIAL_COUNT; m++) {
        for (int k = 0; k <= SAMPLE_STEP; k++) {
            for (int v = 0; v < variantCount((SampleKind)k, m); v++) {
                Data d;
                checked++;
                if (d.open(samplePath((SampleKind)k, m, v).c_str()) != FileErrors::FILEDATA_OK) missing++;
            }
        }
    }
    probe.check("the pack on disk is the pack the module names", missing == 0,
                "%d of %d samples missing", missing, checked);

    // The three files the one-shots and beds name directly, rather than build from a material: a
    // typo in one of them is the same drift the loop above catches, and nothing else would notice
    // until the ambience silently failed to start.
    {
        const char* const singles[] = { "splash.wav", "ambience_day.wav", "ambience_night.wav",
                                        "underwater.wav" };
        int singleMissing = 0;
        for (const char* name : singles) {
            Data d;
            if (d.open((std::string(SAMPLE_DIR) + name).c_str()) != FileErrors::FILEDATA_OK) singleMissing++;
        }
        probe.check("the splash and the three beds are all in the pack", singleMissing == 0,
                    "%d of %d missing", singleMissing, (int)(sizeof(singles) / sizeof(singles[0])));
    }

    Scene* scene = world.scene();
    if (!scene) { probe.skip("no scene to build voices in"); return; }
    if (!attachVoxelAudio(scene)) { probe.skip("sound is switched off"); return; }

    // 3. The pool is a ring of entities, not a list that grows with the noise.
    probe.check("the voice pool is a fixed ring",
                voxelAudioVoiceCount() == ONE_SHOT_POOL && (int)s_loops.size() == AMBIENCE_VOICES,
                "%d voices, %d beds", voxelAudioVoiceCount(), (int)s_loops.size());
    probe.check("a voice has a transform and a bed does not",
                scene->findComponent<Transform>(s_voices[0].target) != nullptr &&
                    scene->findComponent<Transform>(s_loops[0].target) == nullptr,
                "voice %d, bed %d",
                scene->findComponent<Transform>(s_voices[0].target) ? 1 : 0,
                scene->findComponent<Transform>(s_loops[0].target) ? 1 : 0);
    probe.check("the scene has a camera to be the listener", voxelAudioUses3D(),
                "3d %d", voxelAudioUses3D() ? 1 : 0);

    // 4. Fifteen fires into a pool that cannot grow: every one is reported, and the ones past the
    // pool's capacity take a voice from a sound still playing instead of being dropped. The expected
    // steal count is measured, not assumed - the player probe runs before this one and its own
    // break and place already left voices running, so the pool is not empty when the loop starts.
    const uint64_t plays0 = voxelAudioPlays();
    const uint64_t steals0 = voxelAudioSteals();
    const double startNow = Engine::getSystemTime();
    int freeBefore = 0;
    for (const Voice& v : s_voices)
        if (!(v.live && startNow < v.endsAt)) freeBefore++;
    const int fires = ONE_SHOT_POOL + 3;
    for (int i = 0; i < fires; i++) playVoxelDig(SOUND_STONE, 4, 70, 4);
    const uint64_t plays1 = voxelAudioPlays();
    const uint64_t steals1 = voxelAudioSteals();
    const int expectedSteals = fires > freeBefore ? fires - freeBefore : 0;
    probe.check("every fire takes a voice", plays1 - plays0 == (uint64_t)fires,
                "%llu plays, %d voices busy, pool still %d",
                (unsigned long long)(plays1 - plays0), voxelAudioActiveVoices(),
                voxelAudioVoiceCount());
    probe.check("a full pool steals rather than dropping the sound",
                steals1 - steals0 == (uint64_t)expectedSteals,
                "%llu steals for %d fires into %d free voices",
                (unsigned long long)(steals1 - steals0), fires, freeBefore);
    bool holding = false;
    for (const Voice& v : s_voices)
        if (!v.file.empty()) holding = true;
    probe.check("a fired voice carries the sample it was given", holding, "last %s",
                voxelAudioLastFile());

    // 5. A held mining button walks through the grains instead of replaying one transient. The pool is
    // full by now, so the three fires also prove the steal path keeps its own sample name.
    playVoxelDig(SOUND_STONE, 4, 70, 4);
    const std::string grainA = voxelAudioLastFile();
    playVoxelDig(SOUND_STONE, 4, 70, 4);
    const std::string grainB = voxelAudioLastFile();
    playVoxelDig(SOUND_STONE, 4, 70, 4);
    const std::string grainC = voxelAudioLastFile();
    probe.check("three digs in a row ask for three different grains",
                grainA != grainB && grainB != grainC && grainA != grainC,
                "%s, %s, %s", grainA.c_str(), grainB.c_str(), grainC.c_str());

    // 6. The line the F3 overlay reads when nothing is audible. Parsed by hand like the mob line:
    // `sscanf` is the CRT-deprecated name here, and comparing the prefix pins the format in the
    // code that checks it. This runs while the pool is still full, so a zero busy count is a bug
    // rather than a slow frame.
    const std::string stats = audioStats();
    int busy = -1, pool = -1;
    const char* const statsPrefix = "Sound: ";
    if (stats.compare(0, 7, statsPrefix) == 0) {
        size_t i = 7;
        for (; i < stats.size() && stats[i] >= '0' && stats[i] <= '9'; i++)
            busy = (busy < 0 ? 0 : busy) * 10 + (stats[i] - '0');
        if (i < stats.size() && stats[i] == '/') {
            for (++i; i < stats.size() && stats[i] >= '0' && stats[i] <= '9'; i++)
                pool = (pool < 0 ? 0 : pool) * 10 + (stats[i] - '0');
        }
    }
    probe.check("the overlay's sound line reports the pool it is describing",
                busy > 0 && busy <= ONE_SHOT_POOL && pool == ONE_SHOT_POOL, "%s", stats.c_str());

    // 7. The beds answer the daylight and the water.
    VoxelEar ear;
    ear.daylight = 1.0f;
    ear.underwater = false;
    updateVoxelAudio(ear);
    const float dayHigh = voxelAudioAmbienceDay();
    const float nightLow = voxelAudioAmbienceNight();
    ear.daylight = 0.0f;
    updateVoxelAudio(ear);
    const float dayLow = voxelAudioAmbienceDay();
    const float nightHigh = voxelAudioAmbienceNight();
    probe.check("the beds crossfade with the daylight",
                dayHigh > nightLow + 0.05f && nightHigh > dayLow + 0.05f,
                "day %.2f -> %.2f, night %.2f -> %.2f", dayHigh, dayLow, nightLow, nightHigh);

    ear.underwater = true;
    updateVoxelAudio(ear);
    probe.check("under the surface the world bed ducks and the water bed rises",
                voxelAudioUnderwaterGain() > 0.4f && voxelAudioAmbienceNight() < nightHigh * 0.6f,
                "water %.2f, bed %.2f", voxelAudioUnderwaterGain(), voxelAudioAmbienceNight());

    // A probe leaves the world as it found it. The beds go back to a daylight setting and the mute
    // state is restored below; the next real frame overwrites the crossfade with the sun the scene
    // actually has, so the only thing a probe must not do is leave the game in the dark.
    ear.underwater = false;
    ear.daylight = 1.0f;
    updateVoxelAudio(ear);

    // 8. The step is owed by distance rather than by time, and only while the body is on a block.
    // Three frames of exactly one stride each fire exactly three steps whatever the leftover partial
    // stride was, because a fire takes a stride back out and leaves the accumulator under one again;
    // the same distance spent in the air fires nothing at all.
    VoxelEar walker;
    walker.daylight = 1.0f;
    walker.grounded = true;
    walker.footMaterial = SOUND_WOOD;
    walker.x = 4.0f; walker.y = 70.0f; walker.z = 4.0f;
    const uint64_t playsWalk = voxelAudioPlays();
    walker.moved = STRIDE_BLOCKS;
    for (int i = 0; i < 3; i++) updateVoxelAudio(walker);
    const std::string stepFile = voxelAudioLastFile();
    probe.check("one stride of ground is one step, on the block under the feet",
                voxelAudioPlays() - playsWalk == 3 &&
                    stepFile.find("step_wood_") != std::string::npos,
                "%llu steps, last %s", (unsigned long long)(voxelAudioPlays() - playsWalk),
                stepFile.c_str());
    walker.grounded = false;
    walker.moved = STRIDE_BLOCKS * 3.0f;
    const uint64_t playsAir = voxelAudioPlays();
    for (int i = 0; i < 3; i++) updateVoxelAudio(walker);
    probe.check("the same distance spent in the air owes no step", voxelAudioPlays() == playsAir,
                "%llu plays from 3 airborne frames",
                (unsigned long long)(voxelAudioPlays() - playsAir));

    // 9. The master switches a player and the HUD both reach for.
    const float volumeBefore = voxelMasterVolume();
    const bool mutedBefore = voxelIsMuted();
    setVoxelMuted(true);
    probe.check("mute is the platform switch, not a stored zero",
                voxelIsMuted() && voxelMasterVolume() == volumeBefore,
                "volume stayed %.2f", voxelMasterVolume());
    setVoxelMuted(mutedBefore);
    setVoxelMasterVolume(0.5f);
    probe.check("the master gain clamps into the engine",
                voxelMasterVolume() == 0.5f, "global volume %.2f", AudioSystem::getGlobalVolume());
    setVoxelMasterVolume(volumeBefore);
}

}  // namespace voxel
