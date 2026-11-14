// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_VERSION_H
#define QUANTUM_PLUGIN_VERSION_H

#include "Export.h"

#include <string>

#ifndef QUANTUM_ENGINE_VERSION
    // Standalone project builds (engine/CMakeLists.txt used as the top level) never run the
    // version block of the root CMakeLists.txt, so the macro has to have a value here too.
    #define QUANTUM_ENGINE_VERSION "0.0.0"
#endif

namespace quantum {
namespace plugin {

    struct Version {
        int major = 0;
        int minor = 0;
        int patch = 0;

        // False when the text was not "X.Y[.Z]" at all. The build version of this repo comes from
        // `git describe` (CMakeLists.txt:5-19), so it is routinely a commit hash rather than a
        // number, and a comparison against it has to be skipped rather than guessed.
        bool parsed = false;

        bool operator==(const Version& other) const {
            return major == other.major && minor == other.minor && patch == other.patch;
        }
    };

    QUANTUM_API Version parseVersion(const std::string& text);
    QUANTUM_API int compareVersions(const Version& left, const Version& right);
    QUANTUM_API std::string versionToString(const Version& version);

    QUANTUM_API Version engineVersionNumber();
    QUANTUM_API const char* engineVersionText();

    // True when `available` is at least `minimum`. An unparsable either side is accepted with the
    // reason written to `reason`, because refusing to load over a version the engine cannot read
    // would break every local build.
    QUANTUM_API bool isVersionCompatible(const Version& available, const Version& minimum,
                             std::string& reason);

}
}

#endif // QUANTUM_PLUGIN_VERSION_H
