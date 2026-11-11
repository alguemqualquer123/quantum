// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_MANIFEST_H
#define QUANTUM_PLUGIN_MANIFEST_H

#include "PluginUiSchema.h"

#include <string>
#include <vector>

namespace quantum {
namespace plugin {

    struct PluginDependency {
        std::string id;
        std::string minVersion;
    };

    // One plugin.json. Every field the loader needs is here so a plugin can be inspected -
    // listed, filtered, checked for compatibility - without loading its binary, which is what makes
    // the editor's Plugin Browser able to show a plugin that failed to load.
    struct PluginManifest {
        std::string id;
        std::string name;
        std::string description;
        std::string author;
        std::string version = "0.0.0";
        std::string minEngineVersion;
        std::string type = "engine";
        std::string entryPoint;

        unsigned int abiVersion = 0;

        std::vector<std::string> platforms;
        std::vector<std::string> libraries;
        std::vector<std::string> permissions;
        std::vector<PluginDependency> dependencies;
        std::vector<std::string> commands;

        // "settings" block: the defaults a plugin ships with, merged under whatever the user saved.
        UiValueMap defaults;

        // Optional panel declaration. A plugin with no panel leaves this empty and gets no window.
        std::string panelTitle;

        // The presentation fields the package browser shows, in the shape Unreal's .uplugin uses
        // (Category / CreatedBy / DocsURL / MarketplaceURL / CanContainContent / IsBetaVersion).
        // They are inert for the loader: a plugin loads the same with none of them, which is why an
        // existing plugin.json needs no edit to keep working.
        std::string category;
        std::string summary;
        std::vector<std::string> keywords;
        std::string icon;
        std::string preview;
        std::string docsUrl;
        std::string marketplaceUrl;
        std::string supportUrl;
        bool enabledByDefault = true;
        bool canContainContent = false;
        bool isBetaVersion = false;
        // True when the package must be installed before use, false for a plugin that ships inside
        // the engine build. The browser labels the two groups the way Unreal labels Installed and
        // Built-in.
        bool installedPackage = false;

        // Filled in by the loader, not by the file.
        std::string directory;
        std::string path;

        bool valid = false;
        std::vector<std::string> errors;

        QUANTUM_API bool hasPermission(const std::string& permission) const;
        // The first library entry, with the platform suffix added if the manifest left it off.
        QUANTUM_API std::string libraryFileName() const;
        QUANTUM_API std::string errorsAsText() const;
    };

    QUANTUM_API bool parsePluginManifest(const std::string& jsonText, PluginManifest& out);
    QUANTUM_API bool loadPluginManifest(const std::string& filePath, PluginManifest& out);

    // True when the running OS is in `platforms`, or when the manifest lists no platforms at all.
    QUANTUM_API bool supportsCurrentPlatform(const PluginManifest& manifest);

    // "windows" / "linux" / "macos" / "web" - the strings a manifest is written against.
    QUANTUM_API const char* currentPlatformTag();

    // The permission names the plugin system knows. Anything else is a typo in a manifest and is
    // reported at load, because a plugin that asks for "scense" silently gets nothing.
    QUANTUM_API bool isKnownPermission(const std::string& name);

    // True for the permissions the engine itself refuses a call without - "log", "scene", "tick".
    // The rest ("settings", "files", "ui") are declarations the plugin is expected to honour and the
    // editor shows to the user; the engine cannot gate a plugin reading its own settings.
    QUANTUM_API bool isEnforcedPermission(const std::string& name);

}
}

#endif // QUANTUM_PLUGIN_MANIFEST_H
