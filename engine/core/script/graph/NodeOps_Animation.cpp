// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "NodeOps.h"

#include "action/AlphaAction.h"
#include "action/ColorAction.h"
#include "action/PositionAction.h"
#include "action/RotationAction.h"
#include "action/ScaleAction.h"
#include "component/AnimationComponent.h"
#include "component/ModelComponent.h"
#include "component/TimedActionComponent.h"
#include "object/Model.h"

namespace quantum {
namespace graph {

    namespace {

        // Every handler below works through a component it has just looked up. The
        // Action/Animation/TimedAction wrappers read theirs with EntityHandle::getComponent,
        // which throws when the entity does not have it, and a throw aborts the process
        // under this repo's _HAS_EXCEPTIONS=0.
        Entity pinEntity(GraphRuntime& runtime, const BoundNode& node, size_t pin) {
            const GraphValue value = runtime.readInput(node, pin);
            if (value.isNone()) {
                return NULL_ENTITY;
            }
            return value.type == GraphValueType::Entity
                ? value.asEntity()
                : static_cast<Entity>(value.asInt());
        }

        // CUSTOM is the last entry and needs a function a graph cannot supply, so the
        // pin is clamped to STEP.
        EaseType easeType(int64_t index) {
            const int last = static_cast<int>(EaseType::STEP);
            const int clamped = index < 0 ? 0 : (index > last ? last : static_cast<int>(index));
            return static_cast<EaseType>(clamped);
        }

        ModelComponent* modelFor(GraphRuntime& runtime, const BoundNode& node, size_t pin, Entity* outEntity) {
            const Entity entity = op::target(runtime, node, pin);
            if (outEntity) {
                *outEntity = entity;
            }
            return op::component<ModelComponent>(runtime, node, entity, "Model");
        }

        // The action system only drives a clip that carries both halves of an action, so
        // a list entry that lost its entity or its components is reported, not played.
        bool playable(Scene* world, Entity entity) {
            return world && entity != NULL_ENTITY && world->isEntityCreated(entity) &&
                   world->findComponent<AnimationComponent>(entity) &&
                   world->findComponent<ActionComponent>(entity);
        }

        Entity animationAt(const ModelComponent& model, int index) {
            if (index < 0 || static_cast<size_t>(index) >= model.animations.size()) {
                return NULL_ENTITY;
            }
            return model.animations[static_cast<size_t>(index)];
        }

        int firstAnimationNamed(Scene* world, const ModelComponent& model, const std::string& name) {
            for (int i = 0; i < static_cast<int>(model.animations.size()); ++i) {
                const Entity entity = model.animations[static_cast<size_t>(i)];
                if (!world || entity == NULL_ENTITY || !world->isEntityCreated(entity)) {
                    continue;
                }
                if (world->getEntityName(entity) == name) {
                    return i;
                }
            }
            return -1;
        }

        struct ActionHandle {
            Entity entity = NULL_ENTITY;
            ActionComponent* action = nullptr;
        };

        ActionHandle actionPin(GraphRuntime& runtime, const BoundNode& node, size_t pin) {
            ActionHandle handle;
            Scene* world = op::scene(runtime);
            handle.entity = pinEntity(runtime, node, pin);
            if (!world || handle.entity == NULL_ENTITY || !world->isEntityCreated(handle.entity)) {
                op::warn(runtime, node, "action entity does not exist");
                handle.entity = NULL_ENTITY;
                return handle;
            }

            handle.action = world->findComponent<ActionComponent>(handle.entity);
            if (!handle.action) {
                op::warn(runtime, node, "entity is not an action (no ActionComponent)");
            }
            return handle;
        }

        struct AnimationHandle {
            Entity entity = NULL_ENTITY;
            ActionComponent* action = nullptr;
            AnimationComponent* animation = nullptr;
        };

        AnimationHandle animationPin(GraphRuntime& runtime, const BoundNode& node, size_t pin) {
            AnimationHandle handle;
            Scene* world = op::scene(runtime);
            handle.entity = pinEntity(runtime, node, pin);
            if (!world || handle.entity == NULL_ENTITY || !world->isEntityCreated(handle.entity)) {
                op::warn(runtime, node, "animation entity does not exist");
                handle.entity = NULL_ENTITY;
                return handle;
            }

            handle.animation = world->findComponent<AnimationComponent>(handle.entity);
            handle.action = world->findComponent<ActionComponent>(handle.entity);
            if (!handle.animation || !handle.action) {
                op::warn(runtime, node, "entity is not an animation (no Animation/Action component)");
                handle.entity = NULL_ENTITY;
                handle.animation = nullptr;
                handle.action = nullptr;
            }
            return handle;
        }

        // The create nodes hand the action entity over to the scene: the wrapper owns it
        // while it lives on the stack and would destroy the action at the end of the node,
        // leaving the graph holding a dead id on its output pin.
        Entity publishAction(GraphRuntime& runtime, Action& handle, Entity target, const char* label) {
            handle.setName(label);
            handle.setTarget(target);
            handle.start();
            handle.setEntityOwned(false);
            return handle.getEntity();
        }

        // ---- Skeletal animation

        int animPlay(GraphRuntime& runtime, const BoundNode& node) {
            Entity model = NULL_ENTITY;
            ModelComponent* comp = modelFor(runtime, node, 2, &model);
            if (!comp) {
                return -1;
            }

            const std::string name = op::text(runtime, node, 0);
            const int index = firstAnimationNamed(op::scene(runtime), *comp, name);
            if (index < 0) {
                op::warn(runtime, node, "the model has no animation named '" + name + "'");
                return -1;
            }
            if (!playable(op::scene(runtime), animationAt(*comp, index))) {
                op::warn(runtime, node, "animation '" + name + "' is not playable");
                return -1;
            }

            Model(op::scene(runtime), model).playAnimation(index, static_cast<float>(op::number(runtime, node, 1)));
            return 0;
        }

        int animPlayIndex(GraphRuntime& runtime, const BoundNode& node) {
            Entity model = NULL_ENTITY;
            ModelComponent* comp = modelFor(runtime, node, 2, &model);
            if (!comp) {
                return -1;
            }

            const int index = static_cast<int>(op::integer(runtime, node, 0));
            if (!playable(op::scene(runtime), animationAt(*comp, index))) {
                op::warn(runtime, node, "animation index is out of range or not playable");
                return -1;
            }

            Model(op::scene(runtime), model).playAnimation(index, static_cast<float>(op::number(runtime, node, 1)));
            return 0;
        }

        int animStopAll(GraphRuntime& runtime, const BoundNode& node) {
            Entity model = NULL_ENTITY;
            if (!modelFor(runtime, node, 1, &model)) {
                return -1;
            }
            Model(op::scene(runtime), model).stopAnimations(static_cast<float>(op::number(runtime, node, 0)));
            return 0;
        }

        void animCount(GraphRuntime& runtime, const BoundNode& node) {
            ModelComponent* comp = modelFor(runtime, node, 0, nullptr);
            op::outInt(runtime, node, 0, comp ? static_cast<int64_t>(comp->animations.size()) : 0);
        }

        void animName(GraphRuntime& runtime, const BoundNode& node) {
            ModelComponent* comp = modelFor(runtime, node, 1, nullptr);
            const Entity entity = comp ? animationAt(*comp, static_cast<int>(op::integer(runtime, node, 0))) : NULL_ENTITY;
            Scene* world = op::scene(runtime);
            op::outString(runtime, node, 0,
                          entity != NULL_ENTITY && world && world->isEntityCreated(entity)
                              ? world->getEntityName(entity)
                              : std::string());
        }

        void animEntityAt(GraphRuntime& runtime, const BoundNode& node) {
            ModelComponent* comp = modelFor(runtime, node, 1, nullptr);
            op::outEntity(runtime, node, 0,
                          comp ? animationAt(*comp, static_cast<int>(op::integer(runtime, node, 0))) : NULL_ENTITY);
        }

        void animFindByName(GraphRuntime& runtime, const BoundNode& node) {
            ModelComponent* comp = modelFor(runtime, node, 1, nullptr);
            Entity found = NULL_ENTITY;
            if (comp) {
                const int index = firstAnimationNamed(op::scene(runtime), *comp, op::text(runtime, node, 0));
                found = animationAt(*comp, index);
            }
            op::outEntity(runtime, node, 0, found);
        }

        void animIsPlaying(GraphRuntime& runtime, const BoundNode& node) {
            ModelComponent* comp = modelFor(runtime, node, 1, nullptr);
            bool playing = false;
            if (comp) {
                Scene* world = op::scene(runtime);
                const int index = firstAnimationNamed(world, *comp, op::text(runtime, node, 0));
                const Entity entity = animationAt(*comp, index);
                if (world && entity != NULL_ENTITY && world->isEntityCreated(entity)) {
                    const ActionComponent* action = world->findComponent<ActionComponent>(entity);
                    playing = action && action->state == ActionState::Running;
                }
            }
            op::outBool(runtime, node, 0, playing);
        }

        // "Start" with no fade pin uses the clip's authored default, which is what the
        // editor's own preview does.
        int animStart(GraphRuntime& runtime, const BoundNode& node) {
            const AnimationHandle handle = animationPin(runtime, node, 0);
            if (!handle.animation) {
                return -1;
            }
            Animation(op::scene(runtime), handle.entity)
                .fadeIn(handle.animation->defaultFadeTime);
            return 0;
        }

        int animSetState(GraphRuntime& runtime, const BoundNode& node) {
            const AnimationHandle handle = animationPin(runtime, node, 0);
            if (!handle.animation) {
                return -1;
            }

            const int state = static_cast<int>(op::integer(runtime, node, 1));
            if (state <= 0) {
                op::startAction(*handle.action);
            } else if (state == 1) {
                op::pauseAction(*handle.action);
            } else {
                op::stopAction(*handle.action);
            }
            return 0;
        }

        int animSetLoop(GraphRuntime& runtime, const BoundNode& node) {
            const AnimationHandle handle = animationPin(runtime, node, 0);
            if (!handle.animation) {
                return -1;
            }
            handle.animation->loop = op::flag(runtime, node, 1);
            return 0;
        }

        void animIsLoop(GraphRuntime& runtime, const BoundNode& node) {
            const AnimationHandle handle = animationPin(runtime, node, 0);
            op::outBool(runtime, node, 0, handle.animation && handle.animation->loop);
        }

        int animSetSpeed(GraphRuntime& runtime, const BoundNode& node) {
            const AnimationHandle handle = animationPin(runtime, node, 0);
            if (!handle.action) {
                return -1;
            }
            handle.action->speed = static_cast<float>(op::number(runtime, node, 1));
            return 0;
        }

        // Setting the weight directly also ends any fade that was heading elsewhere,
        // exactly as Animation::setBlendWeight does.
        int animSetWeight(GraphRuntime& runtime, const BoundNode& node) {
            const AnimationHandle handle = animationPin(runtime, node, 0);
            if (!handle.animation) {
                return -1;
            }

            double weight = op::number(runtime, node, 1);
            if (weight < 0.0) weight = 0.0;
            if (weight > 1.0) weight = 1.0;

            handle.animation->weight = static_cast<float>(weight);
            handle.animation->fadeTarget = handle.animation->weight;
            handle.animation->fadeSpeed = 0.0f;
            handle.animation->stopOnFadeOut = false;
            return 0;
        }

        int animSetDuration(GraphRuntime& runtime, const BoundNode& node) {
            const AnimationHandle handle = animationPin(runtime, node, 0);
            if (!handle.animation) {
                return -1;
            }
            handle.animation->duration = static_cast<float>(op::number(runtime, node, 1));
            return 0;
        }

        void animProgress(GraphRuntime& runtime, const BoundNode& node) {
            const AnimationHandle handle = animationPin(runtime, node, 0);
            double progress = 0.0;
            if (handle.action && handle.animation && handle.animation->duration > 0.0f) {
                progress = handle.action->timecount / handle.animation->duration;
            }
            op::outFloat(runtime, node, 0, progress);
        }

        int animMorphSet(GraphRuntime& runtime, const BoundNode& node) {
            Entity model = NULL_ENTITY;
            if (!modelFor(runtime, node, 2, &model)) {
                return -1;
            }
            Model(op::scene(runtime), model)
                .setMorphWeight(op::text(runtime, node, 0), static_cast<float>(op::number(runtime, node, 1)));
            return 0;
        }

        void animMorphGet(GraphRuntime& runtime, const BoundNode& node) {
            Entity model = NULL_ENTITY;
            if (!modelFor(runtime, node, 1, &model)) {
                op::outFloat(runtime, node, 0, 0.0);
                return;
            }
            op::outFloat(runtime, node, 0,
                         Model(op::scene(runtime), model).getMorphWeight(op::text(runtime, node, 0)));
        }

        void animBoneEntity(GraphRuntime& runtime, const BoundNode& node) {
            ModelComponent* comp = modelFor(runtime, node, 1, nullptr);
            Entity bone = NULL_ENTITY;
            if (comp) {
                std::map<std::string, Entity>::const_iterator found = comp->bonesNameMapping.find(op::text(runtime, node, 0));
                if (found != comp->bonesNameMapping.end()) {
                    bone = found->second;
                }
            }
            op::outEntity(runtime, node, 0, bone);
        }

        void animBonePosition(GraphRuntime& runtime, const BoundNode& node) {
            ModelComponent* comp = modelFor(runtime, node, 1, nullptr);
            Vector3 position;
            if (!comp) {
                op::outVector3(runtime, node, 0, position);
                return;
            }

            std::map<std::string, Entity>::const_iterator found = comp->bonesNameMapping.find(op::text(runtime, node, 0));
            if (found == comp->bonesNameMapping.end()) {
                op::warn(runtime, node, "the model has no bone named '" + op::text(runtime, node, 0) + "'");
            } else {
                const Transform* transform = op::scene(runtime)->findComponent<Transform>(found->second);
                if (transform) {
                    position = transform->worldPosition;
                }
            }
            op::outVector3(runtime, node, 0, position);
        }

        int animResetBindPose(GraphRuntime& runtime, const BoundNode& node) {
            Entity model = NULL_ENTITY;
            if (!modelFor(runtime, node, 0, &model)) {
                return -1;
            }
            Model(op::scene(runtime), model).resetToBindPose();
            return 0;
        }

        // ---- Tween actions. A create node starts the action immediately and outputs its
        // entity, so the control nodes below can pause or stop it later.

        int actionMoveTo(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            const Entity target = op::target(runtime, node, 6);
            Transform* transform = op::transform(runtime, node, target);
            if (!transform) {
                return -1;
            }

            Vector3 start = op::vector3(runtime, node, 0);
            if (op::flag(runtime, node, 4)) {
                start = transform->position;
            }

            PositionAction action(world);
            action.setAction(start, op::vector3(runtime, node, 1),
                             static_cast<float>(op::number(runtime, node, 2)), op::flag(runtime, node, 3));
            action.setFunctionType(easeType(op::integer(runtime, node, 5)));
            op::outEntity(runtime, node, 0, publishAction(runtime, action, target, "Move To"));
            return 0;
        }

        int actionRotateTo(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            const Entity target = op::target(runtime, node, 5);
            if (!op::transform(runtime, node, target)) {
                return -1;
            }

            RotationAction action(world);
            action.setAction(op::quaternion(runtime, node, 0), op::quaternion(runtime, node, 1),
                             static_cast<float>(op::number(runtime, node, 2)), op::flag(runtime, node, 3));
            action.setFunctionType(easeType(op::integer(runtime, node, 4)));
            op::outEntity(runtime, node, 0, publishAction(runtime, action, target, "Rotate To"));
            return 0;
        }

        int actionSpin(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            const Entity target = op::target(runtime, node, 5);
            Transform* transform = op::transform(runtime, node, target);
            if (!transform) {
                return -1;
            }

            RotationAction action(world);
            action.setSpinAction(transform->rotation, op::vector3(runtime, node, 0),
                                 static_cast<float>(op::number(runtime, node, 1)),
                                 static_cast<float>(op::number(runtime, node, 2)), op::flag(runtime, node, 3));
            action.setFunctionType(easeType(op::integer(runtime, node, 4)));
            op::outEntity(runtime, node, 0, publishAction(runtime, action, target, "Spin"));
            return 0;
        }

        int actionScaleTo(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            const Entity target = op::target(runtime, node, 5);
            if (!op::transform(runtime, node, target)) {
                return -1;
            }

            ScaleAction action(world);
            action.setAction(op::vector3(runtime, node, 0), op::vector3(runtime, node, 1),
                             static_cast<float>(op::number(runtime, node, 2)), op::flag(runtime, node, 3));
            action.setFunctionType(easeType(op::integer(runtime, node, 4)));
            op::outEntity(runtime, node, 0, publishAction(runtime, action, target, "Scale To"));
            return 0;
        }

        // Colour and alpha land on the target's first submesh or its UI color, so a
        // target with neither would run a tween that visibly does nothing.
        bool paintable(GraphRuntime& runtime, const BoundNode& node, Entity target) {
            Scene* world = op::scene(runtime);
            if (world->findComponent<MeshComponent>(target) || world->findComponent<UIComponent>(target)) {
                return true;
            }
            op::warn(runtime, node, "target has no Mesh or UI component to paint");
            return false;
        }

        int actionFadeAlpha(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            const Entity target = op::target(runtime, node, 5);
            if (!op::alive(runtime, target) || !paintable(runtime, node, target)) {
                return -1;
            }

            AlphaAction action(world);
            action.setAction(static_cast<float>(op::number(runtime, node, 0)),
                             static_cast<float>(op::number(runtime, node, 1)),
                             static_cast<float>(op::number(runtime, node, 2)), op::flag(runtime, node, 3));
            action.setFunctionType(easeType(op::integer(runtime, node, 4)));
            op::outEntity(runtime, node, 0, publishAction(runtime, action, target, "Fade Alpha"));
            return 0;
        }

        int actionColorTo(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            const Entity target = op::target(runtime, node, 5);
            if (!op::alive(runtime, target) || !paintable(runtime, node, target)) {
                return -1;
            }

            ColorAction action(world);
            action.setAction(op::vector3(runtime, node, 0), op::vector3(runtime, node, 1),
                             static_cast<float>(op::number(runtime, node, 2)), op::flag(runtime, node, 3));
            action.setFunctionType(easeType(op::integer(runtime, node, 4)));
            op::outEntity(runtime, node, 0, publishAction(runtime, action, target, "Color To"));
            return 0;
        }

        int actionStart(GraphRuntime& runtime, const BoundNode& node) {
            const ActionHandle handle = actionPin(runtime, node, 0);
            if (!handle.action) {
                return -1;
            }
            op::startAction(*handle.action);
            return 0;
        }

        int actionPause(GraphRuntime& runtime, const BoundNode& node) {
            const ActionHandle handle = actionPin(runtime, node, 0);
            if (!handle.action) {
                return -1;
            }
            op::pauseAction(*handle.action);
            return 0;
        }

        int actionStop(GraphRuntime& runtime, const BoundNode& node) {
            const ActionHandle handle = actionPin(runtime, node, 0);
            if (!handle.action) {
                return -1;
            }
            op::stopAction(*handle.action);
            return 0;
        }

        int actionRestart(GraphRuntime& runtime, const BoundNode& node) {
            const ActionHandle handle = actionPin(runtime, node, 0);
            if (!handle.action) {
                return -1;
            }
            op::restartAction(*handle.action);
            return 0;
        }

        int actionSetSpeed(GraphRuntime& runtime, const BoundNode& node) {
            const ActionHandle handle = actionPin(runtime, node, 0);
            if (!handle.action) {
                return -1;
            }
            handle.action->speed = static_cast<float>(op::number(runtime, node, 1));
            return 0;
        }

        int actionSetWeight(GraphRuntime& runtime, const BoundNode& node) {
            const ActionHandle handle = actionPin(runtime, node, 0);
            if (!handle.action) {
                return -1;
            }
            handle.action->weight = static_cast<float>(op::number(runtime, node, 1));
            return 0;
        }

        int actionSetTarget(GraphRuntime& runtime, const BoundNode& node) {
            const ActionHandle handle = actionPin(runtime, node, 0);
            if (!handle.action) {
                return -1;
            }

            const Entity target = op::target(runtime, node, 1);
            if (target == NULL_ENTITY) {
                op::warn(runtime, node, "target entity does not exist");
                return -1;
            }

            // Rebinding a running action restarts it, which is what Action::setTarget does
            // and what a graph reparenting a tween expects.
            handle.action->target = target;
            if (handle.action->state == ActionState::Running) {
                handle.action->stopTrigger = true;
                handle.action->startTrigger = true;
            }
            return 0;
        }

        void actionIsRunning(GraphRuntime& runtime, const BoundNode& node) {
            const ActionHandle handle = actionPin(runtime, node, 0);
            op::outBool(runtime, node, 0, handle.action && handle.action->state == ActionState::Running);
        }

        void actionIsPaused(GraphRuntime& runtime, const BoundNode& node) {
            const ActionHandle handle = actionPin(runtime, node, 0);
            op::outBool(runtime, node, 0, handle.action && handle.action->state == ActionState::Paused);
        }

        void actionTime(GraphRuntime& runtime, const BoundNode& node) {
            const ActionHandle handle = actionPin(runtime, node, 0);
            op::outFloat(runtime, node, 0, handle.action ? handle.action->timecount : 0.0);
        }

        // The eased 0..1 output of the tween, which is what a graph feeds to a node that
        // has no action of its own (a value a shader or a UI slider reads).
        void actionValue(GraphRuntime& runtime, const BoundNode& node) {
            const ActionHandle handle = actionPin(runtime, node, 0);
            double value = 0.0;
            if (handle.action) {
                const TimedActionComponent* timed =
                    op::component<TimedActionComponent>(runtime, node, handle.entity, "TimedAction");
                if (timed) {
                    value = timed->value;
                }
            }
            op::outFloat(runtime, node, 0, value);
        }

        int actionStopAllOnTarget(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            const Entity target = op::target(runtime, node, 0);
            if (!world || target == NULL_ENTITY) {
                op::warn(runtime, node, "target entity does not exist");
                return -1;
            }

            ComponentArray<ActionComponent>* array = world->getComponentArray<ActionComponent>();
            if (!array) {
                return 0;
            }
            for (size_t i = 0; i < array->size(); ++i) {
                ActionComponent& action = array->getComponentFromIndex(i);
                if (action.target == target) {
                    op::stopAction(action);
                }
            }
            return 0;
        }

    }

    void registerAnimationOps() {
        registerGraphOp("anim.play", BoundOpKind::Exec, &animPlay, nullptr);
        registerGraphOp("anim.playIndex", BoundOpKind::Exec, &animPlayIndex, nullptr);
        registerGraphOp("anim.stopAll", BoundOpKind::Exec, &animStopAll, nullptr);
        registerGraphOp("anim.count", BoundOpKind::Pure, nullptr, &animCount);
        registerGraphOp("anim.name", BoundOpKind::Pure, nullptr, &animName);
        registerGraphOp("anim.entity", BoundOpKind::Pure, nullptr, &animEntityAt);
        registerGraphOp("anim.findByName", BoundOpKind::Pure, nullptr, &animFindByName);
        registerGraphOp("anim.isPlaying", BoundOpKind::Pure, nullptr, &animIsPlaying);
        registerGraphOp("anim.start", BoundOpKind::Exec, &animStart, nullptr);
        registerGraphOp("anim.setState", BoundOpKind::Exec, &animSetState, nullptr);
        registerGraphOp("anim.setLoop", BoundOpKind::Exec, &animSetLoop, nullptr);
        registerGraphOp("anim.isLoop", BoundOpKind::Pure, nullptr, &animIsLoop);
        registerGraphOp("anim.setSpeed", BoundOpKind::Exec, &animSetSpeed, nullptr);
        registerGraphOp("anim.setWeight", BoundOpKind::Exec, &animSetWeight, nullptr);
        registerGraphOp("anim.setDuration", BoundOpKind::Exec, &animSetDuration, nullptr);
        registerGraphOp("anim.progress", BoundOpKind::Pure, nullptr, &animProgress);
        registerGraphOp("anim.morphSet", BoundOpKind::Exec, &animMorphSet, nullptr);
        registerGraphOp("anim.morphGet", BoundOpKind::Pure, nullptr, &animMorphGet);
        registerGraphOp("anim.boneEntity", BoundOpKind::Pure, nullptr, &animBoneEntity);
        registerGraphOp("anim.bonePosition", BoundOpKind::Pure, nullptr, &animBonePosition);
        registerGraphOp("anim.resetBindPose", BoundOpKind::Exec, &animResetBindPose, nullptr);

        registerGraphOp("action.moveTo", BoundOpKind::Exec, &actionMoveTo, nullptr);
        registerGraphOp("action.rotateTo", BoundOpKind::Exec, &actionRotateTo, nullptr);
        registerGraphOp("action.spin", BoundOpKind::Exec, &actionSpin, nullptr);
        registerGraphOp("action.scaleTo", BoundOpKind::Exec, &actionScaleTo, nullptr);
        registerGraphOp("action.fadeAlpha", BoundOpKind::Exec, &actionFadeAlpha, nullptr);
        registerGraphOp("action.colorTo", BoundOpKind::Exec, &actionColorTo, nullptr);
        registerGraphOp("action.start", BoundOpKind::Exec, &actionStart, nullptr);
        registerGraphOp("action.pause", BoundOpKind::Exec, &actionPause, nullptr);
        registerGraphOp("action.stop", BoundOpKind::Exec, &actionStop, nullptr);
        registerGraphOp("action.restart", BoundOpKind::Exec, &actionRestart, nullptr);
        registerGraphOp("action.setSpeed", BoundOpKind::Exec, &actionSetSpeed, nullptr);
        registerGraphOp("action.setWeight", BoundOpKind::Exec, &actionSetWeight, nullptr);
        registerGraphOp("action.setTarget", BoundOpKind::Exec, &actionSetTarget, nullptr);
        registerGraphOp("action.isRunning", BoundOpKind::Pure, nullptr, &actionIsRunning);
        registerGraphOp("action.isPaused", BoundOpKind::Pure, nullptr, &actionIsPaused);
        registerGraphOp("action.time", BoundOpKind::Pure, nullptr, &actionTime);
        registerGraphOp("action.value", BoundOpKind::Pure, nullptr, &actionValue);
        registerGraphOp("action.stopAllOnTarget", BoundOpKind::Exec, &actionStopAllOnTarget, nullptr);
    }

}
}
