// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "PluginCatalog.h"
#include "PluginLock.h"

#include <string>
#include <vector>

namespace editor{

    struct PackageInstallReport{
        bool ok = false;
        std::string id;
        std::string version;
        std::string registry;
        // The installed folder under the plugins directory, empty when nothing was promoted.
        std::string directory;
        // What happened, in order: download, hash, extract, promote, lock. The CLI prints these, and
        // they are the reason a failure is diagnosable without a debugger.
        std::vector<std::string> steps;
        std::string error;
    };

    struct PackageUpdateRow{
        std::string id;
        std::string name;
        std::string installed;
        std::string latest;
        std::string registry;
    };

    // The install side of the marketplace: registry config, catalog loading, download, verification,
    // extraction and the lock file.
    //
    // It runs at editor time only. The engine's runtime loader never asks a registry for anything -
    // it reads the folders this class leaves in the plugins directory - so a shipped game cannot
    // reach the network and an offline machine installs from the built-in catalog the same way.
    class PackageManager{
    public:
        explicit PackageManager(const std::string& pluginsDirectory);

        const std::string& pluginsDirectory() const { return pluginsDir; }
        std::string lockPath() const;
        std::string registriesPath() const;
        std::string cacheDirectory() const;
        std::string stagingDirectory() const;

        // The lock, loaded on first use. A malformed file is reported and the caller decides what to
        // do; nothing here silently rewrites it, because that would forget real installs.
        bool lock(quantum::plugin::PluginLock& out, std::string& error);
        bool saveLock(const quantum::plugin::PluginLock& lockFile, std::string& error);

        bool loadRegistries(quantum::plugin::RegistryConfig& out, std::string& error);
        bool addRegistry(const std::string& name, const std::string& url,
                         const std::string& scopesCsv, std::string& error);
        bool removeRegistry(const std::string& name, std::string& error);

        // Every configured source, merged. A source that cannot be reached is a note, not a failure:
        // an offline machine still gets the built-in catalog.
        bool loadCatalog(quantum::plugin::Catalog& out, std::vector<std::string>& notes);
        bool loadCatalogFrom(const quantum::plugin::RegistrySource& source,
                             quantum::plugin::Catalog& out, std::vector<std::string>& notes);

        // "id", "id@1.2.3" or "id@latest".
        PackageInstallReport install(const std::string& idWithOptionalVersion, bool overwrite);

        // A local .dplugin with no registry at all - what `package` produces and what the tests use.
        PackageInstallReport installFromZip(const std::string& zipPath,
                                            const std::string& expectedSha256, bool overwrite);

        // Removes the folder the lock names and its lock entry. Content that was copied into a
        // project is left alone and listed in `notes`, because the user may have edited it.
        bool uninstall(const std::string& id, std::string& error,
                       std::vector<std::string>* notes = nullptr);

        std::vector<PackageUpdateRow> outdated(std::vector<std::string>& notes);
        PackageInstallReport update(const std::string& id);

        // Build a package from a plugin folder, then check it reads back: a ZIP the engine cannot
        // open is worse than no package, because it gets uploaded to a registry and downloaded
        // forever. Build scaffolding is left out and listed in `skipped` when the caller wants it.
        bool packageFolder(const std::string& sourceDirectory, const std::string& zipPath,
                           std::string& error, std::vector<std::string>* skipped = nullptr);

        // Structure, one plugin.json, and the hash when the caller knows it.
        bool verifyPackage(const std::string& zipPath, const std::string& expectedSha256,
                           std::vector<std::string>& findings, std::string& error);

        const std::string& lastError() const { return lastErrorText; }

    private:
        bool fetchToCache(const std::string& location, const std::string& sha256,
                          const std::string& cacheTag, std::string& localFileOut,
                          std::vector<std::string>& steps, std::string& error);
        bool verifyHash(const std::string& file, const std::string& expectedSha256,
                        std::vector<std::string>& steps, std::string& error);

        // Extract into the staging area and hand the folder to the engine's own installer, so one
        // implementation decides what an installed plugin folder looks like.
        PackageInstallReport promote(const std::string& zipPath, const std::string& id,
                                     const std::string& version, const std::string& registry,
                                     const std::string& packageFile, const std::string& sha256,
                                     bool overwrite, std::vector<std::string>& steps);

        std::string pluginsDir;
        std::string lastErrorText;
    };

}
