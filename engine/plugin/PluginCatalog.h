// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_CATALOG_H
#define QUANTUM_PLUGIN_CATALOG_H

#include "PluginManifest.h"

#include <string>
#include <vector>

namespace quantum {
namespace plugin {

    // One release of one package. This is what a download needs to know: which bytes, from where,
    // and what the engine has to satisfy before they can be loaded.
    struct CatalogVersion {
        std::string version;
        // Resolved against the registry root, so a local registry stays a plain folder and a remote
        // one stays a URL - neither the parser nor the caller decides which.
        std::string file;
        std::string sha256;
        unsigned long long size = 0;
        std::string minEngineVersion;
        std::vector<PluginDependency> dependencies;
        std::vector<std::string> permissions;
        std::string description;
        std::string changelog;
        std::string docsUrl;
        std::string marketplaceUrl;
        std::string supportUrl;
        std::string icon;
        std::string preview;
        bool canContainContent = false;
        bool isBetaVersion = false;
    };

    // One package, in every version the registry publishes. `id` is the key a user types.
    struct CatalogEntry {
        std::string id;
        std::string name;
        std::string category;
        std::string author;
        std::string summary;
        std::vector<std::string> keywords;
        std::string latest;
        std::vector<CatalogVersion> versions;

        // Filled by the loader, not by the file.
        std::string registry;
        std::string root;

        // QUANTUM_API on the member, not the struct: the editor links this against quantum.dll, and a
        // definition that only lives in the DLL has to be exported to be callable from there.
        QUANTUM_API const CatalogVersion* findVersion(const std::string& text) const;
    };

    struct Catalog {
        std::string registry;
        std::string root;
        std::string generated;
        std::vector<CatalogEntry> entries;

        bool valid = false;
        std::vector<std::string> errors;
    };

    // A registry the package manager can query. `url` is either a directory (local, offline) or the
    // base of an HTTP server that serves index.json; the shape of both is the same file.
    struct RegistrySource {
        std::string name;
        std::string url;
        // Package-id prefixes this registry answers for, Unity-style. An empty scope list means the
        // registry is a fallback tried for anything.
        std::vector<std::string> scopes;
        // The built-in registry that ships with the engine; it is never removed by `catalog remove`.
        bool builtIn = false;
    };

    struct RegistryConfig {
        std::vector<RegistrySource> sources;
        std::string path;
        std::vector<std::string> errors;

        QUANTUM_API bool save(std::string& errorOut) const;
    };

    // "latest", an exact version or an empty request all mean "the newest listed one".
    QUANTUM_API const CatalogEntry* findCatalogEntry(const Catalog& catalog, const std::string& id);
    QUANTUM_API const CatalogVersion* resolveCatalogVersion(const Catalog& catalog,
                                                            const std::string& idWithOptionalAt,
                                                            const CatalogEntry** entryOut = nullptr);

    struct CatalogCategory {
        std::string name;
        int count = 0;
    };
    QUANTUM_API std::vector<CatalogCategory> catalogCategories(const Catalog& catalog);

    // Case-insensitive over id, name, summary, author and keywords; an empty category keeps all.
    // Results come back newest-category-first so the window's list matches the tree's order.
    QUANTUM_API std::vector<const CatalogEntry*> searchCatalog(const Catalog& catalog,
                                                               const std::string& text,
                                                               const std::string& category);

    QUANTUM_API bool parsePluginCatalog(const std::string& jsonText, const std::string& registry,
                                       const std::string& root, Catalog& out);
    QUANTUM_API bool loadPluginCatalog(const std::string& indexPath, const std::string& registry,
                                      Catalog& out);

    QUANTUM_API bool parseRegistryConfig(const std::string& jsonText, RegistryConfig& out);
    // An absent file is not an error: the caller gets the built-in registry only.
    QUANTUM_API bool loadRegistryConfig(const std::string& filePath, RegistryConfig& out);

    QUANTUM_API bool addRegistrySource(RegistryConfig& config, const std::string& name,
                                      const std::string& url, const std::string& scopesCsv,
                                      std::string& errorOut);
    QUANTUM_API bool removeRegistrySource(RegistryConfig& config, const std::string& name,
                                         std::string& errorOut);

    // The closest matching scope wins, exactly like Unity's scoped registries: a source scoped to
    // "com.example.tools" beats one scoped to "com.example". Sources with no scope are tried last,
    // in the order listed. Returns nullptr when no source claims the id.
    QUANTUM_API const RegistrySource* matchRegistryFor(const RegistryConfig& config,
                                                      const std::string& id);

    // The directory of the registry that ships in the repository, so an offline install works on a
    // fresh clone. Returns an empty string when it is not there.
    QUANTUM_API std::string builtinCatalogRoot(const std::string& pluginsDirectory);

}
}

#endif //QUANTUM_PLUGIN_CATALOG_H
