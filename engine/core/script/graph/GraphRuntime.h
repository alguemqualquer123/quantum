// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef GRAPH_RUNTIME_H
#define GRAPH_RUNTIME_H

#include "ExecutableGraph.h"

#include "Export.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace quantum {

class Scene;

namespace graph {

    struct GraphPendingDelay {
        double remaining = 0.0;
        std::vector<uint32_t> resume;
    };

    // One graph script running on one entity. The ExecutableGraph is shared
    // (cached by path); everything mutable lives here.
    struct GraphInstance {
        Scene* scene = nullptr;
        Entity entity = 0;
        size_t scriptIndex = 0;
        std::shared_ptr<ExecutableGraph> graph;
        std::vector<GraphValue> registers;
        std::vector<GraphValue> variables;
        std::vector<uint8_t> nodeState;
        // Per-node memory a byte cannot hold: a timer's elapsed time, a cooldown's
        // stamp, the value a "store" node captured. Indexed like nodeState.
        std::vector<GraphValue> nodeScratch;
        // Payload of the event being fired, read by the event.arg nodes.
        std::vector<GraphValue> eventArgs;
        std::vector<GraphPendingDelay> delays;
        std::string tag;
        bool dead = false;
    };

    // Interprets one ExecutableGraph inside one GraphInstance. Created on the
    // stack per event (or per tick for pending delays), so it holds no state
    // beyond the current fire.
    class QUANTUM_API GraphRuntime {
    public:
        GraphRuntime(GraphInstance& instance, const ExecutableGraph& graph);

        void fire(const std::string& eventId);
        void fire(const std::string& eventId, const std::vector<GraphValue>& args);
        void tick(float deltaTime);

        GraphInstance& getInstance() { return instance; }
        const ExecutableGraph& getGraph() const { return graph; }

        GraphValue readInput(const BoundNode& node, size_t index);
        void writeOutput(const BoundNode& node, size_t index, const GraphValue& value);
        void scheduleDelay(const BoundNode& node, double seconds);

        // Copies the event payload into one entry node's data outputs, starting at
        // `firstDataOutput` (always 0: an event node has no input pins, so its data
        // outputs are the payload). Values that are not already in the pin's type go
        // through convertTo; a payload that does not reach an output leaves it None.
        void runEventOutputs(const BoundNode& node, size_t firstDataOutput);

        // Runs the chain hanging off one exec output to completion and returns, so a
        // loop node owns its body's iterations. The continuation stack is swapped, not
        // shared: a body cannot unwind into the code that follows the loop.
        void runExecOutput(const BoundNode& node, size_t pin);

        // A break node stops the innermost enclosing loop body only.
        void requestBreak();
        bool consumeBreak();

        // Stops this fire after the node that called it and retires the instance: the
        // entity.destroy node uses it when a graph destroys its own entity, so the rest of
        // the chain cannot act on an id the pool has already handed to another entity.
        void retire();

        // Resumes at one exec output (or at the node itself, for a re-check) once the
        // seconds elapsed; pin 0xFFFFFFFF keeps scheduleDelay's "first output" behaviour.
        void scheduleResume(const BoundNode& node, double seconds, bool self);

        GraphValue scratchValue(const BoundNode& node) const;
        void writeScratch(const BoundNode& node, const GraphValue& value);

        size_t argCount() const;
        GraphValue readArg(size_t index) const;

        bool hasFailed() const { return failed; }
        // True once a node retired the instance: a loop owning a body must stop re-running
        // it, because the entity those nodes act on no longer exists.
        bool isStopped() const { return stopped; }

    private:
        GraphInstance& instance;
        const ExecutableGraph& graph;
        const std::vector<BoundNode>& nodes;
        std::vector<uint64_t> evalStamp;
        std::vector<uint8_t> evalMark;
        std::vector<uint32_t> continuations;
        uint64_t fireSerial = 0;
        uint32_t budget = 0;
        uint32_t breakRequests = 0;
        bool stopped = false;
        bool failed = false;

        void runFrom(uint32_t nodeIndex);
        void evalPure(uint32_t nodeIndex);
        GraphValue readInputValue(const BoundInput& input);
        bool hasScratch(const BoundNode& node) const;
        void pushExecTargets(const BoundNode& node, size_t pin);
        uint32_t popContinuation();
        void reportError(const BoundNode* node, const std::string& message);
        void startFire();
    };

    namespace GraphBinding {

        // Scans ScriptComponents for enabled GRAPH entries without an instance,
        // prepares their .gdat (cached by path) and subscribes event entries.
        QUANTUM_API void initializeGraphScripts(Scene* scene);
        QUANTUM_API void cleanupGraphScripts(Scene* scene);
        QUANTUM_API void cleanupGraphScripts(Scene* scene, Entity entity);
        QUANTUM_API void cleanupAll();
        QUANTUM_API size_t getInstanceCount();

        // Fires an event id on every live graph instance of one entity and returns how
        // many caught it. Custom events ("custom:<name>") have no engine signal to ride
        // on, so the event.dispatch node reaches them here.
        QUANTUM_API size_t dispatchEvent(Scene* scene, Entity entity, const std::string& eventId,
                                        const std::vector<GraphValue>& args);

        // Same, for every instance of every entity in the scene.
        QUANTUM_API size_t dispatchEventAll(Scene* scene, const std::string& eventId,
                                           const std::vector<GraphValue>& args);
    }
}
}

#endif //GRAPH_RUNTIME_H
