// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef TIMEDACTION_H
#define TIMEDACTION_H

#include "Action.h"
#include "Ease.h"

namespace quantum{
    class QUANTUM_API TimedAction: public Action{

    protected:
        void setAction(float duration, bool loop);

    public:
        TimedAction(Scene* scene);
        TimedAction(Scene* scene, Entity entity);

        void setFunctionType(EaseType functionType);

        float getValue() const;
        float getTime() const;

        void setDuration(float duration);
        float getDuration() const;

        void setLoop(bool loop);
        bool isLoop() const;

        void setPingPong(bool pingPong);
        bool isPingPong() const;
    };
}

#endif //TIMEDACTION_H