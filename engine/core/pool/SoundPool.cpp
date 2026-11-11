// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "SoundPool.h"

#include "Engine.h"
#include "Log.h"
#include "io/Data.h"
#include "thread/ResourceProgress.h"
#include "thread/ThreadPoolManager.h"

#include "soloud.h"
#include "soloud_wav.h"

#include <filesystem>
#include <vector>

using namespace quantum;

std::unordered_map<std::string, std::future<SoundBuildResult>> SoundPool::pendingBuilds;
std::unordered_set<std::string> SoundPool::failedBuilds;
std::mutex SoundPool::cacheMutex;
std::atomic<bool> SoundPool::shutdownRequested{false};

sounds_t& SoundPool::getMap(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static sounds_t* map = new sounds_t();
    return *map;
};

SoundLoadResult SoundPool::loadFromFile(const std::string& id, const std::string& filename){
    auto& shared = getMap()[id];

    SoundLoadResult result;
    result.id = id;

    if (shared){
        result.state = ResourceLoadState::Finished;
        result.data = shared;
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (failedBuilds.count(id)) {
            result.state = ResourceLoadState::Failed;
            return result;
        }
    }

    if (Engine::isAsyncLoading()) {
        std::lock_guard<std::mutex> lock(cacheMutex);

        if (shutdownRequested.load()) {
            result.state = ResourceLoadState::Failed;
            result.errorMessage = "Shutdown requested";
            return result;
        }

        auto it = pendingBuilds.find(id);
        if (it != pendingBuilds.end()) {
            auto& future = it->second;
            if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                SoundBuildResult built = future.get();
                pendingBuilds.erase(it);
                if (!built.ok()) {
                    // cacheMutex is already held by the async branch
                    failedBuilds.insert(id);

                    result.state = ResourceLoadState::Failed;
                    result.errorMessage = built.error;
                    return result;
                }

                shared = built.wav;
                result.state = ResourceLoadState::Finished;
                result.data = shared;
                return result;
            } else if (future.valid()) {
                result.state = ResourceLoadState::Loading;
                return result;
            } else {
                pendingBuilds.erase(it);
                result.state = ResourceLoadState::Failed;
                result.errorMessage = "Invalid future";
                return result;
            }
        }

        if (shutdownRequested.load()) {
            result.state = ResourceLoadState::Failed;
            result.errorMessage = "Shutdown requested";
            return result;
        }

        std::string soundName = getSoundDisplayName(filename);
        uint64_t buildId = std::hash<std::string>{}(id);

        ResourceProgress::startBuild(buildId, ResourceType::Sound, soundName);

        pendingBuilds[id] = ThreadPoolManager::getInstance().enqueue(
            [id, filename]() {
                return loadSoundInternal(id, filename, true);
            }
        );
    } else {
        SoundBuildResult built = loadSoundInternal(id, filename, false);
        if (!built.ok()) {
            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                failedBuilds.insert(id);
            }

            result.state = ResourceLoadState::Failed;
            result.errorMessage = built.error;
            return result;
        }

        shared = built.wav;
        result.state = ResourceLoadState::Finished;
        result.data = shared;
        return result;
    }

    result.state = ResourceLoadState::Loading;
    return result;
}

void SoundPool::retry(const std::string& id){
    std::lock_guard<std::mutex> lock(cacheMutex);
    failedBuilds.erase(id);
}

SoundBuildResult SoundPool::loadSoundInternal(const std::string& id, const std::string& filename, bool trackProgress){
    uint64_t buildId = std::hash<std::string>{}(id);
    SoundBuildResult build;

    if (trackProgress) {
        if (shutdownRequested.load()) {
            ResourceProgress::failBuild(buildId);
            build.error = "Shutdown requested";
            return build;
        }
        ResourceProgress::updateProgress(buildId, 0.35f);
    }

    Data filedata;

    if (filedata.open(filename.c_str()) != FileErrors::FILEDATA_OK){
        if (trackProgress) {
            ResourceProgress::failBuild(buildId);
        }
        Log::error("Sound file not found: %s", filename.c_str());
        build.error = "Sound file not found: " + filename;
        return build;
    }

    if (trackProgress) {
        if (shutdownRequested.load()) {
            ResourceProgress::failBuild(buildId);
            build.error = "Shutdown requested";
            return build;
        }
        ResourceProgress::updateProgress(buildId, 0.7f);
    }

    auto sound = std::make_shared<SoLoud::Wav>();
    SoLoud::result res = sound->loadMem(filedata.getMemPtr(), filedata.length(), false, false);

    if (res == SoLoud::SOLOUD_ERRORS::FILE_LOAD_FAILED){
        if (trackProgress) {
            ResourceProgress::failBuild(buildId);
        }
        Log::error("Sound file type of '%s' could not be loaded", filename.c_str());
        build.error = "Sound file type of '" + filename + "' could not be loaded";
        return build;
    }else if (res == SoLoud::SOLOUD_ERRORS::OUT_OF_MEMORY){
        if (trackProgress) {
            ResourceProgress::failBuild(buildId);
        }
        Log::error("Out of memory when loading '%s'", filename.c_str());
        build.error = "Out of memory when loading '" + filename + "'";
        return build;
    }else if (res == SoLoud::SOLOUD_ERRORS::UNKNOWN_ERROR){
        if (trackProgress) {
            ResourceProgress::failBuild(buildId);
        }
        Log::error("Unknown error when loading '%s'", filename.c_str());
        build.error = "Unknown error when loading '" + filename + "'";
        return build;
    }

    sound->setSingleInstance(true);
    sound->setVolume(1.0);

    if (trackProgress) {
        if (shutdownRequested.load()) {
            ResourceProgress::failBuild(buildId);
            build.error = "Shutdown requested";
            return build;
        }
        ResourceProgress::updateProgress(buildId, 1.0f);
        ResourceProgress::completeBuild(buildId);
    }

    build.wav = sound;
    return build;
}

std::string SoundPool::getSoundDisplayName(const std::string& path){
    std::filesystem::path filePath(path);
    return filePath.filename().string();
}

void SoundPool::requestShutdown(){
    std::lock_guard<std::mutex> lock(cacheMutex);
    shutdownRequested = true;

    for (auto& [id, future] : pendingBuilds) {
        if (future.valid()) {
            future.wait();
        }
    }
    pendingBuilds.clear();
}

void SoundPool::cancelShutdown(){
    std::lock_guard<std::mutex> lock(cacheMutex);
    shutdownRequested = false;
}

void SoundPool::remove(const std::string& id){
    bool removedPending = false;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto pendingIt = pendingBuilds.find(id);
        if (pendingIt != pendingBuilds.end()) {
            // ThreadPoolManager uses packaged_task; destroying its future does
            // not cancel the queued task. Wait so it cannot outlive removal or
            // update a later build that reuses the same resource id.
            if (pendingIt->second.valid()) {
                pendingIt->second.wait();
            }
            pendingBuilds.erase(pendingIt);
            removedPending = true;
        }
        failedBuilds.erase(id);
    }

    auto it = getMap().find(id);
    if (it != getMap().end()){
        if (!it->second || it->second.use_count() <= 1){
            getMap().erase(it);
        }
    }else if (!removedPending){
        if (Engine::isViewLoaded()){
            Log::debug("Trying to destroy a non existent sound: %s", id.c_str());
        }
    }
}

void SoundPool::clear(){
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (auto& [id, future] : pendingBuilds) {
            if (future.valid()) {
                future.wait();
            }
        }
        pendingBuilds.clear();
        failedBuilds.clear();
    }
    getMap().clear();
}

void SoundPool::clearUnused(){
    // Quiesce in-flight loads before sweeping the cache. The worker functions do
    // not acquire cacheMutex, while holding it prevents a replacement load from
    // being registered under the same id during the wait.
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (auto& [id, future] : pendingBuilds) {
            if (future.valid()) {
                future.wait();
            }
        }
        pendingBuilds.clear();
        failedBuilds.clear();
    }

    std::vector<std::string> ids;
    ids.reserve(getMap().size());
    for (const auto& [id, _] : getMap()) {
        ids.push_back(id);
    }
    for (const auto& id : ids) {
        remove(id);
    }
}
