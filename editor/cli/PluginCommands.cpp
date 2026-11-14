// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "PluginCommands.h"

#include "PluginManager.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace quantum;
using namespace quantum::plugin;

namespace quantum::editor::cli {

namespace {

    void usage(const char* executableName) {
        std::cout
            << "usage: " << (executableName ? executableName : "quantum-editor-cmd")
            << " plugins <list | describe <id> | run <command> [key=value...] |"
            << " windows | window <id> [--press NAME] [--set field=value] |"
            << " enable <id> | disable <id> | check <dir> | install <dir> [--overwrite]>"
            << " [--plugins-dir DIR] [--settings-dir DIR]\n"
            << "\n"
            << "  list                discover, load and report every plugin with its state\n"
            << "  describe <id>       manifest, commands, settings and the panel it describes\n"
            << "  run <command>       call a plugin command and print what it returned\n"
            << "  windows             every window, menu item and tool the plugins registered\n"
            << "  window <id>         one window's frame; --set writes a field and --press is a\n"
            << "                      button, then the frame the plugin describes after is printed\n"
            << "  enable|disable <id> write the enabled flag and load or unload it\n"
            << "  check <dir>         validate a plugin folder without installing it; exits 1\n"
            << "  install <dir>       copy a plugin folder into the plugins directory\n"
            << "\n"
            << "The directories default to $QUANTUM_PLUGINS_DIR and $QUANTUM_PLUGIN_SETTINGS_DIR, then\n"
            << "to ./plugins and ./plugins/settings. Commands are typed by name in the registry, so\n"
            << "\"run hello.ping repeat=3\" needs no plugin id.\n"
            << "Nothing here opens a window: a plugin that loads here loads in the editor, and one\n"
            << "that fails here says why before anything is mapped.\n";
    }

    std::string pad(const std::string& text, size_t width) {
        std::string out = text;
        while (out.size() < width) out += ' ';
        return out;
    }

    // A settings file and a command line both carry loose values, so the CLI reads them the way the
    // plugin does: digits stay numbers, everything else arrives as text and coerces on the way in.
    UiValue parseValue(const std::string& text) {
        if (text == "true") return UiValue::fromBool(true);
        if (text == "false") return UiValue::fromBool(false);

        char* end = nullptr;
        const long long integer = std::strtoll(text.c_str(), &end, 10);
        if (end && *end == '\0' && !text.empty()) return UiValue::fromInt(integer);

        end = nullptr;
        const double number = std::strtod(text.c_str(), &end);
        if (end && *end == '\0' && !text.empty()) return UiValue::fromFloat(number);

        return UiValue::fromString(text);
    }

    std::string valueText(const UiValue& value) {
        switch (value.kind) {
            case UiValue::Kind::Bool: return value.toBool() ? "true" : "false";
            case UiValue::Kind::Int: return std::to_string(value.toInt());
            case UiValue::Kind::Float: {
                std::ostringstream out;
                out << value.toFloat();
                return out.str();
            }
            case UiValue::Kind::String: return value.toString();
            default: return "-";
        }
    }

    const char* controlName(UiControlType type) {
        switch (type) {
            case UiControlType::Label: return "label";
            case UiControlType::Separator: return "separator";
            case UiControlType::Bool: return "bool";
            case UiControlType::IntSlider: return "int-slider";
            case UiControlType::IntInput: return "int-input";
            case UiControlType::FloatSlider: return "float-slider";
            case UiControlType::FloatInput: return "float-input";
            case UiControlType::Enum: return "enum";
            case UiControlType::Color: return "color";
            case UiControlType::Button: return "button";
            case UiControlType::MultilineText: return "text";
        }
        return "control";
    }

    std::string joinList(const std::vector<std::string>& items, const char* separator) {
        std::string out;
        for (size_t i = 0; i < items.size(); i++) {
            if (i) out += separator;
            out += items[i];
        }
        return out;
    }

    struct Options {
        std::vector<std::string> positional;
        std::string pluginsDir;
        std::string settingsDir;
        // For `window`: the buttons to report pressed and the fields to write, as "field=value"
        // strings kept in order so two --set flags on one field resolve last-wins, as typing does.
        std::vector<std::string> presses;
        std::vector<std::string> sets;
        bool overwrite = false;
        bool wantsHelp = false;
        std::string problem;
    };

    // Flags may come before or after the positional words, because `run` takes arguments of its own
    // and they have to be tellable apart: a value never starts with "--".
    Options readOptions(int argc, char** argv) {
        Options options;
        for (int i = 1; i < argc; i++) {
            const std::string argument(argv[i]);
            if (argument == "-h" || argument == "--help") {
                options.wantsHelp = true;
                continue;
            }
            if (argument == "--overwrite") {
                options.overwrite = true;
                continue;
            }
            if (argument == "--plugins-dir" || argument == "--settings-dir") {
                if (i + 1 >= argc) {
                    options.problem = argument + " needs a directory";
                    return options;
                }
                const std::string value(argv[++i]);
                if (argument == "--plugins-dir") options.pluginsDir = value;
                else options.settingsDir = value;
                continue;
            }
            if (argument == "--press" || argument == "--set") {
                if (i + 1 >= argc) {
                    options.problem = argument + " needs a value";
                    return options;
                }
                const std::string value(argv[++i]);
                if (argument == "--press") options.presses.push_back(value);
                else options.sets.push_back(value);
                continue;
            }
            if (argument.size() > 2 && argument.compare(0, 2, "--") == 0) {
                options.problem = "unknown option " + argument;
                return options;
            }
            options.positional.push_back(argument);
        }
        return options;
    }

    PluginManager& theManager(const Options& options) {
        // One manager for the whole command: it owns the mapped libraries, and a second instance
        // would map the same DLL twice.
        static PluginManager manager;
        if (!options.pluginsDir.empty()) manager.setPluginsDirectory(options.pluginsDir);
        if (!options.settingsDir.empty()) manager.setSettingsDirectory(options.settingsDir);
        return manager;
    }

    std::string permissionsText(const PluginManifest& manifest) {
        if (manifest.permissions.empty()) return "-";
        std::string out;
        for (size_t i = 0; i < manifest.permissions.size(); i++) {
            if (i) out += " ";
            out += manifest.permissions[i];
            // Only the permissions the engine actually refuses a call without are worth marking.
            if (isEnforcedPermission(manifest.permissions[i])) out += "*";
        }
        return out;
    }

    int listCommand(PluginManager& manager) {
        manager.discover();
        manager.loadAll();

        std::cout << "plugins: " << manager.pluginsDirectory() << "\n";
        std::cout << "  settings: " << manager.settingsDirectory() << "\n";
        std::cout << "  engine abi " << QUANTUM_PLUGIN_ABI_VERSION << "\n";

        for (const PluginRecord* record : manager.records()) {
            std::cout << "  " << pad(record->manifest.id, 24)
                      << "[" << pad(pluginStateName(record->state), 13) << "] "
                      << pad(record->manifest.name + " " + record->manifest.version, 28)
                      << "permissions " << permissionsText(record->manifest);
            if (!record->error.empty()) std::cout << "  :: " << record->error;
            std::cout << "\n";
        }

        for (const PluginCommandInfo& command : manager.registry().commands()) {
            std::cout << "    command " << pad(command.name, 24) << "(" << command.pluginId
                      << ") " << command.description << "\n";
        }
        for (const PluginPanelInfo& panel : manager.registry().panels()) {
            std::cout << "    panel " << pad(panel.title, 24) << "(" << panel.pluginId << ")\n";
        }
        for (const PluginToolInfo& tool : manager.registry().tools()) {
            std::cout << "    tool " << pad(tool.name, 24) << "(" << tool.pluginId
                      << ") -> " << tool.action << "\n";
        }

        std::cout << "  " << manager.records().size() << " record(s), "
                  << manager.activeCount() << " active, " << manager.failedCount()
                  << " refused\n";
        // A refused plugin is not an error of the command line: `list` on a machine with a stale
        // build is a normal thing to do. `check` is the one that exits non-zero.
        return 0;
    }

    void printManifest(const PluginManifest& manifest) {
        std::cout << "  id " << manifest.id << "\n";
        std::cout << "  name " << manifest.name << " " << manifest.version << "\n";
        if (!manifest.author.empty()) std::cout << "  author " << manifest.author << "\n";
        if (!manifest.description.empty()) std::cout << "  description " << manifest.description << "\n";
        std::cout << "  type " << manifest.type << ", abi " << manifest.abiVersion;
        if (!manifest.minEngineVersion.empty()) {
            std::cout << ", min engine " << manifest.minEngineVersion;
        }
        std::cout << "\n";
        std::cout << "  platforms " << (manifest.platforms.empty()
                                            ? std::string("any")
                                            : joinList(manifest.platforms, ", ")) << "\n";
        std::cout << "  libraries " << joinList(manifest.libraries, ", ") << "\n";
        std::cout << "  permissions " << permissionsText(manifest) << "\n";
        std::cout << "  commands " << (manifest.commands.empty()
                                           ? std::string("-")
                                           : joinList(manifest.commands, ", ")) << "\n";
        for (const PluginDependency& dependency : manifest.dependencies) {
            std::cout << "  depends " << dependency.id
                      << (dependency.minVersion.empty() ? std::string()
                                                        : (" >= " + dependency.minVersion)) << "\n";
        }
        std::cout << "  folder " << manifest.directory << "\n";
    }

    // One described frame as text. The editor renders exactly this through PluginSchemaPanel, so
    // printing it is the headless half of "the window looks right".
    void printSchema(const char* heading, const UiSchema& schema) {
        if (schema.groups.empty()) {
            std::cout << "  " << heading << " none\n";
            return;
        }
        std::cout << "  " << heading << " \"" << schema.title << "\"\n";
        for (const UiGroup& group : schema.groups) {
            std::cout << "    " << (group.title.empty() ? std::string("(ungrouped)")
                                                        : group.title) << "\n";
            for (const UiField& field : group.fields) {
                std::cout << "      " << pad(field.id.empty() ? std::string("-") : field.id, 20)
                          << controlName(field.type) << "  " << field.label;
                const std::string value = valueText(field.value);
                if (value != "-") std::cout << " = " << value;
                if (field.type == UiControlType::Enum && !field.options.empty()) {
                    std::cout << " of " << joinList(field.options, "/");
                }
                if (field.type == UiControlType::IntSlider ||
                    field.type == UiControlType::FloatSlider) {
                    std::cout << " [" << field.minValue << ".." << field.maxValue << "]";
                }
                if (!field.enabled) std::cout << " (disabled)";
                if (!field.tooltip.empty()) std::cout << " // " << field.tooltip;
                std::cout << "\n";
            }
        }
    }

    int describeCommand(PluginManager& manager, const std::string& id) {
        manager.discover();
        if (!manager.find(id)) {
            std::cout << "plugins describe: no plugin.json with id \"" << id << "\" in "
                      << manager.pluginsDirectory() << "\n";
            return 1;
        }
        manager.load(id);

        const PluginRecord* record = manager.find(id);
        printManifest(record->manifest);
        std::cout << "  state " << pluginStateName(record->state);
        if (!record->error.empty()) std::cout << " :: " << record->error;
        std::cout << "\n";
        if (record->reportedAbiVersion) {
            std::cout << "  binary reports abi " << record->reportedAbiVersion << "\n";
        }
        std::cout << "  library " << (record->libraryPath.empty()
                                          ? std::string("not mapped")
                                          : record->libraryPath) << "\n";

        std::cout << "  settings " << record->settings.path() << "\n";
        for (const auto& entry : record->settings.values()) {
            std::cout << "    " << pad(entry.first, 20) << valueText(entry.second) << "\n";
        }

        // The panel, as the editor would draw it. Describing it is the only thing a plugin may do
        // about pixels, so this is the whole of its UI - which makes it the thing to check headless.
        const UiSchema* schema = manager.schemaOf(id);
        if (schema) printSchema("panel", *schema);
        else std::cout << "  panel none\n";

        for (const PluginLogEntry& entry : manager.log()) {
            if (entry.pluginId != id) continue;
            std::cout << "    log " << entry.message << "\n";
        }
        return 0;
    }

    int runCommand(PluginManager& manager, const Options& options) {
        if (options.positional.size() < 2) {
            std::cout << "plugins run: a command name is required, e.g. \"run hello.ping repeat=3\"\n";
            return 1;
        }

        const std::string command = options.positional[1];
        UiValueMap arguments;
        for (size_t i = 2; i < options.positional.size(); i++) {
            const std::string& pair = options.positional[i];
            const size_t equals = pair.find('=');
            if (equals == std::string::npos) {
                std::cout << "plugins run: \"" << pair << "\" is not key=value\n";
                return 1;
            }
            arguments[pair.substr(0, equals)] = parseValue(pair.substr(equals + 1));
        }

        manager.discover();
        manager.loadAll();

        UiValue result;
        std::string error;
        if (!manager.executeCommand(command, arguments, result, error)) {
            std::cout << "FAILED " << error << "\n";
            const std::string owner = manager.registry().ownerOfCommand(command);
            if (owner.empty()) {
                std::cout << "  commands of this session:\n";
                for (const PluginCommandInfo& known : manager.registry().commands()) {
                    std::cout << "    " << known.name << " (" << known.pluginId << ")\n";
                }
            }
            return 1;
        }

        std::cout << valueText(result) << "\n";
        return 0;
    }

    int setEnabledCommand(PluginManager& manager, const std::string& id, bool enabled) {
        manager.discover();

        if (!manager.setEnabled(id, enabled)) {
            std::cout << "plugins " << (enabled ? "enable" : "disable") << ": " << id
                      << " is not installed in " << manager.pluginsDirectory() << "\n";
            return 1;
        }

        const PluginRecord* record = manager.find(id);
        std::cout << id << " " << (enabled ? "enabled" : "disabled") << ", state "
                  << (record ? pluginStateName(record->state) : "unknown") << " (written to "
                  << manager.settingsDirectory() << "/" << id << ".json)\n";
        return record && record->state == PluginState::Active ? 0 : 1;
    }

    // Validates a folder the way the loader will see it, without copying it anywhere: this is the
    // check a packaged plugin runs in CI, and the library-has-to-be-there half of it is what a
    // manifest-only review misses.
    int checkCommand(const std::string& directory) {
        PluginManifest manifest;
        const std::string manifestPath = directory + "/plugin.json";
        if (!loadPluginManifest(manifestPath, manifest)) {
            std::cout << "CHECK FAILED " << manifest.errorsAsText() << "\n";
            return 1;
        }

        printManifest(manifest);

        std::vector<std::string> problems;
        if (!supportsCurrentPlatform(manifest)) {
            problems.push_back(std::string("not built for ") + currentPlatformTag());
        }
        if (manifest.abiVersion != QUANTUM_PLUGIN_ABI_VERSION) {
            problems.push_back("abi " + std::to_string(manifest.abiVersion) +
                               ", engine abi " + std::to_string(QUANTUM_PLUGIN_ABI_VERSION));
        }
        if (manifest.libraryFileName().empty()) {
            problems.push_back("no \"libraries\" entry, so there is nothing to map");
        } else {
            // The same three places the loader looks, without a PluginManager: the folder, its
            // lib/ subfolder, and beside the executable.
            const std::string fileName = manifest.libraryFileName();
            const std::vector<std::string> candidates = { directory + "/" + fileName,
                                                          directory + "/lib/" + fileName };
            bool found = false;
            for (const std::string& candidate : candidates) {
                std::error_code ec;
                if (std::filesystem::exists(std::filesystem::path(candidate), ec)) {
                    std::cout << "  library " << candidate << "\n";
                    found = true;
                    break;
                }
            }
            if (!found) problems.push_back("library \"" + fileName + "\" is not in " + directory);
        }

        for (const std::string& permission : manifest.permissions) {
            if (isKnownPermission(permission)) continue;
            problems.push_back("unknown permission \"" + permission + "\"");
        }

        for (const std::string& problem : problems) std::cout << "CHECK FAILED " << problem << "\n";
        std::cout << (problems.empty() ? "OK " : "") << manifest.id << ": " << problems.size()
                  << " problem(s)\n";
        return problems.empty() ? 0 : 1;
    }

    int installCommand(PluginManager& manager, const std::string& source, bool overwrite) {
        std::string error;
        const std::string id = manager.installFromDirectory(source, overwrite, error);
        if (id.empty()) {
            std::cout << "plugins install: " << error << "\n";
            return 1;
        }
        std::cout << "installed " << id << " in " << manager.pluginsDirectory() << "\n";
        return manager.load(id) ? 0 : 1;
    }

    // Everything the loaded plugins asked the editor to host, and the frame each one describes.
    // buildWindowSchema() is exactly what PluginWindowHost calls per frame, so a window that prints
    // here is a window that renders - and the menu/tool rows print with the command they would run.
    int windowsCommand(PluginManager& manager) {
        manager.discover();
        manager.loadAll();

        const std::vector<PluginWindowInfo>& windows = manager.registry().windows();
        if (windows.empty()) {
            std::cout << "no plugin window is registered (a plugin calls context().registerWindow() "
                         "from onStart, and needs the \"ui\" permission)\n";
        } else {
            std::cout << windows.size() << " plugin window(s) registered\n";
        }

        for (const PluginWindowInfo& window : windows) {
            std::cout << "window " << pad(window.id, 24) << "(" << window.pluginId << ") \""
                      << window.title << "\"";
            if (!window.category.empty()) std::cout << " in " << window.category;
            std::cout << ", opens " << window.width << "x" << window.height << "\n";

            UiSchema frame;
            std::string error;
            if (!manager.buildWindowSchema(window.id, frame, error)) {
                std::cout << "  FAILED " << error << "\n";
                continue;
            }
            printSchema("frame", frame);
        }

        // A menu row or a tool that names no registered command is a row the user can click with
        // nothing behind it, so the registry is checked rather than trusted.
        PluginCommandInfo target;
        for (const PluginMenuItemInfo& item : manager.registry().menuItems()) {
            std::cout << "menu " << pad(item.category.empty() ? std::string("(top)") : item.category, 14)
                      << pad(item.label, 24) << "-> " << item.command;
            if (!manager.registry().findCommand(item.command, target)) std::cout << "  :: no such command";
            std::cout << "\n";
        }
        for (const PluginToolInfo& tool : manager.registry().tools()) {
            std::cout << "tool " << pad(tool.name, 24) << "-> " << tool.action;
            if (!manager.registry().findCommand(tool.action, target)) std::cout << "  :: no such command";
            std::cout << "\n";
        }
        return 0;
    }

    // One window's frame, answered as the user would answer it: --set writes a field and --press is
    // a button, and both go through the same sendWindowInput() the editor host calls. `after` is the
    // frame the plugin then describes, which is what makes a window that only reads obvious.
    int windowCommand(PluginManager& manager, const Options& options) {
        if (options.positional.size() < 2) {
            std::cout << "plugins window: a window id is required, e.g. "
                         "\"window hello.stats --press note --set lines=3\"\n";
            return 1;
        }
        const std::string windowId = options.positional[1];

        manager.discover();
        manager.loadAll();

        PluginWindowInfo window;
        if (!manager.registry().findWindow(windowId, window)) {
            std::cout << "plugins window: \"" << windowId << "\" is not registered.\n";
            for (const PluginWindowInfo& known : manager.registry().windows()) {
                std::cout << "  " << known.id << " (" << known.pluginId << ") " << known.title << "\n";
            }
            return 1;
        }

        UiSchema frame;
        std::string error;
        if (!manager.buildWindowSchema(windowId, frame, error)) {
            std::cout << "FAILED " << error << "\n";
            return 1;
        }
        printSchema("before", frame);

        for (const std::string& pair : options.sets) {
            const size_t equals = pair.find('=');
            if (equals == std::string::npos) {
                std::cout << "plugins window: \"" << pair << "\" is not field=value\n";
                return 1;
            }
            UiField* field = frame.findField(pair.substr(0, equals));
            if (!field) {
                std::cout << "plugins window: this frame has no field \"" << pair.substr(0, equals)
                          << "\"\n";
                return 1;
            }
            // Written the way the panel writes it, so the values collected below are the values the
            // editor would have sent.
            field->value = parseValue(pair.substr(equals + 1));
            field->changed = true;
        }

        if (options.sets.empty() && options.presses.empty()) return 0;

        const UiValueMap values = collectSchemaValues(frame);
        std::string sendError;
        if (!manager.sendWindowInput(windowId, values, options.presses, sendError)) {
            std::cout << "FAILED " << sendError << "\n";
            return 1;
        }
        std::cout << "sent " << values.size() << " value(s), " << options.presses.size()
                  << " press(es)\n";

        UiSchema after;
        if (!manager.buildWindowSchema(windowId, after, error)) {
            std::cout << "FAILED " << error << "\n";
            return 1;
        }
        printSchema("after", after);
        return 0;
    }

}

int runPluginsCommand(int argc, char** argv, const char* executableName) {
    const Options options = readOptions(argc, argv);
    if (!options.problem.empty()) {
        std::cout << "plugins: " << options.problem << "\n";
        usage(executableName);
        return 1;
    }
    if (options.wantsHelp || options.positional.empty()) {
        usage(executableName);
        // Asking for help is not a failure; running the verb with no argument is.
        return options.wantsHelp ? 0 : 1;
    }

    PluginManager& manager = theManager(options);
    const std::string action = options.positional[0];

    if (action == "list") return listCommand(manager);

    if (action == "describe") {
        if (options.positional.size() < 2) {
            std::cout << "plugins describe: a plugin id is required\n";
            return 1;
        }
        return describeCommand(manager, options.positional[1]);
    }

    if (action == "run") return runCommand(manager, options);

    if (action == "enable" || action == "disable") {
        if (options.positional.size() < 2) {
            std::cout << "plugins " << action << ": a plugin id is required\n";
            return 1;
        }
        return setEnabledCommand(manager, options.positional[1], action == "enable");
    }

    if (action == "check") {
        if (options.positional.size() < 2) {
            std::cout << "plugins check: a plugin folder is required\n";
            return 1;
        }
        return checkCommand(options.positional[1]);
    }

    if (action == "install") {
        if (options.positional.size() < 2) {
            std::cout << "plugins install: a plugin folder is required\n";
            return 1;
        }
        return installCommand(manager, options.positional[1], options.overwrite);
    }

    if (action == "windows") return windowsCommand(manager);

    if (action == "window") return windowCommand(manager, options);

    std::cout << "plugins: unknown action '" << action << "'\n";
    usage(executableName);
    return 1;
}

}
