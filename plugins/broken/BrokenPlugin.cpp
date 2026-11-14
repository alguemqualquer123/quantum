// (c) SR VINIX
// SPDX-License-Identifier: MIT
//
// Deliberately refuses to load: it exports ABI 999. This is the negative half of the FASE 1
// acceptance test - the manager has to notice before it calls anything through the vtable, because
// a plugin built against a different contract crashes instead of reporting.

#include "PluginInterface.h"

using namespace quantum::plugin;

class BrokenAbiPlugin: public PluginBase {
};

// Not QUANTUM_PLUGIN_ENTRY on purpose: the entry macro always reports the engine's own number.
extern "C" QUANTUM_PLUGIN_EXPORT_ATTR unsigned int QuantumPluginAbi() {
    return 999u;
}

extern "C" QUANTUM_PLUGIN_EXPORT_ATTR void* QuantumPluginCreate() {
    return static_cast<void*>(new (std::nothrow) BrokenAbiPlugin());
}

extern "C" QUANTUM_PLUGIN_EXPORT_ATTR void QuantumPluginDestroy(void* created) {
    delete static_cast<BrokenAbiPlugin*>(created);
}
