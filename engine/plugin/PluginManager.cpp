// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "PluginManager.h"

#include "Engine.h"
#include "Log.h"
#include "PluginPlatform.h"
#include "PluginVersion.h"

#include "sokol_time.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace quantum;
using namespace quantum::plugin;

namespace {

    constexpr size_t MAX_PLUGIN_LOG_LINES = 1024;

    std::string joinPath(const std::string& directory, const std::string& leaf) {
        if (directory.empty()) return leaf;
        if (directory.back() == '/' || directory.back() == '\\') return directory + leaf;
        return directory + "/" + leaf;
    }

    std::string environmentDirectory(const char* name) {
        const char* value = std::getenv(name);
        return value ? std::string(value) : std::string();
    }

}

const char* quantum::plugin::pluginStateName(PluginState state) {
    switch (state) {
        case PluginState::Discovered: return "discovered";
        case PluginState::Incompatible: return "incompatible";
        case PluginState::Failed: return "failed";
        case PluginState::Loaded: return "loaded";
        case PluginState::Initialized: return "initialized";
        case PluginState::Active: return "active";
        default: return "disabled";
    }
}

PluginManager& PluginManager::instance() {
    static PluginManager manager;
    return manager;
}

PluginManager::PluginManager() {
}

PluginManager::~PluginManager() {
    shutdownAll();
    for (PluginRecord* record : owned) delete record;
    owned.clear();
}

void PluginManager::refreshDirectories() {
    if (directoriesResolved) return;
    directoriesResolved = true;

    std::error_code ec;
    const std::string cwd = std::filesystem::current_path(ec).generic_string();
    const std::string defaultPlugins = cwd.empty() ? std::string("plugins") : joinPath(cwd, "plugins");

    const std::string fromEnvironment = environmentDirectory("QUANTUM_PLUGINS_DIR");
    pluginsDir = fromEnvironment.empty() ? defaultPlugins : fromEnvironment;

    const std::string settingsFromEnvironment = environmentDirectory("QUANTUM_PLUGIN_SETTINGS_DIR");
    settingsDir = settingsFromEnvironment.empty() ? joinPath(pluginsDir, "settings")
                                                 : settingsFromEnvironment;
}

void PluginManager::setPluginsDirectory(const std::string& directory) {
    pluginsDir = directory;
    directoriesResolved = true;
}

void PluginManager::setSettingsDirectory(const std::string& directory) {
    settingsDir = directory;
    directoriesResolved = true;
}

void PluginManager::logLine(PluginLogLevel level, const std::string& pluginId,
                           const std::string& message) {
    PluginLogEntry entry;
    entry.level = level;
    entry.pluginId = pluginId;
    entry.message = message;
    logEntries.push_back(entry);
    if (logEntries.size() > MAX_PLUGIN_LOG_LINES) {
        logEntries.erase(logEntries.begin(),
                         logEntries.begin() + static_cast<long>(
                             logEntries.size() - MAX_PLUGIN_LOG_LINES));
    }

    const std::string line = "[plugin" + (pluginId.empty() ? std::string(" system")
                                                            : (" " + pluginId)) + "] " + message;
    if (level == PluginLogLevel::Error) Log::error("%s", line.c_str());
    else if (level == PluginLogLevel::Warning) Log::warn("%s", line.c_str());
    else Log::print("%s", line.c_str());
}

PluginRecord* PluginManager::find(const std::string& id) {
    for (PluginRecord* record : owned) {
        if (record->manifest.id == id) return record;
    }
    return nullptr;
}

const PluginRecord* PluginManager::find(const std::string& id) const {
    for (const PluginRecord* record : owned) {
        if (record->manifest.id == id) return record;
    }
    return nullptr;
}

int PluginManager::discover() {
    refreshDirectories();

    int parsed = 0;
    parsed += discoverFrom(pluginsDir, true);
    for (const std::string& directory : extraDirs) parsed += discoverFrom(directory, false);
    return parsed;
}

void PluginManager::addSearchDirectory(const std::string& directory) {
    if (directory.empty()) return;
    refreshDirectories();

    const std::string normalized = std::filesystem::path(directory).generic_string();
    if (normalized == pluginsDir) return;
    for (const std::string& known : extraDirs) {
        if (known == normalized) return;
    }
    extraDirs.push_back(normalized);
}

int PluginManager::discoverFrom(const std::string& directory, bool primary) {
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path(directory), ec)) {
        // A missing engine plugins directory is worth one line; a project without a Plugins folder
        // is the normal case, so only the primary directory reports it.
        if (primary) {
            logLine(PluginLogLevel::Info, std::string(),
                    "no plugins directory at " + directory);
        }
        return 0;
    }

    int parsed = 0;
    std::filesystem::directory_iterator walk(std::filesystem::path(directory), ec);
    if (ec) {
        logLine(PluginLogLevel::Error, std::string(),
                "cannot read " + directory + ": " + ec.message());
        return 0;
    }

    const std::filesystem::directory_iterator end;
    for (; walk != end; walk.increment(ec)) {
        if (ec) break;
        std::error_code entryEc;
        if (!walk->is_directory(entryEc) || entryEc) continue;

        const std::string folder = walk->path().filename().generic_string();
        const std::string manifestPath = joinPath(walk->path().generic_string(), "plugin.json");

        std::error_code fileEc;
        if (!std::filesystem::exists(std::filesystem::path(manifestPath), fileEc)) continue;

        PluginManifest manifest;
        if (!loadPluginManifest(manifestPath, manifest)) {
            // Keep the failure visible: an invalid manifest still has to appear in the browser,
            // otherwise a typo in plugin.json reads as "the plugin is not installed".
            const std::string placeholder = manifest.id.empty() ? folder : manifest.id;
            PluginRecord* record = find(placeholder);
            if (!record) {
                record = new PluginRecord();
                record->manifest.id = placeholder;
                record->manifest.directory = walk->path().generic_string();
                record->manifest.path = manifestPath;
                owned.push_back(record);
            }
            record->state = PluginState::Failed;
            record->error = "invalid manifest: " + manifest.errorsAsText();
            logLine(PluginLogLevel::Error, placeholder, record->error);
            continue;
        }

        PluginRecord* known = find(manifest.id);
        if (known) {
            // discover() runs again on every loadAll(), and the record from the first pass is still
            // there. Only a *different* folder claiming the same id is the conflict worth reporting.
            if (known->manifest.path == manifestPath) {
                parsed++;
                continue;
            }
            logLine(PluginLogLevel::Warning, manifest.id,
                    "duplicate manifest for id \"" + manifest.id + "\" in " + folder + ", skipped");
            continue;
        }

        PluginRecord* record = new PluginRecord();
        record->manifest = manifest;
        record->settings.setPluginId(manifest.id);
        record->settings.setPath(joinPath(settingsDir, manifest.id + ".json"));
        owned.push_back(record);
        parsed++;

        // Reported at discovery rather than at load: a misspelled permission is a packaging mistake
        // in plugin.json, and a plugin that never loads still has to show why it could not work.
        for (const std::string& permission : manifest.permissions) {
            if (isKnownPermission(permission)) continue;
            logLine(PluginLogLevel::Warning, manifest.id,
                    "unknown permission \"" + permission + "\" in plugin.json");
        }

        logLine(PluginLogLevel::Verbose, manifest.id,
                "discovered " + manifest.name + " " + manifest.version);
    }

    return parsed;
}

std::string PluginManager::resolveLibraryPath(const PluginManifest& manifest) const {
    const std::string fileName = manifest.libraryFileName();
    if (fileName.empty()) return std::string();

    std::error_code ec;
    const std::string direct = joinPath(manifest.directory, fileName);
    if (std::filesystem::exists(std::filesystem::path(direct), ec)) return direct;

    // The layout a CMake build naturally produces: binaries under lib/ beside the manifest.
    const std::string nested = joinPath(joinPath(manifest.directory, "lib"), fileName);
    if (std::filesystem::exists(std::filesystem::path(nested), ec)) return nested;

    // And the layout of an installed editor, where the binaries sit next to quantum.dll so their
    // own imports resolve without a manifest change.
    const std::string beside = joinPath(pluginsDir, fileName);
    if (std::filesystem::exists(std::filesystem::path(beside), ec)) return beside;

    return std::string();
}

bool PluginManager::checkCompatibility(PluginRecord& record) {
    const PluginManifest& manifest = record.manifest;

    if (!manifest.valid) {
        record.state = PluginState::Incompatible;
        record.error = "manifest did not parse";
        return false;
    }

    if (!supportsCurrentPlatform(manifest)) {
        record.state = PluginState::Incompatible;
        record.error = std::string("not built for ") + currentPlatformTag();
        return false;
    }

    if (manifest.abiVersion != 0 && manifest.abiVersion != QUANTUM_PLUGIN_ABI_VERSION) {
        record.state = PluginState::Incompatible;
        record.error = "manifest declares abi " + std::to_string(manifest.abiVersion) +
                       ", engine is abi " + std::to_string(QUANTUM_PLUGIN_ABI_VERSION);
        return false;
    }

    if (!manifest.minEngineVersion.empty()) {
        std::string reason;
        const bool compatible = isVersionCompatible(engineVersionNumber(),
                                                    parseVersion(manifest.minEngineVersion), reason);
        if (!compatible) {
            record.state = PluginState::Incompatible;
            record.error = reason;
            return false;
        }
        if (!reason.empty()) logLine(PluginLogLevel::Warning, manifest.id, reason);
    }

    record.error.clear();
    return true;
}

bool PluginManager::mapLibrary(PluginRecord& record) {
    record.libraryPath = resolveLibraryPath(record.manifest);
    if (record.libraryPath.empty()) {
        record.state = PluginState::Failed;
        record.error = "library \"" + record.manifest.libraryFileName() + "\" not found in " +
                       record.manifest.directory;
        return false;
    }

    PluginLoader::Loaded mapped = PluginLoader::open(record.libraryPath);
    if (!mapped.ok()) {
        record.state = PluginState::Failed;
        record.error = mapped.error;
        return false;
    }
    record.libraryHandle = mapped.handle;

    PluginAbiFunction abiFunction = reinterpret_cast<PluginAbiFunction>(
        PluginLoader::symbol(record.libraryHandle, QUANTUM_PLUGIN_ABI_SYMBOL));
    if (!abiFunction) {
        record.state = PluginState::Failed;
        record.error = std::string("exported symbol \"") + QUANTUM_PLUGIN_ABI_SYMBOL + "\" missing";
        teardownInstance(record);
        return false;
    }

    record.reportedAbiVersion = abiFunction();
    if (record.reportedAbiVersion != QUANTUM_PLUGIN_ABI_VERSION) {
        record.state = PluginState::Incompatible;
        record.error = "plugin abi " + std::to_string(record.reportedAbiVersion) +
                       ", engine abi " + std::to_string(QUANTUM_PLUGIN_ABI_VERSION);
        teardownInstance(record);
        return false;
    }

    PluginCreateFunction createFunction = reinterpret_cast<PluginCreateFunction>(
        PluginLoader::symbol(record.libraryHandle, QUANTUM_PLUGIN_CREATE_SYMBOL));
    record.destroyFunction = reinterpret_cast<PluginDestroyFunction>(
        PluginLoader::symbol(record.libraryHandle, QUANTUM_PLUGIN_DESTROY_SYMBOL));

    if (!createFunction || !record.destroyFunction) {
        record.state = PluginState::Failed;
        record.error = std::string("exported symbols \"") + QUANTUM_PLUGIN_CREATE_SYMBOL + "\" and \"" +
                       QUANTUM_PLUGIN_DESTROY_SYMBOL + "\" are required";
        teardownInstance(record);
        return false;
    }

    void* created = createFunction();
    if (!created) {
        record.state = PluginState::Failed;
        record.error = "QuantumPluginCreate returned null";
        teardownInstance(record);
        return false;
    }

    // The entry macro casts a concrete plugin back to void*, so the pointer is an IPlugin* only
    // when IPlugin is the first base - which the macro's own static_cast relies on.
    record.instance = static_cast<IPlugin*>(created);
    record.state = PluginState::Loaded;
    return true;
}

bool PluginManager::startInstance(PluginRecord& record) {
    // The one line that explains a plugin whose onUpdate() never runs. It has to be said here rather
    // than at the gate, because a plugin with no interest in the frame loop is refused silently,
    // every frame, and 60 warnings a second is not a report.
    if (!record.manifest.hasPermission("tick")) {
        logLine(PluginLogLevel::Verbose, record.manifest.id,
                "no \"tick\" permission: onUpdate() will not be called");
    }

    record.settings.setPluginId(record.manifest.id);
    record.settings.setPath(joinPath(settingsDir, record.manifest.id + ".json"));
    record.settings.setDefaults(record.manifest.defaults);
    if (!record.settings.load()) {
        logLine(PluginLogLevel::Warning, record.manifest.id,
                "settings not read: " + record.settings.lastError());
    }

    // Heap-allocated with the record: PluginBase stores this address in initialize() and reads
    // through it from every later command, so it has to outlive the call that hands it out.
    record.context = std::make_unique<PluginContext>(record.manifest, record.settings, extensions,
                                                     logEntries, pluginDataDirectory(record.manifest.id));

    if (!record.instance->initialize(*record.context)) {
        record.state = PluginState::Failed;
        record.error = "initialize() returned false";
        return false;
    }

    // The binary and the manifest both claim identity; a disagreement is a packaging bug that
    // would otherwise show up as settings written under the wrong id.
    const std::string reportedId = record.instance->pluginId();
    if (!reportedId.empty() && reportedId != record.manifest.id) {
        logLine(PluginLogLevel::Warning, record.manifest.id,
                "plugin reports id \"" + reportedId + "\" but manifest says \"" +
                record.manifest.id + "\"");
    }

    const std::string reportedVersion = record.instance->pluginVersion();
    if (!reportedVersion.empty() && reportedVersion != record.manifest.version) {
        logLine(PluginLogLevel::Warning, record.manifest.id,
                "plugin reports version \"" + reportedVersion + "\" but manifest says \"" +
                record.manifest.version + "\"");
    }

    record.state = PluginState::Initialized;
    return true;
}

bool PluginManager::activateInstance(PluginRecord& record) {
    if (!record.instance->activate()) {
        record.state = PluginState::Failed;
        record.error = "activate() returned false";
        return false;
    }

    if (!record.manifest.panelTitle.empty()) {
        extensions.registerPanel(record.manifest.id, record.manifest.panelTitle);
    }

    record.state = PluginState::Active;
    refreshSchema(record.manifest.id);
    return true;
}

void PluginManager::teardownInstance(PluginRecord& record) {
    if (record.instance) {
        // Deactivate first so a plugin can release what its commands still reference.
        record.instance->deactivate();
        record.instance->shutdown();
        if (record.destroyFunction) record.destroyFunction(record.instance);
        record.instance = nullptr;
    }
    record.destroyFunction = nullptr;

    // After the instance: shutdown() is allowed to log, and logging reads the context.
    record.context.reset();

    if (record.libraryHandle) {
        PluginLoader::close(record.libraryHandle);
        record.libraryHandle = nullptr;
    }

    extensions.clearPlugin(record.manifest.id);
    record.schema.clear();
    record.schemaBuilt = false;
}

int PluginManager::dependencyCheck(PluginRecord& record, std::string& reason) {
    for (const PluginDependency& dependency : record.manifest.dependencies) {
        const PluginRecord* owner = find(dependency.id);
        if (!owner) {
            reason = "depends on \"" + dependency.id + "\", which is not installed";
            return -1;
        }
        if (owner->state == PluginState::Active || owner->state == PluginState::Initialized) {
            continue;
        }
        if (owner->state == PluginState::Failed || owner->state == PluginState::Incompatible) {
            reason = "depends on \"" + dependency.id + "\", which did not load: " + owner->error;
            return -1;
        }
        if (!dependency.minVersion.empty() &&
            compareVersions(parseVersion(owner->manifest.version),
                            parseVersion(dependency.minVersion)) < 0) {
            reason = "depends on " + dependency.id + " >= " + dependency.minVersion + ", found " +
                     owner->manifest.version;
            return -1;
        }
        reason = "waits for " + dependency.id;
        return 0;
    }
    reason.clear();
    return 1;
}

int PluginManager::loadAll() {
    discover();

    std::vector<PluginRecord*> pending;
    for (PluginRecord* record : owned) {
        if (record->state != PluginState::Discovered) continue;
        if (!isEnabled(record->manifest.id)) {
            record->state = PluginState::Disabled;
            logLine(PluginLogLevel::Verbose, record->manifest.id, "disabled by settings, skipped");
            continue;
        }
        pending.push_back(record);
    }

    // Dependencies are a partial order, not a graph the manifest author has to sort: keep taking
    // anything whose dependencies are already up until a pass changes nothing.
    bool progressed = true;
    while (!pending.empty() && progressed) {
        progressed = false;
        for (size_t i = 0; i < pending.size();) {
            PluginRecord* record = pending[i];
            std::string reason;
            const int check = dependencyCheck(*record, reason);
            if (check < 0) {
                record->state = PluginState::Incompatible;
                record->error = reason;
                logLine(PluginLogLevel::Error, record->manifest.id, reason);
                pending.erase(pending.begin() + static_cast<long>(i));
                progressed = true;
                continue;
            }
            if (check == 0) {
                i++;
                continue;
            }
            if (load(record->manifest.id)) progressed = true;
            pending.erase(pending.begin() + static_cast<long>(i));
        }
    }

    for (PluginRecord* record : pending) {
        std::string reason;
        dependencyCheck(*record, reason);
        record->state = PluginState::Incompatible;
        record->error = reason.empty() ? "dependency cycle" : reason;
        logLine(PluginLogLevel::Error, record->manifest.id, record->error);
    }

    return activeCount();
}

bool PluginManager::load(const std::string& id) {
    PluginRecord* record = find(id);
    if (!record) {
        logLine(PluginLogLevel::Error, id, "no plugin with that id");
        return false;
    }
    if (record->state == PluginState::Active) return true;

    if (!checkCompatibility(*record)) {
        logLine(PluginLogLevel::Error, id, record->error);
        return false;
    }
    if (!mapLibrary(*record)) {
        logLine(PluginLogLevel::Error, id, record->error);
        return false;
    }
    if (!startInstance(*record)) {
        logLine(PluginLogLevel::Error, id, record->error);
        teardownInstance(*record);
        return false;
    }
    if (!activateInstance(*record)) {
        logLine(PluginLogLevel::Error, id, record->error);
        teardownInstance(*record);
        return false;
    }

    logLine(PluginLogLevel::Info, id,
            "loaded " + record->manifest.name + " " + record->manifest.version + " from " +
            record->libraryPath);
    return true;
}

bool PluginManager::unload(const std::string& id) {
    PluginRecord* record = find(id);
    if (!record) return false;
    if (!record->instance && record->state != PluginState::Active) return true;
    if (ticking) {
        logLine(PluginLogLevel::Warning, id, "unload refused during the plugin tick");
        return false;
    }

    teardownInstance(*record);
    record->state = PluginState::Disabled;
    if (record->settings.isDirty()) record->settings.save();
    logLine(PluginLogLevel::Info, id, "unloaded");
    return true;
}

bool PluginManager::reload(const std::string& id) {
    unload(id);
    PluginRecord* record = find(id);
    if (!record) return false;
    record->state = PluginState::Discovered;
    record->error.clear();
    return load(id);
}

bool PluginManager::isEnabled(const std::string& id) const {
    // Read straight off disk with the same parser the settings use: the decision has to be made
    // before the plugin's binary is mapped, and a disabled plugin must stay disabled without
    // loading anything of its own. A file that does not parse falls back to enabled and is
    // reported when the plugin itself loads.
    PluginSettings probe;
    probe.setPluginId(id);
    probe.setPath(joinPath(settingsDir, id + ".json"));
    probe.load();
    return probe.getBool("enabled", true);
}

bool PluginManager::setEnabled(const std::string& id, bool enabled) {
    PluginRecord* record = find(id);
    if (!record) return false;

    record->settings.setPluginId(id);
    record->settings.setPath(joinPath(settingsDir, id + ".json"));
    if (record->state == PluginState::Discovered) record->settings.setDefaults(record->manifest.defaults);
    record->settings.set("enabled", UiValue::fromBool(enabled));
    record->settings.save();

    if (enabled) return load(id);
    return unload(id);
}

void PluginManager::shutdownAll() {
    // Reverse order: a plugin that depends on another has to go first.
    for (size_t i = owned.size(); i > 0; i--) {
        PluginRecord* record = owned[i - 1];
        if (!record->instance) continue;
        teardownInstance(*record);
        if (record->settings.isDirty()) record->settings.save();
        record->state = PluginState::Disabled;
    }
}

std::string PluginManager::pluginDataDirectory(const std::string& pluginId) {
    refreshDirectories();
    return joinPath(joinPath(settingsDir, "data"), pluginId);
}

PluginRecord* PluginManager::recordForWindow(const std::string& windowId, std::string& error) {
    error.clear();

    PluginWindowInfo window;
    if (!extensions.findWindow(windowId, window)) {
        error = "no plugin registered the window \"" + windowId + "\"";
        return nullptr;
    }

    PluginRecord* record = find(window.pluginId);
    if (!record || !record->instance) {
        error = "plugin " + window.pluginId + " is not loaded";
        return nullptr;
    }
    if (record->state != PluginState::Active) {
        error = "plugin " + window.pluginId + " is " + pluginStateName(record->state);
        return nullptr;
    }
    // The gate the context also applies at registration, checked again here because the caller is
    // the editor: a plugin that registered a window while permitted and then had the permission
    // taken away by an edited plugin.json should not keep drawing.
    if (!record->manifest.hasPermission("ui")) {
        error = "plugin " + window.pluginId + " has no \"ui\" permission in plugin.json";
        return nullptr;
    }
    return record;
}

bool PluginManager::buildWindowSchema(const std::string& windowId, UiSchema& out,
                                      std::string& error) {
    PluginRecord* record = recordForWindow(windowId, error);
    if (!record) return false;

    out.clear();
    out.pluginId = record->manifest.id;

    // The title the plugin gave the window at registration, not the panel title: a plugin with one
    // panel and two windows has to be able to name them apart.
    PluginWindowInfo window;
    if (extensions.findWindow(windowId, window) && !window.title.empty()) out.title = window.title;
    else out.title = record->manifest.name;

    UiSchemaBuilder builder(out);
    record->instance->onDrawWindow(windowId, builder);
    return true;
}

bool PluginManager::sendWindowInput(const std::string& windowId, const UiValueMap& values,
                                    const std::vector<std::string>& pressedButtons,
                                    std::string& error) {
    PluginRecord* record = recordForWindow(windowId, error);
    if (!record) return false;

    record->instance->onWindowInput(windowId, values, pressedButtons);
    return true;
}

bool PluginManager::refreshSchema(const std::string& pluginId) {
    PluginRecord* record = find(pluginId);
    if (!record || !record->instance) return false;

    record->schema.clear();
    record->schema.pluginId = record->manifest.id;
    record->schema.title = record->manifest.panelTitle.empty() ? record->manifest.name
                                                               : record->manifest.panelTitle;

    UiSchemaBuilder builder(record->schema);
    record->instance->buildUiSchema(builder);
    record->schemaBuilt = true;
    return true;
}

const UiSchema* PluginManager::schemaOf(const std::string& pluginId) {
    PluginRecord* record = find(pluginId);
    if (!record || !record->instance) return nullptr;
    if (!record->schemaBuilt) refreshSchema(pluginId);
    return &record->schema;
}

bool PluginManager::executeCommand(const std::string& command, const UiValueMap& arguments,
                                   UiValue& result, std::string& error) {
    error.clear();

    PluginCommandInfo info;
    if (!extensions.findCommand(command, info)) {
        error = "no plugin registers the command \"" + command + "\"";
        return false;
    }

    PluginRecord* record = find(info.pluginId);
    if (!record || !record->instance) {
        error = "plugin " + info.pluginId + " is not loaded";
        return false;
    }
    if (record->state != PluginState::Active) {
        error = "plugin " + info.pluginId + " is " + pluginStateName(record->state);
        return false;
    }

    if (!record->instance->executeCommand(command, arguments, result)) {
        error = "plugin " + info.pluginId + " refused the command \"" + command + "\"";
        return false;
    }
    return true;
}

void PluginManager::update(float deltaTime) {
    // Called from Engine::runFrame every frame, so the cost with no plugins is one branch.
    if (owned.empty()) return;

    ticking = true;
    for (PluginRecord* record : owned) {
        if (record->state != PluginState::Active) continue;
        if (!record->instance || !record->context) continue;

        // hasPermission, not require(): the engine is offering a tick, not a plugin asking for a
        // capability, so a plugin that never touches the frame loop must not fill the log with
        // refusals. startInstance() writes the one line that explains it.
        if (!record->manifest.hasPermission("tick")) continue;

        const uint64_t start = Engine::isFrameRunning() ? stm_now() : 0;
        record->instance->onUpdate(deltaTime);
        // Only inside a real frame: the stats table belongs to the frame being measured, and the
        // plugin tests drive update() with no frame running at all.
        if (start && Engine::isFrameRunning()) {
            Engine::addSystemStats(("plugin:" + record->manifest.id).c_str(),
                                   stm_ms(stm_diff(stm_now(), start)), 1);
        }
    }
    ticking = false;
}

std::string PluginManager::installFromDirectory(const std::string& sourceDirectory, bool overwrite,
                                                std::string& error) {
    error.clear();
    refreshDirectories();

    const std::string manifestPath = joinPath(sourceDirectory, "plugin.json");
    PluginManifest manifest;
    if (!loadPluginManifest(manifestPath, manifest)) {
        error = "no usable plugin.json in " + sourceDirectory + ": " + manifest.errorsAsText();
        return std::string();
    }

    std::error_code ec;
    const std::string folder = std::filesystem::path(sourceDirectory).filename().generic_string();
    const std::string target = joinPath(pluginsDir, folder);
    if (std::filesystem::exists(std::filesystem::path(target), ec) && !overwrite) {
        error = target + " already exists";
        return std::string();
    }

    std::filesystem::create_directories(std::filesystem::path(target), ec);
    if (ec) {
        error = "cannot create " + target + ": " + ec.message();
        return std::string();
    }

    std::filesystem::copy_options options = std::filesystem::copy_options::recursive;
    options |= overwrite ? std::filesystem::copy_options::update_existing
                         : std::filesystem::copy_options::skip_existing;

    std::filesystem::copy(std::filesystem::path(sourceDirectory), std::filesystem::path(target),
                          options, ec);
    if (ec) {
        error = "copy into " + target + " failed: " + ec.message();
        return std::string();
    }

    discover();
    logLine(PluginLogLevel::Info, manifest.id, "installed from " + sourceDirectory);
    return manifest.id;
}

int PluginManager::activeCount() const {
    int count = 0;
    for (const PluginRecord* record : owned) {
        if (record->state == PluginState::Active) count++;
    }
    return count;
}

int PluginManager::failedCount() const {
    int count = 0;
    for (const PluginRecord* record : owned) {
        if (record->state == PluginState::Failed || record->state == PluginState::Incompatible) {
            count++;
        }
    }
    return count;
}

void PluginManager::clearLog() {
    logEntries.clear();
}

std::string PluginManager::describe() const {
    std::ostringstream out;

    out << "plugins: directory " << pluginsDir << "\n";
    out << "  engine " << engineVersionText() << ", abi " << QUANTUM_PLUGIN_ABI_VERSION
        << ", " << currentPlatformTag() << ", " << engineToolchainTag() << ", "
        << enginePointerSize() * 8 << "-bit\n";
    out << "  " << owned.size() << " record(s), " << activeCount() << " active, "
        << failedCount() << " refused, " << extensions.commands().size() << " command(s), "
        << extensions.panels().size() << " panel(s), " << extensions.windows().size()
        << " window(s), " << extensions.menuItems().size() << " menu item(s)\n";

    for (const PluginRecord* record : owned) {
        out << "  - " << record->manifest.id << " [" << pluginStateName(record->state) << "] "
            << record->manifest.name << " " << record->manifest.version;
        if (!record->manifest.author.empty()) out << " by " << record->manifest.author;
        if (!record->error.empty()) out << " :: " << record->error;
        if (record->schemaBuilt) out << " :: " << record->schema.groups.size() << " group(s)";
        out << "\n";
    }

    for (const PluginCommandInfo& command : extensions.commands()) {
        out << "    command " << command.name << " (" << command.pluginId << "): "
            << command.description << "\n";
    }

    // The ABI 2 surface, printed here because `plugins list` is how a packaged plugin is checked
    // without a window: a plugin that registered nothing shows as nothing, which is the failure a
    // missing "ui" permission produces and no build log says anything about.
    for (const PluginWindowInfo& window : extensions.windows()) {
        out << "    window " << window.id << " (" << window.pluginId << ") \"" << window.title
            << "\" in " << (window.category.empty() ? std::string("Plugins") : window.category)
            << ", " << window.width << "x" << window.height << "\n";
    }
    for (const PluginMenuItemInfo& item : extensions.menuItems()) {
        out << "    menu " << (item.category.empty() ? std::string("Plugins") : item.category)
            << "/" << item.label << " -> " << item.command << " (" << item.pluginId << ")\n";
    }

    return out.str();
}
