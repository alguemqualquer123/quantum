// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_LOCK_H
#define QUANTUM_PLUGIN_LOCK_H

#include "Export.h"

#include <string>
#include <vector>

namespace quantum {
namespace plugin {

    // What one install actually put on disk. The engine needs it to say "installed, version x" in
    // the browser; the package manager needs `contentTargets` to undo an install without touching a
    // file the user made, which is the only reason the list exists.
    struct InstalledPackage {
        std::string id;
        std::string name;
        std::string version;
        std::string registry;
        // Where the package file came from, kept so `update` and `outdated` can re-resolve it.
        std::string packageFile;
        std::string sha256;
        unsigned long long size = 0;
        // The installed plugin folder, absolute or relative to the plugins directory.
        std::string directory;
        std::string installedAt;
        // Files copied into the project by a content package, relative to the project root.
        std::vector<std::string> contentTargets;
    };

    class QUANTUM_API PluginLock {
    public:
        // An absent file is an empty lock, not a failure: nothing has been installed yet on a fresh
        // checkout. A malformed file is a failure, because overwriting it would forget real installs.
        bool load(const std::string& filePath, std::string& errorOut);
        bool save(std::string& errorOut) const;

        const InstalledPackage* find(const std::string& id) const;
        void put(const InstalledPackage& package);
        bool remove(const std::string& id);

        const std::vector<InstalledPackage>& packages() const { return entries; }
        const std::string& filePath() const { return path; }

        static std::string formatTimestamp();

    private:
        std::vector<InstalledPackage> entries;
        std::string path;
    };

}
}

#endif //QUANTUM_PLUGIN_LOCK_H
