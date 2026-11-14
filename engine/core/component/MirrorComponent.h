// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef MIRROR_COMPONENT_H
#define MIRROR_COMPONENT_H

#include "math/Vector3.h"

namespace quantum{

    // Planar reflection: RenderSystem drives an internal RTT camera mirrored across the mesh plane.
    struct QUANTUM_API MirrorComponent{
        Vector3 normal = Vector3(0, 0, 1); //local reflecting surface normal; +Z matches a Wall mesh
    };

}

#endif //MIRROR_COMPONENT_H
