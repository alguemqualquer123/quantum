// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "PluginPlatform.h"

using namespace quantum::plugin;

const char* quantum::plugin::currentPlatformTag() {
    #if defined(__EMSCRIPTEN__)
        return "web";
    #elif defined(_WIN32)
        return "windows";
    #elif defined(__APPLE__)
        return "macos";
    #elif defined(__linux__)
        return "linux";
    #else
        return "unknown";
    #endif
}

const char* quantum::plugin::platformLibraryExtension() {
    #if defined(_WIN32)
        return ".dll";
    #elif defined(__APPLE__)
        return ".dylib";
    #else
        return ".so";
    #endif
}

const char* quantum::plugin::engineToolchainTag() {
    #if defined(_MSC_VER)
        return "msvc";
    #elif defined(__clang__)
        return "clang";
    #elif defined(__GNUC__)
        return "gcc";
    #else
        return "unknown";
    #endif
}

int quantum::plugin::enginePointerSize() {
    return static_cast<int>(sizeof(void*));
}
