// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "PluginManifest.h"
#include "PluginPlatform.h"

#include "json.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>

using namespace quantum;
using namespace quantum::plugin;

namespace {

    void requireString(const nlohmann::json& root, const char* key, std::string& out,
                       PluginManifest& manifest) {
        auto found = root.find(key);
        if (found == root.end() || !found->is_string()) {
            manifest.errors.push_back(std::string("missing string field \"") + key + "\"");
            return;
        }
        out = found->get<std::string>();
    }

    void optionalString(const nlohmann::json& root, const char* key, std::string& out) {
        auto found = root.find(key);
        if (found != root.end() && found->is_string()) out = found->get<std::string>();
    }

    void optionalStringList(const nlohmann::json& root, const char* key,
                            std::vector<std::string>& out) {
        auto found = root.find(key);
        if (found == root.end() || !found->is_array()) return;
        for (const nlohmann::json& entry : *found) {
            if (entry.is_string()) out.push_back(entry.get<std::string>());
        }
    }

    void optionalBool(const nlohmann::json& root, const char* key, bool& out) {
        auto found = root.find(key);
        if (found != root.end() && found->is_boolean()) out = found->get<bool>();
    }

    // A settings default is a scalar. Objects and arrays are reported instead of being dropped
    // silently, because a plugin that ships a nested default would otherwise load with a blank.
    void readDefaults(const nlohmann::json& root, PluginManifest& manifest) {
        auto found = root.find("settings");
        if (found == root.end() || !found->is_object()) return;
        for (auto it = found->begin(); it != found->end(); ++it) {
            const nlohmann::json& value = it.value();
            const std::string& key = it.key();
            if (value.is_boolean()) {
                manifest.defaults[key] = UiValue::fromBool(value.get<bool>());
            } else if (value.is_number_integer()) {
                manifest.defaults[key] = UiValue::fromInt(value.get<long long>());
            } else if (value.is_number()) {
                manifest.defaults[key] = UiValue::fromFloat(value.get<double>());
            } else if (value.is_string()) {
                manifest.defaults[key] = UiValue::fromString(value.get<std::string>());
            } else {
                manifest.errors.push_back("settings.\""+ key + "\" is not a scalar");
            }
        }
    }

    void readDependencies(const nlohmann::json& root, PluginManifest& manifest) {
        auto found = root.find("dependencies");
        if (found == root.end() || !found->is_array()) return;
        for (const nlohmann::json& entry : *found) {
            PluginDependency dependency;
            if (entry.is_string()) {
                dependency.id = entry.get<std::string>();
            } else if (entry.is_object()) {
                auto id = entry.find("id");
                if (id == entry.end() || !id->is_string()) {
                    manifest.errors.push_back("dependency object without a string \"id\"");
                    continue;
                }
                dependency.id = id->get<std::string>();
                auto minimum = entry.find("minVersion");
                if (minimum != entry.end() && minimum->is_string()) {
                    dependency.minVersion = minimum->get<std::string>();
                }
            } else {
                manifest.errors.push_back("dependency entry is neither a string nor an object");
                continue;
            }
            if (!dependency.id.empty()) manifest.dependencies.push_back(dependency);
        }
    }

}

bool quantum::plugin::parsePluginManifest(const std::string& jsonText, PluginManifest& out) {
    out.errors.clear();
    out.valid = false;

    // allow_exceptions = false: a malformed file is a data error reported through the record,
    // not something that unwinds through the loader (see the no-throw rule in PluginManager).
    nlohmann::json root = nlohmann::json::parse(jsonText, nullptr, false);
    if (root.is_discarded()) {
        out.errors.push_back("plugin.json is not valid JSON");
        return false;
    }
    if (!root.is_object()) {
        out.errors.push_back("plugin.json must contain an object");
        return false;
    }

    requireString(root, "id", out.id, out);
    requireString(root, "name", out.name, out);
    requireString(root, "version", out.version, out);

    optionalString(root, "description", out.description);
    optionalString(root, "author", out.author);
    optionalString(root, "minEngineVersion", out.minEngineVersion);
    optionalString(root, "type", out.type);
    optionalString(root, "entryPoint", out.entryPoint);

    optionalStringList(root, "platforms", out.platforms);
    optionalStringList(root, "libraries", out.libraries);
    optionalStringList(root, "permissions", out.permissions);
    optionalStringList(root, "commands", out.commands);

    auto abi = root.find("abiVersion");
    if (abi != root.end() && abi->is_number_unsigned()) {
        out.abiVersion = abi->get<unsigned int>();
    } else {
        out.errors.push_back("missing number field \"abiVersion\"");
    }

    auto panel = root.find("panel");
    if (panel != root.end() && panel->is_object()) {
        optionalString(*panel, "title", out.panelTitle);
    }

    optionalString(root, "category", out.category);
    optionalString(root, "summary", out.summary);
    optionalString(root, "icon", out.icon);
    optionalString(root, "preview", out.preview);
    optionalString(root, "docsUrl", out.docsUrl);
    optionalString(root, "marketplaceUrl", out.marketplaceUrl);
    optionalString(root, "supportUrl", out.supportUrl);
    optionalStringList(root, "keywords", out.keywords);
    optionalBool(root, "enabledByDefault", out.enabledByDefault);
    optionalBool(root, "canContainContent", out.canContainContent);
    optionalBool(root, "isBetaVersion", out.isBetaVersion);
    optionalBool(root, "installedPackage", out.installedPackage);

    readDefaults(root, out);
    readDependencies(root, out);

    if (out.libraries.empty()) out.errors.push_back("\"libraries\" is empty");
    if (out.id.find(' ') != std::string::npos) out.errors.push_back("\"id\" must not contain spaces");

    out.valid = out.errors.empty();
    return out.valid;
}

bool quantum::plugin::loadPluginManifest(const std::string& filePath, PluginManifest& out) {
    std::ifstream stream(filePath, std::ios::binary);
    if (!stream.is_open()) {
        out.valid = false;
        out.errors.clear();
        out.errors.push_back("cannot open " + filePath);
        return false;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();

    out.path = filePath;
    std::error_code ec;
    out.directory = std::filesystem::path(filePath).parent_path().generic_string();
    (void)ec;

    return parsePluginManifest(buffer.str(), out);
}

bool quantum::plugin::supportsCurrentPlatform(const PluginManifest& manifest) {
    if (manifest.platforms.empty()) return true;
    const std::string here = currentPlatformTag();
    for (const std::string& platform : manifest.platforms) {
        if (platform == here) return true;
    }
    return false;
}

bool PluginManifest::hasPermission(const std::string& permission) const {
    for (const std::string& owned : permissions) {
        if (owned == permission) return true;
    }
    return false;
}

std::string PluginManifest::libraryFileName() const {
    if (libraries.empty()) return std::string();
    std::string name = libraries.front();
    const std::string suffix = platformLibraryExtension();
    if (name.size() < suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        name += suffix;
    }
    return name;
}

std::string PluginManifest::errorsAsText() const {
    std::string out;
    for (size_t i = 0; i < errors.size(); i++) {
        if (i) out += "; ";
        out += errors[i];
    }
    return out;
}

namespace {

    // What the engine actually enforces, by checking the name on the way through the call.
    const char* const ENFORCED_PERMISSIONS[] = { "log", "scene", "tick", "files", "ui" };
    // Known, parseable, and not checked anywhere: the manifest is telling the user that the plugin
    // touches its settings file, which the engine never gates.
    const char* const DECLARED_ONLY_PERMISSIONS[] = { "settings" };

    template <size_t N>
    bool namesThe(const char* const (&table)[N], const std::string& name) {
        for (const char* known : table) {
            if (name == known) return true;
        }
        return false;
    }

}

bool quantum::plugin::isEnforcedPermission(const std::string& name) {
    return namesThe(ENFORCED_PERMISSIONS, name);
}

bool quantum::plugin::isKnownPermission(const std::string& name) {
    return namesThe(ENFORCED_PERMISSIONS, name) || namesThe(DECLARED_ONLY_PERMISSIONS, name);
}
