// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "registry/EntityRegistry.h"

namespace quantum::editor {

    class ScopedDefaultEntityPool {
    public:
        ScopedDefaultEntityPool(EntityRegistry& r, EntityPool p)
            : reg(r), old(r.getDefaultEntityPool()) { reg.setDefaultEntityPool(p); }
        ~ScopedDefaultEntityPool() { reg.setDefaultEntityPool(old); }
    private:
        EntityRegistry& reg;
        EntityPool old;
    };

}