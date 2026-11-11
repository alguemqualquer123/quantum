// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include "VoxelBlocks.h"
#include "VoxelConfig.h"

#include <cstdint>
#include <string>

namespace quantum { class Scene; }

namespace voxel {

class VoxelWorld;
struct Probe;

// The five surfaces the sound pack is built for. scripts/generate_voxel_sounds.py names its files
// after these strings, so the enum order and the file names are one contract in two places and a
// rename has to land in both.
enum SoundMaterial : int {
    SOUND_STONE = 0,
    SOUND_DIRT,      // soil, grass, snow, clay: everything soft and non-resonant
    SOUND_WOOD,      // logs, planks, crafting tables
    SOUND_SAND,      // loose grains: no pitch, all hiss
    SOUND_GLASS,     // glass, ice: tonal, it rings rather than thuds
    SOUND_MATERIAL_COUNT
};

// Which surface a block makes noise as, or -1 for a block that makes no sound at all: air, and the
// liquids, which you splash into rather than hit. The table is in VoxelAudio.cpp, keyed by id range,
// because g_blocks in VoxelBlocks.h is 59 positional aggregates and a new field there would have to
// be written out 59 times - a row that forgot it would silently inherit the previous block's value.
int voxelSoundMaterial(uint16_t type);

// The name the module logs and the probe reports, for a material the caller may not have mapped.
const char* voxelSoundMaterialName(int material);

// What the host knows each frame and the audio module turns into sound. Filled by VoxelGame, which
// is the only place that has the player, the daylight and the block under the feet at once.
//
// x/y/z are a source position, not the ear: SoLoud's listener is the scene camera, which AudioSystem
// reads from the camera's own transform every frame. The feet are here because a footstep needs a
// place to be struck, and the origin would be a sound that gets quieter as the player spawns.
struct VoxelEar {
    float x = 0.0f, y = 0.0f, z = 0.0f;   // the feet, i.e. where a step is struck
    float moved = 0.0f;                    // blocks travelled this frame, horizontal only
    float daylight = 1.0f;                 // 0..1, drives the day/night ambience crossfade
    bool grounded = false;                 // footsteps only while the body is on a block
    bool underwater = false;               // the eye is inside a liquid
    int footMaterial = SOUND_DIRT;         // the block being walked on
};

// Builds the voice entities. Safe to call every frame, like every attach() in this project: it
// rebuilds only if the scene changed or the entities were destroyed with a scene teardown. Returns
// false when VOXEL_NO_SOUND is set, which is the switch that lets a capture prove a crack is the
// crack and not the noise that came with it - and the only way to hear what a single sound does.
bool attachVoxelAudio(quantum::Scene* scene);

// True when the module is compiled in and not turned off by the environment.
bool voxelAudioEnabled();

// ---- one-shots, all 3D, positioned at the block they are about ----
// The dig is the mining tick, the break is the block giving way, the place is a block arriving.
void playVoxelDig(int material, float x, float y, float z);
void playVoxelBreak(int material, float x, float y, float z);
void playVoxelPlace(int material, float x, float y, float z);
void playVoxelSplash(float x, float y, float z);

// Called once per frame after the world and the player have moved. Re-arms the live voices, runs the
// footstep cadence and crossfades the ambience. Nothing is timed by dt: a step is owed by distance
// and a voice's life is its own sample length, so the engine's clock is the only clock needed.
void updateVoxelAudio(const VoxelEar& ear);

// Master controls, on top of the engine's own global volume rather than instead of it.
void setVoxelMasterVolume(float volume);
float voxelMasterVolume();
void setVoxelMuted(bool muted);
bool voxelIsMuted();

// "Sound: 3/12 voices (57 plays, 2 steals, 0 missing)" for the debug overlay. The overlay is the
// only way to see this module working without listening to it, and the last number is the one that
// says a renamed sample is absent rather than merely quiet.
std::string audioStats();

// ---- diagnostics the probe reads ----
int voxelAudioVoiceCount();
uint64_t voxelAudioPlays();
uint64_t voxelAudioSteals();        // fires that had to take a voice still holding another sound
uint64_t voxelAudioLoadFailures();  // files the pool could not open: a pack/module drift shows here
const char* voxelAudioLastFile();
int voxelAudioActiveVoices();       // voices inside their time budget right now
bool voxelAudioUses3D();            // whether the scene has a camera to be the listener
float voxelAudioAmbienceDay();      // the gain the day bed is currently set to
float voxelAudioAmbienceNight();
float voxelAudioUnderwaterGain();

// The claim this module makes, tested headless: every material maps to a surface, every surface has
// a file that the engine's own file layer can open, a fire takes a voice and reports it, the pool
// steals instead of growing, and the ambience crossfades with the daylight.
void probeVoxelAudio(VoxelWorld& world, Probe& probe);

}  // namespace voxel
