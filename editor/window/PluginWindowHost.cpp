// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "window/PluginWindowHost.h"

#include "window/PluginSchemaPanel.h"
#include "Out.h"
#include "PluginManager.h"
#include "PluginRegistry.h"
#include "imgui.h"

#include <algorithm>

using namespace quantum;
using namespace quantum::plugin;

namespace {

    // The size a window opens at, from what the plugin asked for. It is applied once: after that the
    // user's own resize wins, which ImGui remembers by window key.
    ImVec2 openingSize(const PluginWindowInfo& info) {
        const int width = info.width > 0 ? info.width : 420;
        const int height = info.height > 0 ? info.height : 320;
        return ImVec2(static_cast<float>(width), static_cast<float>(height));
    }

    bool contains(const std::vector<std::string>& ids, const std::string& id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }

}

editor::PluginWindowHost::PluginWindowHost()
    : manager(PluginManager::instance()) {
}

void editor::PluginWindowHost::show() {
    // A plugin asking for its own window is a queued request, not a call into editor code: this is
    // the frame boundary, and it is the only place ImGui is touched on its behalf.
    for (const std::string& windowId : manager.registry().takeWindowOpenRequests()) open(windowId);
    resolveWaiting();

    // A snapshot, because drawing one window can close another: a plugin may answer a button by
    // unregistering a window, and the manager drops every extension of a plugin that unloads.
    const std::vector<std::string> thisFrame = openIds;
    for (const std::string& windowId : thisFrame) {
        if (!isOpen(windowId)) continue;

        PluginWindowInfo info;
        if (!manager.registry().findWindow(windowId, info)) {
            // Its plugin unloaded, was disabled or was reloaded without re-registering it. The window
            // goes with the registration instead of drawing an empty frame for the rest of the run.
            close(windowId);
            continue;
        }
        drawOne(info);
    }
}

void editor::PluginWindowHost::drawOne(const PluginWindowInfo& info) {
    UiSchema& schema = frames[info.id];
    std::string error;
    if (!manager.buildWindowSchema(info.id, schema, error)) {
        // Refused or gone: one line in the output, then the window closes. Repeating the error every
        // frame would bury the log entry that explains it.
        Out::error("plugin window \"%s\": %s", info.id.c_str(), error.c_str());
        close(info.id);
        return;
    }

    const std::string title = (schema.title.empty() ? info.title : schema.title) + "###" + info.id;
    ImGui::SetNextWindowSize(openingSize(info), ImGuiCond_FirstUseEver);

    bool userWantsItOpen = true;
    if (!ImGui::Begin(title.c_str(), &userWantsItOpen)) {
        ImGui::End();
        if (!userWantsItOpen) close(info.id);
        return;
    }

    if (schema.groups.empty()) {
        // A plugin that registered a window and describes nothing is a plugin bug, and saying so on
        // the panel is the only report the user ever sees - the plugin's own log needs "log" too.
        ImGui::TextDisabled("%s described no controls for this window.", info.pluginId.c_str());
    } else {
        PluginSchemaPanel::Touched touched;
        if (PluginSchemaPanel::draw(info.id, schema, touched)) {
            std::string sendError;
            if (!manager.sendWindowInput(info.id, collectSchemaValues(schema), touched.pressed,
                                         sendError)) {
                Out::error("plugin window \"%s\": %s", info.id.c_str(), sendError.c_str());
            }
        }
    }

    ImGui::End();
    // The flag is read after End() because Begin() leaves it true when the window only failed to
    // appear this frame (fully transparent, or behind another).
    if (!userWantsItOpen) close(info.id);
}

bool editor::PluginWindowHost::isOpen(const std::string& windowId) const {
    return contains(openIds, windowId);
}

bool editor::PluginWindowHost::isWindowOpen(const std::string& windowId) const {
    return isOpen(windowId);
}

void editor::PluginWindowHost::open(const std::string& windowId) {
    if (windowId.empty() || isOpen(windowId)) return;
    openIds.push_back(windowId);
    waitingIds.erase(std::remove(waitingIds.begin(), waitingIds.end(), windowId), waitingIds.end());
    visibilityChanged = true;
}

void editor::PluginWindowHost::close(const std::string& windowId) {
    openIds.erase(std::remove(openIds.begin(), openIds.end(), windowId), openIds.end());
    frames.erase(windowId);
    waitingIds.erase(std::remove(waitingIds.begin(), waitingIds.end(), windowId), waitingIds.end());
    visibilityChanged = true;
}

void editor::PluginWindowHost::setWindowOpen(const std::string& windowId, bool open) {
    if (open) this->open(windowId);
    else close(windowId);
}

void editor::PluginWindowHost::toggleWindow(const std::string& windowId) {
    if (isOpen(windowId)) close(windowId);
    else open(windowId);
}

void editor::PluginWindowHost::resolveWaiting() {
    if (waitingIds.empty()) return;

    std::vector<std::string> stillWaiting;
    for (const std::string& windowId : waitingIds) {
        PluginWindowInfo info;
        if (manager.registry().findWindow(windowId, info)) open(windowId);
        else stillWaiting.push_back(windowId);
    }
    waitingIds.swap(stillWaiting);
}

std::vector<std::string> editor::PluginWindowHost::wantedWindowIds() const {
    std::vector<std::string> wanted = openIds;
    for (const std::string& windowId : waitingIds) {
        if (!contains(wanted, windowId)) wanted.push_back(windowId);
    }
    return wanted;
}

void editor::PluginWindowHost::restoreWantedWindows(const std::vector<std::string>& windowIds) {
    // Only the first call primes the list. AppSettings is read from more than one place - start-up,
    // the menu build, the frame boundary - and a later read would erase windows the user has opened
    // in the meantime.
    if (primed) return;
    primed = true;

    openIds.clear();
    frames.clear();
    waitingIds.clear();
    for (const std::string& windowId : windowIds) {
        if (!windowId.empty() && !contains(waitingIds, windowId)) waitingIds.push_back(windowId);
    }
    // Priming is not a user change: writing the same list back on the first frame would count as one.
    visibilityChanged = false;
}

bool editor::PluginWindowHost::takeVisibilityChange() {
    const bool changed = visibilityChanged;
    visibilityChanged = false;
    return changed;
}
