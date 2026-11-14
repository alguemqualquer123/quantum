// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace quantum::editor {

    namespace fs = std::filesystem;

    struct AssetRecord {
        std::string path;
        std::string kind;
        uint64_t size = 0;
        int64_t mtimeRaw = 0;
        std::string sha1;
        std::map<std::string, std::string> metadata;
        std::vector<std::string> dependencies;
        std::vector<std::string> missingDependencies;
        std::string error;

        bool broken() const {
            return !error.empty() || !missingDependencies.empty();
        }
    };

    struct AssetDatabaseReport {
        bool changed = false;
        size_t scanned = 0;
        size_t hashed = 0;
        size_t reused = 0;
        size_t removed = 0;
        size_t broken = 0;
        std::string manifestSha1;
    };

    // Persistent, project-local asset inventory. The JSON is derived data and is
    // intentionally content-addressed: paths and hashes are sorted, while scan
    // timestamps are kept out of the file so identical inputs produce identical output.
    class AssetDatabase {
    public:
        explicit AssetDatabase(fs::path projectDir);

        static fs::path databasePath(const fs::path& projectDir);

        // Scans assets, lua, scenes and scripts. Existing records with the same size
        // and mtime keep their SHA-1 and dependency list; dependency existence is
        // rechecked every scan so a newly restored file clears a broken warning.
        bool update(bool force, AssetDatabaseReport& report, std::string& error);

        // Reads the persisted database without touching the project tree.
        bool load(std::string& error);

        const fs::path& projectDir() const { return projectDir_; }
        const std::string& manifestSha1() const { return manifestSha1_; }
        const std::map<std::string, AssetRecord>& records() const { return records_; }

    private:
        fs::path projectDir_;
        fs::path databasePath_;
        std::map<std::string, AssetRecord> records_;
        std::string manifestSha1_;

        bool readExisting(std::map<std::string, AssetRecord>& out, std::string& error) const;
        bool write(std::string& error) const;
    };

}
