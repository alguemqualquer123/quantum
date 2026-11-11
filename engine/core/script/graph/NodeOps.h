// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef GRAPH_NODE_OPS_H
#define GRAPH_NODE_OPS_H

#include "GraphRuntime.h"
#include "NodeDefs.h"

#include "Log.h"
#include "Scene.h"
#include "component/ActionComponent.h"
#include "component/Transform.h"

#include <string>

namespace quantum {
namespace graph {

    // NodeDefs owns the single kind -> operation table. Each group file appends its
    // own rows from one registerXOps() entry point, and NodeDefs calls them all
    // before the first lookup, so no static-initialisation order is involved.
    void registerGraphOp(const char* kind, BoundOpKind opKind, GraphExecFn exec, GraphPureFn pure);

    void registerFlowOps();
    void registerVariableOps();
    void registerMathOps();
    void registerLogicOps();
    void registerEntityOps();
    void registerAnimationOps();
    void registerPhysicsOps();
    void registerAudioOps();
    void registerSystemOps();
    void registerReflectionOps();

    // Shared pin/entity plumbing for the group files. Pin ordinals are the order the
    // pins are declared in NodeCatalog, so a handler reads its inputs by position and
    // never by a name that a variadic node could have regenerated.
    namespace op {

        inline GraphInstance& instance(GraphRuntime& runtime) {
            return runtime.getInstance();
        }

        inline Scene* scene(GraphRuntime& runtime) {
            return runtime.getInstance().scene;
        }

        inline Entity selfEntity(GraphRuntime& runtime) {
            return runtime.getInstance().entity;
        }

        inline GraphValue in(GraphRuntime& runtime, const BoundNode& node, size_t index) {
            return runtime.readInput(node, index);
        }

        inline void out(GraphRuntime& runtime, const BoundNode& node, size_t index, const GraphValue& value) {
            runtime.writeOutput(node, index, value);
        }

        inline void outBool(GraphRuntime& runtime, const BoundNode& node, size_t index, bool v) {
            runtime.writeOutput(node, index, GraphValue::makeBool(v));
        }

        inline void outInt(GraphRuntime& runtime, const BoundNode& node, size_t index, int64_t v) {
            runtime.writeOutput(node, index, GraphValue::makeInt(v));
        }

        inline void outFloat(GraphRuntime& runtime, const BoundNode& node, size_t index, double v) {
            runtime.writeOutput(node, index, GraphValue::makeFloat(v));
        }

        inline void outString(GraphRuntime& runtime, const BoundNode& node, size_t index, const std::string& v) {
            runtime.writeOutput(node, index, GraphValue::makeString(v));
        }

        inline void outVector2(GraphRuntime& runtime, const BoundNode& node, size_t index, const Vector2& v) {
            runtime.writeOutput(node, index, GraphValue::makeVector2(v));
        }

        inline void outVector3(GraphRuntime& runtime, const BoundNode& node, size_t index, const Vector3& v) {
            runtime.writeOutput(node, index, GraphValue::makeVector3(v));
        }

        inline void outVector4(GraphRuntime& runtime, const BoundNode& node, size_t index, const Vector4& v) {
            runtime.writeOutput(node, index, GraphValue::makeVector4(v));
        }

        inline void outColor3(GraphRuntime& runtime, const BoundNode& node, size_t index, const Vector3& v) {
            runtime.writeOutput(node, index, GraphValue::makeColor3(v));
        }

        inline void outColor4(GraphRuntime& runtime, const BoundNode& node, size_t index, const Vector4& v) {
            runtime.writeOutput(node, index, GraphValue::makeColor4(v));
        }

        inline void outQuaternion(GraphRuntime& runtime, const BoundNode& node, size_t index, const Quaternion& v) {
            runtime.writeOutput(node, index, GraphValue::makeQuaternion(v));
        }

        inline void outRect(GraphRuntime& runtime, const BoundNode& node, size_t index, const Rect& v) {
            runtime.writeOutput(node, index, GraphValue::makeRect(v));
        }

        inline void outEntity(GraphRuntime& runtime, const BoundNode& node, size_t index, Entity v) {
            runtime.writeOutput(node, index, GraphValue::makeEntity(v));
        }

        inline bool flag(GraphRuntime& runtime, const BoundNode& node, size_t index) {
            return runtime.readInput(node, index).asBool();
        }

        inline int64_t integer(GraphRuntime& runtime, const BoundNode& node, size_t index) {
            const GraphValue value = runtime.readInput(node, index);
            return value.type == GraphValueType::String ? 0 : value.asInt();
        }

        inline double number(GraphRuntime& runtime, const BoundNode& node, size_t index) {
            const GraphValue value = runtime.readInput(node, index);
            return value.type == GraphValueType::String ? 0.0 : value.asFloat();
        }

        inline std::string text(GraphRuntime& runtime, const BoundNode& node, size_t index) {
            return runtime.readInput(node, index).asString();
        }

        inline Vector2 vector2(GraphRuntime& runtime, const BoundNode& node, size_t index) {
            return runtime.readInput(node, index).asVector2();
        }

        inline Vector3 vector3(GraphRuntime& runtime, const BoundNode& node, size_t index) {
            return runtime.readInput(node, index).asVector3();
        }

        inline Vector4 vector4(GraphRuntime& runtime, const BoundNode& node, size_t index) {
            return runtime.readInput(node, index).asVector4();
        }

        inline Quaternion quaternion(GraphRuntime& runtime, const BoundNode& node, size_t index) {
            return runtime.readInput(node, index).asQuaternion();
        }

        inline Rect rect(GraphRuntime& runtime, const BoundNode& node, size_t index) {
            return runtime.readInput(node, index).asRect();
        }

        // An unconnected target pin holds the None value, which means "the entity
        // running this graph" - what almost every gameplay node wants by default.
        // A connected pin whose entity is gone resolves to NULL_ENTITY so the caller
        // can report it instead of acting on a recycled id.
        inline Entity target(GraphRuntime& runtime, const BoundNode& node, size_t pin) {
            GraphInstance& inst = runtime.getInstance();
            const GraphValue value = runtime.readInput(node, pin);
            if (value.isNone()) {
                return inst.entity;
            }
            const Entity id = value.type == GraphValueType::Entity
                ? value.asEntity()
                : static_cast<Entity>(value.asInt());
            if (id == NULL_ENTITY) {
                return inst.entity;
            }
            if (!inst.scene || !inst.scene->isEntityCreated(id)) {
                return NULL_ENTITY;
            }
            return id;
        }

        inline bool alive(GraphRuntime& runtime, Entity entity) {
            return entity != NULL_ENTITY && runtime.getInstance().scene &&
                   runtime.getInstance().scene->isEntityCreated(entity);
        }

        // One warning per node execution, tagged with the graph and the node so a
        // gameplay graph that wired the wrong entity is diagnosable from a log file.
        inline void warn(GraphRuntime& runtime, const BoundNode& node, const std::string& message) {
            Log::error("graph '%s': node %u (%s): %s",
                       runtime.getGraph().getAsset().name.c_str(), node.id,
                       node.source ? node.source->kind.c_str() : "?",
                       message.c_str());
        }

        inline Transform* transform(GraphRuntime& runtime, const BoundNode& node, Entity entity) {
            if (!alive(runtime, entity)) {
                warn(runtime, node, "target entity does not exist");
                return nullptr;
            }
            Transform* component = scene(runtime)->findComponent<Transform>(entity);
            if (!component) {
                warn(runtime, node, "target entity has no Transform");
            }
            return component;
        }

        template <typename T>
        inline T* component(GraphRuntime& runtime, const BoundNode& node, Entity entity, const char* name) {
            if (!alive(runtime, entity)) {
                warn(runtime, node, "target entity does not exist");
                return nullptr;
            }
            T* component = scene(runtime)->findComponent<T>(entity);
            if (!component) {
                warn(runtime, node, std::string("target entity has no ") + name);
            }
            return component;
        }

        // The Action lifecycle, kept in sync with quantum::Action: the triggers are
        // consumed by ActionSystem on the next update, and start() means "ensure
        // running" - a repeated call must not leave a trigger that would resurrect the
        // action after a later stop. Graph nodes drive the component directly because
        // Action::getComponent() throws when the entity has no ActionComponent, and a
        // throw is fatal under this repo's _HAS_EXCEPTIONS=0.
        inline void startAction(ActionComponent& action) {
            action.startTrigger = action.state != ActionState::Running;
            action.stopTrigger = false;
            action.pauseTrigger = false;
        }

        inline void stopAction(ActionComponent& action) {
            action.startTrigger = false;
            action.stopTrigger = true;
            action.pauseTrigger = false;
        }

        inline void pauseAction(ActionComponent& action) {
            action.pauseTrigger = true;
        }

        inline void restartAction(ActionComponent& action) {
            action.startTrigger = true;
            action.stopTrigger = action.state != ActionState::Stopped;
            action.pauseTrigger = false;
        }

    }

}
}

#endif //GRAPH_NODE_OPS_H
