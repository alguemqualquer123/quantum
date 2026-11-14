// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_MANAGER_H
#define QUANTUM_PLUGIN_MANAGER_H

#include "Export.h"
#include "PluginContext.h"
#include "PluginInterface.h"
#include "PluginLoader.h"
#include "PluginManifest.h"
#include "PluginRegistry.h"
#include "PluginSettings.h"
#include "PluginUiSchema.h"

#include <memory>
#include <string>
#include <vector>

namespace quantum {
namespace plugin {

    enum class PluginState : uint8_t {
        Discovered,     // plugin.json parsed, nothing mapped yet
        Incompatible,   // refused before load: platform, abi, engine version, dependency
        Failed,         // mapped but could not be initialised
        Loaded,         // instance created, initialize() not run yet
        Initialized,    // initialize() returned true
        Active,         // activate() returned true
        Disabled        // unloaded on request; the record stays so the editor can show why
    };

    QUANTUM_API const char* pluginStateName(PluginState state);

    // Everything the manager knows about one plugin. The manager owns these; a caller only ever
    // holds a pointer, because a reload replaces the record's contents in place.
    struct QUANTUM_API PluginRecord {
        PluginManifest manifest;
        PluginSettings settings;
        UiSchema schema;

        PluginState state = PluginState::Discovered;
        std::string error;

        std::string libraryPath;
        void* libraryHandle = nullptr;
        IPlugin* instance = nullptr;
        PluginDestroyFunction destroyFunction = nullptr;
        unsigned int reportedAbiVersion = 0;

        // Owned here, not on startInstance()'s stack: PluginBase keeps the address it is handed in
        // initialize() and reads through it in every later command, so a context that dies with the
        // call leaves every plugin dereferencing freed memory.
        std::unique_ptr<PluginContext> context;

        // The schema is built on demand and cached; rebuilt when the plugin reports it changed.
        bool schemaBuilt = false;

        bool isActive() const { return state == PluginState::Active; }
        bool isUsable() const { return instance != nullptr; }
    };

    // Discovers, checks, loads and owns plugins. This is the single place a plugin binary is
    // mapped, so the editor and a shipped game get identical behaviour from the same directory
    // layout.
    class QUANTUM_API PluginManager {
    public:
        // The manager the engine bootstrap uses. A caller can own its own instance instead - the
        // tests do, so each case gets a clean record list - but then nothing loads it for them.
        static PluginManager& instance();

        PluginManager();
        ~PluginManager();
        PluginManager(const PluginManager&) = delete;
        PluginManager& operator=(const PluginManager&) = delete;

        // Both default to what QUANTUM_PLUGINS_DIR / QUANTUM_PLUGIN_SETTINGS_DIR name, or to
        // "<cwd>/plugins" and "<cwd>/plugins/settings".
        void setPluginsDirectory(const std::string& directory);
        void setSettingsDirectory(const std::string& directory);
        const std::string& pluginsDirectory() const { return pluginsDir; }
        const std::string& settingsDirectory() const { return settingsDir; }

        // Extra folders scanned beside the plugins directory, Unity's Packages/ and Unreal's
        // Plugins/ convention: a project carries its own extensions and nothing is installed into
        // the engine for it. Settings still live under the settings directory, so one id has one
        // enabled flag whichever folder it was found in.
        void addSearchDirectory(const std::string& directory);
        const std::vector<std::string>& searchDirectories() const { return extraDirs; }

        // Scans one level of subdirectories for plugin.json. Returns the number of records that
        // parse; records already known are kept, not duplicated.
        int discover();

        // discover() + load everything not saved as disabled, in dependency order.
        int loadAll();

        bool load(const std::string& id);
        bool unload(const std::string& id);
        bool reload(const std::string& id);

        // Persisted in the plugin's own settings, so a disabled plugin stays disabled next run.
        bool setEnabled(const std::string& id, bool enabled);
        bool isEnabled(const std::string& id) const;

        void shutdownAll();

        const std::vector<PluginRecord*>& records() const { return owned; }
        const PluginRecord* find(const std::string& id) const;
        PluginRecord* find(const std::string& id);

        const UiSchema* schemaOf(const std::string& pluginId);
        bool refreshSchema(const std::string& pluginId);

        // The one directory a plugin may write through its "files" capability: <settings>/data/<id>.
        // It lives under the settings directory and not beside plugin.json because a packaged install
        // is read-only, and the settings location is the one path this engine is allowed to write to
        // without knowing whether the game is installed for all users.
        std::string pluginDataDirectory(const std::string& pluginId);

        // ABI 2. The two halves of a hosted window: describe one frame of it, then hand back what the
        // user did. The "ui" gate and the "is this plugin actually up" check live here rather than in
        // the editor panel, so the CLI probe, the window host and a later Lua binding are refused in
        // exactly the same words. `out` is cleared first: a window is described per frame, and a
        // stale field would be a control the plugin no longer wants.
        bool buildWindowSchema(const std::string& windowId, UiSchema& out, std::string& error);
        bool sendWindowInput(const std::string& windowId, const UiValueMap& values,
                             const std::vector<std::string>& pressedButtons, std::string& error);

        bool executeCommand(const std::string& command, const UiValueMap& arguments,
                            UiValue& result, std::string& error);

        // Advances every Active plugin that was granted the "tick" permission. Engine::runFrame calls
        // this once per frame in the variable-timestep phase, after the scenes have updated, so a
        // plugin reads scene state the same frame it was written.
        void update(float deltaTime);

        // Copies a plugin folder (plugin.json and everything beside it) into the plugins directory.
        // Returns the installed id, empty on failure with the reason in error.
        std::string installFromDirectory(const std::string& sourceDirectory, bool overwrite,
                                        std::string& error);

        PluginRegistry& registry() { return extensions; }
        const PluginRegistry& registry() const { return extensions; }

        const std::vector<PluginLogEntry>& log() const { return logEntries; }
        void clearLog();

        int activeCount() const;
        int failedCount() const;

        // Multi-line human summary: what was found, what loaded, and why the rest did not. This is
        // what the headless probe prints, because a build that compiles proves nothing about
        // whether a DLL actually mapped.
        std::string describe() const;

    private:
        void logLine(PluginLogLevel level, const std::string& pluginId, const std::string& message);
        void refreshDirectories();

        // One directory's pass of discover(); `primary` is the engine plugins directory, whose
        // absence is worth a log line and whose binaries may also sit beside quantum.dll.
        int discoverFrom(const std::string& directory, bool primary);

        bool checkCompatibility(PluginRecord& record);
        bool mapLibrary(PluginRecord& record);
        bool startInstance(PluginRecord& record);
        bool activateInstance(PluginRecord& record);
        void teardownInstance(PluginRecord& record);

        // -1 when the dependency can never be satisfied, 0 when it is merely not loaded yet,
        // 1 when every dependency is up.
        int dependencyCheck(PluginRecord& record, std::string& reason);
        std::string resolveLibraryPath(const PluginManifest& manifest) const;

        // The window half of executeCommand(): the registered window, the record that owns it, and
        // the reasons a call cannot happen - unknown id, plugin not active, no "ui" permission.
        PluginRecord* recordForWindow(const std::string& windowId, std::string& error);

        std::vector<PluginRecord*> owned;
        std::vector<PluginLogEntry> logEntries;
        PluginRegistry extensions;

        std::string pluginsDir;
        std::string settingsDir;
        std::vector<std::string> extraDirs;
        bool directoriesResolved = false;

        // True while update() is inside a plugin's onUpdate(). Unloading from there would free the
        // record the loop is standing on, so it is refused instead of crashing.
        bool ticking = false;
    };

}
}

#endif // QUANTUM_PLUGIN_MANAGER_H
