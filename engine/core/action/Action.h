// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef ACTION_H
#define ACTION_H

#include "object/EntityHandle.h"
#include "object/Object.h"

namespace quantum{
    class QUANTUM_API Action: public EntityHandle{
    public:
        Action(Scene* scene);
        Action(Scene* scene, Entity entity);
        virtual ~Action();

        void start();
        void pause();
        void stop();
        void restart();

        void setOwnedTarget(bool ownedTarget);
        bool getOwnedTarget() const;

        void setTarget(Object* target);
        void setTarget(Entity target);
        Entity getTarget() const;

        void setSpeed(float speed);
        float getSpeed() const;

        void setWeight(float weight);
        float getWeight() const;

        void setStartOffset(float startOffset);
        float getStartOffset() const;

        void setRandomStart(bool randomStart);
        bool isRandomStart() const;

        bool isRunning() const;
        bool isStopped() const;
        bool isPaused() const;

        float getTimeCount() const;
    };
}

#endif //ACTION_H