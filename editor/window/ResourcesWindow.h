// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "window/CodeEditor.h"
#include "util/EngineAssets.h"
#include "util/EntityPayload.h"
#include "render/preview/ModelRender.h"

#include <iostream>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <unordered_map>
#include <condition_variable>
#include <cstdint>

#include "imgui.h"

namespace fs = std::filesystem;

namespace SoLoud {
    class Wav;
}

namespace quantum::editor {

    class ImageViewerWindow;

    enum class FileType {
        NONE,
        IMAGE,
        MATERIAL,
        SCENE,
        BUNDLE,
        MODEL,
        SOURCE,
        HEADER,
        LUA,
        GRAPH,
        AUDIO,
        FONT
    };

    struct FileEntry {
        std::string name;
        // name and name-without-extension reshaped for display, built with the entry
        // because ImGui cannot lay out right-to-left text and this list has no clipper
        std::string displayName;
        std::string displayBaseName;
        std::string extension;
        FileType type = FileType::NONE;
        bool isDirectory;
        TextureRender* icon = nullptr;
        bool hasThumbnail;
        std::string thumbnailPath;
        // Engine-shipped entries are drawn with an origin badge and are read-only:
        // the engine folder is a shipped copy, not something a project may edit.
        AssetOrigin origin = AssetOrigin::Project;
    };

    struct ThumbnailRequest {
        fs::path path;
        FileType type = FileType::NONE;
        uint64_t generation = 0;
    };

    struct ThumbnailTexture {
        Texture texture;
        int lastUsedFrame = 0;
        bool failed = false;
        // A row can ask for its thumbnail before the worker has written the file. That is not a
        // load failure - caching one would leave the row blank for the rest of the session - so
        // the path is remembered as pending and tried again after THUMBNAIL_RETRY_FRAMES.
        int retryFrame = 0;
    };

    // One directory child as the scanner thread sees it: names and flags only, so the
    // listing itself (icons, bidi text, thumbnails) is still built on the main thread.
    struct RawDirectoryEntry {
        std::string name;
        bool isDirectory = false;
        std::string extension;
    };

    class ResourcesWindow {
    private:
        Project* project;
        CodeEditor* codeEditor;
        ImageViewerWindow* imageViewerWindow;

        enum class LayoutType {
            AUTO,              // Automatically switch between GRID and SPLIT_FILES_ONLY based on window size
            GRID,              // Single panel, directories shown in the file listing
            SPLIT_FILES_ONLY,  // Directory tree sidebar, listing shows files only
            SPLIT              // Directory tree sidebar, directories included in the listing
        };
        LayoutType currentLayout = LayoutType::AUTO;

        enum class ItemViewStyle {
            CARD,
            CLASSIC
        };
        ItemViewStyle itemViewStyle = ItemViewStyle::CLASSIC;

        float layoutAutoThreshold = 600.0f;
        float leftPanelWidth = 200.0f;  // Width of the left panel in SPLIT layout

        bool firstOpen;
        bool requestSort;
        fs::path currentPath;
        fs::path treeSelectedPath;
        std::vector<editor::FileEntry> files;
        // Directory the rows in `files` belong to. A rescan of the folder the user is
        // already standing in keeps them (no flicker on a create or a delete); moving
        // somewhere else blanks them at once.
        fs::path displayedPath;

        Texture folderIcon;
        Texture fileIcon;
        Texture sceneIcon;
        Texture entityIcon;
        Texture sourceIcon;
        Texture headerIcon;
        Texture luaIcon;
        Texture audioIcon;
        Texture fontIcon;
        // Bounded cache of GPU thumbnail textures; must stay well below the
        // sokol image/sampler pool sizes (2048), which are shared with scenes
        static constexpr size_t MAX_THUMBNAIL_TEXTURES = 512;
        // How often a row whose thumbnail file has not been written yet re-checks the disk
        static constexpr int THUMBNAIL_RETRY_FRAMES = 30;
        std::unordered_map<std::string, ThumbnailTexture> thumbnailTextures;
        std::unordered_set<std::string> pendingThumbnailRequests;

        int iconSize;

        std::string lastSelectedFile;
        std::unordered_set<std::string> selectedFiles;
        std::string hoveredFile;
        std::unordered_set<std::string> projectFiles;  // project-relative scene and built bundle paths
        bool ctrlPressed;
        bool shiftPressed;

        bool isDragging;
        ImVec2 dragStart;
        ImVec2 dragEnd;
        ImVec2 windowPos;
        ImVec2 scrollOffset;

        bool isDragDropTarget;
        bool isExternalDragHovering;

        std::vector<std::string> clipboardFiles;
        bool clipboardCut;

        char nameBuffer[256];

        bool isRenaming;
        bool renameSelectPending;
        std::string fileBeingRenamed;

        enum class NewItemType {
            FOLDER,
            LUA_SCRIPT,
            CPP_SOURCE,
            CPP_HEADER,
            GRAPH
        };
        bool isCreatingNewItem;
        NewItemType newItemType = NewItemType::FOLDER;
        fs::path newItemDirectory;

        std::filesystem::file_time_type lastWriteTime;
        float timeSinceLastCheck;

        // Children of each folder-tree node: renderDirectoryTree() runs every frame,
        // and enumerating there was the tree's whole cost. Re-read only when the
        // folder's own write time changes, on the same 1 Hz check as the listing.
        struct DirTreeEntry {
            std::vector<fs::path> subDirs;
            std::filesystem::file_time_type writeTime{};
            double lastCheckTime = 0.0;
            uint64_t generation = 0;
        };
        std::unordered_map<std::string, DirTreeEntry> dirTreeCache;
        // Invalidated by bumping this rather than clearing the map: scanDirectory()
        // is reachable from inside the tree walk, which holds a reference into it.
        // Starts at 1 so a default-constructed entry is already stale.
        uint64_t dirTreeGeneration = 1;

        bool windowOpen;
        bool focusRequested;
        bool windowFocused;

        // True when the UILab host owns the window and calls drawContent().
        bool hostedWindow = false;

        bool showDeleteConfirmation;

        MaterialRender materialRender;
        ModelRender modelRender;

        std::shared_ptr<SoLoud::Wav> audioPreviewSample;
        fs::path audioPreviewPath;
        std::vector<float> audioPreviewWaveform;
        unsigned int audioPreviewHandle = 0;
        double audioPreviewLength = 0.0;
        double audioPreviewTime = 0.0;
        bool audioPreviewPlaying = false;
        std::string audioPreviewError;

        // Thumbnail generation
        std::thread thumbnailThread;
        std::mutex thumbnailMutex;
        std::queue<ThumbnailRequest> thumbnailQueue;
        std::atomic<bool> stopThumbnailThread;
        std::condition_variable thumbnailCondition;
        bool thumbnailWorkerBusy = false; // guarded by thumbnailMutex
        bool thumbnailWorkSuspended = false; // guarded by thumbnailMutex
        uint64_t thumbnailGeneration = 0; // guarded by thumbnailMutex
        size_t activeThumbnailSaves = 0; // guarded by thumbnailMutex

        // Queue for completed thumbnails
        std::mutex completedThumbnailMutex;
        std::queue<ThumbnailRequest> completedThumbnailQueue;

        // Add these to the private section of the ResourcesWindow class
        fs::path pendingMaterialPath;
        bool hasPendingMaterialRender = false; // guarded by thumbnailMutex
        uint64_t pendingMaterialGeneration = 0; // guarded by thumbnailMutex
        std::mutex materialRenderMutex;

        fs::path pendingModelPath;
        bool hasPendingModelRender = false; // guarded by thumbnailMutex
        uint64_t pendingModelGeneration = 0; // guarded by thumbnailMutex
        std::mutex modelRenderMutex;

        // Listing a directory is UI-thread work only for the draw: walking it is not.
        // A project folder with thousands of files used to block the frame inside
        // fs::directory_iterator, so a worker reads the names and hops back with
        // EditorHost::enqueueMainThreadTask to build what renderFileListing() draws.
        std::thread scanThread;
        // mutable: isScanPending() is a const read of the UI state, and a mutex locked
        // to read a guarded counter is not a logical change of the window.
        mutable std::mutex scanMutex;
        std::condition_variable scanCondition;
        bool scanStopRequested = false;      // guarded by scanMutex
        bool scanJobPending = false;         // guarded by scanMutex
        bool scanWorkerBusy = false;         // guarded by scanMutex
        fs::path scanJobPath;                // guarded by scanMutex
        // One outstanding job: a request that lands while another is still queued
        // simply replaces it, because only the newest directory is worth listing.
        uint64_t scanGeneration = 0;
        uint64_t scanAppliedGeneration = 0;
        // Neither the worker nor the queued task can be joined from inside a frame, so
        // both check this instead of touching a window that is already gone.
        std::shared_ptr<std::atomic<bool>> scanAlive;

        // Names the listing, so a folder with thousands of entries stays usable.
        char filterBuffer[256];

        ImU32 fileSeparatorColor(const FileEntry& fe) const;
        void refreshProjectFiles();
        bool fileNotInProject(const FileEntry& fe) const;

        // Engine content is browsable but never writable: the folder next to the
        // executable is a shipped copy, and the AppImage one is temporary.
        bool browsingEngine() const;
        bool isEnginePath(const fs::path& path) const;
        // The folder the breadcrumb and the back button stop at: the project root for
        // project content, the engine root for engine content.
        fs::path browseRoot() const;
        bool isScanPending() const;
        void drawOriginBadge(const FileEntry& file, const ImVec2& itemMin);
        bool matchesFilter(const FileEntry& file) const;

        void renderHeader();
        void renderSourceSelector();
        void renderFilterRow();
        void renderPathBreadcrumb(const ImVec2& size);
        void renderFileListing(bool showDirectories);
        void renderNewItemMenu(const fs::path& directory);
        void renderDirectoryTree(const fs::path& path, AssetOrigin origin = AssetOrigin::Project);
        void renderEngineTree();
        const std::vector<fs::path>& treeSubdirectories(const fs::path& path);

        void scanDirectory(const fs::path& path);
        void startDirectoryScan(const fs::path& path);
        void scanWorker();
        void applyScanResult(const fs::path& path, uint64_t generation, std::vector<RawDirectoryEntry> entries);
        static std::vector<RawDirectoryEntry> readDirectory(const fs::path& path);
        void stopScanWorker();
        void sortWithSortSpecs(ImGuiTableSortSpecs* sortSpecs, std::vector<FileEntry>& files);
        void highlightDragAndDrop();
        void handleInternalDragAndDrop(const fs::path& targetDirectory);
        void startNewItem(NewItemType type, const fs::path& directory);
        void handleNewItem();
        void startRename(const std::string& fileName);
        void handleRename();
        void copySelectedFiles(bool cut);
        void pasteFiles(const fs::path& targetDirectory);
        // The engine folder is read-only, so taking one of its assets into the project
        // is the action that makes it usable: a plain copy, undoable like an import.
        void copyEngineSelectionIntoProject();
        void importExternalPaths(const std::vector<std::string>& sourcePaths);
        static FileType classifyThumbnailFileType(const fs::path& filePath);
        static std::string thumbnailRequestKey(const fs::path& filePath);
        static unsigned char textureByteAt(TextureData& data, size_t pixel, int channel, int bytesPerChannel);
        static bool writeImageThumbnail(const fs::path& sourcePath, const fs::path& thumbnailPath);

        void queueThumbnailGeneration(const fs::path& filePath, FileType type, bool forceRegenerate = false);
        void thumbnailWorker();
        ImTextureID getThumbnailTexture(const std::string& thumbnailPath, int& outWidth, int& outHeight);
        void evictThumbnailTexture(const std::string& thumbnailKey);
        void enforceThumbnailCacheLimit();
        void clearThumbnailTextures();

        void openAudioPreview(const fs::path& filePath);
        void stopAudioPreview(bool unload = true);
        void drawAudioPreview();
        static std::vector<float> loadWaveform(const fs::path& filePath, size_t sampleCount);

        void saveMaterialFile(const fs::path& directory, const char* materialContent, size_t contentLen, const MaterialPayload* sourceMaterial = nullptr);

        fs::path uniqueRelativePath(const fs::path& directory, const std::string& baseName, const std::string& extension);

    public:
        static constexpr const char* WINDOW_NAME = "Resources";

        ResourcesWindow(Project* project, CodeEditor* codeEditor, ImageViewerWindow* imageViewerWindow);
        ~ResourcesWindow();

        bool isFocused() const;

        void notifyProjectPathChange();
        void cancelThumbnailWork();
        void resumeThumbnailWork();

        void handleExternalDragEnter();
        void handleExternalDragLeave();

        // need to be after draw
        void processMaterialThumbnails();
        void processModelThumbnails();

        // True while any thumbnail is still generating; the render loop must keep
        // drawing while set (thumbnail scenes share systemDraw).
        bool hasPendingThumbnailWork();

        void notifyResourceFileChanged(const fs::path& filePath);
        void requestThumbnailGeneration(const fs::path& filePath, bool forceRegenerate = false);
        ImTextureID getAssetThumbnail(const fs::path& filePath, int& outWidth, int& outHeight);

        void cleanupThumbnails();

        void refreshCurrentDirectory();
        fs::path getCurrentPath() const { return currentPath; }
        void saveEntityFile(const fs::path& directory, const std::string& entityYaml);

        void show();
        void setOpen(bool open);
        bool isOpen() const;

        // Draws the panel's contents inside a window somebody else opened, and runs
        // the work that has to happen once the window is closed. The UILab host
        // calls both around its asset browser panel.
        void drawContent();
        void afterDraw();

        // True when the UILab host owns the window: show() then keeps the per-frame
        // bookkeeping and leaves Begin/End to the library.
        void setHostedWindow(bool hosted);
        bool* openFlag();
    };

}

