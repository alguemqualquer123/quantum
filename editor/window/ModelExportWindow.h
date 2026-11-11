// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"

#include "model/MeshImport.h"
#include "model/MeshReport.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace quantum::editor {

    class Project;

    // One mesh, read by the modelling kernel and written back out by it. The point is not
    // the copy: it is that the file is answered with the numbers the kernel actually got
    // (welded vertices kept, polygons kept, what the format dropped) instead of the silent
    // assumption that a loader read what it was given.
    //
    // It is also the only way to see the editing operations work before a viewport exists:
    // an .obj edited by MeshModeling can be exported here and opened in another tool.
    class ModelExportWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Model Export";

        explicit ModelExportWindow(Project* project);

        ModelExportWindow(const ModelExportWindow&) = delete;
        ModelExportWindow& operator=(const ModelExportWindow&) = delete;

        void show();
        void setOpen(bool open);
        bool isOpen() const;
        void focus();

        // Load a file the content browser or a drop handed over, without waiting for the
        // panel to be open: the list is rebuilt on the next frame.
        void openFile(const std::filesystem::path& path);

    private:
        enum class Format {
            Glb,
            Obj
        };

        struct Source {
            std::filesystem::path path;
            // An .fbx has no reader in the kernel (ufbx is linked into the editor, not the
            // engine), so its import - the .glb the editor already keeps beside it - is
            // what gets read.
            std::filesystem::path readFile;
            bool fromImport = false;
        };

        Project* project;

        std::vector<Source> sources;
        std::vector<int> visible;
        int selected = -1;
        char filterInput[128] = "";

        std::unique_ptr<quantum::model::ImportedModel> loaded;
        quantum::model::MeshStats stats;
        std::vector<std::string> loadWarnings;
        std::string loadError;

        Format format = Format::Glb;
        char nameInput[256] = "mesh";
        bool writeNormals = true;
        bool writeUVs = true;
        char outputDirInput[512] = "";
        bool verify = true;

        std::string result;
        bool resultOk = false;

        bool windowOpen = false;
        bool focusRequested = false;
        bool scrollToSelection = false;
        bool rescanPending = true;
        // The folder the last scan walked. A project that arrives after the window was
        // opened changes it, and the list of models has to follow.
        std::filesystem::path scannedRoot;

        void rescan();
        void applyFilter();
        void rebuildVisibility();

        void select(const std::filesystem::path& path);
        bool load(const Source& source);
        void browseSource();
        void browseOutput();
        std::filesystem::path outputDirectory() const;
        std::filesystem::path outputPath() const;

        // Writes the file and, when asked, reads it back through the kernel's own importer
        // and compares the two sets of numbers.
        void runExport();

        void drawSources();
        void drawDetails();
        void drawOptions();
        void drawStat(const char* label, const std::string& value);

        std::string formatName() const;
    };

}
