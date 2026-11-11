// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

// PLACEHOLDER - DELETE THIS FILE as soon as the real group files exist.
//
// NodeDefs.cpp calls registerPhysicsOps/registerAudioOps/registerSystemOps and NodeOps.h
// declares them, but the three group files were never written, so the engine library does
// not link (LNK2019 on all three). These empty bodies unblock the link; implement each
// group in its own NodeOps_<Group>.cpp, add it to the QUANTUM sources in
// engine/CMakeLists.txt, and remove this file. Writing a node here instead of there means
// the row silently disappears the moment this placeholder is deleted.

#include "NodeOps.h"

namespace quantum {
namespace graph {

    void registerPhysicsOps() {}

    void registerAudioOps() {}

    void registerSystemOps() {}

}
}
