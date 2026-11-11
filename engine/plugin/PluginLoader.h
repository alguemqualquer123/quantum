// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_LOADER_H
#define QUANTUM_PLUGIN_LOADER_H

#include "Export.h"
#include "PluginAbi.h"

#include <string>

namespace quantum {
namespace plugin {

    // The three functions every plugin exports, resolved by name after open().
    typedef unsigned int (*PluginAbiFunction)();
    typedef void* (*PluginCreateFunction)();
    typedef void (*PluginDestroyFunction)(void*);

    // Thin wrapper over the platform loader. Kept separate from PluginManager so the OS calls and
    // the lifecycle rules can be tested apart, and so the same trick the script hot-reload already
    // uses (editor/Conector.cpp:70-100) has one implementation instead of two.
    class QUANTUM_API PluginLoader {
    public:
        struct Loaded {
            void* handle = nullptr;
            std::string resolvedPath;
            std::string error;

            bool ok() const { return handle != nullptr; }
        };

        static Loaded open(const std::string& path);
        static void close(void* handle);

        static void* symbol(void* handle, const char* name);

        static std::string systemErrorMessage();
    };

}
}

#endif // QUANTUM_PLUGIN_LOADER_H
