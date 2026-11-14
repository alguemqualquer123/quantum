// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_CONTEXT_H
#define QUANTUM_PLUGIN_CONTEXT_H

#include "PluginAbi.h"
#include "PluginManifest.h"
#include "PluginRegistry.h"
#include "PluginSettings.h"
#include "PluginVersion.h"

#include <string>
#include <vector>

namespace quantum {

    // Declared here so scene() can hand out a pointer without dragging Scene.h, and everything it
    // pulls in, across the plugin boundary. Looked up from quantum::plugin as this same class.
    class Scene;

namespace plugin {

    enum class PluginLogLevel : uint8_t { Verbose = 0, Debug, Info, Warning, Error };

    struct PluginLogEntry {
        PluginLogLevel level = PluginLogLevel::Info;
        std::string pluginId;
        std::string message;
    };

    // Everything a plugin is allowed to reach. The manager builds one per plugin and hands it to
    // IPlugin::initialize; nothing else crosses the boundary implicitly, which is what keeps a
    // third-party plugin from depending on engine internals that are not part of this contract.
    class QUANTUM_API PluginContext {
    public:
        // `dataRoot` is the one directory this plugin may write into; PluginManager owns it and
        // builds it under the settings directory as `<settings>/data/<id>`. Left empty, the file API
        // refuses every path with a line saying this build has no data directory - which is what a
        // test harness that never gives the plugin somewhere to write should read as.
        PluginContext(const PluginManifest& ownerManifest, PluginSettings& ownerSettings,
                      PluginRegistry& extensions, std::vector<PluginLogEntry>& logSink,
                      std::string dataRoot = std::string());

        const PluginManifest& manifest() const { return *manifestRef; }
        PluginSettings& settings() const { return *settingsRef; }
        PluginRegistry& registry() const { return *extensions; }

        // The plugin's own folder, so Resources/ and Shaders/ are found without the plugin
        // guessing where it was installed from.
        const std::string& directory() const { return manifestRef->directory; }
        std::string resourcePath(const std::string& relative) const;

        std::string engineVersion() const { return versionToString(engineVersionNumber()); }
        unsigned int engineAbiVersion() const { return QUANTUM_PLUGIN_ABI_VERSION; }

        // Permission gate: the manifest has to name the permission before the call is honoured,
        // so a plugin that quietly starts writing files shows up as a refusal in the log.
        bool hasPermission(const std::string& permission) const;

        // The engine-enforced version of the same question. Returns the manifest answer, and on the
        // first denial writes one warning line naming the capability - after that it stays quiet,
        // because a gated call inside a tick would otherwise flood the log every frame.
        bool require(const std::string& permission) const;

        // The scene this plugin is running in, or nullptr when the manifest did not ask for the
        // "scene" permission. Ownership stays with the engine: the pointer is valid for the current
        // frame only, and a plugin that keeps it across a scene change is reading freed memory.
        Scene* scene() const;

        // const because the sink is the manager's, not the context's: a plugin may log from a
        // registration method that only holds a const reference.
        // Needs the "log" permission; a denial is silent because there is no other channel left.
        void log(PluginLogLevel level, const char* format, ...) const;

        bool registerCommand(const std::string& name, const std::string& description) const;
        bool registerPanel(const std::string& title) const;
        bool registerTool(const std::string& name, const std::string& label,
                          const std::string& action) const;

        // ABI 2 - the "ui" capability. A plugin does not draw and does not own an ImGui context: it
        // declares a window, and the editor hosts it. The content comes from
        // IPlugin::onDrawWindow every frame that window is open.
        // `width`/`height` of 0 mean "the editor's default", and `category` is the menu the window
        // is listed under. The id is namespaced by the manager: what the registry keys on is the
        // string you pass, so make it your own ("<yourid>.stats").
        bool registerWindow(const std::string& windowId, const std::string& title,
                            const std::string& category = std::string(),
                            const std::string& icon = std::string(),
                            int width = 0, int height = 0) const;

        // Adds a menu entry that runs `command` - which has to be a command this plugin registers,
        // because the item is removed with the plugin and executing it goes through the same gate.
        bool registerMenuItem(const std::string& label, const std::string& command,
                              const std::string& category = std::string()) const;

        // Asks the editor to open (or raise) one of this plugin's windows. It is a request, not a
        // call: nothing happens until the editor drains it, and a headless run only shows that the
        // request was accepted. Needs "ui".
        bool openWindow(const std::string& windowId) const;

        // ABI 2 - the "files" capability. Every path here is relative to this plugin's own data
        // directory: an absolute path, a drive letter, a UNC prefix or a ".." component is refused
        // with a log line rather than resolved, so a plugin cannot reach the project or the engine
        // folder it was installed into. The directory itself is created on the first write.
        const std::string& dataDirectory() const { return dataRoot; }

        // The checked join. Returns false, and says why through the plugin log, for a path that
        // escapes the data directory; `absoluteOut` is only written when it returns true.
        bool resolveDataPath(const std::string& relative, std::string& absoluteOut) const;

        bool readFile(const std::string& relative, std::string& contentOut) const;
        bool writeTextFile(const std::string& relative, const std::string& content) const;
        bool appendTextFile(const std::string& relative, const std::string& content) const;
        bool removeFile(const std::string& relative) const;
        bool fileExists(const std::string& relative) const;

        // Recursive, paths relative to this plugin's data directory and separated by '/'. Empty when
        // the directory does not exist yet, which is the normal state on first run. An empty
        // `relativeDirectory` means the data directory itself.
        // `includeDirectories` adds the directories with a trailing slash - useful for a plugin that
        // keeps one subfolder per generated asset.
        std::vector<std::string> listFiles(const std::string& relativeDirectory = std::string(),
                                           bool includeDirectories = false) const;

    private:
        // The sink write, with no gate in front of it: require() reports through this, and routing
        // that through log() would mean a plugin refused "log" could never learn why it is silent.
        void writeLog(PluginLogLevel level, const std::string& message) const;

        // The two public spellings of the same write, so a plugin cannot ask for both at once.
        bool writeFile(const std::string& relative, const std::string& content, bool append) const;

        const PluginManifest* manifestRef = nullptr;
        PluginSettings* settingsRef = nullptr;
        PluginRegistry* extensions = nullptr;
        std::vector<PluginLogEntry>* logSink = nullptr;
        std::string dataRoot;

        // mutable because the whole surface a plugin holds is const-correct reads; this is a
        // diagnostic latch, not state the plugin can observe.
        mutable std::vector<std::string> denialsReported;
    };

}
}

#endif // QUANTUM_PLUGIN_CONTEXT_H
