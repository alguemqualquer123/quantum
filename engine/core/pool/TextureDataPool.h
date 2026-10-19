// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef TEXTUREDATAPOOL_H
#define TEXTUREDATAPOOL_H

#include "Engine.h"
#include "render/TextureRender.h"
#include "texture/TextureData.h"

#include <map>
#include <memory>
#include <array>

#include <mutex>
#include <future>
#include <unordered_map>
#include <atomic>

namespace doriax{

    typedef std::map< std::string, std::shared_ptr<std::array<TextureData,6>> > texturesdata_t;

    struct TextureLoadResult {
        std::string id;
        ResourceLoadState state = ResourceLoadState::NotStarted;
        std::string errorMessage;
        std::shared_ptr<std::array<TextureData, 6>> data = nullptr;

        TextureLoadResult() = default;

        explicit operator bool() const {
            return state == ResourceLoadState::Finished;
        }
    };

    // What a build thread hands back. A worker must never throw: Jolt propagates
    // _HAS_EXCEPTIONS=0 into the engine, std::packaged_task then keeps no exception
    // slot, and the escaped throw ends the whole process in abort() - so a failed
    // asset is reported as a value and the caller decides what to show.
    struct TextureBuildResult {
        std::array<TextureData, 6> data;
        std::string error;

        bool ok() const {
            return error.empty();
        }
    };

    class DORIAX_API TextureDataPool{
    private:
        static texturesdata_t& getMap();

        static std::unordered_map<std::string, std::future<TextureBuildResult>> pendingBuilds;
        static std::mutex cacheMutex;
        static std::atomic<bool> shutdownRequested;

        static bool hasTexturePixels(const std::shared_ptr<std::array<TextureData,6>>& data, size_t numFaces);
        static size_t getTextureFaces(std::array<TextureData,6>& data);

        static TextureBuildResult loadTextureInternal(const std::string& id, const std::array<std::string, 6>& paths, size_t numFaces, bool trackProgress);
        static std::string getTextureDisplayName(const std::string& path);
        static std::string validateTextureFaces(std::array<TextureData,6>& data, size_t numFaces);

    public:
        static std::shared_ptr<std::array<TextureData,6>> get(const std::string& id);
        static std::shared_ptr<std::array<TextureData,6>> get(const std::string& id, std::array<TextureData,6> data);
        // Inserts an already-built face array without copying it (used to hand pre-decoded model
        // textures to the pool). No-op if the id is already cached with pixels. Main-thread only.
        static void put(const std::string& id, std::shared_ptr<std::array<TextureData,6>> data);

        static TextureLoadResult loadFromFile(const std::string& id, const std::array<std::string, 6>& paths, size_t numFaces);

        static void requestShutdown();

        static void remove(const std::string& id);
        static void clear();
        // removes only entries no one else references (safe while other scenes are alive)
        static void clearUnused();
    };
}

#endif /* TEXTUREDATAPOOL_H */
