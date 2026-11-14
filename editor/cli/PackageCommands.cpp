// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "PackageCommands.h"

#include "PluginManager.h"
#include "util/PackageManager.h"
#include "util/PackageZip.h"
#include "util/Checksum.h"

#include "json.hpp"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace quantum;
using namespace quantum::plugin;
// The package manager and the ZIP reader are editor-side, so they live in the global `editor`
// namespace - which `using namespace quantum` has just made ambiguous with `quantum::editor`.
using namespace ::editor;

namespace quantum::editor::cli {

namespace {

    void usage(const char* executableName) {
        std::cout
            << "usage: " << (executableName ? executableName : "quantum-editor-cmd")
            << " packages <verb> [args] [--plugins-dir DIR]\n"
            << "\n"
            << "  catalog list                    the registries this machine queries\n"
            << "  catalog add <name> <url> [s]    add one; <s> is a comma separated scope list\n"
            << "  catalog remove <name>           drop one (the built-in registry cannot go)\n"
            << "  catalog refresh                 read every index.json again and report the count\n"
            << "  search <text> [--category C]    case-insensitive over id, name, summary, author,\n"
            << "                                  keywords; \"--category All\" skips the filter\n"
            << "  install <id>[@ver]              download, verify, extract, promote, lock\n"
            << "  install-file <zip>              the same, from a package on disk\n"
            << "  uninstall <id>                  remove the folder the lock names\n"
            << "  outdated                        installed versus the registry's latest\n"
            << "  update [<id>]                   one package, or every outdated one\n"
            << "  installed                       what packages.lock.json holds\n"
            << "  package <dir> --out <zip>       build a package from a plugin folder\n"
            << "  verify <zip> [--sha256 HEX]     read it back: structure, hash, plugin.json\n"
            << "  selftest                        the whole path on a temporary directory\n"
            << "\n"
            << "A package is a ZIP with one folder inside it and that folder's plugin.json at its\n"
            << "root. --plugins-dir defaults to $QUANTUM_PLUGINS_DIR, then ./plugins; the lock and the\n"
            << "registries live inside it, and the engine's runtime loader only ever reads the folders\n"
            << "this tool promotes, never a registry. --overwrite replaces an installed folder.\n";
    }

    std::string pad(const std::string& text, size_t width) {
        std::string out = text;
        while (out.size() < width) out += ' ';
        return out;
    }

    struct Options {
        std::vector<std::string> positional;
        std::string pluginsDir;
        std::string out;
        std::string category;
        std::string sha256;
        bool overwrite = false;
        bool wantsHelp = false;
        std::string problem;
    };

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
            if (argument == "--plugins-dir" || argument == "--out" ||
                argument == "--category" || argument == "--sha256") {
                if (i + 1 >= argc) {
                    options.problem = argument + " needs a value";
                    return options;
                }
                const std::string value(argv[++i]);
                if (argument == "--plugins-dir") options.pluginsDir = value;
                else if (argument == "--out") options.out = value;
                else if (argument == "--category") options.category = value;
                else options.sha256 = value;
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

    std::string defaultPluginsDirectory() {
        std::error_code ec;
        const std::string fromEnvironment = getenv("QUANTUM_PLUGINS_DIR")
            ? std::string(getenv("QUANTUM_PLUGINS_DIR"))
            : std::string();
        if (!fromEnvironment.empty()) return fromEnvironment;
        const std::string cwd = std::filesystem::current_path(ec).generic_string();
        return cwd.empty() ? std::string("plugins") : cwd + "/plugins";
    }

    PackageManager thePackages(const Options& options) {
        return PackageManager(options.pluginsDir.empty() ? defaultPluginsDirectory()
                                                         : options.pluginsDir);
    }

    void printReport(const PackageInstallReport& report) {
        for (const std::string& step : report.steps) std::cout << "  " << step << "\n";
        if (!report.ok) {
            std::cout << "PACKAGES FAILED " << report.error << "\n";
            return;
        }
        std::cout << "installed " << report.id << " " << report.version << " -> "
                  << report.directory << "\n";
        std::cout << "  the engine picks it up on the next start (a plugin binary already mapped "
                     "cannot be replaced in this process)\n";
    }

    int catalogCommand(PackageManager& packages, const Options& options) {
        const std::string action = options.positional.size() > 1 ? options.positional[1] : "";
        RegistryConfig config;
        std::string error;

        if (action == "add") {
            if (options.positional.size() < 4) {
                std::cout << "packages catalog add: a name and a url are required\n";
                return 1;
            }
            const std::string scopes = options.positional.size() > 4 ? options.positional[4] : "";
            if (!packages.addRegistry(options.positional[2], options.positional[3], scopes, error)) {
                std::cout << "packages catalog add: " << error << "\n";
                return 1;
            }
            std::cout << "registry " << options.positional[2] << " -> " << options.positional[3]
                      << "\n  written to " << packages.registriesPath() << "\n";
            return 0;
        }

        if (action == "remove") {
            if (options.positional.size() < 3) {
                std::cout << "packages catalog remove: a registry name is required\n";
                return 1;
            }
            if (!packages.removeRegistry(options.positional[2], error)) {
                std::cout << "packages catalog remove: " << error << "\n";
                return 1;
            }
            std::cout << "registry " << options.positional[2] << " removed from "
                      << packages.registriesPath() << "\n";
            return 0;
        }

        if (!packages.loadRegistries(config, error)) {
            std::cout << "packages catalog: " << error << "\n";
            return 1;
        }

        if (action == "refresh" || action == "list") {
            std::cout << "registries: " << packages.registriesPath() << "\n";
            for (const RegistrySource& source : config.sources) {
                std::cout << "  " << pad(source.name, 18) << source.url;
                if (source.builtIn) std::cout << "  (built-in)";
                if (!source.scopes.empty()) {
                    std::cout << "  scopes";
                    for (const std::string& scope : source.scopes) std::cout << " " << scope;
                }
                std::cout << "\n";
            }
            if (action == "list") return 0;

            Catalog catalog;
            std::vector<std::string> notes;
            const bool loaded = packages.loadCatalog(catalog, notes);
            for (const std::string& note : notes) std::cout << "  note " << note << "\n";
            std::cout << "catalog: " << catalog.entries.size() << " package(s) from "
                      << config.sources.size() << " registry(ies)\n";
            if (!loaded) return 1;
            return 0;
        }

        std::cout << "packages catalog: unknown action '" << action << "'\n";
        return 1;
    }

    void printEntry(const CatalogEntry& entry) {
        std::cout << "  " << pad(entry.id, 28) << pad(entry.name, 24)
                  << pad(entry.latest, 10) << "[" << entry.category << "] "
                  << entry.summary << "\n";
        for (const CatalogVersion& version : entry.versions) {
            std::cout << "      " << pad(version.version, 10) << version.file;
            if (version.sha256.empty()) std::cout << "  (no sha256)";
            else std::cout << "  sha256 " << version.sha256.substr(0, 16) << "...";
            if (version.size) std::cout << "  " << version.size << " B";
            if (version.isBetaVersion) std::cout << "  beta";
            if (!version.minEngineVersion.empty())
                std::cout << "  engine >= " << version.minEngineVersion;
            std::cout << "\n";
        }
    }

    int searchCommand(PackageManager& packages, const Options& options) {
        Catalog catalog;
        std::vector<std::string> notes;
        if (!packages.loadCatalog(catalog, notes)) {
            for (const std::string& note : notes) std::cout << "  note " << note << "\n";
            std::cout << "packages search: no usable catalog\n";
            return 1;
        }
        for (const std::string& note : notes) std::cout << "  note " << note << "\n";

        const std::string text = options.positional.size() > 1 ? options.positional[1] : "";
        const std::vector<const CatalogEntry*> found =
            searchCatalog(catalog, text, options.category);

        PluginLock lockFile;
        std::string lockError;
        lockFile.load(packages.lockPath(), lockError);

        std::cout << "catalog: " << catalog.entries.size() << " package(s), "
                  << found.size() << " match";
        if (!text.empty()) std::cout << " \"" << text << "\"";
        if (!options.category.empty()) std::cout << " in [" << options.category << "]";
        std::cout << "\n";

        for (const CatalogEntry* entry : found) {
            printEntry(*entry);
            const InstalledPackage* record = lockFile.find(entry->id);
            std::cout << "      " << (record ? ("installed " + record->version)
                                             : std::string("not installed")) << "\n";
        }
        return 0;
    }

    int installCommand(PackageManager& packages, const Options& options, bool fromFile) {
        if (options.positional.size() < 2) {
            std::cout << (fromFile ? "packages install-file: a package path is required\n"
                                   : "packages install: a package id is required\n");
            return 1;
        }
        const PackageInstallReport report = fromFile
            ? packages.installFromZip(options.positional[1], options.sha256, options.overwrite)
            : packages.install(options.positional[1], options.overwrite);
        printReport(report);
        return report.ok ? 0 : 1;
    }

    int uninstallCommand(PackageManager& packages, const Options& options) {
        if (options.positional.size() < 2) {
            std::cout << "packages uninstall: a package id is required\n";
            return 1;
        }
        std::vector<std::string> notes;
        std::string error;
        if (!packages.uninstall(options.positional[1], error, &notes)) {
            std::cout << "packages uninstall: " << error << "\n";
            return 1;
        }
        std::cout << options.positional[1] << " removed\n";
        for (const std::string& note : notes) std::cout << "  note " << note << "\n";
        return 0;
    }

    int installedCommand(PackageManager& packages) {
        PluginLock lockFile;
        std::string error;
        if (!lockFile.load(packages.lockPath(), error)) {
            std::cout << "packages installed: " << error << "\n";
            return 1;
        }
        std::cout << "lock: " << packages.lockPath() << "\n";
        for (const InstalledPackage& record : lockFile.packages()) {
            std::cout << "  " << pad(record.id, 28) << pad(record.version, 10)
                      << pad(record.directory, 24) << record.registry;
            if (!record.sha256.empty())
                std::cout << "  sha256 " << record.sha256.substr(0, 16) << "...";
            if (!record.installedAt.empty()) std::cout << "  " << record.installedAt;
            std::cout << "\n";
        }
        std::cout << "  " << lockFile.packages().size() << " installed\n";
        return 0;
    }

    int outdatedCommand(PackageManager& packages) {
        std::vector<std::string> notes;
        const std::vector<PackageUpdateRow> rows = packages.outdated(notes);
        for (const std::string& note : notes) std::cout << "  note " << note << "\n";
        if (rows.empty()) {
            std::cout << "packages outdated: nothing to update\n";
            return 0;
        }
        for (const PackageUpdateRow& row : rows) {
            std::cout << "  " << pad(row.id, 28) << pad(row.installed, 10) << "-> "
                      << pad(row.latest, 10) << row.registry << "\n";
        }
        std::cout << rows.size() << " package(s) behind\n";
        return 0;
    }

    int updateCommand(PackageManager& packages, const Options& options) {
        if (options.positional.size() > 1) {
            const PackageInstallReport report = packages.update(options.positional[1]);
            printReport(report);
            return report.ok ? 0 : 1;
        }

        std::vector<std::string> notes;
        const std::vector<PackageUpdateRow> rows = packages.outdated(notes);
        for (const std::string& note : notes) std::cout << "  note " << note << "\n";
        if (rows.empty()) {
            std::cout << "packages update: nothing to update\n";
            return 0;
        }
        int failures = 0;
        for (const PackageUpdateRow& row : rows) {
            std::cout << row.id << ": " << row.installed << " -> " << row.latest << "\n";
            const PackageInstallReport report = packages.update(row.id);
            for (const std::string& step : report.steps) std::cout << "  " << step << "\n";
            if (!report.ok) {
                std::cout << "  FAILED " << report.error << "\n";
                failures++;
            }
        }
        return failures ? 1 : 0;
    }

    int packageCommand(PackageManager& packages, const Options& options) {
        if (options.positional.size() < 2 || options.out.empty()) {
            std::cout << "packages package: a plugin folder and --out <zip> are required\n";
            return 1;
        }
        std::string error;
        std::vector<std::string> skipped;
        if (!packages.packageFolder(options.positional[1], options.out, error, &skipped)) {
            std::cout << "packages package: " << error << "\n";
            return 1;
        }
        std::vector<std::string> findings;
        std::string verifyError;
        packages.verifyPackage(options.out, std::string(), findings, verifyError);
        std::cout << "wrote " << options.out << "\n";
        for (const std::string& finding : findings) std::cout << "  " << finding << "\n";
        if (!skipped.empty()) {
            // Reported, not hidden: the archive is smaller than the folder it came from, and the
            // reason has to be visible.
            std::cout << "  skipped " << skipped.size() << " build artifact(s):";
            size_t shown = 0;
            for (const std::string& name : skipped) {
                if (shown++ == 8) {
                    std::cout << " ...";
                    break;
                }
                std::cout << " " << name;
            }
            std::cout << "\n";
        }
        return 0;
    }

    int verifyCommand(PackageManager& packages, const Options& options) {
        if (options.positional.size() < 2) {
            std::cout << "packages verify: a package path is required\n";
            return 1;
        }
        std::vector<std::string> findings;
        std::string error;
        const bool ok = packages.verifyPackage(options.positional[1], options.sha256, findings,
                                               error);
        for (const std::string& finding : findings) std::cout << "  " << finding << "\n";
        if (!ok) {
            std::cout << "VERIFY FAILED " << error << "\n";
            return 1;
        }
        std::cout << "OK " << options.positional[1] << "\n";
        return 0;
    }

    // ---------------------------------------------------------------- selftest

    // The whole marketplace path on a temporary directory: write a plugin folder, package it, build
    // a registry that lists it, install by id from that registry, check the lock, then uninstall.
    // Every step is a line of output, because the point of the probe is to say which half broke.
    struct Probe {
        int checks = 0;
        int failures = 0;
        std::string root;

        void check(const std::string& name, bool ok, const std::string& detail) {
            checks++;
            if (!ok) failures++;
            std::cout << (ok ? "PASS " : "FAIL ") << pad(name, 40) << detail << "\n";
        }

        std::string path(const std::string& relative) const {
            return root + "/" + relative;
        }

        bool write(const std::string& relative, const std::string& text) const {
            std::error_code ec;
            const std::filesystem::path file = std::filesystem::path(path(relative));
            std::filesystem::create_directories(file.parent_path(), ec);
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return false;
            out << text;
            return bool(out);
        }

        bool exists(const std::string& relative) const {
            std::error_code ec;
            return std::filesystem::exists(std::filesystem::path(path(relative)), ec);
        }
    };

    std::string probeRoot() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec) return "quantum-package-probe";
        return (base / "quantum-package-probe").generic_string();
    }

    // A manifest with no library is installable - the loader refuses to map it later, which is a
    // different question from what the package manager does. `selftest` is about bytes and locks.
    const char* PROBE_ID = "com.quantum.probe";

    std::string probeManifest(const std::string& version) {
        std::ostringstream json;
        json << "{\n"
             << "  \"id\": \"" << PROBE_ID << "\",\n"
             << "  \"name\": \"Probe\",\n"
             << "  \"version\": \"" << version << "\",\n"
             << "  \"abiVersion\": " << QUANTUM_PLUGIN_ABI_VERSION << ",\n"
             << "  \"libraries\": [\"quantum-probe\"],\n"
             << "  \"permissions\": [\"log\"],\n"
             << "  \"category\": \"Testing\",\n"
             << "  \"summary\": \"written by packages selftest\",\n"
             << "  \"keywords\": [\"probe\", \"marketplace\"]\n"
             << "}\n";
        return json.str();
    }

    // Writes a copy of `zipPath` whose central-directory size field is 12 bytes too large - exactly
    // the mistake this writer made before it was measured. A reader that compensates for it accepts an
    // archive every other tool rejects, so the package manager has to reject it too.
    bool damageCentralDirectorySize(const std::string& zipPath, const std::string& outPath,
                                    std::string& error) {
        std::ifstream source(zipPath, std::ios::binary);
        if (!source.is_open()) {
            error = "cannot read " + zipPath;
            return false;
        }
        std::vector<unsigned char> raw((std::istreambuf_iterator<char>(source)),
                                      std::istreambuf_iterator<char>());
        if (raw.size() < 22) {
            error = "the package is too small to hold an end record";
            return false;
        }
        size_t end = 0;
        bool found = false;
        for (size_t i = raw.size() - 22 + 1; i-- > 0;) {
            if (raw[i] == 0x50 && raw[i + 1] == 0x4b && raw[i + 2] == 0x05 && raw[i + 3] == 0x06) {
                end = i;
                found = true;
                break;
            }
        }
        if (!found) {
            error = "no ZIP end record in the package just written";
            return false;
        }
        const size_t at = end + 12;                       // size of the central directory, little endian
        if (at + 4 > raw.size()) {
            error = "the end record runs past the end of the file";
            return false;
        }
        const unsigned int size = raw[at] | (raw[at + 1] << 8) | (raw[at + 2] << 16) |
                                  (static_cast<unsigned int>(raw[at + 3]) << 24);
        raw[at] = static_cast<unsigned char>((size + 12) & 0xFFu);
        raw[at + 1] = static_cast<unsigned char>(((size + 12) >> 8) & 0xFFu);
        raw[at + 2] = static_cast<unsigned char>(((size + 12) >> 16) & 0xFFu);
        raw[at + 3] = static_cast<unsigned char>(((size + 12) >> 24) & 0xFFu);

        std::ofstream target(std::filesystem::path(outPath), std::ios::binary | std::ios::trunc);
        if (!target.is_open()) {
            error = "cannot write " + outPath;
            return false;
        }
        target.write(reinterpret_cast<const char*>(raw.data()),
                     static_cast<std::streamsize>(raw.size()));
        return bool(target);
    }

    int selftestCommand(const Options& options) {        Probe probe;
        probe.root = probeRoot();
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(probe.root), ec);
        std::filesystem::create_directories(std::filesystem::path(probe.root), ec);

        const std::string pluginsDir = probe.path("plugins");
        PackageManager packages(pluginsDir);

        // 1. a plugin folder on disk
        const std::string sourceFolder = probe.path("build/ProbePlugin");
        const bool wroteManifest = probe.write("build/ProbePlugin/plugin.json", probeManifest("1.0.0"));
        probe.write("build/ProbePlugin/data/note.txt", "written by the probe\n");
        // What CMake leaves beside a plugin's own files in a build tree: never package content.
        probe.write("build/ProbePlugin/CMakeFiles/probe-target.vcxproj", "<JunkProject/>\n");
        probe.write("build/ProbePlugin/probe-target.dir/Debug/probe.obj", "junk");
        probe.check("fixture plugin folder", wroteManifest && probe.exists("build/ProbePlugin/plugin.json"),
                    sourceFolder);

        // 2. package it
        const std::string zipPath = probe.path("out/probe-1.0.0.dplugin");
        std::string error;
        std::vector<std::string> skipped;
        bool ok = packages.packageFolder(sourceFolder, zipPath, error, &skipped);
        probe.check("packageFolder writes a readable zip", ok, ok ? zipPath : error);
        probe.check("build scaffolding is left out of the package", skipped.size() == 2,
                    skipped.empty() ? "nothing skipped" : skipped.front() + " + " +
                        std::to_string(skipped.size()) + " skipped");

        std::string hashError;
        const std::string sha = quantum::sha256HexOfFile(zipPath, hashError);
        probe.check("package has a sha256", !sha.empty(), sha.empty() ? hashError : sha);

        std::vector<ZipEntry> entries;
        std::string zipError;
        const bool listed = PackageZip::list(zipPath, entries, zipError);
        bool hasManifestEntry = false;
        bool hasDataEntry = false;
        for (const ZipEntry& entry : entries) {
            if (entry.name.find("plugin.json") != std::string::npos) hasManifestEntry = true;
            if (entry.name.find("note.txt") != std::string::npos) hasDataEntry = true;
        }
        probe.check("zip lists plugin.json and the data file",
                    listed && hasManifestEntry && hasDataEntry,
                    std::to_string(entries.size()) + " entries");

        // 3. verify reports it
        std::vector<std::string> findings;
        ok = packages.verifyPackage(zipPath, sha, findings, error);
        probe.check("verifyPackage accepts the right hash", ok, error);

        ok = packages.verifyPackage(zipPath, std::string(64, '0'), findings, error);
        probe.check("verifyPackage refuses a wrong hash", !ok, error);

        // 3b. the archive is only good if a strict reader agrees: the size field of the central
        // directory has to describe the bytes actually written.
        const std::string brokenZip = probe.path("out/broken.dplugin");
        std::string damageError;
        const bool damaged = damageCentralDirectorySize(zipPath, brokenZip, damageError);
        std::string brokenError;
        const bool brokenReads = damaged && PackageZip::isReadable(brokenZip, brokenError);
        probe.check("a package with a lying index is refused", damaged && !brokenReads,
                    damaged ? (brokenReads ? "it still opened" : brokenError) : damageError);

        // 4. install straight from the file: no registry involved
        PackageInstallReport report = packages.installFromZip(zipPath, sha, false);
        probe.check("install-file promotes the folder",
                    report.ok && probe.exists("plugins/ProbePlugin/plugin.json"),
                    report.ok ? report.directory : report.error);

        PluginLock lockFile;
        std::string lockError;
        ok = lockFile.load(packages.lockPath(), lockError);
        const InstalledPackage* record = ok ? lockFile.find(PROBE_ID) : nullptr;
        probe.check("lock records the install", record != nullptr,
                    record ? (record->directory + " " + record->version + " " + record->sha256.substr(0, 8))
                           : lockError);

        // 5. a second install without --overwrite has to refuse, not double-install
        report = packages.installFromZip(zipPath, sha, false);
        probe.check("reinstall without --overwrite refuses", !report.ok, report.error);
        report = packages.installFromZip(zipPath, sha, true);
        probe.check("reinstall with --overwrite works", report.ok, report.ok ? report.directory : report.error);

        // 6. a registry directory with an index.json, installed by id
        const std::string registryDir = probe.path("registry");
        unsigned long long zipBytes = 0;
        std::error_code sizeEc;
        const auto zipSize = std::filesystem::file_size(std::filesystem::path(zipPath), sizeEc);
        if (!sizeEc) zipBytes = static_cast<unsigned long long>(zipSize);

        std::ostringstream index;
        index << "{\n"
              << "  \"generated\": \"packages selftest\",\n"
              << "  \"packages\": [\n"
              << "    {\n"
              << "      \"id\": \"" << PROBE_ID << "\",\n"
              << "      \"name\": \"Probe\",\n"
              << "      \"category\": \"Testing\",\n"
              << "      \"summary\": \"the probe package\",\n"
              << "      \"keywords\": [\"probe\"],\n"
              << "      \"latest\": \"1.0.0\",\n"
              << "      \"versions\": [\n"
              << "        {\n"
              << "          \"version\": \"1.0.0\",\n"
              << "          \"file\": \"out/probe-1.0.0.dplugin\",\n"
              << "          \"sha256\": \"" << sha << "\",\n"
              << "          \"size\": " << zipBytes << ",\n"
              << "          \"description\": \"first release\"\n"
              << "        }\n"
              << "      ]\n"
              << "    }\n"
              << "  ]\n"
              << "}\n";
        probe.write("registry/index.json", index.str());
        std::filesystem::create_directories(std::filesystem::path(registryDir + "/out"), ec);
        std::filesystem::copy_file(std::filesystem::path(zipPath),
                                   std::filesystem::path(registryDir + "/out/probe-1.0.0.dplugin"),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        // The index resolves "file" against its own directory, so the registry copy needs the
        // package under <registry>/out.
        std::ostringstream registries;
        registries << "{\n  \"registries\": [\n    { \"name\": \"probe\", \"url\": \""
                   << registryDir << "\", \"scopes\": [\"com.quantum\"] }\n  ]\n}\n";
        probe.write("plugins/registries.json", registries.str());

        Catalog catalog;
        std::vector<std::string> notes;
        ok = packages.loadCatalog(catalog, notes);
        const CatalogEntry* entry = ok ? findCatalogEntry(catalog, PROBE_ID) : nullptr;
        std::string catalogDetail = entry ? (std::to_string(catalog.entries.size()) + " package(s)")
                                          : (notes.empty() ? std::string("no catalog")
                                                           : notes.front());
        if (!entry) catalogDetail += " / " + error;
        probe.check("catalog loads the registry", entry != nullptr, catalogDetail);

        const CatalogVersion* version = resolveCatalogVersion(catalog, std::string(PROBE_ID) + "@1.0.0",
                                                              nullptr);
        probe.check("resolveCatalogVersion finds 1.0.0", version != nullptr,
                    version ? version->file : "not listed");
        RegistryConfig probedConfig;
        std::string configError;
        packages.loadRegistries(probedConfig, configError);
        const RegistrySource* claimed = matchRegistryFor(probedConfig, PROBE_ID);
        probe.check("scope match picks the probe registry",
                    claimed != nullptr && claimed->name == "probe",
                    claimed ? claimed->url : configError);

        // The installed copy is gone first, so this proves the registry path did the work.
        std::string uninstallError;
        packages.uninstall(PROBE_ID, uninstallError);
        report = packages.install(PROBE_ID, true);
        probe.check("install by id from the registry",
                    report.ok && probe.exists("plugins/ProbePlugin/plugin.json"),
                    report.ok ? report.directory : report.error);

        // 7. outdated and update: publish 1.1.0, keep 1.0.0 installed
        const std::string sourceFolderNext = probe.path("build/ProbePluginNext");
        probe.write("build/ProbePluginNext/plugin.json", probeManifest("1.1.0"));
        const std::string nextZip = probe.path("out/probe-1.1.0.dplugin");
        ok = packages.packageFolder(sourceFolderNext, nextZip, error);
        std::string nextSha = quantum::sha256HexOfFile(nextZip, hashError);
        std::filesystem::copy_file(std::filesystem::path(nextZip),
                                   std::filesystem::path(registryDir + "/out/probe-1.1.0.dplugin"),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        std::ostringstream indexNext;
        indexNext << "{\n"
                  << "  \"generated\": \"packages selftest 1.1.0\",\n"
                  << "  \"packages\": [\n"
                  << "    {\n"
                  << "      \"id\": \"" << PROBE_ID << "\",\n"
                  << "      \"name\": \"Probe\",\n"
                  << "      \"category\": \"Testing\",\n"
                  << "      \"latest\": \"1.1.0\",\n"
                  << "      \"versions\": [\n"
                  << "        { \"version\": \"1.0.0\", \"file\": \"out/probe-1.0.0.dplugin\", \"sha256\": \""
                  << sha << "\" },\n"
                  << "        { \"version\": \"1.1.0\", \"file\": \"out/probe-1.1.0.dplugin\", \"sha256\": \""
                  << nextSha << "\" }\n"
                  << "      ]\n"
                  << "    }\n"
                  << "  ]\n}\n";
        probe.write("registry/index.json", indexNext.str());

        std::vector<std::string> notes2;
        const std::vector<PackageUpdateRow> rows = packages.outdated(notes2);
        probe.check("outdated reports the new version", rows.size() == 1 && rows[0].latest == "1.1.0",
                    rows.empty() ? "nothing behind" : (rows[0].installed + " -> " + rows[0].latest));

        report = packages.update(PROBE_ID);
        probe.check("update installs 1.1.0", report.ok && report.version == "1.1.0",
                    report.ok ? report.version : report.error);

        // Discovery keeps the first manifest it reads for an id and warns about the rest, so an
        // update whose package folder is named differently than the install it replaces must not
        // leave two directories behind - the older one would keep answering for the id.
        int installedFolders = 0;
        std::string installedNames;
        std::error_code listEc;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(std::filesystem::path(pluginsDir), listEc)) {
            std::error_code entryEc;
            if (!entry.is_directory(entryEc)) continue;
            const std::string name = entry.path().filename().generic_string();
            if (name == ".cache" || name == ".staging") continue;
            installedFolders++;
            installedNames += " " + name;
        }
        probe.check("one folder per id after an update", installedFolders == 1,
                    installedNames.empty() ? "no folder installed" : installedNames);

        // 8. uninstall removes the folder and the lock entry
        ok = packages.uninstall(PROBE_ID, uninstallError);
        probe.check("uninstall removes the folder",
                    ok && !probe.exists("plugins/ProbePlugin/plugin.json"), uninstallError);

        PluginLock afterRemoval;
        lockError.clear();
        ok = afterRemoval.load(packages.lockPath(), lockError);
        probe.check("lock no longer lists it",
                    ok && afterRemoval.find(PROBE_ID) == nullptr &&
                        afterRemoval.packages().empty(),
                    std::to_string(afterRemoval.packages().size()) + " left");

        // 9. a tampered package must not install: one byte changed, hash now disagrees
        std::error_code lengthEc;
        const std::uintmax_t zipLength =
            std::filesystem::file_size(std::filesystem::path(zipPath), lengthEc);
        if (!lengthEc && zipLength > 40) {
            std::ofstream tamper(std::filesystem::path(zipPath),
                                 std::ios::binary | std::ios::in | std::ios::out);
            if (tamper.is_open()) {
                tamper.seekp(static_cast<std::streamoff>(zipLength) - 40);
                const char junk = 0x5A;
                tamper.write(&junk, 1);
            }
        }
        report = packages.installFromZip(zipPath, sha, true);
        probe.check("a package that does not match its hash is refused", !report.ok, report.error);

        std::filesystem::remove_all(std::filesystem::path(probe.root), ec);
        std::cout << probe.checks << " checks, " << probe.failures << " failed\n";
        return probe.failures == 0 ? 0 : 1;
    }

}

int runPackagesCommand(int argc, char** argv, const char* executableName) {
    const Options options = readOptions(argc, argv);
    if (!options.problem.empty()) {
        std::cout << "packages: " << options.problem << "\n";
        usage(executableName);
        return 1;
    }
    if (options.wantsHelp || options.positional.empty()) {
        usage(executableName);
        return options.wantsHelp ? 0 : 1;
    }

    const std::string action = options.positional[0];
    if (action == "selftest") return selftestCommand(options);

    PackageManager packages = thePackages(options);

    if (action == "catalog") return catalogCommand(packages, options);
    if (action == "search") return searchCommand(packages, options);
    if (action == "install") return installCommand(packages, options, false);
    if (action == "install-file") return installCommand(packages, options, true);
    if (action == "uninstall") return uninstallCommand(packages, options);
    if (action == "installed" || action == "lock") return installedCommand(packages);
    if (action == "outdated") return outdatedCommand(packages);
    if (action == "update") return updateCommand(packages, options);
    if (action == "package") return packageCommand(packages, options);
    if (action == "verify") return verifyCommand(packages, options);

    std::cout << "packages: unknown action '" << action << "'\n";
    usage(executableName);
    return 1;
}

}
