// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_SETTINGS_H
#define QUANTUM_PLUGIN_SETTINGS_H

#include "PluginUiSchema.h"

#include <string>

namespace quantum {
namespace plugin {

    // Per-plugin key/value store, persisted as JSON next to the engine's own settings. Defaults
    // come from the manifest; saved values win over them; a plugin only ever sees its own keys.
    class QUANTUM_API PluginSettings {
    public:
        void setPluginId(const std::string& id);
        void setPath(const std::string& filePath);

        const std::string& pluginId() const { return id; }
        const std::string& path() const { return filePath; }

        void setDefaults(const UiValueMap& newDefaults);
        const UiValueMap& defaults() const { return defaultValues; }

        bool load();
        bool save();

        // Drops everything the user saved and goes back to the manifest defaults.
        void reset();

        bool has(const std::string& key) const;
        UiValue get(const std::string& key) const;
        bool getBool(const std::string& key, bool fallback = false) const;
        long long getInt(const std::string& key, long long fallback = 0) const;
        double getFloat(const std::string& key, double fallback = 0.0) const;
        std::string getString(const std::string& key, const std::string& fallback = std::string()) const;

        void set(const std::string& key, const UiValue& value);
        void erase(const std::string& key);

        const UiValueMap& values() const { return currentValues; }
        const std::string& lastError() const { return error; }
        bool isDirty() const { return dirty; }

    private:
        std::string id;
        std::string filePath;
        std::string error;
        UiValueMap defaultValues;
        UiValueMap currentValues;
        bool dirty = false;
    };

}
}

#endif // QUANTUM_PLUGIN_SETTINGS_H
