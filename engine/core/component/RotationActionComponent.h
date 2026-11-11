// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef ROTATIONACTION_COMPONENT_H
#define ROTATIONACTION_COMPONENT_H

#include "math/Quaternion.h"

namespace quantum{

    struct QUANTUM_API RotationActionComponent{
        Quaternion endRotation;
        Quaternion startRotation;

        bool shortestPath = false;

        // turns startRotation by angle (degrees, any size) around axis instead of reaching endRotation
        bool spin = false;
        Vector3 axis = Vector3(0, 1, 0);
        float angle = 360;
    };

}

#endif //ROTATIONACTION_COMPONENT_H