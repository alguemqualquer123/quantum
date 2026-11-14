// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT
//
// GPU-free tests of the marketplace data layer: the hashes a package is verified by, the registry
// index a catalog is read from, the scoped-registry resolution, and the lock file that records what
// is installed. The ZIP itself is editor-side and is probed by `quantum-editor-cmd packages selftest`.

#include "doctest.h"

#include "PluginCatalog.h"
#include "PluginLock.h"
#include "PluginVersion.h"
#include "util/Checksum.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace quantum;
using namespace quantum::plugin;

namespace {

    const char* VALID_INDEX = R"json({
      "generated": "fixture",
      "packages": [
        {
          "id": "com.acme.tools",
          "name": "Acme Tools",
          "category": "Editor",
          "author": "Acme",
          "summary": "tools for the editor",
          "keywords": ["acme", "editor"],
          "versions": [
            { "version": "1.0.0", "file": "acme-tools-1.0.0.dplugin",
              "sha256": "1111111111111111111111111111111111111111111111111111111111111111",
              "size": 1000 },
            { "version": "1.2.0", "file": "acme-tools-1.2.0.dplugin",
              "sha256": "2222222222222222222222222222222222222222222222222222222222222222",
              "minEngineVersion": "0.9.0", "dependencies": [{"id": "com.acme.base", "minVersion": "2.0.0"}],
              "permissions": ["log", "tick"], "canContainContent": true }
          ]
        },
        {
          "id": "com.acme.empty",
          "name": "No category",
          "versions": [ { "version": "0.1.0", "file": "empty.dplugin" } ]
        }
      ]
    })json";

    std::string tempDirectory(const std::string& name) {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        REQUIRE_FALSE(ec);
        const std::filesystem::path dir = base / ("quantum-catalog-test-" + name);
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        REQUIRE_FALSE(ec);
        return dir.generic_string();
    }

    void writeFile(const std::string& path, const std::string& text) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out.is_open());
        out << text;
    }

}

TEST_CASE("Checksum: the hashes a package is verified by") {
    CHECK(sha256Hex(std::string()) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex(std::string("abc")) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // The 56-byte FIPS vector: the padding has to start a second block here.
    CHECK(sha256Hex(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // Exactly one block: no room for the length field in it, so a second block is written.
    CHECK(sha256Hex(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq01234567")) ==
          "9e4117732130ab9dc766d1184ecb8dccf82bc6a42a12378a0fd067b95d04ebff");
    // Multi-block streaming, the shape a real package file has.
    CHECK(sha256Hex(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    CHECK(crc32Of(nullptr, 0) == 0u);
    const std::string abc = "abc";
    CHECK(crc32Of(reinterpret_cast<const unsigned char*>(abc.data()), abc.size()) == 0x352441c2u);
}

TEST_CASE("Checksum: a file hashes the same as its bytes") {
    const std::string dir = tempDirectory("hash");
    const std::string file = dir + "/payload.bin";

    std::string payload;
    for (int i = 0; i < 5000; i++) payload += "quantum package payload " + std::to_string(i) + "\n";
    writeFile(file, payload);

    std::string error;
    CHECK(sha256HexOfFile(file, error) == sha256Hex(payload));
    CHECK(error.empty());

    error.clear();
    CHECK(sha256HexOfFile(dir + "/absent.bin", error).empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Catalog: parsing a registry index") {
    Catalog catalog;
    REQUIRE(parsePluginCatalog(VALID_INDEX, "fixture", "C:/packages", catalog));
    CHECK(catalog.valid);
    CHECK(catalog.registry == "fixture");
    CHECK(catalog.root == "C:/packages");
    REQUIRE(catalog.entries.size() == 2);

    const CatalogEntry* tools = findCatalogEntry(catalog, "com.acme.tools");
    REQUIRE(tools != nullptr);
    CHECK(tools->category == "Editor");
    CHECK(tools->latest == "1.2.0");   // not listed in the file: taken as the newest version
    CHECK(tools->registry == "fixture");
    CHECK(tools->root == "C:/packages");
    REQUIRE(tools->versions.size() == 2);

    const CatalogVersion* version = tools->findVersion("1.2.0");
    REQUIRE(version != nullptr);
    CHECK(version->file == "acme-tools-1.2.0.dplugin");
    CHECK(version->minEngineVersion == "0.9.0");
    CHECK(version->canContainContent);
    CHECK_FALSE(version->isBetaVersion);
    REQUIRE(version->dependencies.size() == 1);
    CHECK(version->dependencies[0].id == "com.acme.base");
    CHECK(version->dependencies[0].minVersion == "2.0.0");
    CHECK(version->permissions.size() == 2);

    // A version with no sha256 is legal: the catalog says so out loud rather than failing.
    const CatalogVersion* bare = tools->findVersion("1.0.0");
    REQUIRE(bare != nullptr);
    CHECK(bare->size == 1000ull);
    CHECK_FALSE(bare->sha256.empty());
    const CatalogEntry* empty = findCatalogEntry(catalog, "com.acme.empty");
    REQUIRE(empty != nullptr);
    CHECK(empty->versions[0].sha256.empty());

    Catalog broken;
    CHECK_FALSE(parsePluginCatalog("{not json", "fixture", "C:/", broken));
    CHECK_FALSE(broken.valid);
    CHECK_FALSE(broken.errors.empty());

    Catalog noVersions;
    CHECK_FALSE(parsePluginCatalog(R"json({"packages":[{"id":"a.b","versions":[]}]})json",
                                   "fixture", "C:/", noVersions));

    Catalog duplicated;
    CHECK_FALSE(parsePluginCatalog(
        R"json({"packages":[{"id":"a.b","versions":[{"version":"1","file":"x"}]},
                             {"id":"a.b","versions":[{"version":"1","file":"y"}]}]})json",
        "fixture", "C:/", duplicated));
}

TEST_CASE("Catalog: what a user types resolves to") {
    Catalog catalog;
    REQUIRE(parsePluginCatalog(VALID_INDEX, "fixture", "C:/packages", catalog));

    const CatalogEntry* entry = nullptr;
    CHECK(resolveCatalogVersion(catalog, "com.acme.tools", &entry) ==
          catalog.entries[0].versions.data() + 1);
    REQUIRE(entry != nullptr);
    CHECK(entry->id == "com.acme.tools");

    const CatalogVersion* exact = resolveCatalogVersion(catalog, "com.acme.tools@1.0.0", nullptr);
    REQUIRE(exact != nullptr);
    CHECK(exact->version == "1.0.0");

    CHECK(resolveCatalogVersion(catalog, "com.acme.tools@latest")->version == "1.2.0");
    CHECK(resolveCatalogVersion(catalog, "com.acme.tools@9.9.9") == nullptr);
    CHECK(resolveCatalogVersion(catalog, "com.nothere.here") == nullptr);
}

TEST_CASE("Catalog: categories and search") {
    Catalog catalog;
    REQUIRE(parsePluginCatalog(VALID_INDEX, "fixture", "C:/packages", catalog));

    const std::vector<CatalogCategory> categories = catalogCategories(catalog);
    REQUIRE(categories.size() == 2);
    // An entry with no category lands in the fallback bucket rather than an empty one.
    bool hasGeneral = false;
    for (const CatalogCategory& category : categories) {
        if (category.name == "General") hasGeneral = true;
    }
    CHECK(hasGeneral);

    CHECK(searchCatalog(catalog, "acme", "").size() == 2);
    CHECK(searchCatalog(catalog, "TOOLS", "").size() == 1);   // case-insensitive
    CHECK(searchCatalog(catalog, "", "Editor").size() == 1);  // category only
    CHECK(searchCatalog(catalog, "editor", "All").size() == 1);
    CHECK(searchCatalog(catalog, "nothing matches this", "").empty());
}

TEST_CASE("Catalog: the index says where its files hang") {
    const std::string dir = tempDirectory("root");
    writeFile(dir + "/index.json",
              R"json({"root":"C:/cdn/packages","packages":[{"id":"a.b","versions":[{"version":"1","file":"a.zip"}]}]})json");

    Catalog catalog;
    REQUIRE(loadPluginCatalog(dir + "/index.json", "remote", catalog));
    // A declared root wins over the directory the file was read from, so a registry can publish an
    // index locally and point its files somewhere else.
    CHECK(catalog.root == "C:/cdn/packages");
    CHECK(catalog.entries[0].root == "C:/cdn/packages");

    writeFile(dir + "/plain/index.json",
              R"json({"packages":[{"id":"a.b","versions":[{"version":"1","file":"a.zip"}]}]})json");
    Catalog local;
    REQUIRE(loadPluginCatalog(dir + "/plain/index.json", "local", local));
    CHECK(local.root == (dir + "/plain"));
}

TEST_CASE("Registries: scopes decide who answers") {
    RegistryConfig config;
    REQUIRE(parseRegistryConfig(R"json({"registries":[
        {"name":"all","url":"C:/all"},
        {"name":"broad","url":"C:/broad","scopes":["com.acme"]},
        {"name":"exact","url":"C:/exact","scopes":["com.acme.tools"]}
    ]})json", config));
    REQUIRE(config.sources.size() == 3);

    // The longest prefix wins, Unity's rule.
    CHECK(matchRegistryFor(config, "com.acme.tools") != nullptr);
    CHECK(matchRegistryFor(config, "com.acme.tools")->name == "exact");
    CHECK(matchRegistryFor(config, "com.acme.other")->name == "broad");
    // Unscoped sources are a fallback, tried only when no scope claims the id.
    CHECK(matchRegistryFor(config, "net.example.thing")->name == "all");

    std::string error;
    CHECK(addRegistrySource(config, "second", "C:/second", "com.other", error));
    CHECK(removeRegistrySource(config, "second", error));
    CHECK_FALSE(removeRegistrySource(config, "missing", error));
    CHECK_FALSE(error.empty());

    const std::string dir = tempDirectory("registries");
    config.path = dir + "/registries.json";
    REQUIRE(config.save(error));

    RegistryConfig reloaded;
    REQUIRE(loadRegistryConfig(config.path, reloaded));
    CHECK(reloaded.sources.size() == 3);
}

TEST_CASE("Registries: the built-in folder is always there and never removable") {
    const std::string dir = tempDirectory("builtin");
    writeFile(dir + "/catalog/index.json", R"json({"packages":[]})json");

    RegistryConfig config;
    REQUIRE(loadRegistryConfig(dir + "/registries.json", config));
    REQUIRE_FALSE(config.sources.empty());
    CHECK(config.sources.front().builtIn);
    CHECK(config.sources.front().name == "built-in");

    // An absent file is not an error, and a directory without a catalog leaves the list empty.
    RegistryConfig nothing;
    REQUIRE(loadRegistryConfig(dir + "/nowhere/registries.json", nothing));
    CHECK(nothing.sources.empty());

    std::string error;
    CHECK_FALSE(removeRegistrySource(config, "built-in", error));
    CHECK_FALSE(error.empty());
    CHECK(builtinCatalogRoot(dir) == dir + "/catalog");
    CHECK(builtinCatalogRoot(dir + "/absent").empty());
}

TEST_CASE("Lock: what an install records survives a round trip") {
    const std::string dir = tempDirectory("lock");
    const std::string path = dir + "/packages.lock.json";

    PluginLock fresh;
    std::string error;
    // Absent file: an empty lock, not a failure - nothing has been installed yet.
    REQUIRE(fresh.load(path, error));
    CHECK(fresh.packages().empty());

    InstalledPackage record;
    record.id = "com.acme.tools";
    record.name = "Acme Tools";
    record.version = "1.2.0";
    record.registry = "built-in";
    record.packageFile = "acme-tools-1.2.0.dplugin";
    record.sha256 = sha256Hex(std::string("package bytes"));
    record.size = 4096;
    record.directory = "acme-tools";
    record.installedAt = PluginLock::formatTimestamp();
    record.contentTargets.push_back("assets/textures/noise.png");
    fresh.put(record);
    // Putting the same id twice replaces, it does not duplicate: one folder per id.
    record.version = "1.2.1";
    fresh.put(record);
    REQUIRE(fresh.save(error));
    REQUIRE(error.empty());
    CHECK(fresh.packages().size() == 1);

    PluginLock reloaded;
    REQUIRE(reloaded.load(path, error));
    const InstalledPackage* stored = reloaded.find("com.acme.tools");
    REQUIRE(stored != nullptr);
    CHECK(stored->version == "1.2.1");
    CHECK(stored->sha256 == record.sha256);
    CHECK(stored->size == 4096ull);
    REQUIRE(stored->contentTargets.size() == 1);
    CHECK(stored->contentTargets[0] == "assets/textures/noise.png");
    CHECK_FALSE(stored->installedAt.empty());

    CHECK(reloaded.find("com.nothere") == nullptr);
    CHECK(reloaded.remove("com.acme.tools"));
    CHECK(reloaded.packages().empty());
    CHECK_FALSE(reloaded.remove("com.acme.tools"));

    // Malformed: a failure, because overwriting it would forget real installs.
    writeFile(path, "{ this is not json");
    PluginLock damaged;
    CHECK_FALSE(damaged.load(path, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Version: a commit hash is not a number and comparison says so") {
    const Version parsed = parseVersion("1.2.3");
    CHECK(parsed.parsed);
    CHECK(parsed.major == 1);
    CHECK(parsed.patch == 3);

    const Version commit = parseVersion("7d197c71");
    CHECK_FALSE(commit.parsed);

    CHECK(compareVersions(parseVersion("1.2.0"), parseVersion("1.10.0")) < 0);
    CHECK(versionToString(parseVersion("2.0.0")) == "2.0.0");
}
