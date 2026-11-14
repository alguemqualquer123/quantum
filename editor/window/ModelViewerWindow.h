// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"
#include "math/Vector3.h"
#include "render/preview/ModelRender.h"
#include "texture/Texture.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace quantum::editor {

    class Project;

    // One model on its own, in a window of its own. Double clicking a model asset used to
    // send it to the text editor, which shows nothing of a binary mesh.
    class ModelViewerWindow {
    private:
        static constexpr float MIN_ZOOM = 0.05f;
        static constexpr float MAX_ZOOM = 40.0f;

        struct Instance {
            bool isOpen = true;
            bool focusRequested = false;

            std::filesystem::path filepath;
            std::string loadError;
            std::string notice;

            ModelRender render;

            // Orbit state kept in the model's own units, so any asset frames the same
            Vector3 focus{0.0f, 0.0f, 0.0f};
            float yaw = 0.65f;
            float pitch = 0.32f;
            float distance = 3.0f;
            float fitDistance = 3.0f;
            float radius = 1.0f;
            bool framed = false;

            int frameWidth = 0;
            int frameHeight = 0;
            float lightIntensity = 6.0f;
            bool needsRender = true;
            // A reload tears the preview scene down, so it waits for the frame that no
            // longer has its framebuffer in the submitted draw data
            bool needsReload = false;

            unsigned int meshCount = 0;
            Vector3 boundsSize{0.0f, 0.0f, 0.0f};
        };

        Project* project;
        std::unordered_map<std::string, std::unique_ptr<Instance>> instances;
        // A closed tab is torn down on the next frame: its framebuffer is still in the
        // draw data ImGui submitted for this one
        std::vector<std::unique_ptr<Instance>> retired;

        std::filesystem::path resolveFilepath(const std::filesystem::path& path) const;
        std::string toRelativePath(const std::filesystem::path& path) const;

        bool ensureImported(Instance& instance, const std::filesystem::path& fullPath);
        void loadModel(Instance& instance);
        void frameModel(Instance& instance);

        void drawViewport(Instance& instance, const std::filesystem::path& fullPath);
        void drawMaterials(Instance& instance);
        void drawActions(Instance& instance, const std::filesystem::path& fullPath);
        void clearInstance(Instance& instance);

    public:
        explicit ModelViewerWindow(Project* project);
        ~ModelViewerWindow();

        void openFile(const std::filesystem::path& path, bool dockToCentral = true);
        void closeFile(const std::filesystem::path& path);
        void closeAll();
        void show();

        bool isFileOpen(const std::filesystem::path& path) const;
        bool handleFileRename(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        // The preview of a model the user reimported or whose file changed on disk
        void reloadFile(const std::filesystem::path& path);

        static std::string getWindowId(const std::filesystem::path& relativePath);
    };

}
