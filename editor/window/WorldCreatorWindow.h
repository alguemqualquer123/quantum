// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"
#include "texture/Texture.h"
#include "util/TerrainGenerator.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace quantum::editor {

    class Project;

    // Procedural heightmaps inside the editor: the same TerrainGenerator a Lua script or the CLI
    // calls, a preview of what it produced, and a path onto the selected terrain that undoes.
    //
    // Generation is CPU work that runs in the frame it is asked for. A 512 map with 6000 droplets
    // takes about half a second; the async path is FASE 7 and is not faked here.
    class WorldCreatorWindow {
    public:
        static constexpr const char* WINDOW_NAME = "World Creator";

        explicit WorldCreatorWindow(Project* project);

        WorldCreatorWindow(const WorldCreatorWindow&) = delete;
        WorldCreatorWindow& operator=(const WorldCreatorWindow&) = delete;

        ~WorldCreatorWindow();

        void show();
        void setOpen(bool open);
        bool isOpen() const;
        void focus();

        struct HeightmapStats {
            float lowest = 0.0f;
            float average = 0.0f;
            float highest = 0.0f;
            // Share of the texels below the preset's sea level, not a count: a bigger map with the
            // same coast line reports the same number.
            float submerged = 0.0f;
        };

        // The readout under the preview, from the field the generator produced.
        static HeightmapStats measureField(const std::vector<float>& field,
                                           const quantum::TerrainGenParams& params);

        // The write behind the Generate button with no window, no project and no ImGui: the CLI's
        // `worldcreator selftest` calls this same function, so what the probe checks is what the
        // button does. `directory` is created if missing; `writtenFile` gets the full path;
        // `heights`, when given, keeps the field so a caller that also wants a preview does not
        // run the generator twice.
        static bool writeHeightmap(const std::filesystem::path& directory, const std::string& fileName,
                                   const quantum::TerrainGenParams& params, std::string& writtenFile,
                                   HeightmapStats& stats, std::vector<float>* heights,
                                   std::string& error);

    private:
        Project* project;

        bool windowOpen = false;
        bool focusRequested = false;

        quantum::TerrainGenParams params;
        int presetIndex = 1;

        std::vector<float> heights;
        int mapSize = 0;
        bool hasMap = false;

        float lowest = 0.0f;
        float average = 0.0f;
        float highest = 0.0f;
        float submerged = 0.0f;

        quantum::Texture preview;
        unsigned long long previewGeneration = 0;

        char nameInput[128] = "worldgen";
        // The heightmap this window last wrote, relative to the project's assets - which is the
        // spelling a terrain component stores.
        std::string writtenPath;
        std::string message;
        bool messageOk = true;

        bool hasTarget = false;
        uint32_t targetSceneId = 0;
        unsigned int targetEntity = 0;

        void drawParameters();
        void drawPreview();
        void drawTarget();

        void applyPreset();
        void generate();
        void applyToTarget();

        void updateTarget();
        void rebuildPreview();
        void measure();
        std::string mapFileName() const;

        void setMessage(bool ok, const char* format, ...);
    };

}
