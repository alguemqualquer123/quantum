// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"
#include "render/preview/MaterialRender.h"
#include "texture/Texture.h"
#include "util/MaterialSets.h"

#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace quantum::editor {

    class Project;

    // The texture page of a material library, drawn the way the free PBR sites draw it:
    // a grid of lit studio spheres, one per material set, so a ground, a rock and a
    // metal read as surfaces at a glance instead of as file names.
    //
    // A set is found from the file names in a folder (see MaterialSets), not from a
    // manifest, so anything the user downloads off the internet browses without a stop
    // in between.
    class MaterialBrowserWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Material Library";

        explicit MaterialBrowserWindow(Project* project);
        ~MaterialBrowserWindow();

        MaterialBrowserWindow(const MaterialBrowserWindow&) = delete;
        MaterialBrowserWindow& operator=(const MaterialBrowserWindow&) = delete;

        void show();
        void setOpen(bool open);
        bool isOpen() const;

        // The library does not follow the project's textures only: a folder of maps
        // downloaded anywhere on disk is a library too.
        void chooseFolder();

        // Runs from App::engineRender() right after Engine::systemDraw(): executeSceneOnce
        // only schedules the sphere, so its framebuffer has content after that draw and
        // never during the ImGui pass.
        void processPendingPreviews();

    private:
        static constexpr int MAX_SOURCE_FILES = 20000;

        struct Thumb {
            Texture texture;
            bool failed = false;
            unsigned long lastUsedFrame = 0;
        };

        struct Entry {
            MaterialSet set;
            // Content hash: the same maps at the same tiling keep the same cached sphere,
            // so a rescan of an unchanged library renders nothing.
            std::string key;
            std::filesystem::path thumbPath;
            bool thumbReady = false;
            bool thumbBusy = false;
        };

        Project* project;

        MaterialRender gridRender;
        MaterialRender heroRender;

        std::vector<Entry> entries;
        std::vector<size_t> visible;
        std::unordered_map<std::string, Thumb> thumbs;

        std::string filter;
        char filterInput[128] = "";
        std::filesystem::path externalDir;
        std::filesystem::path cacheDir;

        bool rescanPending = true;
        int selected = -1;
        float tiling = 2.0f;
        float cellSize = 0.0f;
        float lightIntensity = 2.0f;
        bool receiveIBL = true;
        std::string notice;

        // Hero orbit, in the material's own units
        float yaw = 0.6f;
        float pitch = 0.25f;
        float distance = 2.6f;
        int heroWidth = 0;
        int heroHeight = 0;
        bool needsHeroRender = true;

        // The PNG write runs on a detached thread, so the window keeps a generation
        // counter and waits for the saves it started before it is destroyed.
        std::mutex saveMutex;
        std::condition_variable saveCondition;
        std::vector<std::string> finishedKeys;
        unsigned long generation = 0;
        int activeSaves = 0;

        bool hasActiveSave();
        void retireGeneration();

        // The sphere staged by the UI pass, read back by the next engineRender(). Held by
        // key and path rather than by entry: a rescan while it is in flight only means the
        // PNG lands in a cache slot nobody asks for again.
        std::string stagedKey;
        std::filesystem::path stagedPath;
        bool stagePending = false;

        // The maps arrive over several frames (the editor loads textures asynchronously),
        // and each of those frames has to be drawn for the next one to see them.
        int warmFrames = 0;

        Entry* entryByKey(const std::string& key);

        bool windowOpen = true;

        std::filesystem::path thumbnailPath(const std::string& key) const;
        static std::string hashSet(const MaterialSet& set, float tiling);

        void rescan();
        void collectFrom(const std::filesystem::path& root, std::vector<std::filesystem::path>& out) const;
        void applyFilter();
        void stageNextThumb();
        void drainFinishedSaves();
        void invalidateThumbs();

        ImTextureID thumbTexture(const std::filesystem::path& path);
        void evictThumbs();

        void drawToolbar();
        void drawHero(Entry& entry);
        void drawGrid();
    };

}
