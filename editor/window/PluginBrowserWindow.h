// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"

#include "PluginManager.h"

#include <string>
#include <vector>

namespace quantum::editor {

    // What the engine found in the plugins directory, in the order it found it, and why anything is
    // not running. A record stays in the list when its plugin fails or is disabled, because the
    // reason is the only useful thing such a row has to show.
    //
    // The engine owns the records: this window reads PluginManager::instance(), the same singleton
    // the bootstrap loaded through. Plugins are installed per directory, not per project, so the
    // window takes no Project pointer - unlike the World Creator panel, which writes into one.
    class PluginBrowserWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Plugins";

        PluginBrowserWindow();

        PluginBrowserWindow(const PluginBrowserWindow&) = delete;
        PluginBrowserWindow& operator=(const PluginBrowserWindow&) = delete;

        void show();
        void setOpen(bool open);
        bool isOpen() const;
        void focus();

    private:
        quantum::plugin::PluginManager& manager;

        bool windowOpen = false;
        bool focusRequested = false;

        // Index into the manager's record list, not a pointer: a reload replaces a record's
        // contents and an install appends to the vector.
        int selected = -1;

        std::string statusLine;
        std::string lastResult;
        char installPathInput[512] = "";
        bool overwriteInstall = false;

        void drawToolbar();
        void drawList();
        void drawDetails(quantum::plugin::PluginRecord& record);
        void drawManifest(quantum::plugin::PluginRecord& record);
        void drawPanel(quantum::plugin::PluginRecord& record);
        void drawSettings(quantum::plugin::PluginRecord& record);
        void drawLog(const std::string& pluginId);

        quantum::plugin::PluginRecord* selectedRecord();
        void setStatus(const char* format, ...);
    };

}
