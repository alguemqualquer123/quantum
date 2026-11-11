// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include "Scene.h"

namespace fs = std::filesystem;

namespace quantum::editor{

    class Conector{
    private:
        void* libHandle;
        fs::path loadedLibCopy;

        bool fileExists(const fs::path& path);
        void* loadSharedLibrary(const std::string& libPath);
        void unloadSharedLibrary(void* libHandle);
        bool stageLibrary(const fs::path& buildPath, const std::string& libName,
                          void*& outHandle, fs::path& outCopy);

    public:
        Conector();
        virtual ~Conector();

        bool connect(const fs::path& buildPath, std::string libName);
        // The caller invokes this on the owner thread: cleanup/init touch Scene and subscriptions.
        // A replacement is staged and ABI-checked before the current library is torn down.
        bool reload(const fs::path& buildPath, const std::string& libName,
                    const std::vector<Scene*>& scenes);
        void disconnect();

        void init(Scene* sceneProject);
        void cleanup(Scene* sceneProject);
        void cleanupEntity(Scene* scene, Entity entity);

        bool isLibraryConnected() const;
    };

}
