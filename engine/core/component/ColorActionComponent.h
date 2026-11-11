// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef COLORACTION_COMPONENT_H
#define COLORACTION_COMPONENT_H

#include "math/Vector3.h"

namespace quantum{

    struct QUANTUM_API ColorActionComponent{
        Vector3 endColor;
        Vector3 startColor;

        bool useSRGB = true;
    };

}

#endif //COLORACTION_COMPONENT_H