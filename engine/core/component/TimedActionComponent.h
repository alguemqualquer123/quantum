// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef TIMEDACTION_COMPONENT_H
#define TIMEDACTION_COMPONENT_H

#include "action/Ease.h"

namespace quantum{

    struct QUANTUM_API TimedActionComponent{
        float time = 0;
        float value = 0;
        
        float duration = 0;
        bool loop = false;
        bool pingPong = false; // plays forward and then back, in twice the duration

        Ease function;
    };

}

#endif //TIMEDACTION_COMPONENT_H