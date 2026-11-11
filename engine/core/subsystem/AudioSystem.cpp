// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "AudioSystem.h"

#include "pool/SoundPool.h"
#include "Scene.h"

#include "soloud.h"
#include "soloud_thread.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"
#include "io/Data.h"
#include "io/FileData.h"
#include "io/ResourcePack.h"
#include "math/Ray.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

using namespace quantum;

bool AudioSystem::inited = false;

float AudioSystem::globalVolume = 1.0;
bool AudioSystem::muted = false;
std::shared_ptr<SoLoud::WavStream> AudioSystem::musicSource;
std::shared_ptr<SoLoud::WavStream> AudioSystem::nextMusicSource;
unsigned int AudioSystem::musicHandle = 0;
unsigned int AudioSystem::nextMusicHandle = 0;
float AudioSystem::musicFadeDuration = 0.0f;
float AudioSystem::musicFadeElapsed = 0.0f;

namespace {
std::unordered_map<std::string, float> gBusVolumes;

struct AmbienceVoice {
    std::shared_ptr<SoLoud::WavStream> source;
    std::string filename;
    unsigned int handle = 0;
    float currentVolume = 0.0f;
    float targetVolume = 0.0f;
    float fadeDuration = 0.0f;
    float fadeElapsed = 0.0f;
    bool stopping = false;
};

std::unordered_map<std::string, AmbienceVoice> gAmbienceVoices;

float busGain(const std::string& bus) {
    float gain = 1.0f;
    std::string current = bus.empty() ? "master" : bus;
    while (!current.empty()) {
        const auto it = gBusVolumes.find(current);
        if (it != gBusVolumes.end()) gain *= it->second;
        const size_t slash = current.rfind('/');
        if (slash == std::string::npos) break;
        current.resize(slash);
    }
    if (bus != "master") {
        const auto master = gBusVolumes.find("master");
        if (master != gBusVolumes.end()) gain *= master->second;
    }
    return gain;
}

std::shared_ptr<SoLoud::WavStream> loadStream(const std::string& filename) {
    if (filename.empty()) return nullptr;
    auto stream = std::make_shared<SoLoud::WavStream>();
    SoLoud::result result = SoLoud::FILE_LOAD_FAILED;
    const bool packed = ResourcePack::contains(filename.c_str());
    if (!packed) {
        const std::string systemPath = FileData::getSystemPath(filename);
        result = stream->loadToMem(systemPath.c_str());
    } else {
        Data filedata;
        if (filedata.open(filename.c_str()) != FileErrors::FILEDATA_OK) return nullptr;
        result = stream->loadMem(filedata.getMemPtr(), filedata.length(), true, false);
    }
    return result == SoLoud::SOLOUD_ERRORS::SO_NO_ERROR ? stream : nullptr;
}
}

AudioSystem::AudioSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<SoundComponent>());

    cameraLastPosition = Vector3(0, 0, 0);
}

SoLoud::Soloud& AudioSystem::getSoloud(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static SoLoud::Soloud* soloud = new SoLoud::Soloud();
    return *soloud;
};

bool AudioSystem::init(){
    if (!inited) {
        SoLoud::result result = getSoloud().init();
        if (result != SoLoud::SOLOUD_ERRORS::SO_NO_ERROR) {
            Log::error("Failed to initialize audio output");
            return false;
        }

        getSoloud().setGlobalVolume(muted ? 0.0f : globalVolume);

        //Wait for mixing thread
        //SoLoud::Thread::sleep(10);

        inited = true;
    }

    return true;
}

void AudioSystem::deInit(){
    if (inited){
        stopMusic();
        stopAmbience();
        getSoloud().deinit();

        inited = false;
    }
}

bool AudioSystem::loadSoundSample(SoundComponent& audio){
    if (audio.filename.empty()) {
        return false;
    }

    if (!audio.sample){
        SoundLoadResult result = SoundPool::loadFromFile(audio.filename, audio.filename);
        if (result.state == ResourceLoadState::Loading) {
            return false;
        }
        if (result.state == ResourceLoadState::Failed) {
            audio.loaded = false;
            audio.loadFailed = true;
            audio.length = 0;
            audio.startTrigger = false;
            audio.state = SoundState::Stopped;
            audio.sample.reset();
            return false;
        }

        audio.sample = result.data;
        if (!audio.sample) {
            return false;
        }
    }

    audio.length = audio.sample->getLength();
    audio.loaded = true;
    audio.loadFailed = false;

    return true;
}

bool AudioSystem::loadSound(SoundComponent& audio){
    if (!loadSoundSample(audio)) {
        return false;
    }

    if (!init()) {
        return false;
    }

    return true;
}

void AudioSystem::preloadSoundAssets(){
    auto audios = scene->getComponentArray<SoundComponent>();
    for (int i = 0; i < audios->size(); i++) {
        SoundComponent& audio = audios->getComponentFromIndex(i);

        if (audio.filename.empty()) {
            if (audio.loaded || audio.handle != 0 || audio.sample) {
                destroySound(audio);
            }
            continue;
        }

        if (!audio.loaded) {
            loadSoundSample(audio);
        }
    }
}

void AudioSystem::getLoadCount(size_t& loaded, size_t& total) const{
    loaded = 0;
    total = 0;

    auto audios = scene->getComponentArray<SoundComponent>();
    for (int i = 0; i < audios->size(); i++) {
        const SoundComponent& audio = audios->getComponentFromIndex(i);
        if (audio.filename.empty()) {
            continue;
        }

        total++;
        // a missing file must not hold the load
        if (audio.loaded || audio.loadFailed) {
            loaded++;
        }
    }
}

void AudioSystem::destroySound(SoundComponent& audio){
    if (inited && audio.handle != 0) {
        getSoloud().stop(audio.handle);
    }

    audio.handle = 0;
    audio.playingTime = 0;
    audio.occlusionElapsed = 0.0f;
    audio.occlusionGain = 1.0f;
    audio.startTrigger = false;
    audio.pauseTrigger = false;
    audio.stopTrigger = false;
    audio.loaded = false;
    audio.loadFailed = false;
    audio.length = 0;
    audio.sample.reset();
    if (!audio.filename.empty()) {
        SoundPool::remove(audio.filename);
    }
}

bool AudioSystem::seekSound(SoundComponent& audio, double time){
    SoLoud::result res = getSoloud().seek(audio.handle, time);

    if (res != SoLoud::SOLOUD_ERRORS::SO_NO_ERROR)
        return false;

    return true;
}

void AudioSystem::setPaused(bool paused) {
    this->paused = paused;
    if (inited) {
        auto audios = scene->getComponentArray<SoundComponent>();
        for (int i = 0; i < audios->size(); i++) {
            SoundComponent& audio = audios->getComponentFromIndex(i);
            if (audio.handle != 0) {
                getSoloud().setPause(audio.handle, paused);
            }
        }
    }
}

void AudioSystem::stopSceneSounds() {
    auto audios = scene->getComponentArray<SoundComponent>();
    for (int i = 0; i < audios->size(); i++) {
        SoundComponent& audio = audios->getComponentFromIndex(i);

        if (inited && audio.handle != 0) {
            getSoloud().stop(audio.handle);
        }

        audio.handle = 0;
        audio.playingTime = 0;
        audio.startTrigger = false;
        audio.pauseTrigger = false;
        audio.stopTrigger = false;
    }
}

void AudioSystem::stopAll(){
    if (inited){
        getSoloud().stopAll();
    }
    stopMusic();
    stopAmbience();
}

void AudioSystem::pauseAll(){
    if (inited) {
        getSoloud().setPauseAll(true);
    }
}

void AudioSystem::resumeAll(){
    if (inited) {
        getSoloud().setPauseAll(false);
    }
}

void AudioSystem::checkActive(){
    if (inited) {
        if (getSoloud().getVoiceCount() == 0){
            deInit();
        }
    }
}

bool AudioSystem::loadMusicStream(const std::string& filename){
    nextMusicSource = loadStream(filename);
    return nextMusicSource != nullptr;
}

bool AudioSystem::playMusicStream(const std::string& filename, float crossFadeSeconds){
    if (!loadMusicStream(filename) || !init()) return false;

    auto source = std::move(nextMusicSource);
    const unsigned int handle = getSoloud().play(*source);
    if (handle == 0) return false;
    getSoloud().setLooping(handle, true);

    const float duration = std::max(0.0f, crossFadeSeconds);
    if (musicHandle != 0 && duration > 0.0f) {
        nextMusicSource = std::move(source);
        nextMusicHandle = handle;
        musicFadeDuration = duration;
        musicFadeElapsed = 0.0f;
        getSoloud().setVolume(nextMusicHandle, 0.0f);
    } else {
        if (musicHandle != 0) getSoloud().stop(musicHandle);
        musicSource = std::move(source);
        musicHandle = handle;
        musicFadeDuration = 0.0f;
        musicFadeElapsed = 0.0f;
        getSoloud().setVolume(musicHandle, getBusVolume("music"));
    }
    return true;
}

void AudioSystem::stopMusic(){
    if (inited) {
        if (musicHandle != 0) getSoloud().stop(musicHandle);
        if (nextMusicHandle != 0) getSoloud().stop(nextMusicHandle);
    }
    musicHandle = 0;
    nextMusicHandle = 0;
    musicSource.reset();
    nextMusicSource.reset();
    musicFadeDuration = 0.0f;
    musicFadeElapsed = 0.0f;
}

bool AudioSystem::isMusicPlaying(){
    return inited && (musicHandle != 0 || nextMusicHandle != 0);
}

bool AudioSystem::setAmbienceLayer(const std::string& name, const std::string& filename,
    float volume, float fadeSeconds) {
    if (name.empty() || filename.empty()) return false;
    auto existing = gAmbienceVoices.find(name);
    if (existing != gAmbienceVoices.end() && existing->second.filename == filename) {
        existing->second.targetVolume = std::clamp(volume, 0.0f, 1.0f);
        existing->second.fadeDuration = std::max(0.0f, fadeSeconds);
        existing->second.fadeElapsed = 0.0f;
        existing->second.stopping = false;
        return true;
    }

    auto source = loadStream(filename);
    if (!source || !init()) return false;
    if (existing != gAmbienceVoices.end()) {
        if (existing->second.handle != 0) getSoloud().stop(existing->second.handle);
        gAmbienceVoices.erase(existing);
    }
    AmbienceVoice voice;
    voice.source = std::move(source);
    voice.filename = filename;
    voice.handle = getSoloud().play(*voice.source);
    if (voice.handle == 0) return false;
    getSoloud().setLooping(voice.handle, true);
    voice.targetVolume = std::clamp(volume, 0.0f, 1.0f);
    voice.fadeDuration = std::max(0.0f, fadeSeconds);
    voice.fadeElapsed = voice.fadeDuration;
    voice.currentVolume = voice.targetVolume;
    getSoloud().setVolume(voice.handle, voice.currentVolume * getBusVolume("ambience"));
    gAmbienceVoices[name] = std::move(voice);
    return true;
}

void AudioSystem::stopAmbienceLayer(const std::string& name, float fadeSeconds) {
    auto it = gAmbienceVoices.find(name);
    if (it == gAmbienceVoices.end()) return;
    if (fadeSeconds <= 0.0f) {
        if (it->second.handle != 0 && inited) getSoloud().stop(it->second.handle);
        gAmbienceVoices.erase(it);
        return;
    }
    it->second.targetVolume = 0.0f;
    it->second.fadeDuration = fadeSeconds;
    it->second.fadeElapsed = 0.0f;
    it->second.stopping = true;
}

void AudioSystem::stopAmbience() {
    if (inited) {
        for (auto& entry : gAmbienceVoices)
            if (entry.second.handle != 0) getSoloud().stop(entry.second.handle);
    }
    gAmbienceVoices.clear();
}

size_t AudioSystem::getAmbienceLayerCount() {
    return gAmbienceVoices.size();
}

// using global volume in a static var to save because init/deinit reset volume value
void AudioSystem::setGlobalVolume(float volume){
    globalVolume = volume;
    getSoloud().setGlobalVolume(muted ? 0.0f : globalVolume);
}

Vector3 AudioSystem::calculateDopplerVelocity(const Vector3& current,
    const Vector3& previous, double deltaSeconds) {
    if (!(deltaSeconds > 0.000001) || !std::isfinite(deltaSeconds)) return Vector3::ZERO;
    const float inverseDelta = static_cast<float>(1.0 / deltaSeconds);
    return (current - previous) * inverseDelta;
}

float AudioSystem::getGlobalVolume(){
    //return getSoloud().getGlobalVolume();
    return globalVolume;
}

void AudioSystem::setBusVolume(const std::string& bus, float volume) {
    if (bus.empty()) return;
    gBusVolumes[bus] = std::clamp(volume, 0.0f, 1.0f);
}

float AudioSystem::getBusVolume(const std::string& bus) {
    return busGain(bus);
}

void AudioSystem::setMuted(bool muted){
    AudioSystem::muted = muted;
    getSoloud().setGlobalVolume(muted ? 0.0f : globalVolume);
}

void AudioSystem::load(){
    // a file that failed before is tried again by each scene that loads it
    auto audios = scene->getComponentArray<SoundComponent>();
    for (int i = 0; i < audios->size(); i++) {
        const SoundComponent& audio = audios->getComponentFromIndex(i);
        if (!audio.filename.empty() && !audio.loaded) {
            SoundPool::retry(audio.filename);
        }
    }

    preloadSoundAssets();
}

void AudioSystem::destroy(){
}

void AudioSystem::update(double dt){
    preloadSoundAssets();

    if (scene == Engine::getMainScene() && nextMusicHandle != 0) {
        musicFadeElapsed = std::min(musicFadeDuration, musicFadeElapsed + static_cast<float>(dt));
        const float factor = musicFadeDuration > 0.0f ? musicFadeElapsed / musicFadeDuration : 1.0f;
        getSoloud().setVolume(musicHandle, getBusVolume("music") * (1.0f - factor));
        getSoloud().setVolume(nextMusicHandle, getBusVolume("music") * factor);
        if (factor >= 1.0f) {
            if (musicHandle != 0) getSoloud().stop(musicHandle);
            musicHandle = nextMusicHandle;
            nextMusicHandle = 0;
            musicSource = std::move(nextMusicSource);
            musicFadeDuration = 0.0f;
            musicFadeElapsed = 0.0f;
        }
    } else if (scene == Engine::getMainScene() && musicHandle != 0) {
        getSoloud().setVolume(musicHandle, getBusVolume("music"));
    }

    for (auto it = gAmbienceVoices.begin(); it != gAmbienceVoices.end();) {
        AmbienceVoice& voice = it->second;
        voice.fadeElapsed = std::min(voice.fadeDuration,
            voice.fadeElapsed + static_cast<float>(dt));
        const float factor = voice.fadeDuration > 0.0f
            ? voice.fadeElapsed / voice.fadeDuration : 1.0f;
        voice.currentVolume += (voice.targetVolume - voice.currentVolume) * factor;
        if (factor >= 1.0f) voice.currentVolume = voice.targetVolume;
        if (voice.handle != 0)
            getSoloud().setVolume(voice.handle, voice.currentVolume * getBusVolume("ambience"));
        if (voice.stopping && factor >= 1.0f) {
            if (voice.handle != 0) getSoloud().stop(voice.handle);
            it = gAmbienceVoices.erase(it);
        } else {
            ++it;
        }
    }

    if (paused) {
        return;
    }

    auto audios = scene->getComponentArray<SoundComponent>();

    // open the output early, a device started by the first play cuts that sound
    if (!outputRequested && audios->size() > 0) {
        outputRequested = true;
        init();
    }

    // 3D sources are placed against the main camera, so without one they play in 2D
    bool hasListener = scene->findComponent<CameraComponent>(scene->getCamera()) &&
                       scene->findComponent<Transform>(scene->getCamera());

    for (int i = 0; i < audios->size(); i++){
		SoundComponent& audio = audios->getComponentFromIndex(i);

        Entity entity = audios->getEntity(i);
        Signature signature = scene->getSignature(entity);
        bool use3DAudio = hasListener && signature.test(scene->getComponentId<Transform>());

        Vector3 worldPosition = Vector3(0, 0, 0);
        if (use3DAudio){
            Transform& transform = scene->getComponent<Transform>(entity);

            worldPosition = transform.worldPosition;
        }

        if (audio.filename.empty()) {
            if (audio.loaded || audio.handle != 0 || audio.sample) {
                destroySound(audio);
            }
            continue;
        }

        bool pendingStart = audio.startTrigger || (audio.state == SoundState::Playing && audio.handle == 0);

        if (audio.state == SoundState::Playing || pendingStart){
            if (!audio.loaded){
                loadSound(audio);
            }
        }

        if (audio.loaded){
            if (audio.pauseTrigger){
                audio.pauseTrigger = false;

                getSoloud().setPause(audio.handle, true);
                audio.state = SoundState::Paused;
            }
            if (audio.stopTrigger){
                audio.stopTrigger = false;

                getSoloud().stop(audio.handle);
                audio.handle = 0;
                audio.playingTime = 0;
                audio.state = SoundState::Stopped;
            }
            if (pendingStart){
                audio.startTrigger = false;

                if (audio.state != SoundState::Paused || audio.handle == 0) {
                    if (!init()) {
                        continue;
                    }

                    if (use3DAudio){
                        if (audio.enableClocked){
                            audio.handle = getSoloud().play3dClocked(Engine::getDeltatime(), *audio.sample, worldPosition.x, worldPosition.y, worldPosition.z);
                        }else{
                            audio.handle = getSoloud().play3d(*audio.sample, worldPosition.x, worldPosition.y, worldPosition.z);
                        }
                    }else{
                        if (audio.enableClocked){
                            audio.handle = getSoloud().playClocked(Engine::getDeltatime(), *audio.sample);
                        }else{
                            audio.handle = getSoloud().play(*audio.sample);
                        }
                    }
                    // a new voice starts with the sample defaults
                    audio.needUpdate = true;
                }else{
                    getSoloud().setPause(audio.handle, false);
                }
                if (use3DAudio){
                    Transform& cameraTransform =  scene->getComponent<Transform>(scene->getCamera());

                    audio.lastPosition = worldPosition;
                    cameraLastPosition = cameraTransform.worldPosition;
                }

                audio.state = SoundState::Playing;
            }

            if (audio.state == SoundState::Playing){
                audio.playingTime = getSoloud().getStreamTime(audio.handle);

                auto update3DAudio = [&]() {
                    if (!use3DAudio) return;

                    CameraComponent& camera = scene->getComponent<CameraComponent>(scene->getCamera());
                    Transform& cameraTransform = scene->getComponent<Transform>(scene->getCamera());
                    const double delta = Engine::getDeltatime();
                    Vector3 velocity = calculateDopplerVelocity(worldPosition, audio.lastPosition, delta);
                    Vector3 camWorldPos = cameraTransform.worldPosition;
                    Vector3 camWorldView = camera.worldTarget - camWorldPos;
                    Vector3 camWorldUp = camera.worldUp;
                    Vector3 camVelocity = calculateDopplerVelocity(camWorldPos, cameraLastPosition, delta);

                    unsigned int attModel = SoLoud::AudioSource::NO_ATTENUATION;
                    if (audio.attenuationModel == SoundAttenuation::INVERSE_DISTANCE)
                        attModel = SoLoud::AudioSource::INVERSE_DISTANCE;
                    if (audio.attenuationModel == SoundAttenuation::LINEAR_DISTANCE)
                        attModel = SoLoud::AudioSource::LINEAR_DISTANCE;
                    if (audio.attenuationModel == SoundAttenuation::EXPONENTIAL_DISTANCE)
                        attModel = SoLoud::AudioSource::EXPONENTIAL_DISTANCE;

                    getSoloud().set3dSourceMinMaxDistance(audio.handle, audio.minDistance, audio.maxDistance);
                    getSoloud().set3dSourceAttenuation(audio.handle, attModel, audio.attenuationRolloffFactor);
                    getSoloud().set3dSourceDopplerFactor(audio.handle, audio.dopplerFactor);
                    getSoloud().set3dSourceParameters(
                        audio.handle,
                        worldPosition.x, worldPosition.y, worldPosition.z,
                        velocity.x, velocity.y, velocity.z);
                    getSoloud().set3dListenerParameters(
                        camWorldPos.x, camWorldPos.y, camWorldPos.z,
                        camWorldView.x, camWorldView.y, camWorldView.z,
                        camWorldUp.x, camWorldUp.y, camWorldUp.z,
                        camVelocity.x, camVelocity.y, camVelocity.z);
                    getSoloud().update3dAudio();

                    audio.lastPosition = worldPosition;
                    cameraLastPosition = camWorldPos;
                };

                // Bus changes are global settings, so they must affect voices that are
                // already playing even when the component itself has no pending update.
                if (!audio.occlusionEnabled || !use3DAudio){
                    audio.occlusionGain = 1.0f;
                    audio.occlusionElapsed = 0.0f;
                }else{
                    audio.occlusionElapsed += static_cast<float>(dt);
                    const float interval = std::max(0.0f, audio.occlusionUpdateInterval);
                    if (audio.occlusionElapsed >= interval){
                        audio.occlusionElapsed = 0.0f;
                        Transform& cameraTransform = scene->getComponent<Transform>(scene->getCamera());
                        const Vector3 toListener = cameraTransform.worldPosition - worldPosition;
                        const std::vector<Entity> ignoredEntities{entity, scene->getCamera()};
                        const RayReturn hit = Ray(worldPosition, toListener).intersects(
                            scene, RayFilter::BODY_3D, true, 0xFFFFu, 0xFFFFu, ignoredEntities);
                        audio.occlusionGain = hit.hit
                            ? std::clamp(audio.occlusionVolume, 0.0f, 1.0f)
                            : 1.0f;
                    }
                }
                getSoloud().setVolume(audio.handle, audio.volume * getBusVolume(audio.bus) * audio.occlusionGain);

                if (audio.needUpdate){
                    getSoloud().setRelativePlaySpeed(audio.handle, audio.speed);
                    getSoloud().setPan(audio.handle, audio.pan);
                    getSoloud().setLooping(audio.handle, audio.looping);
                    getSoloud().setLoopPoint(audio.handle, audio.loopingPoint);
                    getSoloud().setProtectVoice(audio.handle, audio.protectVoice);
                    getSoloud().setInaudibleBehavior(audio.handle, audio.inaudibleBehaviorMustTick, audio.inaudibleBehaviorKill);

                    audio.needUpdate = false;
                }

                // Source and listener positions are dynamic even when all static voice
                // properties remain unchanged; update them every frame for correct spatial
                // movement and Doppler response.
                update3DAudio();
            }
        }
    }
}

void AudioSystem::draw(){

}

void AudioSystem::onComponentAdded(Entity entity, ComponentId componentId) {

}

void AudioSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
	if (componentId == scene->getComponentId<SoundComponent>()) {
		SoundComponent& audio = scene->getComponent<SoundComponent>(entity);
		destroySound(audio);
	}
}
