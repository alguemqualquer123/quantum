// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef SAVEGAME_H
#define SAVEGAME_H

#include "Export.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace quantum {

// Versioned slot persistence for primitive gameplay state.
class QUANTUM_API SaveGame {
public:
    using MigrationFunction = bool (*)(std::unordered_map<std::string, std::string>& values);

    explicit SaveGame(const std::string& slot);

    // Registers a migration from one on-disk schema to the next schema.
    // Migrations are applied in order during load and never run concurrently
    // with a SaveGame instance's mutation.
    static bool registerMigration(uint32_t fromVersion, MigrationFunction migration);

    bool isValid() const;
    bool load();
    bool save() const;
    bool exists() const;
    bool deleteSlot();
    void clear();

    void setString(const std::string& key, const std::string& value);
    void setBlob(const std::string& key, const std::vector<uint8_t>& value);
    void setInt(const std::string& key, int64_t value);
    void setNumber(const std::string& key, double value);
    std::string getString(const std::string& key, const std::string& defaultValue = {}) const;
    bool getBlob(const std::string& key, std::vector<uint8_t>& value) const;
    int64_t getInt(const std::string& key, int64_t defaultValue = 0) const;
    double getNumber(const std::string& key, double defaultValue = 0.0) const;
    bool has(const std::string& key) const;
    void remove(const std::string& key);

private:
    std::string slot;
    std::unordered_map<std::string, std::string> values;

    std::string path() const;
    static bool validName(const std::string& value);
};

}

#endif // SAVEGAME_H
