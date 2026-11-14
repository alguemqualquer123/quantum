// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMATOR_CONTROLLER_H
#define ANIMATOR_CONTROLLER_H

#include "EntityHandle.h"
#include "Object.h"
#include "component/AnimatorControllerComponent.h"

#include <string>

namespace quantum{

    // Handle for an entity's AnimatorControllerComponent, the way Sound is a
    // handle for SoundComponent. The state machine itself is stepped by
    // ActionSystem; this is the script-facing control: parameters, forced
    // state plays, and start/stop, like Unity's Animator.
    class QUANTUM_API AnimatorController: public EntityHandle{
    public:
        AnimatorController(Scene* scene);
        AnimatorController(Scene* scene, Entity entity);
        virtual ~AnimatorController();

        Object getObject() const;

        // Restart the state machine from the Entry/default state.
        void play();
        // Stop playback, fading out every running clip.
        void stop();
        // Jump directly to a state by name (Unity Animator.Play).
        void playState(const std::string& name);
        void playState(const std::string& name, float fadeTime);

        void setEnabled(bool enabled);
        bool isEnabled() const;

        bool isPlaying() const;
        std::string getCurrentStateName() const;
        float getCurrentStateTime() const;

        void setFloat(const std::string& name, float value);
        float getFloat(const std::string& name) const;
        void setInteger(const std::string& name, int value);
        int getInteger(const std::string& name) const;
        void setBool(const std::string& name, bool value);
        bool getBool(const std::string& name) const;
        void setTrigger(const std::string& name);
        void resetTrigger(const std::string& name);
    };

}

#endif //ANIMATOR_CONTROLLER_H
