// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "PluginLock.h"

#include "json.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace quantum::plugin;
using nlohmann::json;

namespace {

    bool readString(const json& node, const char* key, std::string& out) {
        auto found = node.find(key);
        if (found == node.end() || !found->is_string()) return false;
        out = found->get<std::string>();
        return true;
    }

}

bool PluginLock::load(const std::string& filePath, std::string& errorOut) {
    errorOut.clear();
    entries.clear();
    path = filePath;

    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec)) return true;

    std::ifstream stream(filePath, std::ios::binary);
    if (!stream.is_open()) {
        errorOut = "cannot open " + filePath;
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    json root = json::parse(buffer.str(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        errorOut = filePath + " is not a JSON object";
        return false;
    }

    auto packages = root.find("packages");
    if (packages == root.end() || !packages->is_array()) {
        errorOut = filePath + " has no \"packages\" array";
        return false;
    }

    for (const json& node : *packages) {
        if (!node.is_object()) continue;
        InstalledPackage installed;
        if (!readString(node, "id", installed.id) || installed.id.empty()) continue;
        readString(node, "name", installed.name);
        readString(node, "version", installed.version);
        readString(node, "registry", installed.registry);
        readString(node, "file", installed.packageFile);
        readString(node, "sha256", installed.sha256);
        readString(node, "directory", installed.directory);
        readString(node, "installedAt", installed.installedAt);

        auto size = node.find("size");
        if (size != node.end() && size->is_number_unsigned()) {
            installed.size = size->get<unsigned long long>();
        }
        auto content = node.find("content");
        if (content != node.end() && content->is_array()) {
            for (const json& target : *content) {
                if (target.is_string()) installed.contentTargets.push_back(target.get<std::string>());
            }
        }
        put(installed);
    }
    return true;
}

bool PluginLock::save(std::string& errorOut) const {
    errorOut.clear();
    if (path.empty()) {
        errorOut = "no installed.json path";
        return false;
    }

    json list = json::array();
    for (const InstalledPackage& installed : entries) {
        json node;
        node["id"] = installed.id;
        node["name"] = installed.name;
        node["version"] = installed.version;
        node["registry"] = installed.registry;
        node["file"] = installed.packageFile;
        node["sha256"] = installed.sha256;
        node["size"] = installed.size;
        node["directory"] = installed.directory;
        node["installedAt"] = installed.installedAt;
        node["content"] = installed.contentTargets;
        list.push_back(node);
    }

    json root;
    root["packages"] = list;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        errorOut = "cannot write " + path;
        return false;
    }
    stream << root.dump(2) << "\n";
    if (!stream.good()) {
        errorOut = "the disk refused " + path;
        return false;
    }
    return true;
}

const InstalledPackage* PluginLock::find(const std::string& id) const {
    for (const InstalledPackage& installed : entries) {
        if (installed.id == id) return &installed;
    }
    return nullptr;
}

void PluginLock::put(const InstalledPackage& package) {
    for (InstalledPackage& existing : entries) {
        if (existing.id == package.id) {
            existing = package;
            return;
        }
    }
    entries.push_back(package);
}

bool PluginLock::remove(const std::string& id) {
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].id != id) continue;
        entries.erase(entries.begin() + i);
        return true;
    }
    return false;
}

std::string PluginLock::formatTimestamp() {
    // UTC and fixed-width, because this string is compared and sorted, not read as prose.
    std::time_t now = std::time(nullptr);
    std::tm stamped = {};
#if defined(_WIN32)
    gmtime_s(&stamped, &now);
#else
    gmtime_r(&now, &stamped);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &stamped);
    return std::string(buffer);
}
