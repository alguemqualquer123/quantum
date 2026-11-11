// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "PluginUiSchema.h"

#include <map>
#include <string>
#include <vector>

namespace quantum {
namespace plugin {
    class PluginManager;
    struct PluginWindowInfo;
}
}

namespace quantum::editor {

    // The editor half of a plugin window. The plugin never draws: it registers an id, and once per
    // frame describes its controls through IPlugin::onDrawWindow, which this renders with the
    // editor's own ImGui context (see PLUGIN_MARKETPLACE_PLAN.md, D6). Nothing here knows what any
    // plugin's window contains, so a new plugin needs no change in the editor.
    //
    // Windows are keyed by their registry id and their ImGui label carries "title###id": two
    // plugins are allowed to name a window "Settings", and ImGui keys a window by its label.
    class PluginWindowHost {
    public:
        PluginWindowHost();

        PluginWindowHost(const PluginWindowHost&) = delete;
        PluginWindowHost& operator=(const PluginWindowHost&) = delete;

        // Drains the engine's open-request queue, drops any window whose plugin went away and draws
        // the rest. Called once per frame, after the fixed panels.
        void show();

        bool isWindowOpen(const std::string& windowId) const;
        void setWindowOpen(const std::string& windowId, bool open);
        void toggleWindow(const std::string& windowId);

        // What to persist: the windows drawn now plus the ones still waiting for their plugin. Both
        // are "open windows" from the user's point of view, and a project plugin's window is waiting
        // until the next start rather than lost.
        std::vector<std::string> wantedWindowIds() const;

        // The list read back from settings. Idempotent by design: a second call would erase the
        // windows the user opened since the first, so priming is safe from any path that runs early.
        void restoreWantedWindows(const std::vector<std::string>& windowIds);

        // True once after anything opened or closed, so the list is written at the frame boundary
        // instead of every frame.
        bool takeVisibilityChange();

    private:
        quantum::plugin::PluginManager& manager;

        // Draw order, oldest first.
        std::vector<std::string> openIds;

        // One described frame per open window, kept between frames so a text buffer the panel holds
        // can be re-synced instead of reset on every rebuild.
        std::map<std::string, quantum::plugin::UiSchema> frames;

        // Restored ids no plugin has registered yet. Engine bootstrap maps the binaries, so this is
        // normally empty by the first frame; a project's own plugins only load on the next start.
        std::vector<std::string> waitingIds;

        // Set by the first restoreWantedWindows() call, which is the one that counts.
        bool primed = false;

        bool visibilityChanged = false;

        bool isOpen(const std::string& windowId) const;
        void open(const std::string& windowId);
        void close(const std::string& windowId);
        void resolveWaiting();
        void drawOne(const quantum::plugin::PluginWindowInfo& info);
    };

}
