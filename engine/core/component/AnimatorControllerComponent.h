// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMATOR_CONTROLLER_COMPONENT_H
#define ANIMATOR_CONTROLLER_COMPONENT_H

#include "Engine.h"

#include <string>
#include <vector>

namespace quantum{

    // Special graph node ids shared with the editor's Animator window: these
    // nodes have no AnimatorState data, transitions reference them by id.
    constexpr uint32_t ANIMATOR_ENTRY_STATE = 1000;
    constexpr uint32_t ANIMATOR_EXIT_STATE = 1001;
    constexpr uint32_t ANIMATOR_ANY_STATE = 1002;
    constexpr uint32_t MAX_ANIMATOR_LAYERS = 8;

    struct AnimatorLayer{
        std::string name;
        float weight = 1.0f;
        bool additive = false;
        bool enabled = true;
    };

    struct AnimatorIKChain{
        Entity root = NULL_ENTITY;
        Entity middle = NULL_ENTITY;
        Entity tip = NULL_ENTITY;
        Entity target = NULL_ENTITY;
        Entity pole = NULL_ENTITY;
        float weight = 1.0f;
        bool enabled = true;
        bool orientBones = true;
    };

    // Serializable definition of a single animation clip played inside an
    // Animator state. An entity may hold several clips (e.g. idle + idle variants)
    // that the controller blends or picks between.
    struct AnimatorClip{
        std::string name;
        // Entity carrying an AnimationComponent that drives this clip.
        Entity animation = NULL_ENTITY;
        // Optional entity carrying a SoundComponent whose sample plays while
        // this clip is active (footsteps, swing whoosh, etc.).
        Entity audio = NULL_ENTITY;

        float speed = 1.0f;
        float offset = 0.0f; // seconds into the clip where playback starts
        bool loop = true;
        uint32_t layer = 0;

        // Animation events fired at authored times during the clip.
        struct Event{
            float time = 0.0f;   // seconds into the clip
            std::string functionName; // Lua/C++ script function to invoke
            std::string parameter;    // optional string passed to the function
        };
        std::vector<Event> events;

        // Runtime-only event cursor: clip playback time seen last frame, -1
        // until the clip enters a state. Not serialized.
        float runtimePrevEventTime = -1.0f;
    };

    // One node of the Animator state machine graph. Authored in the editor's
    // Animator window and stored alongside the controller asset.
    struct AnimatorState{
        uint32_t id = 0;
        std::string name;
        std::vector<AnimatorClip> clips;
        bool isDefault = false;

        // Graph position (editor only, not meaningful to runtime beyond layout).
        float x = 0.0f;
        float y = 0.0f;
    };

    // Directed edge between two states. Guards playback via conditions on the
    // controller's parameters (float/int/bool/trigger).
    struct AnimatorTransition{
        uint32_t id = 0;
        std::string name;
        uint32_t fromState = 0;
        uint32_t toState = 0;

        bool hasExitTime = false;
        float exitTime = 0.0f;          // normalized time in source state (0..1)
        float transitionDuration = 0.25f;
        float offset = 0.0f;            // normalized time in destination state
        bool canTransitionToSelf = false;

        enum class InterruptionSource : int{
            None = 0,
            CurrentInSource = 1,
            NextInSource = 2,
            CurrentInSourceAndNextInSource = 3
        };
        InterruptionSource interruptionSource = InterruptionSource::None;

        struct Condition{
            std::string parameter;   // name of a controller parameter
            enum class Mode : int{
                Greater = 0,
                Less = 1,
                Equals = 2,
                NotEqual = 3
            };
            Mode mode = Mode::Greater;
            float value = 0.0f;
            bool boolValue = false;
        };
        std::vector<Condition> conditions;
    };

    // One exposed parameter of the animator controller, settable from scripts
    // or from transition conditions. Drives blend trees and state switching.
    struct AnimatorParameter{
        std::string name;
        enum class Type : int{
            Float = 0,
            Int = 1,
            Bool = 2,
            Trigger = 3
        };
        Type type = Type::Float;
        float defaultValue = 0.0f;
        bool boolValue = false;

        // Runtime value, not serialized (triggers auto-reset).
        float runtimeValue = 0.0f;
        bool triggerPending = false;
    };

    struct QUANTUM_API AnimatorControllerComponent{
        // Path to the authored controller asset (.animcontroller / .graph).
        // When empty, the states/transitions below are authored inline.
        std::string controllerAsset;

        // Parameters exposed to scripts and transition conditions.
        std::vector<AnimatorParameter> parameters;

        // Ordered pose layers. Layer zero is the base pose; higher layers
        // override or add to it after the base pose has been sampled.
        std::vector<AnimatorLayer> layers;
        std::vector<AnimatorIKChain> ikChains;

        // When enabled, translation of the configured root target is consumed as
        // locomotion and applied to the controller entity instead of moving the
        // animated root in place. A null target uses this controller's entity.
        bool rootMotionEnabled = false;
        Entity rootMotionTarget = NULL_ENTITY;
        float rootMotionScale = 1.0f;

        // States and transitions of the state machine graph.
        std::vector<AnimatorState> states;
        std::vector<AnimatorTransition> transitions;

        // Fires when a clip event time is reached: (functionName, parameter).
        FunctionSubscribe<void(const std::string&, const std::string&)> onEvent;
        // Fires on every state change: (previousStateName, newStateName).
        FunctionSubscribe<void(const std::string&, const std::string&)> onStateChange;

        // Runtime state (not serialized).
        bool enabled = true;
        uint32_t currentState = 0;
        float currentStateTime = 0.0f;
        bool isPlaying = false;

        // Runtime-only root-motion sampling state.
        Vector3 rootMotionPrevious = Vector3::ZERO;
        uint32_t rootMotionState = 0;
        float rootMotionTime = 0.0f;
        bool rootMotionHasSample = false;

        // --- Parameter API (Unity Animator.Set*/Get* equivalents) ---

        AnimatorParameter* findParameter(const std::string& name){
            for (AnimatorParameter& parameter : parameters){
                if (parameter.name == name){
                    return &parameter;
                }
            }
            return nullptr;
        }

        const AnimatorParameter* findParameter(const std::string& name) const{
            for (const AnimatorParameter& parameter : parameters){
                if (parameter.name == name){
                    return &parameter;
                }
            }
            return nullptr;
        }

        AnimatorState* findState(uint32_t id){
            for (AnimatorState& state : states){
                if (state.id == id){
                    return &state;
                }
            }
            return nullptr;
        }

        const AnimatorState* findState(uint32_t id) const{
            for (const AnimatorState& state : states){
                if (state.id == id){
                    return &state;
                }
            }
            return nullptr;
        }

        AnimatorState* findStateByName(const std::string& name){
            for (AnimatorState& state : states){
                if (state.name == name){
                    return &state;
                }
            }
            return nullptr;
        }

        // The state flagged as default, or the first one when none is.
        AnimatorState* getDefaultState(){
            if (states.empty()){
                return nullptr;
            }
            for (AnimatorState& state : states){
                if (state.isDefault){
                    return &state;
                }
            }
            return &states[0];
        }

        void setFloat(const std::string& name, float value){
            if (AnimatorParameter* parameter = findParameter(name)){
                parameter->runtimeValue = value;
            }else{
                Log::warn("Animator parameter not found: %s", name.c_str());
            }
        }

        float getFloat(const std::string& name) const{
            const AnimatorParameter* parameter = findParameter(name);
            return parameter ? parameter->runtimeValue : 0.0f;
        }

        void setInteger(const std::string& name, int value){
            if (AnimatorParameter* parameter = findParameter(name)){
                parameter->runtimeValue = (float)value;
            }else{
                Log::warn("Animator parameter not found: %s", name.c_str());
            }
        }

        int getInteger(const std::string& name) const{
            const AnimatorParameter* parameter = findParameter(name);
            return parameter ? (int)parameter->runtimeValue : 0;
        }

        void setBool(const std::string& name, bool value){
            if (AnimatorParameter* parameter = findParameter(name)){
                parameter->boolValue = value;
                parameter->runtimeValue = value ? 1.0f : 0.0f;
            }else{
                Log::warn("Animator parameter not found: %s", name.c_str());
            }
        }

        bool getBool(const std::string& name) const{
            const AnimatorParameter* parameter = findParameter(name);
            return parameter ? parameter->boolValue : false;
        }

        void setTrigger(const std::string& name){
            if (AnimatorParameter* parameter = findParameter(name)){
                parameter->triggerPending = true;
                parameter->boolValue = true;
                parameter->runtimeValue = 1.0f;
            }else{
                Log::warn("Animator parameter not found: %s", name.c_str());
            }
        }

        void resetTrigger(const std::string& name){
            if (AnimatorParameter* parameter = findParameter(name)){
                parameter->triggerPending = false;
                parameter->boolValue = false;
                parameter->runtimeValue = 0.0f;
            }
        }

        std::string getCurrentStateName() const{
            const AnimatorState* state = findState(currentState);
            return state ? state->name : std::string();
        }
    };

}

#endif //ANIMATOR_CONTROLLER_COMPONENT_H
