// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "PluginRegistry.h"

using namespace quantum;
using namespace quantum::plugin;

bool PluginRegistry::registerCommand(const std::string& pluginId, const std::string& name,
                                     const std::string& description) {
    error.clear();
    if (name.empty()) {
        error = "empty command name";
        return false;
    }

    for (const PluginCommandInfo& existing : commandList) {
        if (existing.name != name) continue;
        if (existing.pluginId == pluginId) return true;
        error = "command \"" + name + "\" is already registered by " + existing.pluginId;
        return false;
    }

    commandList.push_back(PluginCommandInfo{ pluginId, name, description });
    return true;
}

bool PluginRegistry::registerPanel(const std::string& pluginId, const std::string& title) {
    error.clear();
    if (pluginId.empty()) {
        error = "panel registered without a plugin id";
        return false;
    }

    for (const PluginPanelInfo& existing : panelList) {
        if (existing.pluginId == pluginId) return true;
    }

    panelList.push_back(PluginPanelInfo{ pluginId, title });
    return true;
}

bool PluginRegistry::registerTool(const std::string& pluginId, const std::string& name,
                                  const std::string& label, const std::string& action) {
    error.clear();
    if (name.empty()) {
        error = "empty tool name";
        return false;
    }

    for (const PluginToolInfo& existing : toolList) {
        if (existing.name != name) continue;
        if (existing.pluginId == pluginId) return true;
        error = "tool \"" + name + "\" is already registered by " + existing.pluginId;
        return false;
    }

    toolList.push_back(PluginToolInfo{ pluginId, name, label, action });
    return true;
}

bool PluginRegistry::registerWindow(const PluginWindowInfo& window) {
    error.clear();
    if (window.pluginId.empty()) {
        error = "window registered without a plugin id";
        return false;
    }
    if (window.id.empty()) {
        error = "empty window id";
        return false;
    }

    for (PluginWindowInfo& existing : windowList) {
        if (existing.id != window.id) continue;
        // The same plugin re-naming or re-sizing its own window: replace, so a plugin that reads
        // its size out of settings does not have to be reloaded to show the change.
        if (existing.pluginId == window.pluginId) {
            existing = window;
            return true;
        }
        error = "window \"" + window.id + "\" is already registered by " + existing.pluginId;
        return false;
    }

    windowList.push_back(window);
    return true;
}

bool PluginRegistry::registerMenuItem(const PluginMenuItemInfo& item) {
    error.clear();
    if (item.pluginId.empty()) {
        error = "menu item registered without a plugin id";
        return false;
    }
    if (item.label.empty()) {
        error = "empty menu item label";
        return false;
    }
    if (item.command.empty()) {
        error = "menu item \"" + item.label + "\" has no command to run";
        return false;
    }

    // The key is the category and the text the user reads, not the command: two plugins putting the
    // same words in the same menu would show one item twice with no way to tell which ran.
    for (PluginMenuItemInfo& existing : menuList) {
        if (existing.category != item.category || existing.label != item.label) continue;
        if (existing.pluginId == item.pluginId) {
            existing = item;
            return true;
        }
        error = "menu item \"" + item.label + "\" is already in " + item.category +
                " (" + existing.pluginId + ")";
        return false;
    }

    menuList.push_back(item);
    return true;
}

bool PluginRegistry::requestWindowOpen(const std::string& pluginId, const std::string& windowId) {
    error.clear();

    PluginWindowInfo window;
    if (!findWindow(windowId, window)) {
        error = "no plugin registered the window \"" + windowId + "\"";
        return false;
    }
    if (window.pluginId != pluginId) {
        error = "window \"" + windowId + "\" belongs to " + window.pluginId;
        return false;
    }
    for (const std::string& pending : openRequests) {
        if (pending == windowId) return true;
    }
    openRequests.push_back(windowId);
    return true;
}

std::vector<std::string> PluginRegistry::takeWindowOpenRequests() {
    std::vector<std::string> pending;
    pending.swap(openRequests);
    return pending;
}

void PluginRegistry::clearPlugin(const std::string& pluginId) {
    // Before the windows go: a request still pending for a window that is about to be
    // unregistered would be a name the host cannot resolve.
    for (size_t i = 0; i < openRequests.size();) {
        if (ownerOfWindow(openRequests[i]) == pluginId) openRequests.erase(openRequests.begin() + i);
        else i++;
    }
    for (size_t i = 0; i < commandList.size();) {
        if (commandList[i].pluginId == pluginId) commandList.erase(commandList.begin() + i);
        else i++;
    }
    for (size_t i = 0; i < panelList.size();) {
        if (panelList[i].pluginId == pluginId) panelList.erase(panelList.begin() + i);
        else i++;
    }
    for (size_t i = 0; i < toolList.size();) {
        if (toolList[i].pluginId == pluginId) toolList.erase(toolList.begin() + i);
        else i++;
    }
    for (size_t i = 0; i < windowList.size();) {
        if (windowList[i].pluginId == pluginId) windowList.erase(windowList.begin() + i);
        else i++;
    }
    for (size_t i = 0; i < menuList.size();) {
        if (menuList[i].pluginId == pluginId) menuList.erase(menuList.begin() + i);
        else i++;
    }
}

bool PluginRegistry::findCommand(const std::string& name, PluginCommandInfo& out) const {
    for (const PluginCommandInfo& existing : commandList) {
        if (existing.name == name) {
            out = existing;
            return true;
        }
    }
    return false;
}

std::string PluginRegistry::ownerOfCommand(const std::string& name) const {
    for (const PluginCommandInfo& existing : commandList) {
        if (existing.name == name) return existing.pluginId;
    }
    return std::string();
}

bool PluginRegistry::findWindow(const std::string& id, PluginWindowInfo& out) const {
    for (const PluginWindowInfo& existing : windowList) {
        if (existing.id == id) {
            out = existing;
            return true;
        }
    }
    return false;
}

std::string PluginRegistry::ownerOfWindow(const std::string& id) const {
    for (const PluginWindowInfo& existing : windowList) {
        if (existing.id == id) return existing.pluginId;
    }
    return std::string();
}
