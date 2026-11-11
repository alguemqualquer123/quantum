// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef ACTION_COMPONENT_H
#define ACTION_COMPONENT_H

#include "Engine.h"

namespace quantum{

    enum class ActionState{
        Running,
        Paused,
        Stopped
    };

    struct QUANTUM_API ActionComponent{
        ActionState state = ActionState::Stopped;
        float timecount = 0;

        bool startTrigger = false;
        bool stopTrigger = false;
        bool pauseTrigger = false;

        bool ownedTarget = false;

        Entity target = NULL_ENTITY;

        float speed = 1;

        // seconds into the action it starts at, or a random time of its duration
        float startOffset = 0;
        bool randomStart = false;
        float phase = 0; // where the current run started, from the two above

        // Blend weight (0..1) this action contributes when its output is combined
        // with other actions on the same target. Propagated from the owning
        // AnimationComponent during crossfades. Runtime-only (not serialized).
        float weight = 1;
        uint32_t blendLayer = 0;
        float blendLayerWeight = 1.0f;
        bool blendLayerAdditive = false;

        FunctionSubscribe<void()> onStart;
        FunctionSubscribe<void()> onPause;
        FunctionSubscribe<void()> onStop;

        FunctionSubscribe<void()> onStep;
    };

    
}

#endif //ACTION_COMPONENT_H
