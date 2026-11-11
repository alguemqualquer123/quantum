// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "PluginCatalog.h"
#include "PluginVersion.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace quantum::plugin;
using nlohmann::json;

namespace {

    const char* const FALLBACK_CATEGORY = "General";

    std::string lowered(const std::string& text) {
        std::string out = text;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    bool readString(const json& node, const char* key, std::string& out) {
        auto found = node.find(key);
        if (found == node.end() || !found->is_string()) return false;
        out = found->get<std::string>();
        return true;
    }

    bool readBool(const json& node, const char* key, bool& out) {
        auto found = node.find(key);
        if (found == node.end() || !found->is_boolean()) return false;
        out = found->get<bool>();
        return true;
    }

    void readStringList(const json& node, const char* key, std::vector<std::string>& out) {
        auto found = node.find(key);
        if (found == node.end() || !found->is_array()) return;
        for (const json& entry : *found) {
            if (entry.is_string()) out.push_back(entry.get<std::string>());
        }
    }

    void readDependencies(const json& node, std::vector<PluginDependency>& out) {
        auto found = node.find("dependencies");
        if (found == node.end() || !found->is_array()) return;
        for (const json& entry : *found) {
            PluginDependency dependency;
            if (entry.is_string()) {
                dependency.id = entry.get<std::string>();
            } else if (entry.is_object()) {
                readString(entry, "id", dependency.id);
                readString(entry, "minVersion", dependency.minVersion);
            }
            if (!dependency.id.empty()) out.push_back(dependency);
        }
    }

    // The newest listed version by number, which is what a catalog without an explicit "latest"
    // still has to answer with.
    const CatalogVersion* newest(const CatalogEntry& entry) {
        const CatalogVersion* best = nullptr;
        for (const CatalogVersion& version : entry.versions) {
            if (!best) { best = &version; continue; }
            if (compareVersions(parseVersion(version.version), parseVersion(best->version)) > 0) {
                best = &version;
            }
        }
        return best;
    }

    bool parseVersionList(const json& node, CatalogEntry& entry, std::vector<std::string>& errors) {
        auto versions = node.find("versions");
        if (versions == node.end() || !versions->is_array() || versions->empty()) {
            errors.push_back(entry.id + ": \"versions\" must be a non-empty array");
            return false;
        }

        for (const json& item : *versions) {
            if (!item.is_object()) {
                errors.push_back(entry.id + ": a versions entry is not an object");
                continue;
            }
            CatalogVersion version;
            if (!readString(item, "version", version.version) || version.version.empty()) {
                errors.push_back(entry.id + ": a versions entry has no \"version\"");
                continue;
            }
            if (!readString(item, "file", version.file) || version.file.empty()) {
                errors.push_back(entry.id + " " + version.version + ": no \"file\" to download");
                continue;
            }
            readString(item, "sha256", version.sha256);
            readString(item, "minEngineVersion", version.minEngineVersion);
            readString(item, "description", version.description);
            readString(item, "changelog", version.changelog);
            readString(item, "docsUrl", version.docsUrl);
            readString(item, "marketplaceUrl", version.marketplaceUrl);
            readString(item, "supportUrl", version.supportUrl);
            readString(item, "icon", version.icon);
            readString(item, "preview", version.preview);
            readBool(item, "canContainContent", version.canContainContent);
            readBool(item, "isBetaVersion", version.isBetaVersion);

            auto size = item.find("size");
            if (size != item.end() && size->is_number_unsigned()) {
                version.size = size->get<unsigned long long>();
            }
            readStringList(item, "permissions", version.permissions);
            readDependencies(item, version.dependencies);

            bool repeated = false;
            for (const CatalogVersion& existing : entry.versions) {
                if (existing.version == version.version) {
                    errors.push_back(entry.id + ": version " + version.version + " is listed twice");
                    repeated = true;
                    break;
                }
            }
            if (!repeated) entry.versions.push_back(version);
        }
        return !entry.versions.empty();
    }

}

const CatalogVersion* CatalogEntry::findVersion(const std::string& text) const {
    if (text.empty() || text == "latest") {
        const CatalogVersion* byName = nullptr;
        for (const CatalogVersion& version : versions) {
            if (version.version == latest) byName = &version;
        }
        return byName ? byName : newest(*this);
    }
    for (const CatalogVersion& version : versions) {
        if (version.version == text) return &version;
    }
    return nullptr;
}

const CatalogEntry* quantum::plugin::findCatalogEntry(const Catalog& catalog, const std::string& id) {
    for (const CatalogEntry& entry : catalog.entries) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

const CatalogVersion* quantum::plugin::resolveCatalogVersion(const Catalog& catalog,
                                                            const std::string& idWithOptionalAt,
                                                            const CatalogEntry** entryOut) {
    std::string id = idWithOptionalAt;
    std::string requested;
    const size_t at = idWithOptionalAt.find('@');
    if (at != std::string::npos) {
        id = idWithOptionalAt.substr(0, at);
        requested = idWithOptionalAt.substr(at + 1);
    }

    const CatalogEntry* entry = findCatalogEntry(catalog, id);
    if (entryOut) *entryOut = entry;
    if (!entry) return nullptr;

    const CatalogVersion* version = entry->findVersion(requested);
    if (!version && !requested.empty()) {
        // "latest" named explicitly but the catalog has no such tag is a user error worth a line;
        // an empty request must never fail, because `install <id>` is the common form.
        return nullptr;
    }
    return version;
}

std::vector<CatalogCategory> quantum::plugin::catalogCategories(const Catalog& catalog) {
    std::vector<CatalogCategory> out;
    for (const CatalogEntry& entry : catalog.entries) {
        const std::string name = entry.category.empty() ? FALLBACK_CATEGORY : entry.category;
        bool found = false;
        for (CatalogCategory& category : out) {
            if (category.name == name) { category.count++; found = true; break; }
        }
        if (!found) out.push_back({ name, 1 });
    }
    std::sort(out.begin(), out.end(), [](const CatalogCategory& a, const CatalogCategory& b) {
        return a.name < b.name;
    });
    return out;
}

std::vector<const CatalogEntry*> quantum::plugin::searchCatalog(const Catalog& catalog,
                                                               const std::string& text,
                                                               const std::string& category) {
    const std::string needle = lowered(text);
    std::vector<const CatalogEntry*> out;
    for (const CatalogEntry& entry : catalog.entries) {
        const std::string entryCategory = entry.category.empty() ? FALLBACK_CATEGORY : entry.category;
        if (!category.empty() && category != "All" && entryCategory != category) continue;

        if (needle.empty()) {
            out.push_back(&entry);
            continue;
        }
        bool hits = lowered(entry.id).find(needle) != std::string::npos ||
                    lowered(entry.name).find(needle) != std::string::npos ||
                    lowered(entry.summary).find(needle) != std::string::npos ||
                    lowered(entry.author).find(needle) != std::string::npos;
        for (const std::string& keyword : entry.keywords) {
            if (lowered(keyword).find(needle) != std::string::npos) hits = true;
        }
        if (hits) out.push_back(&entry);
    }
    std::sort(out.begin(), out.end(), [](const CatalogEntry* a, const CatalogEntry* b) {
        return a->name < b->name;
    });
    return out;
}

bool quantum::plugin::parsePluginCatalog(const std::string& jsonText, const std::string& registry,
                                        const std::string& root, Catalog& out) {
    out.errors.clear();
    out.valid = false;
    out.entries.clear();
    out.registry = registry;
    out.root = root;

    json parsed = json::parse(jsonText, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        out.errors.push_back("the catalog index is not a JSON object");
        return false;
    }

    readString(parsed, "generated", out.generated);
    std::string namedRegistry;
    if (readString(parsed, "registry", namedRegistry) && out.registry.empty()) {
        out.registry = namedRegistry;
    }

    auto packages = parsed.find("packages");
    if (packages == parsed.end() || !packages->is_array()) {
        out.errors.push_back("the catalog index has no \"packages\" array");
        return false;
    }

    for (const json& node : *packages) {
        if (!node.is_object()) {
            out.errors.push_back("a packages entry is not an object");
            continue;
        }
        CatalogEntry entry;
        entry.registry = out.registry;
        entry.root = out.root;
        if (!readString(node, "id", entry.id) || entry.id.empty()) {
            out.errors.push_back("a package has no \"id\"");
            continue;
        }
        readString(node, "name", entry.name);
        if (entry.name.empty()) entry.name = entry.id;
        readString(node, "category", entry.category);
        readString(node, "author", entry.author);
        readString(node, "summary", entry.summary);
        readStringList(node, "keywords", entry.keywords);
        readString(node, "latest", entry.latest);

        if (!parseVersionList(node, entry, out.errors)) continue;

        bool duplicate = false;
        for (const CatalogEntry& existing : out.entries) {
            if (existing.id == entry.id) {
                out.errors.push_back(entry.id + ": listed twice");
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        if (entry.latest.empty()) {
            const CatalogVersion* top = newest(entry);
            if (top) entry.latest = top->version;
        }
        out.entries.push_back(entry);
    }

    out.valid = out.errors.empty();
    return out.valid;
}

bool quantum::plugin::loadPluginCatalog(const std::string& indexPath, const std::string& registry,
                                       Catalog& out) {
    std::ifstream stream(indexPath, std::ios::binary);
    if (!stream.is_open()) {
        out.errors.clear();
        out.valid = false;
        out.root = std::filesystem::path(indexPath).parent_path().generic_string();
        out.registry = registry;
        out.errors.push_back("cannot open " + indexPath);
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();

    // A remote registry publishes where its files hang inside its own index, so the caller does not
    // have to know whether it was reached as a directory or a URL.
    std::string root = std::filesystem::path(indexPath).parent_path().generic_string();
    json probe = json::parse(text, nullptr, false);
    std::string declaredRoot;
    if (!probe.is_discarded() && probe.is_object() && readString(probe, "root", declaredRoot) &&
        !declaredRoot.empty()) {
        root = declaredRoot;
    }

    return parsePluginCatalog(text, registry, root, out);
}

bool quantum::plugin::parseRegistryConfig(const std::string& jsonText, RegistryConfig& out) {
    out.errors.clear();
    out.sources.clear();

    json parsed = json::parse(jsonText, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        out.errors.push_back("registries.json is not a JSON object");
        return false;
    }

    auto registries = parsed.find("registries");
    if (registries == parsed.end() || !registries->is_array()) {
        out.errors.push_back("registries.json has no \"registries\" array");
        return false;
    }

    for (const json& node : *registries) {
        if (!node.is_object()) continue;
        RegistrySource source;
        if (!readString(node, "name", source.name) || source.name.empty()) continue;
        if (!readString(node, "url", source.url) || source.url.empty()) {
            out.errors.push_back(source.name + ": no \"url\"");
            continue;
        }
        readStringList(node, "scopes", source.scopes);
        readBool(node, "builtIn", source.builtIn);

        bool duplicate = false;
        for (const RegistrySource& existing : out.sources) {
            if (existing.name == source.name) {
                out.errors.push_back(source.name + ": listed twice");
                duplicate = true;
                break;
            }
        }
        if (!duplicate) out.sources.push_back(source);
    }
    return out.errors.empty();
}

bool quantum::plugin::loadRegistryConfig(const std::string& filePath, RegistryConfig& out) {
    out.path = filePath;
    out.sources.clear();
    out.errors.clear();

    // The built-in registry is always there, whether or not the file exists: an offline install has
    // to work on a fresh clone with no user configuration at all.
    const std::string pluginsDirectory =
        std::filesystem::path(filePath).parent_path().generic_string();
    const std::string builtin = builtinCatalogRoot(pluginsDirectory);
    if (!builtin.empty()) {
        RegistrySource source;
        source.name = "built-in";
        source.url = builtin;
        source.builtIn = true;
        out.sources.push_back(source);
    }

    std::ifstream stream(filePath, std::ios::binary);
    if (!stream.is_open()) return true;

    std::ostringstream buffer;
    buffer << stream.rdbuf();

    RegistryConfig saved;
    if (!parseRegistryConfig(buffer.str(), saved)) {
        out.errors = saved.errors;
        return false;
    }
    for (const RegistrySource& source : saved.sources) {
        if (source.builtIn) continue;
        bool shadowed = false;
        for (const RegistrySource& existing : out.sources) {
            if (existing.name == source.name) shadowed = true;
        }
        if (!shadowed) out.sources.push_back(source);
    }
    out.path = filePath;
    return true;
}

bool quantum::plugin::RegistryConfig::save(std::string& errorOut) const {
    if (path.empty()) {
        errorOut = "no registries.json path";
        return false;
    }

    json root = json::object();
    json list = json::array();
    for (const RegistrySource& source : sources) {
        if (source.builtIn) continue;
        json node;
        node["name"] = source.name;
        node["url"] = source.url;
        node["scopes"] = source.scopes;
        list.push_back(node);
    }
    root["registries"] = list;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        errorOut = "cannot write " + path;
        return false;
    }
    stream << root.dump(2) << "\n";
    return stream.good();
}

bool quantum::plugin::addRegistrySource(RegistryConfig& config, const std::string& name,
                                       const std::string& url, const std::string& scopesCsv,
                                       std::string& errorOut) {
    if (name.empty()) {
        errorOut = "a registry needs a name";
        return false;
    }
    if (url.empty()) {
        errorOut = "a registry needs a url or directory";
        return false;
    }
    for (const RegistrySource& source : config.sources) {
        if (source.name == name) {
            errorOut = "registry \"" + name + "\" already exists";
            return false;
        }
    }

    RegistrySource added;
    added.name = name;
    added.url = url;
    std::stringstream stream(scopesCsv);
    std::string scope;
    while (std::getline(stream, scope, ',')) {
        const size_t first = scope.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        const size_t last = scope.find_last_not_of(" \t");
        added.scopes.push_back(scope.substr(first, last - first + 1));
    }
    config.sources.push_back(added);
    return true;
}

bool quantum::plugin::removeRegistrySource(RegistryConfig& config, const std::string& name,
                                          std::string& errorOut) {
    for (size_t i = 0; i < config.sources.size(); i++) {
        if (config.sources[i].name != name) continue;
        if (config.sources[i].builtIn) {
            errorOut = "\"built-in\" is the registry that ships with the engine and cannot be removed";
            return false;
        }
        config.sources.erase(config.sources.begin() + i);
        return true;
    }
    errorOut = "no registry named \"" + name + "\"";
    return false;
}

const RegistrySource* quantum::plugin::matchRegistryFor(const RegistryConfig& config,
                                                       const std::string& id) {
    const RegistrySource* best = nullptr;
    size_t bestLength = 0;
    for (const RegistrySource& source : config.sources) {
        for (const std::string& scope : source.scopes) {
            if (id == scope || (id.size() > scope.size() &&
                                id.compare(0, scope.size(), scope) == 0 &&
                                id[scope.size()] == '.')) {
                if (scope.size() > bestLength) {
                    bestLength = scope.size();
                    best = &source;
                }
            }
        }
    }
    if (best) return best;

    // Nothing claimed the id by scope, so the first unscoped registry answers - the built-in last,
    // because a user-added registry is the one they expect to be tried first.
    for (const RegistrySource& source : config.sources) {
        if (!source.scopes.empty()) continue;
        if (source.builtIn) continue;
        return &source;
    }
    for (const RegistrySource& source : config.sources) {
        if (!source.scopes.empty()) continue;
        return &source;
    }
    return nullptr;
}

std::string quantum::plugin::builtinCatalogRoot(const std::string& pluginsDirectory) {
    if (pluginsDirectory.empty()) return std::string();
    std::error_code ec;
    const std::filesystem::path candidate =
        std::filesystem::path(pluginsDirectory) / "catalog" / "index.json";
    if (std::filesystem::exists(candidate, ec)) {
        return candidate.parent_path().generic_string();
    }
    return std::string();
}
