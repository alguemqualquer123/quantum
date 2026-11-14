// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "PluginInterface.h"

using namespace quantum;
using namespace quantum::plugin;

bool PluginBase::initialize(PluginContext& context) {
    pluginContext = &context;
    return true;
}

PluginContext& PluginBase::context() const {
    return *pluginContext;
}
