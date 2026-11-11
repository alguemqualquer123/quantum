// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "PackageManager.h"

#include "PackageZip.h"
#include "PluginManager.h"
#include "util/Checksum.h"

#include "ai/HttpClient.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace editor;
using namespace quantum::plugin;

namespace{

    bool readZipText(const std::string& zipPath, const std::string& entryName,
                     std::string& textOut, std::string& error){
        std::vector<unsigned char> bytes;
        if (!PackageZip::readEntry(zipPath, entryName, bytes, error)) return false;
        textOut.assign(bytes.begin(), bytes.end());
        return true;
    }

    // plugin.json is read out of the archive for the same reason the install path checks it before
    // promoting: a package whose manifest does not parse must never reach the plugins directory.
    bool readManifestFromZip(const std::string& zipPath, PluginManifest& out, std::string& error){
        std::vector<ZipEntry> entries;
        std::string zipError;
        if (!PackageZip::list(zipPath, entries, zipError)){
            error = zipError;
            return false;
        }

        const std::string base = PackageZip::baseFolderOf(zipPath, zipError);
        std::string manifestEntry;
        for (const ZipEntry& entry : entries){
            if (entry.directory) continue;
            const std::string& name = entry.name;
            const bool isManifest = name.size() >= 11 &&
                                    name.compare(name.size() - 11, 11, "plugin.json") == 0;
            if (!isManifest) continue;
            // Inside the declared folder, or at the root of a hand-made archive; anything deeper is a
            // nested plugin and not the one being installed.
            const bool underBase = !base.empty() &&
                                   name.compare(0, base.size(), base) == 0;
            std::string rest = underBase ? name.substr(base.size()) : name;
            while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\'))
                rest.erase(rest.begin());
            if (rest.find('/') != std::string::npos) continue;
            if (rest != "plugin.json") continue;
            manifestEntry = name;
            break;
        }

        if (manifestEntry.empty()){
            error = "no plugin.json at the root of " + zipPath;
            return false;
        }

        std::string text;
        if (!readZipText(zipPath, manifestEntry, text, error)) return false;
        if (!parsePluginManifest(text, out)){
            error = "plugin.json is not usable: " + out.errorsAsText();
            return false;
        }
        return true;
    }

    bool isRemote(const std::string& location){
        return location.compare(0, 7, "http://") == 0 ||
               location.compare(0, 8, "https://") == 0;
    }

    std::string trimmed(const std::string& text){
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) begin++;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
        return text.substr(begin, end - begin);
    }

    std::string lower(const std::string& text){
        std::string out = text;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return out;
    }

    std::string joinUrl(const std::string& base, const std::string& relative){
        if (relative.empty()) return base;
        if (isRemote(relative)) return relative;
        std::string out = base;
        while (!out.empty() && out.back() == '/') out.pop_back();
        if (relative.front() == '/') return out + relative;
        return out + "/" + relative;
    }

    std::string joinPath(const std::string& base, const std::string& relative){
        std::error_code ec;
        std::filesystem::path out = std::filesystem::path(base) / relative;
        (void)ec;
        return out.generic_string();
    }

    // A registry entry can point anywhere on the machine or at any server; the file name alone would
    // collide between two registries, so the caller passes the identity it resolved.
    std::string cacheNameFor(const std::string& tag, const std::string& location){
        std::string base = std::filesystem::path(location).filename().generic_string();
        if (base.empty()) base = "package.zip";
        std::string clean = tag;
        for (char& c : clean){
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'))
                c = '_';
        }
        return clean.empty() ? base : clean + "-" + base;
    }

    bool insideDirectory(const std::string& candidate, const std::string& parent){
        std::error_code ec;
        const std::filesystem::path a = std::filesystem::weakly_canonical(candidate, ec);
        const std::filesystem::path b = std::filesystem::weakly_canonical(parent, ec);
        if (ec) return false;
        const std::string left = a.generic_string();
        const std::string right = b.generic_string();
        if (left.size() <= right.size()) return false;
        return left.compare(0, right.size(), right) == 0 &&
               (left[right.size()] == '/' || left[right.size()] == '\\');
    }

    std::string fileSizeText(unsigned long long bytes){
        std::ostringstream out;
        if (bytes >= 1024ull * 1024ull) out << (bytes / (1024ull * 1024ull)) << " MiB";
        else if (bytes >= 1024ull) out << (bytes / 1024ull) << " KiB";
        else out << bytes << " B";
        return out.str();
    }

}

PackageManager::PackageManager(const std::string& pluginsDirectory)
    : pluginsDir(pluginsDirectory){
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(pluginsDir), ec);
}

std::string PackageManager::lockPath() const{
    return joinPath(pluginsDir, "packages.lock.json");
}

std::string PackageManager::registriesPath() const{
    return joinPath(pluginsDir, "registries.json");
}

std::string PackageManager::cacheDirectory() const{
    return joinPath(pluginsDir, ".cache");
}

std::string PackageManager::stagingDirectory() const{
    return joinPath(pluginsDir, ".staging");
}

bool PackageManager::lock(PluginLock& out, std::string& error){
    return out.load(lockPath(), error);
}

bool PackageManager::saveLock(const PluginLock& lockFile, std::string& error){
    return lockFile.save(error);
}

bool PackageManager::loadRegistries(RegistryConfig& out, std::string& error){
    error.clear();
    out.path = registriesPath();
    if (!loadRegistryConfig(registriesPath(), out)){
        error = "cannot read " + registriesPath() + ": ";
        for (size_t i = 0; i < out.errors.size(); i++){
            if (i) error += "; ";
            error += out.errors[i];
        }
        return false;
    }
    return true;
}

bool PackageManager::addRegistry(const std::string& name, const std::string& url,
                                 const std::string& scopesCsv, std::string& error){
    RegistryConfig config;
    if (!loadRegistries(config, error)) return false;
    if (!addRegistrySource(config, name, url, scopesCsv, error)) return false;
    return config.save(error);
}

bool PackageManager::removeRegistry(const std::string& name, std::string& error){
    RegistryConfig config;
    if (!loadRegistries(config, error)) return false;
    if (!removeRegistrySource(config, name, error)) return false;
    return config.save(error);
}

bool PackageManager::loadCatalogFrom(const RegistrySource& source, Catalog& out,
                                     std::vector<std::string>& notes){
    out.registry = source.name;
    out.root = source.url;
    out.valid = false;

    const std::string location = joinUrl(source.url, "index.json");
    if (isRemote(location)){
        quantum::editor::ai::HttpClient http;
        const quantum::editor::ai::HttpResponse response = http.get(location);
        if (!response.error.empty()){
            notes.push_back(source.name + ": " + response.error);
            return false;
        }
        if (response.status < 200 || response.status >= 300){
            notes.push_back(source.name + ": index.json answered " + std::to_string(response.status));
            return false;
        }
        // The root stays the URL, so a relative "file" in the index resolves against the server.
        if (!parsePluginCatalog(response.body, source.name, source.url, out)){
            for (const std::string& problem : out.errors)
                notes.push_back(source.name + ": " + problem);
            return false;
        }
        return true;
    }

    if (!loadPluginCatalog(location, source.name, out)){
        for (const std::string& problem : out.errors) notes.push_back(source.name + ": " + problem);
        return false;
    }
    return true;
}

bool PackageManager::loadCatalog(Catalog& out, std::vector<std::string>& notes){
    out = Catalog();
    out.registry = "merged";

    RegistryConfig config;
    std::string error;
    if (!loadRegistries(config, error)){
        notes.push_back(error);
        return false;
    }
    if (config.sources.empty()){
        notes.push_back("no registry configured and no built-in catalog at " +
                        joinPath(pluginsDir, "catalog/index.json"));
        return false;
    }

    bool anyValid = false;
    for (const RegistrySource& source : config.sources){
        Catalog one;
        if (!loadCatalogFrom(source, one, notes)) continue;
        if (!one.valid) continue;
        anyValid = true;
        if (out.generated.empty()) out.generated = one.generated;
        for (const CatalogEntry& entry : one.entries){
            bool repeated = false;
            for (const CatalogEntry& known : out.entries){
                if (known.id == entry.id){
                    notes.push_back(entry.id + ": also listed by " + source.name +
                                    ", keeping the first registry that claimed it");
                    repeated = true;
                    break;
                }
            }
            if (!repeated) out.entries.push_back(entry);
        }
    }

    // Entries are listed by id so search output is stable across runs and machines.
    std::sort(out.entries.begin(), out.entries.end(),
              [](const CatalogEntry& a, const CatalogEntry& b){ return a.id < b.id; });

    out.valid = anyValid;
    if (!anyValid) notes.push_back("no registry returned a usable catalog");
    return anyValid;
}

bool PackageManager::fetchToCache(const std::string& location, const std::string& sha256,
                                  const std::string& cacheTag,
                                  std::string& localFileOut, std::vector<std::string>& steps,
                                  std::string& error){
    error.clear();
    localFileOut.clear();

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(cacheDirectory()), ec);
    if (ec){
        error = "cannot create " + cacheDirectory() + ": " + ec.message();
        return false;
    }

    if (!isRemote(location)){
        // A local registry is a plain folder: no copy, no download, the bytes are already here.
        if (!std::filesystem::is_regular_file(std::filesystem::path(location), ec)){
            error = "package file not found: " + location;
            return false;
        }
        localFileOut = location;
        steps.push_back("source: local file " + location);
        return true;
    }

    const std::string cached = joinPath(cacheDirectory(), cacheNameFor(cacheTag, location));

    std::string existing;
    if (std::filesystem::is_regular_file(std::filesystem::path(cached), ec))
        existing = quantum::sha256HexOfFile(cached, error);
    if (!sha256.empty() && !existing.empty() && lower(existing) == lower(sha256)){
        localFileOut = cached;
        steps.push_back("source: cache hit " + cached);
        return true;
    }

    quantum::editor::ai::HttpClient http;
    const quantum::editor::ai::HttpResponse response =
        http.downloadToFile(location, std::filesystem::path(cached));
    if (!response.error.empty()){
        error = "download of " + location + " failed: " + response.error;
        return false;
    }
    if (response.status < 200 || response.status >= 300){
        error = "download of " + location + " answered " + std::to_string(response.status);
        return false;
    }
    if (!std::filesystem::is_regular_file(std::filesystem::path(cached), ec)){
        error = "download reported success but left no file at " + cached;
        return false;
    }
    steps.push_back("download: " + location + " -> " + cached);
    error.clear();
    localFileOut = cached;
    return true;
}

bool PackageManager::verifyHash(const std::string& file, const std::string& expectedSha256,
                                std::vector<std::string>& steps, std::string& error){
    error.clear();
    if (expectedSha256.empty()){
        steps.push_back("hash: none listed in the registry, integrity not verified");
        return true;
    }
    std::string readError;
    const std::string actual = quantum::sha256HexOfFile(file, readError);
    if (actual.empty()){
        error = readError.empty() ? "cannot hash " + file : readError;
        return false;
    }
    if (lower(actual) != lower(expectedSha256)){
        error = "sha256 mismatch for " + file + ": expected " + lower(expectedSha256) +
                ", got " + lower(actual);
        return false;
    }
    steps.push_back("hash: sha256 " + lower(actual) + " matches the registry");
    return true;
}

PackageInstallReport PackageManager::promote(const std::string& zipPath, const std::string& id,
                                             const std::string& version,
                                             const std::string& registry,
                                             const std::string& packageFile,
                                             const std::string& sha256, bool overwrite,
                                             std::vector<std::string>& steps){
    PackageInstallReport report;
    report.id = id;
    report.version = version;
    report.registry = registry;

    std::string zipError;
    const std::string base = PackageZip::baseFolderOf(zipPath, zipError);
    const std::string stageRoot = stagingDirectory();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(stageRoot), ec);

    // A package built here always has one folder at its top, so extracting it into the staging root
    // lands the folder where it is expected. An archive with plugin.json at the root gets extracted
    // into a folder named after the id, so the installed directory still has a sane name.
    const std::string namedFolder = joinPath(stageRoot, id.empty() ? std::string("package") : id);
    const std::string stageTarget = base.empty() ? namedFolder : stageRoot;
    const std::string sourceFolder = base.empty() ? namedFolder : joinPath(stageRoot, base);

    std::filesystem::remove_all(std::filesystem::path(stageRoot), ec);
    if (ec){
        report.error = "cannot clear " + stageRoot + ": " + ec.message();
        return report;
    }
    if (!base.empty()) std::filesystem::create_directories(std::filesystem::path(stageTarget), ec);

    if (!PackageZip::extract(zipPath, stageTarget, zipError)){
        report.error = "extract failed: " + zipError;
        return report;
    }
    if (!std::filesystem::is_directory(std::filesystem::path(sourceFolder), ec)){
        report.error = "the package does not contain the folder it declares (" + sourceFolder + ")";
        return report;
    }
    steps.push_back("extract: " + sourceFolder);

    const std::string folderName = std::filesystem::path(sourceFolder).filename().generic_string();
    const std::string target = joinPath(pluginsDir, folderName);

    // Loaded before anything moves, because the previous record is what says which folder this id
    // already owns.
    PluginLock lockFile;
    std::string lockError;
    const bool haveLock = lockFile.load(lockPath(), lockError);
    const InstalledPackage* previous = haveLock ? lockFile.find(id) : nullptr;
    std::string previousFolder;
    if (previous && !previous->directory.empty()){
        previousFolder = std::filesystem::path(previous->directory).is_absolute()
            ? previous->directory
            : joinPath(pluginsDir, previous->directory);
    }

    if (std::filesystem::exists(std::filesystem::path(target), ec)){
        if (!overwrite){
            report.error = target + " already exists; use --overwrite or uninstall first";
            return report;
        }
        // A DLL mapped by the running editor cannot be deleted, and the copy would half-succeed.
        std::filesystem::remove_all(std::filesystem::path(target), ec);
        if (ec){
            report.error = "cannot replace " + target + ": " + ec.message() +
                           " (a plugin binary is probably in use - close the editor and retry)";
            return report;
        }
        steps.push_back("promote: replaced " + target);
    }
    else if (!previousFolder.empty() && previousFolder != target && !overwrite){
        report.error = "\"" + id + "\" is installed in " + previousFolder + "; the package offers it "
                       "as " + folderName + " - use --overwrite to move it";
        return report;
    }

    // Discovery keeps the first manifest it reads for an id and warns about the rest, so a package
    // that renames its top folder between releases would leave the *old* install answering for the id.
    // Only the folder this lock recorded is ever removed here.
    if (!previousFolder.empty() && previousFolder != target){
        std::error_code staleEc;
        if (!insideDirectory(previousFolder, pluginsDir)){
            report.error = "refusing to remove " + previousFolder + ": outside the plugins directory";
            return report;
        }
        if (std::filesystem::exists(std::filesystem::path(previousFolder), staleEc)){
            std::filesystem::remove_all(std::filesystem::path(previousFolder), staleEc);
            if (staleEc){
                report.error = "cannot remove the previous install " + previousFolder + ": " +
                               staleEc.message();
                return report;
            }
            steps.push_back("promote: " + previousFolder + " removed, this package installs as " +
                            folderName);
        }
    }

    PluginManager manager;
    manager.setPluginsDirectory(pluginsDir);
    std::string installError;
    const std::string installedId = manager.installFromDirectory(sourceFolder, true, installError);
    if (installedId.empty()){
        report.error = installError;
        return report;
    }
    steps.push_back("promote: installed as \"" + installedId + "\" in " + target);

    std::filesystem::remove_all(std::filesystem::path(sourceFolder), ec);

    if (!haveLock){
        report.error = "installed, but the lock file is unreadable and must be fixed before the "
                       "next install: " + lockError;
        report.directory = target;
        return report;
    }

    unsigned long long size = 0;
    const auto bytes = std::filesystem::file_size(std::filesystem::path(zipPath), ec);
    if (!ec) size = static_cast<unsigned long long>(bytes);

    InstalledPackage record;
    record.id = installedId;
    record.version = version;
    record.registry = registry;
    record.packageFile = packageFile;
    record.sha256 = lower(sha256);
    record.size = size;
    record.directory = folderName;
    record.installedAt = PluginLock::formatTimestamp();
    // The name and the version shown in the browser come from the manifest the engine just parsed,
    // so a catalog that forgot to list one cannot make the installed entry disagree with the folder.
    const PluginRecord* discovered = manager.find(installedId);
    if (discovered){
        record.name = discovered->manifest.name.empty() ? installedId
                                                        : discovered->manifest.name;
        if (record.version.empty()) record.version = discovered->manifest.version;
    }
    lockFile.put(record);
    if (!lockFile.save(lockError)){
        report.error = "installed, but packages.lock.json could not be written: " + lockError;
        report.directory = target;
        return report;
    }
    steps.push_back("lock: " + lockPath());

    report.ok = true;
    report.id = installedId;
    report.version = record.version;
    report.directory = target;
    return report;
}

PackageInstallReport PackageManager::installFromZip(const std::string& zipPath,
                                                    const std::string& expectedSha256,
                                                    bool overwrite){
    PackageInstallReport report;
    report.steps.push_back("package: " + zipPath);

    std::vector<std::string> findings;
    std::string verifyError;
    if (!verifyPackage(zipPath, expectedSha256, findings, verifyError)){
        report.error = verifyError;
        report.steps = findings;
        return report;
    }
    report.steps.insert(report.steps.end(), findings.begin(), findings.end());

    // The id and version come from the manifest inside, not from the file name.
    PluginManifest manifest;
    std::string manifestError;
    if (!readManifestFromZip(zipPath, manifest, manifestError)){
        report.error = manifestError;
        return report;
    }

    // A local install has no registry to quote a hash from, so the bytes themselves become the
    // recorded one: `outdated` and a later re-verify then still have something to compare against.
    std::string recorded = expectedSha256;
    if (recorded.empty()){
        std::string hashError;
        const std::string actual = quantum::sha256HexOfFile(zipPath, hashError);
        if (!actual.empty()) recorded = actual;
    }

    return promote(zipPath, manifest.id, manifest.version, "file", zipPath,
                   recorded, overwrite, report.steps);
}

PackageInstallReport PackageManager::install(const std::string& idWithOptionalVersion,
                                             bool overwrite){
    PackageInstallReport report;

    Catalog catalog;
    std::vector<std::string> notes;
    if (!loadCatalog(catalog, notes)){
        report.error = "no catalog available";
        for (const std::string& note : notes) report.steps.push_back("catalog: " + note);
        return report;
    }

    const CatalogEntry* entry = nullptr;
    const CatalogVersion* version = resolveCatalogVersion(catalog, idWithOptionalVersion, &entry);
    if (!version || !entry){
        report.error = "\"" + idWithOptionalVersion + "\" is not in any configured registry";
        return report;
    }

    report.id = entry->id;
    report.version = version->version;
    report.registry = entry->registry;
    report.steps.push_back("catalog: " + entry->id + " " + version->version + " from " +
                           entry->registry);

    std::string file;
    std::string fetchError;
    if (!fetchToCache(joinUrl(entry->root, version->file), version->sha256,
                      entry->registry + "-" + version->version + "-" + entry->id,
                      file, report.steps, fetchError)){
        report.error = fetchError;
        return report;
    }

    std::vector<std::string> findings;
    std::string verifyError;
    if (!verifyPackage(file, version->sha256, findings, verifyError)){
        report.error = verifyError;
        report.steps.insert(report.steps.end(), findings.begin(), findings.end());
        return report;
    }
    report.steps.insert(report.steps.end(), findings.begin(), findings.end());

    return promote(file, entry->id, version->version, entry->registry, version->file,
                   version->sha256, overwrite, report.steps);
}

bool PackageManager::uninstall(const std::string& id, std::string& error,
                               std::vector<std::string>* notes){
    error.clear();
    PluginLock lockFile;
    if (!lockFile.load(lockPath(), error)){
        error = "cannot read " + lockPath() + ": " + error;
        return false;
    }
    const InstalledPackage* record = lockFile.find(id);
    if (!record){
        error = "\"" + id + "\" is not in " + lockPath() + "; nothing removed";
        return false;
    }

    const std::string directory = std::filesystem::path(record->directory).is_absolute()
        ? record->directory
        : joinPath(pluginsDir, record->directory);

    // The lock is engine-written, but a hand-edited one must not be able to point the remover at
    // the project or at a sibling folder.
    if (!insideDirectory(directory, pluginsDir)){
        error = "refusing to remove " + directory + ": outside the plugins directory";
        return false;
    }

    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(directory), ec);
    if (ec){
        error = "cannot remove " + directory + ": " + ec.message() +
                " (a plugin binary is probably in use - close the editor and retry)";
        return false;
    }

    if (notes && !record->contentTargets.empty()){
        // Content copied into a project is not deleted here: the user may have edited it. The list
        // is reported so the removal can be done by hand, which is what Unity's editor does too.
        std::ostringstream leftover;
        leftover << "left " << record->contentTargets.size()
                 << " project file(s) in place:";
        for (const std::string& target : record->contentTargets) leftover << " " << target;
        notes->push_back(leftover.str());
    }

    lockFile.remove(id);
    std::string saveError;
    if (!lockFile.save(saveError)){
        error = "the folder is gone but " + lockPath() + " could not be written: " + saveError;
        return false;
    }
    return true;
}

std::vector<PackageUpdateRow> PackageManager::outdated(std::vector<std::string>& notes){
    std::vector<PackageUpdateRow> rows;

    PluginLock lockFile;
    std::string error;
    if (!lockFile.load(lockPath(), error)){
        notes.push_back("lock: " + error);
        return rows;
    }
    if (lockFile.packages().empty()) return rows;

    Catalog catalog;
    if (!loadCatalog(catalog, notes)){
        notes.push_back("catalog: cannot list what is new until a registry answers");
        return rows;
    }

    for (const InstalledPackage& record : lockFile.packages()){
        const CatalogEntry* entry = findCatalogEntry(catalog, record.id);
        if (!entry){
            notes.push_back(record.id + ": installed but listed by no registry");
            continue;
        }
        const CatalogVersion* latest = entry->findVersion(entry->latest);
        if (!latest){
            notes.push_back(entry->id + ": registry does not list its own latest version " +
                            entry->latest);
            continue;
        }

        const Version installedVersion = parseVersion(record.version);
        const Version latestVersion = parseVersion(latest->version);
        if (!installedVersion.parsed || !latestVersion.parsed){
            notes.push_back(entry->id + ": version \"" + record.version + "\" or \"" +
                            latest->version + "\" is not a number, so it cannot be compared");
            continue;
        }
        if (compareVersions(installedVersion, latestVersion) >= 0) continue;

        PackageUpdateRow row;
        row.id = record.id;
        row.name = record.name;
        row.installed = record.version;
        row.latest = latest->version;
        row.registry = entry->registry;
        rows.push_back(row);
    }
    return rows;
}

PackageInstallReport PackageManager::update(const std::string& id){
    PackageInstallReport report;
    PluginLock lockFile;
    std::string error;
    if (!lockFile.load(lockPath(), error)){
        report.error = "cannot read " + lockPath() + ": " + error;
        return report;
    }
    const InstalledPackage* record = lockFile.find(id);
    if (!record){
        report.error = "\"" + id + "\" is not installed; install it first";
        return report;
    }

    Catalog catalog;
    std::vector<std::string> notes;
    if (!loadCatalog(catalog, notes)){
        report.error = "no catalog available";
        for (const std::string& note : notes) report.steps.push_back("catalog: " + note);
        return report;
    }
    const CatalogEntry* entry = findCatalogEntry(catalog, id);
    if (!entry){
        report.error = id + " is installed but listed by no configured registry";
        return report;
    }
    if (lower(entry->latest) == lower(record->version)){
        report.ok = true;
        report.id = id;
        report.version = record->version;
        report.directory = joinPath(pluginsDir, record->directory);
        report.steps.push_back("already at " + record->version);
        return report;
    }

    return install(id + "@" + entry->latest, true);
}

bool PackageManager::packageFolder(const std::string& sourceDirectory,
                                   const std::string& zipPath, std::string& error,
                                   std::vector<std::string>* skipped){
    error.clear();
    const std::string manifestPath = joinPath(sourceDirectory, "plugin.json");
    PluginManifest manifest;
    if (!loadPluginManifest(manifestPath, manifest)){
        error = "no usable plugin.json in " + sourceDirectory + ": " + manifest.errorsAsText();
        return false;
    }

    std::vector<std::string> dropped;
    if (!PackageZip::create(zipPath, sourceDirectory, error, &dropped)) return false;
    if (skipped) *skipped = dropped;

    std::vector<std::string> findings;
    std::string verifyError;
    if (!verifyPackage(zipPath, std::string(), findings, verifyError)){
        error = verifyError + " (the archive was written but is not usable)";
        return false;
    }
    return true;
}

bool PackageManager::verifyPackage(const std::string& zipPath, const std::string& expectedSha256,
                                   std::vector<std::string>& findings, std::string& error){
    findings.clear();
    error.clear();

    std::string zipError;
    if (!PackageZip::isReadable(zipPath, zipError)){
        error = zipError;
        return false;
    }

    std::string hashError;
    const std::string sha = quantum::sha256HexOfFile(zipPath, hashError);
    if (sha.empty()){
        error = hashError;
        return false;
    }
    findings.push_back("package: " + zipPath + " sha256 " + lower(sha));

    if (!expectedSha256.empty() && lower(sha) != lower(expectedSha256)){
        error = "sha256 mismatch: expected " + lower(expectedSha256) + ", got " + lower(sha);
        return false;
    }

    std::vector<ZipEntry> entries;
    if (!PackageZip::list(zipPath, entries, zipError)){
        error = zipError;
        return false;
    }

    std::error_code ec;
    const std::uintmax_t bytes =
        std::filesystem::file_size(std::filesystem::path(zipPath), ec);
    unsigned long long files = 0;
    for (const ZipEntry& entry : entries){
        if (!entry.directory) files++;
    }
    findings.push_back("entries: " + std::to_string(entries.size()) + " (" +
                       std::to_string(files) + " files, " +
                       fileSizeText(static_cast<unsigned long long>(ec ? 0 : bytes)) + ")");

    PluginManifest manifest;
    std::string manifestError;
    if (!readManifestFromZip(zipPath, manifest, manifestError)){
        error = manifestError;
        return false;
    }
    findings.push_back("plugin.json: id \"" + manifest.id + "\" version " + manifest.version +
                       " abi " + std::to_string(manifest.abiVersion));
    if (!manifest.libraries.empty())
        findings.push_back("library: " + manifest.libraryFileName());
    if (!manifest.errors.empty())
        findings.push_back("warnings: " + manifest.errorsAsText());
    return true;
}
