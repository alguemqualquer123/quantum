// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "PluginSettings.h"

#include "json.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>

using namespace quantum;
using namespace quantum::plugin;

namespace {

// One file holds every plugin's settings, keyed by id; an id-less store falls back to a literal
// section name rather than writing at the root of the file.
std::string sectionKey(const std::string& id) {
    return id.empty() ? std::string("values") : id;
}

}

void PluginSettings::setPluginId(const std::string& newId) {
    id = newId;
}

void PluginSettings::setPath(const std::string& filePathValue) {
    filePath = filePathValue;
}

void PluginSettings::setDefaults(const UiValueMap& newDefaults) {
    defaultValues = newDefaults;
    reset();
}

void PluginSettings::reset() {
    currentValues = defaultValues;
    dirty = false;
}

bool PluginSettings::load() {
    // Start from the shipped defaults so a key the plugin added after the file was written still
    // has a value.
    reset();
    error.clear();

    if (filePath.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(filePath), ec)) return true;

    std::ifstream stream(filePath, std::ios::binary);
    if (!stream.is_open()) {
        error = "cannot open " + filePath;
        return false;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();

    const nlohmann::json root = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (root.is_discarded()) {
        error = "corrupt JSON in " + filePath;
        return false;
    }
    if (!root.is_object()) {
        error = "settings file must contain an object";
        return false;
    }

    // The file is keyed by plugin id so one directory can hold every plugin's settings without
    // collisions, and a plugin cannot read another's by pointing at the same file.
    const std::string key = sectionKey(id);
    auto section = root.find(key);
    const nlohmann::json* source = &root;
    if (section != root.end() && section->is_object()) source = &(*section);

    for (auto it = source->begin(); it != source->end(); ++it) {
        const nlohmann::json& value = it.value();
        const std::string& name = it.key();
        if (value.is_boolean()) {
            currentValues[name] = UiValue::fromBool(value.get<bool>());
        } else if (value.is_number_integer()) {
            currentValues[name] = UiValue::fromInt(value.get<long long>());
        } else if (value.is_number()) {
            currentValues[name] = UiValue::fromFloat(value.get<double>());
        } else if (value.is_string()) {
            currentValues[name] = UiValue::fromString(value.get<std::string>());
        }
    }

    dirty = false;
    return true;
}

bool PluginSettings::save() {
    error.clear();
    if (filePath.empty()) {
        error = "no settings path for plugin " + id;
        return false;
    }

    nlohmann::json section = nlohmann::json::object();
    for (const auto& entry : currentValues) {
        const UiValue& value = entry.second;
        switch (value.kind) {
            case UiValue::Kind::Bool: section[entry.first] = value.boolValue; break;
            case UiValue::Kind::Int: section[entry.first] = value.intValue; break;
            case UiValue::Kind::Float: section[entry.first] = value.floatValue; break;
            case UiValue::Kind::String: section[entry.first] = value.stringValue; break;
            default: break;
        }
    }

    nlohmann::json root;
    root[sectionKey(id)] = section;

    std::error_code ec;
    const std::filesystem::path target(filePath);
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }

    std::ofstream stream(filePath, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        error = "cannot write " + filePath;
        return false;
    }
    stream << root.dump(2);
    if (!stream.good()) {
        error = "write failed for " + filePath;
        return false;
    }

    dirty = false;
    return true;
}

bool PluginSettings::has(const std::string& key) const {
    return currentValues.find(key) != currentValues.end();
}

UiValue PluginSettings::get(const std::string& key) const {
    auto found = currentValues.find(key);
    if (found == currentValues.end()) return UiValue();
    return found->second;
}

bool PluginSettings::getBool(const std::string& key, bool fallback) const {
    return get(key).toBool(fallback);
}

long long PluginSettings::getInt(const std::string& key, long long fallback) const {
    return get(key).toInt(fallback);
}

double PluginSettings::getFloat(const std::string& key, double fallback) const {
    return get(key).toFloat(fallback);
}

std::string PluginSettings::getString(const std::string& key, const std::string& fallback) const {
    UiValue value = get(key);
    return value.kind == UiValue::Kind::None ? fallback : value.toString(fallback);
}

void PluginSettings::set(const std::string& key, const UiValue& value) {
    auto found = currentValues.find(key);
    if (found != currentValues.end() && found->second.toString() == value.toString() &&
        found->second.kind == value.kind) {
        return;
    }
    currentValues[key] = value;
    dirty = true;
}

void PluginSettings::erase(const std::string& key) {
    auto found = currentValues.find(key);
    if (found == currentValues.end()) return;
    currentValues.erase(found);
    dirty = true;
}
