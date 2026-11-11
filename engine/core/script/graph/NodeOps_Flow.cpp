// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "NodeOps.h"

#include "Engine.h"

#include <cmath>

namespace quantum {
namespace graph {

    namespace {

        // A loop whose body never runs (nothing wired, or a condition a graph cannot
        // change) must still terminate: the interpreter's node budget covers executed
        // nodes only, so a spin in one of these handlers needs its own cap.
        const size_t MAX_LOOP_ITERATIONS = 100000;

        int eventEntry(GraphRuntime& runtime, const BoundNode& node) {
            runtime.runEventOutputs(node, 0);
            return 0;
        }

        int flowBranch(GraphRuntime& runtime, const BoundNode& node) {
            return op::flag(runtime, node, 0) ? 0 : 1;
        }

        int flowSequence(GraphRuntime&, const BoundNode&) {
            // The runtime schedules the remaining exec outputs before continuing.
            return 0;
        }

        int flowDoOnce(GraphRuntime& runtime, const BoundNode& node) {
            std::vector<uint8_t>& state = op::instance(runtime).nodeState;
            if (node.index >= state.size()) return -1;
            if (state[node.index]) return -1;
            state[node.index] = 1;
            return 0;
        }

        int flowDelay(GraphRuntime& runtime, const BoundNode& node) {
            runtime.scheduleDelay(node, op::number(runtime, node, 0));
            return -1;
        }

        int flowNextFrame(GraphRuntime& runtime, const BoundNode& node) {
            runtime.scheduleDelay(node, 0.0);
            return -1;
        }

        // Writes the running index into the node's data output and owns the body: the
        // chain under "loop" is re-run per iteration, and "completed" only fires after.
        int flowForLoop(GraphRuntime& runtime, const BoundNode& node) {
            int64_t index = op::integer(runtime, node, 0);
            const int64_t last = op::integer(runtime, node, 1);
            int64_t step = op::integer(runtime, node, 2);
            if (step == 0) {
                step = 1;        // a zero step would never leave the loop
            }
            const bool upward = step > 0;

            size_t iterations = 0;
            for (; (upward ? index <= last : index >= last); index += step) {
                op::outInt(runtime, node, 0, index);
                runtime.runExecOutput(node, 0);
                if (runtime.consumeBreak() || runtime.hasFailed() || runtime.isStopped()) break;
                if (++iterations >= MAX_LOOP_ITERATIONS) {
                    op::warn(runtime, node, "for loop hit the iteration cap");
                    break;
                }
            }

            return (runtime.hasFailed() || runtime.isStopped()) ? -1 : 1;
        }

        int flowRepeat(GraphRuntime& runtime, const BoundNode& node) {
            const int64_t count = op::integer(runtime, node, 0);
            size_t iterations = 0;
            for (int64_t i = 0; i < count; ++i) {
                op::outInt(runtime, node, 0, i);
                runtime.runExecOutput(node, 0);
                if (runtime.consumeBreak() || runtime.hasFailed() || runtime.isStopped()) break;
                if (++iterations >= MAX_LOOP_ITERATIONS) {
                    op::warn(runtime, node, "repeat hit the iteration cap");
                    break;
                }
            }

            return (runtime.hasFailed() || runtime.isStopped()) ? -1 : 1;
        }

        int flowWhileLoop(GraphRuntime& runtime, const BoundNode& node) {
            size_t iterations = 0;
            while (op::flag(runtime, node, 0)) {
                runtime.runExecOutput(node, 0);
                if (runtime.consumeBreak() || runtime.hasFailed() || runtime.isStopped()) break;
                if (++iterations >= MAX_LOOP_ITERATIONS) {
                    op::warn(runtime, node, "while loop hit the iteration cap");
                    break;
                }
            }

            return (runtime.hasFailed() || runtime.isStopped()) ? -1 : 1;
        }

        int flowBreak(GraphRuntime& runtime, const BoundNode&) {
            runtime.requestBreak();
            return -1;
        }

        int flowGate(GraphRuntime& runtime, const BoundNode& node) {
            return op::flag(runtime, node, 0) ? 0 : -1;
        }

        int flowFlipFlop(GraphRuntime& runtime, const BoundNode& node) {
            std::vector<uint8_t>& state = op::instance(runtime).nodeState;
            if (node.index >= state.size()) return -1;

            if (op::flag(runtime, node, 0)) {
                state[node.index] = 0;
            }

            const int pin = state[node.index] ? 1 : 0;
            state[node.index] = state[node.index] ? 0 : 1;
            return pin;
        }

        int flowCooldown(GraphRuntime& runtime, const BoundNode& node) {
            const double now = Engine::getSystemTime();
            const double last = runtime.scratchValue(node).asFloat();
            const double seconds = op::number(runtime, node, 0);

            if (last > 0.0 && now - last < seconds) {
                return 1;
            }

            op::out(runtime, node, 0, GraphValue::makeFloat(now));
            runtime.writeScratch(node, GraphValue::makeFloat(now));
            return 0;
        }

        int flowWait(GraphRuntime& runtime, const BoundNode& node) {
            if (op::flag(runtime, node, 0)) {
                runtime.writeScratch(node, GraphValue::makeNone());
                return 0;
            }

            const double now = Engine::getSystemTime();
            const double timeout = op::number(runtime, node, 1);
            double started = runtime.scratchValue(node).asFloat();
            if (started <= 0.0) {
                started = now;
                runtime.writeScratch(node, GraphValue::makeFloat(started));
            }

            if (timeout > 0.0 && now - started >= timeout) {
                runtime.writeScratch(node, GraphValue::makeNone());
                return 1;
            }

            const double interval = op::number(runtime, node, 2);
            runtime.scheduleResume(node, interval > 0.0 ? interval : 0.05, true);
            return -1;
        }

        int flowSwitchInt(GraphRuntime& runtime, const BoundNode& node) {
            if (node.execNames.empty()) return -1;
            const int defaultPin = static_cast<int>(node.execNames.size()) - 1;
            const int64_t value = op::integer(runtime, node, 0);
            if (value >= 0 && value < static_cast<int64_t>(defaultPin)) {
                return static_cast<int>(value);
            }
            return defaultPin;
        }

        // The case labels come from the node's "case0".."caseN-1" params, which are the
        // same strings the editor draws on the pins; the pin order is the case order.
        int flowSwitchString(GraphRuntime& runtime, const BoundNode& node) {
            if (node.execNames.empty()) return -1;
            const int defaultPin = static_cast<int>(node.execNames.size()) - 1;
            const std::string value = op::text(runtime, node, 0);

            if (node.source) {
                for (int c = 0; c < defaultPin; ++c) {
                    if (node.source->getStringParam("case" + std::to_string(c), "") == value) {
                        return c;
                    }
                }
            }

            return defaultPin;
        }

        std::vector<GraphValue> dispatchPayload(GraphRuntime& runtime, const BoundNode& node, size_t first) {
            std::vector<GraphValue> args;
            args.reserve(3);
            for (size_t p = first; p < first + 3 && p < node.inputs.size(); ++p) {
                args.push_back(runtime.readInput(node, p));
            }
            return args;
        }

        int eventDispatch(GraphRuntime& runtime, const BoundNode& node) {
            GraphInstance& inst = op::instance(runtime);
            if (!inst.scene) return 0;

            const Entity target = op::target(runtime, node, 1);
            if (target == NULL_ENTITY) {
                op::warn(runtime, node, "dispatch target entity does not exist");
                return 0;
            }

            GraphBinding::dispatchEvent(inst.scene, target,
                                        std::string(GRAPH_CUSTOM_EVENT_PREFIX) + op::text(runtime, node, 0),
                                        dispatchPayload(runtime, node, 2));
            return 0;
        }

        int eventDispatchAll(GraphRuntime& runtime, const BoundNode& node) {
            GraphInstance& inst = op::instance(runtime);
            if (!inst.scene) return 0;

            GraphBinding::dispatchEventAll(inst.scene,
                                           std::string(GRAPH_CUSTOM_EVENT_PREFIX) + op::text(runtime, node, 0),
                                           dispatchPayload(runtime, node, 1));
            return 0;
        }

    }

    void registerFlowOps() {
        static const char* const kEntryEvents[] = {
            "event.start", "event.update", "event.fixedUpdate", "event.postUpdate", "event.draw",
            "event.pause", "event.resume", "event.shutdown", "event.sceneLoaded",
            "event.keyDown", "event.keyUp", "event.charInput",
            "event.mouseDown", "event.mouseUp", "event.mouseMove", "event.mouseScroll",
            "event.mouseEnter", "event.mouseLeave",
            "event.touchStart", "event.touchEnd", "event.touchMove", "event.touchCancel",
            "event.gamepadButtonDown", "event.gamepadButtonUp", "event.gamepadAxisMove",
            "event.gamepadConnect", "event.gamepadDisconnect", "event.custom"
        };
        for (const char* kind : kEntryEvents) {
            registerGraphOp(kind, BoundOpKind::Exec, &eventEntry, nullptr);
        }

        registerGraphOp("event.dispatch", BoundOpKind::Exec, &eventDispatch, nullptr);
        registerGraphOp("event.dispatchAll", BoundOpKind::Exec, &eventDispatchAll, nullptr);

        registerGraphOp("flow.branch", BoundOpKind::Exec, &flowBranch, nullptr);
        registerGraphOp("flow.sequence", BoundOpKind::Exec, &flowSequence, nullptr);
        registerGraphOp("flow.doOnce", BoundOpKind::Exec, &flowDoOnce, nullptr);
        registerGraphOp("flow.delay", BoundOpKind::Exec, &flowDelay, nullptr);
        registerGraphOp("flow.nextFrame", BoundOpKind::Exec, &flowNextFrame, nullptr);
        registerGraphOp("flow.forLoop", BoundOpKind::Exec, &flowForLoop, nullptr);
        registerGraphOp("flow.repeat", BoundOpKind::Exec, &flowRepeat, nullptr);
        registerGraphOp("flow.whileLoop", BoundOpKind::Exec, &flowWhileLoop, nullptr);
        registerGraphOp("flow.break", BoundOpKind::Exec, &flowBreak, nullptr);
        registerGraphOp("flow.gate", BoundOpKind::Exec, &flowGate, nullptr);
        registerGraphOp("flow.flipFlop", BoundOpKind::Exec, &flowFlipFlop, nullptr);
        registerGraphOp("flow.cooldown", BoundOpKind::Exec, &flowCooldown, nullptr);
        registerGraphOp("flow.wait", BoundOpKind::Exec, &flowWait, nullptr);
        registerGraphOp("flow.switchInt", BoundOpKind::Exec, &flowSwitchInt, nullptr);
        registerGraphOp("flow.switchString", BoundOpKind::Exec, &flowSwitchString, nullptr);
    }

}
}
