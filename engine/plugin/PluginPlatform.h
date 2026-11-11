// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_PLATFORM_H
#define QUANTUM_PLUGIN_PLATFORM_H

#include "Export.h"

namespace quantum {
namespace plugin {

    // "windows" / "linux" / "macos" / "web" - the strings a manifest's "platforms" is written
    // against, and the only way to compare them without #ifdefs in three different files.
    QUANTUM_API const char* currentPlatformTag();

    // ".dll" / ".so" / ".dylib", with the dot.
    QUANTUM_API const char* platformLibraryExtension();

    // What compiler built the engine, so a plugin built by another one is refused with a reason
    // instead of a crash. Mirrors the check the project loader already does
    // (editor/Generator.cpp:890-911).
    QUANTUM_API const char* engineToolchainTag();

    // 32 or 64 - the other half of that same check.
    QUANTUM_API int enginePointerSize();

}
}

#endif // QUANTUM_PLUGIN_PLATFORM_H
