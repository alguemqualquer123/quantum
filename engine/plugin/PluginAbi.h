// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_ABI_H
#define QUANTUM_PLUGIN_ABI_H

#include <cstdint>
#include <new>

// The contract between a plugin binary and the engine. A plugin built against a different
// number is refused at load time rather than being called through a mismatched vtable,
// which is the only failure mode here that crashes instead of reporting.
//
// 1: the FASE 1 contract - lifecycle, schema-described panel, commands, tick, scene.
// 2: windows, menu items and the root-bounded file API (see PLUGIN_MARKETPLACE_PLAN.md, D7).
//    IPlugin gained two virtuals and PluginContext gained methods, so the vtable moved and a
//    plugin built against 1 must not be called through this one.
#define QUANTUM_PLUGIN_ABI_VERSION 2u

// The three C symbols every plugin exports. Named here so PluginLoader and the
// QUANTUM_PLUGIN_ENTRY macro cannot drift apart.
#define QUANTUM_PLUGIN_ABI_SYMBOL "QuantumPluginAbi"
#define QUANTUM_PLUGIN_CREATE_SYMBOL "QuantumPluginCreate"
#define QUANTUM_PLUGIN_DESTROY_SYMBOL "QuantumPluginDestroy"

#if defined(_WIN32)
    #define QUANTUM_PLUGIN_EXPORT_ATTR __declspec(dllexport)
#else
    #define QUANTUM_PLUGIN_EXPORT_ATTR __attribute__((visibility("default")))
#endif

// Destroy casts back to the concrete type on purpose: the manager only ever holds an
// IPlugin*, and deleting through it would need the vtable to be reachable from a module
// that may already be unmapped.
#define QUANTUM_PLUGIN_ENTRY(PluginClass)                                          \
    extern "C" QUANTUM_PLUGIN_EXPORT_ATTR unsigned int QuantumPluginAbi() {          \
        return QUANTUM_PLUGIN_ABI_VERSION;                                          \
    }                                                                             \
    extern "C" QUANTUM_PLUGIN_EXPORT_ATTR void* QuantumPluginCreate() {              \
        return static_cast<void*>(new (std::nothrow) PluginClass());               \
    }                                                                             \
    extern "C" QUANTUM_PLUGIN_EXPORT_ATTR void QuantumPluginDestroy(void* created) {  \
        delete static_cast<PluginClass*>(created);                                 \
    }

#endif // QUANTUM_PLUGIN_ABI_H
