// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef POSITIONACTION_COMPONENT_H
#define POSITIONACTION_COMPONENT_H

#include "math/Vector3.h"

namespace quantum{

    struct QUANTUM_API PositionActionComponent{
        Vector3 endPosition;
        Vector3 startPosition;
    };

}

#endif //POSITIONACTION_COMPONENT_H