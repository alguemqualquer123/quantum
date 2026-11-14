// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef SOUNDPOOL_H
#define SOUNDPOOL_H

#include "Engine.h"

#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace SoLoud{
    class Wav;
}

namespace quantum{

    typedef std::map<std::string, std::shared_ptr<SoLoud::Wav>> sounds_t;

    struct SoundLoadResult {
        std::string id;
        ResourceLoadState state = ResourceLoadState::NotStarted;
        std::string errorMessage;
        std::shared_ptr<SoLoud::Wav> data = nullptr;

    };

    // See TextureBuildResult: a build thread reports a bad file through this value,
    // because _HAS_EXCEPTIONS=0 leaves std::packaged_task without an exception slot
    // and a throw on a pool thread aborts the process.
    struct SoundBuildResult {
        std::shared_ptr<SoLoud::Wav> wav;
        std::string error;

        bool ok() const {
            return error.empty();
        }
    };

    class QUANTUM_API SoundPool{
    private:
        static sounds_t& getMap();

        static std::unordered_map<std::string, std::future<SoundBuildResult>> pendingBuilds;
        static std::unordered_set<std::string> failedBuilds;
        static std::mutex cacheMutex;
        static std::atomic<bool> shutdownRequested;

        static SoundBuildResult loadSoundInternal(const std::string& id, const std::string& filename, bool trackProgress);
        static std::string getSoundDisplayName(const std::string& path);

    public:
        static SoundLoadResult loadFromFile(const std::string& id, const std::string& filename);
        // A failed file is not tried again until this or remove()
        static void retry(const std::string& id);

        static void requestShutdown();
        // re-allows async loads after a recreated view
        static void cancelShutdown();

        static void remove(const std::string& id);

        // necessary for engine shutdown
        static void clear();
        // removes only entries no one else references (safe while other scenes are alive)
        static void clearUnused();
    };
}

#endif /* SOUNDPOOL_H */