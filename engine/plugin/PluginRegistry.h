// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_REGISTRY_H
#define QUANTUM_PLUGIN_REGISTRY_H

#include "Export.h"

#include <string>
#include <vector>

namespace quantum {
namespace plugin {

    struct PluginCommandInfo {
        std::string pluginId;
        std::string name;
        std::string description;
    };

    struct PluginPanelInfo {
        std::string pluginId;
        std::string title;
    };

    struct PluginToolInfo {
        std::string pluginId;
        std::string name;
        std::string label;
        std::string action;
    };

    // A window the plugin wants the editor to host. The plugin never draws it and never sees ImGui:
    // it names the window here and describes its contents per frame through
    // IPlugin::onDrawWindow, because an ImGui context pointer across the boundary would tie every
    // plugin binary to one exact imgui build (see PLUGIN_MARKETPLACE_PLAN.md, D6).
    struct PluginWindowInfo {
        std::string pluginId;
        std::string id;       // unique across plugins, so a menu item can name it
        std::string title;    // the window label the editor shows
        std::string category; // groups it in the menus ("World", "Tools")
        std::string icon;     // a glyph name, empty for none
        int width = 420;      // the size the window opens at, not a lock
        int height = 320;
    };

    // A menu entry the plugin adds. It runs a registered command, which is what keeps the menu
    // honest: the item cannot call into a plugin that did not publish that command.
    struct PluginMenuItemInfo {
        std::string pluginId;
        std::string category; // which menu hosts it
        std::string label;
        std::string command;
    };

    // What a plugin adds to the engine. Kept apart from PluginManager, which owns binaries and
    // lifecycle: this table is what the editor and the Lua/C++ callers actually query, and it has
    // to survive being read while a plugin is being unloaded.
    class QUANTUM_API PluginRegistry {
    public:
        // Names are unique across plugins; a collision is refused rather than silently shadowing,
        // because the second plugin would otherwise win depending on load order.
        bool registerCommand(const std::string& pluginId, const std::string& name,
                             const std::string& description);
        bool registerPanel(const std::string& pluginId, const std::string& title);
        bool registerTool(const std::string& pluginId, const std::string& name,
                          const std::string& label, const std::string& action);

        // Re-registering your own window or menu item replaces it rather than duplicating it, so a
        // plugin can retitle a window or change the size it opens at. Another plugin claiming the
        // same key is refused, as with commands: the winner would otherwise be load order.
        bool registerWindow(const PluginWindowInfo& window);
        bool registerMenuItem(const PluginMenuItemInfo& item);

        // Called on unload/disable: every extension of that plugin disappears with it.
        void clearPlugin(const std::string& pluginId);

        const std::vector<PluginCommandInfo>& commands() const { return commandList; }
        const std::vector<PluginPanelInfo>& panels() const { return panelList; }
        const std::vector<PluginToolInfo>& tools() const { return toolList; }
        const std::vector<PluginWindowInfo>& windows() const { return windowList; }
        const std::vector<PluginMenuItemInfo>& menuItems() const { return menuList; }

        bool findCommand(const std::string& name, PluginCommandInfo& out) const;
        std::string ownerOfCommand(const std::string& name) const;

        bool findWindow(const std::string& id, PluginWindowInfo& out) const;
        std::string ownerOfWindow(const std::string& id) const;

        // A plugin asking the editor to show one of its windows. The queue is here because this is
        // the one object both sides reach: the plugin writes through PluginContext, the editor's
        // window host drains it once per frame, and a headless run can read what was asked for
        // without opening anything. Only the owner of a registered window may request it, and a
        // request already pending is not queued twice.
        bool requestWindowOpen(const std::string& pluginId, const std::string& windowId);

        // What the host consumes. Everything pending is returned and the queue is left empty.
        std::vector<std::string> takeWindowOpenRequests();
        const std::vector<std::string>& windowOpenRequests() const { return openRequests; }

        const std::string& lastError() const { return error; }

    private:
        std::vector<PluginCommandInfo> commandList;
        std::vector<PluginPanelInfo> panelList;
        std::vector<PluginToolInfo> toolList;
        std::vector<PluginWindowInfo> windowList;
        std::vector<PluginMenuItemInfo> menuList;
        std::vector<std::string> openRequests;
        std::string error;
    };

}
}

#endif // QUANTUM_PLUGIN_REGISTRY_H
