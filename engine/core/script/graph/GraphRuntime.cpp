// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "GraphRuntime.h"

#include "GraphReader.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/ScriptComponent.h"
#include "io/Data.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>

namespace quantum {
namespace graph {

    namespace {

        const uint32_t INVALID_NODE = 0xFFFFFFFFu;
        const uint32_t MAX_NODES_PER_FIRE = 50000;
        const char* TICK_TAG = "_graph_runtime_tick_";

        std::vector<std::unique_ptr<GraphInstance>> graphInstances;
        std::map<std::string, std::shared_ptr<ExecutableGraph>> graphCache;
        bool tickSubscribed = false;

        std::string makeInstanceTag(GraphInstance* instance) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "_graph_%p_", static_cast<void*>(instance));
            return std::string(buffer);
        }

        void applyPropertyOverrides(GraphInstance& instance, const ScriptEntry& entry) {
            const GraphAsset& asset = instance.graph->getAsset();
            for (const ScriptProperty& property : entry.properties) {
                for (size_t i = 0; i < asset.variables.size(); ++i) {
                    const GraphVariable& variable = asset.variables[i];
                    if (!variable.exposed || variable.name != property.name) continue;

                    switch (variable.type) {
                        case GraphValueType::Bool:
                            if (const bool* value = std::get_if<bool>(&property.value)) {
                                instance.variables[i] = GraphValue::makeBool(*value);
                            }
                            break;
                        case GraphValueType::Int:
                            if (const int* value = std::get_if<int>(&property.value)) {
                                instance.variables[i] = GraphValue::makeInt(*value);
                            } else if (const float* value = std::get_if<float>(&property.value)) {
                                instance.variables[i] = GraphValue::makeInt(static_cast<int64_t>(*value));
                            }
                            break;
                        case GraphValueType::Float:
                            if (const float* value = std::get_if<float>(&property.value)) {
                                instance.variables[i] = GraphValue::makeFloat(*value);
                            } else if (const int* value = std::get_if<int>(&property.value)) {
                                instance.variables[i] = GraphValue::makeFloat(*value);
                            }
                            break;
                        case GraphValueType::String:
                            if (const std::string* value = std::get_if<std::string>(&property.value)) {
                                instance.variables[i] = GraphValue::makeString(*value);
                            }
                            break;
                        case GraphValueType::Vector2:
                            if (const Vector2* value = std::get_if<Vector2>(&property.value)) {
                                instance.variables[i] = GraphValue::makeVector2(*value);
                            }
                            break;
                        case GraphValueType::Vector3:
                            if (const Vector3* value = std::get_if<Vector3>(&property.value)) {
                                instance.variables[i] = GraphValue::makeVector3(*value);
                            }
                            break;
                        case GraphValueType::Vector4:
                            if (const Vector4* value = std::get_if<Vector4>(&property.value)) {
                                instance.variables[i] = GraphValue::makeVector4(*value);
                            }
                            break;
                        case GraphValueType::Color3:
                            if (const Vector3* value = std::get_if<Vector3>(&property.value)) {
                                instance.variables[i] = GraphValue::makeColor3(*value);
                            }
                            break;
                        case GraphValueType::Color4:
                            if (const Vector4* value = std::get_if<Vector4>(&property.value)) {
                                instance.variables[i] = GraphValue::makeColor4(*value);
                            }
                            break;
                        case GraphValueType::Entity:
                            if (const EntityReference* value = std::get_if<EntityReference>(&property.value)) {
                                instance.variables[i] = GraphValue::makeEntity(value->entity);
                            }
                            break;
                        default:
                            break;
                    }
                    break;
                }
            }
        }

        std::shared_ptr<ExecutableGraph> loadGraph(const std::string& path, std::string& error) {
            std::map<std::string, std::shared_ptr<ExecutableGraph>>::const_iterator cached = graphCache.find(path);
            if (cached != graphCache.end()) {
                return cached->second;
            }

            Data data;
            const std::string assetPath = "asset://" + path;
            if (data.open(assetPath.c_str()) != FILEDATA_OK) {
                error = "cannot open '" + assetPath + "'";
                return nullptr;
            }

            std::vector<uint8_t> bytes(data.getMemPtr(), data.getMemPtr() + data.length());
            GraphAsset asset;
            GraphBlobInfo info;
            if (!GraphReader::read(bytes, asset, info, error)) {
                error = "'" + assetPath + "': " + error;
                return nullptr;
            }

            std::shared_ptr<ExecutableGraph> graph = std::make_shared<ExecutableGraph>();
            if (!graph->prepare(asset, error)) {
                error = "'" + assetPath + "': " + error;
                return nullptr;
            }

            graphCache[path] = graph;
            return graph;
        }

        void tickAll() {
            if (graphInstances.empty()) return;

            const float deltaTime = Engine::getDeltatime();
            for (size_t i = 0; i < graphInstances.size(); ++i) {
                GraphInstance* instance = graphInstances[i].get();
                if (!instance || instance->dead || instance->delays.empty() || !instance->graph) {
                    continue;
                }
                if (!instance->scene || !instance->scene->isEntityCreated(instance->entity)) {
                    continue;
                }
                GraphRuntime runtime(*instance, *instance->graph);
                runtime.tick(deltaTime);
            }
        }

        void ensureTickSubscription() {
            if (tickSubscribed) return;
            Engine::onUpdate.add(TICK_TAG, &tickAll);
            tickSubscribed = true;
        }

        void stopTickSubscription() {
            if (!tickSubscribed) return;
            Engine::onUpdate.remove(TICK_TAG);
            tickSubscribed = false;
        }

        void subscribeGraphEvent(GraphInstance& instance, const std::string& eventId) {
            const std::string tag = instance.tag + eventId;

            // Parametric input events carry their filter in the id ("keydown:87"), so one
            // subscription per node instead of a global one that would fire for every key.
            const size_t separator = eventId.find(':');
            const std::string base = separator == std::string::npos ? eventId : eventId.substr(0, separator);
            int filter = -1;
            if (separator != std::string::npos) {
                filter = std::atoi(eventId.c_str() + separator + 1);
            }

            std::function<void()> callback = [&instance, eventId]() {
                if (instance.dead || !instance.graph) return;
                GraphRuntime runtime(instance, *instance.graph);
                runtime.fire(eventId);
            };

            // The payloads below are ordered exactly like the event node's data
            // outputs in NodeCatalog, because runEventOutputs fills them positionally.
            auto emit = [&instance, eventId](const std::vector<GraphValue>& args) {
                if (instance.dead || !instance.graph) return;
                GraphRuntime runtime(instance, *instance.graph);
                runtime.fire(eventId, args);
            };

            if (eventId == "update") {
                Engine::onUpdate.add(tag, callback);
            } else if (eventId == "fixedUpdate") {
                Engine::onFixedUpdate.add(tag, callback);
            } else if (eventId == "postUpdate") {
                Engine::onPostUpdate.add(tag, callback);
            } else if (eventId == "draw") {
                Engine::onDraw.add(tag, callback);
            } else if (eventId == "pause") {
                Engine::onPause.add(tag, callback);
            } else if (eventId == "resume") {
                Engine::onResume.add(tag, callback);
            } else if (eventId == "shutdown") {
                Engine::onShutdown.add(tag, callback);
            } else if (eventId == "sceneLoaded") {
                Engine::onSceneLoaded.add(tag, callback);
            } else if (eventId == "mouseEnter") {
                Engine::onMouseEnter.add(tag, callback);
            } else if (eventId == "mouseLeave") {
                Engine::onMouseLeave.add(tag, callback);
            } else if (base == "keydown" || base == "keyup") {
                FunctionSubscribe<void(int, bool, int)>& signal =
                    base == "keydown" ? Engine::onKeyDown : Engine::onKeyUp;
                const std::string id = eventId;
                signal.add(tag, [&instance, id, filter, emit](int key, bool repeat, int mods) {
                    if (filter >= 0 && key != filter) return;
                    std::vector<GraphValue> args;
                    args.push_back(GraphValue::makeInt(key));
                    args.push_back(GraphValue::makeBool(repeat));
                    args.push_back(GraphValue::makeInt(mods));
                    emit(args);
                });
            } else if (base == "mousedown" || base == "mouseup") {
                FunctionSubscribe<void(int, float, float, int)>& signal =
                    base == "mousedown" ? Engine::onMouseDown : Engine::onMouseUp;
                const std::string id = eventId;
                signal.add(tag, [&instance, id, filter, emit](int button, float x, float y, int mods) {
                    if (filter >= 0 && button != filter) return;
                    std::vector<GraphValue> args;
                    args.push_back(GraphValue::makeInt(button));
                    args.push_back(GraphValue::makeVector2(Vector2(x, y)));
                    args.push_back(GraphValue::makeInt(mods));
                    emit(args);
                });
            } else if (base == "mousemove") {
                Engine::onMouseMove.add(tag, [&instance, eventId, emit](float x, float y, int buttons) {
                    std::vector<GraphValue> args;
                    args.push_back(GraphValue::makeVector2(Vector2(x, y)));
                    args.push_back(GraphValue::makeInt(buttons));
                    emit(args);
                });
            } else if (base == "mousescroll") {
                Engine::onMouseScroll.add(tag, [&instance, eventId, emit](float dx, float dy, int buttons) {
                    std::vector<GraphValue> args;
                    args.push_back(GraphValue::makeVector2(Vector2(dx, dy)));
                    args.push_back(GraphValue::makeInt(buttons));
                    emit(args);
                });
            } else if (base == "touchstart" || base == "touchend" ||
                       base == "touchmove" || base == "touchcancel") {
                FunctionSubscribe<void(int, float, float)>& signal =
                    base == "touchstart" ? Engine::onTouchStart
                  : base == "touchend"   ? Engine::onTouchEnd
                  : base == "touchmove"  ? Engine::onTouchMove
                                         : Engine::onTouchCancel;
                signal.add(tag, [&instance, eventId, emit](int pointer, float x, float y) {
                    std::vector<GraphValue> args;
                    args.push_back(GraphValue::makeInt(pointer));
                    args.push_back(GraphValue::makeVector2(Vector2(x, y)));
                    emit(args);
                });
            } else if (base == "gamepaddown" || base == "gamepadup") {
                // Both halves of one node id would double-subscribe the same graph, so
                // down and up are separate ids; the second signal is registered under a
                // prefix of the instance tag, which is what removeSubscriptionsByTag
                // matches when the instance dies.
                FunctionSubscribe<void(int, int)>& signal =
                    base == "gamepaddown" ? Engine::onGamepadButtonDown : Engine::onGamepadButtonUp;
                const std::string upTag = tag + "_up";
                signal.add(base == "gamepaddown" ? tag : upTag,
                           [&instance, eventId, filter, emit](int pad, int button) {
                    if (filter >= 0 && button != filter) return;
                    std::vector<GraphValue> args;
                    args.push_back(GraphValue::makeInt(pad));
                    args.push_back(GraphValue::makeInt(button));
                    emit(args);
                });
            } else if (base == "gamepadaxis") {
                Engine::onGamepadAxisMove.add(tag, [&instance, eventId, filter, emit](int pad, int axis, float value) {
                    if (filter >= 0 && axis != filter) return;
                    std::vector<GraphValue> args;
                    args.push_back(GraphValue::makeInt(pad));
                    args.push_back(GraphValue::makeInt(axis));
                    args.push_back(GraphValue::makeFloat(value));
                    emit(args);
                });
            } else if (base == "gamepadconnect" || base == "gamepaddisconnect") {
                FunctionSubscribe<void(int)>& signal =
                    base == "gamepadconnect" ? Engine::onGamepadConnect : Engine::onGamepadDisconnect;
                signal.add(tag, [&instance, eventId, emit](int pad) {
                    std::vector<GraphValue> args;
                    args.push_back(GraphValue::makeInt(pad));
                    emit(args);
                });
            } else if (base == "char") {
                Engine::onCharInput.add(tag, [&instance, eventId, emit](wchar_t character) {
                    std::vector<GraphValue> args;
                    args.push_back(GraphValue::makeInt(static_cast<int64_t>(character)));
                    emit(args);
                });
            } else if (eventId.compare(0, 7, "custom:") == 0) {
                // Reached through GraphBinding::dispatchEvent, which is what the
                // event.dispatch node calls; there is no engine signal to subscribe to.
            } else {
                Log::warn("graph '%s': event '%s' has no runtime dispatch",
                          instance.graph->getAsset().name.c_str(), eventId.c_str());
            }
        }

        void destroyInstance(GraphInstance* instance) {
            if (!instance) return;
            Engine::removeSubscriptionsByTag(instance->tag);
            instance->dead = true;

            if (instance->scene && instance->scene->isEntityCreated(instance->entity)) {
                ScriptComponent* component = instance->scene->findComponent<ScriptComponent>(instance->entity);
                if (component && instance->scriptIndex < component->scripts.size()) {
                    ScriptEntry& entry = component->scripts[instance->scriptIndex];
                    if (entry.instance == instance) {
                        entry.instance = nullptr;
                    }
                }
            }
        }

        void maybeStopTickSubscription() {
            if (graphInstances.empty()) {
                stopTickSubscription();
            }
        }

    }

    GraphRuntime::GraphRuntime(GraphInstance& instance, const ExecutableGraph& graph)
        : instance(instance),
          graph(graph),
          nodes(graph.getNodes()),
          evalStamp(graph.getNodes().size(), 0),
          evalMark(graph.getNodes().size(), 0) {
    }

    void GraphRuntime::startFire() {
        if (++fireSerial == 0) {
            std::fill(evalStamp.begin(), evalStamp.end(), 0);
            fireSerial = 1;
        }
        breakRequests = 0;
        stopped = false;
        failed = false;
    }

    void GraphRuntime::retire() {
        stopped = true;
        continuations.clear();
        instance.delays.clear();
        // The subscriptions are left in place on purpose: this runs from inside a node of
        // a chain that an engine signal may itself have started, and removing a callback
        // while its list is being walked is a different bug. dead already stops every
        // entry point, and the scene cleanup releases the instance.
        instance.dead = true;
    }

    void GraphRuntime::fire(const std::string& eventId) {
        fire(eventId, std::vector<GraphValue>());
    }

    void GraphRuntime::fire(const std::string& eventId, const std::vector<GraphValue>& args) {
        if (instance.dead) return;

        const std::vector<uint32_t>* entries = graph.findEventEntries(eventId);
        if (!entries) return;

        if (!instance.scene || !instance.scene->isEntityCreated(instance.entity)) return;

        instance.eventArgs = args;

        for (size_t e = 0; e < entries->size(); ++e) {
            startFire();
            continuations.clear();
            budget = MAX_NODES_PER_FIRE;
            runFrom((*entries)[e]);
            if (failed) break;
        }
    }

    void GraphRuntime::tick(float deltaTime) {
        if (instance.dead || instance.delays.empty()) return;
        if (!instance.scene || !instance.scene->isEntityCreated(instance.entity)) return;

        std::vector<GraphPendingDelay> due;
        for (size_t i = 0; i < instance.delays.size();) {
            instance.delays[i].remaining -= deltaTime;
            if (instance.delays[i].remaining <= 0.0f) {
                due.push_back(std::move(instance.delays[i]));
                instance.delays.erase(instance.delays.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }

        for (size_t d = 0; d < due.size() && !failed && !stopped; ++d) {
            startFire();
            continuations.clear();
            budget = MAX_NODES_PER_FIRE;
            for (size_t r = 0; r < due[d].resume.size() && !failed; ++r) {
                runFrom(due[d].resume[r]);
            }
        }
    }

    GraphValue GraphRuntime::readInput(const BoundNode& node, size_t index) {
        if (index >= node.inputs.size()) {
            return GraphValue();
        }
        return readInputValue(node.inputs[index]);
    }

    GraphValue GraphRuntime::readInputValue(const BoundInput& input) {
        if (input.pureNode >= 0) {
            evalPure(static_cast<uint32_t>(input.pureNode));
        }
        if (input.reg < 0 || static_cast<size_t>(input.reg) >= instance.registers.size()) {
            return GraphValue();
        }
        return instance.registers[static_cast<size_t>(input.reg)];
    }

    void GraphRuntime::writeOutput(const BoundNode& node, size_t index, const GraphValue& value) {
        if (index >= node.outputs.size()) return;
        const int reg = node.outputs[index];
        if (reg < 0 || static_cast<size_t>(reg) >= instance.registers.size()) return;
        instance.registers[static_cast<size_t>(reg)] = value;
    }

    bool GraphRuntime::hasScratch(const BoundNode& node) const {
        return node.index < instance.nodeScratch.size();
    }

    void GraphRuntime::runEventOutputs(const BoundNode& node, size_t firstDataOutput) {
        // Event entry nodes have no input pins, so their data outputs are numbered
        // in the order the runtime pushes the payload: fill them positionally and
        // retag each value the way the catalog says the pin is typed.
        for (size_t i = 0; i < node.outputs.size(); ++i) {
            const size_t argIndex = firstDataOutput + i;
            if (argIndex >= instance.eventArgs.size()) {
                writeOutput(node, i, GraphValue::makeNone());
                continue;
            }
            GraphValue value = instance.eventArgs[argIndex];
            const GraphValueType pinType = i < node.outputTypes.size()
                ? node.outputTypes[i] : GraphValueType::Any;
            if (pinType != GraphValueType::Any && value.type != pinType) {
                GraphValue converted;
                if (value.convertTo(pinType, converted)) {
                    value = converted;
                } else {
                    value.type = pinType;
                }
            }
            writeOutput(node, i, value);
        }
    }

    void GraphRuntime::evalPure(uint32_t nodeIndex) {
        if (nodeIndex >= nodes.size()) return;
        if (evalStamp[nodeIndex] == fireSerial) return;

        if (evalMark[nodeIndex]) {
            reportError(&nodes[nodeIndex], "circular pure dependency");
            return;
        }
        if (budget == 0) {
            reportError(&nodes[nodeIndex], "execution budget exceeded (possible cycle)");
            return;
        }
        budget--;

        const BoundNode& node = nodes[nodeIndex];
        evalMark[nodeIndex] = 1;
        if (node.pure) {
            node.pure(*this, node);
        } else {
            reportError(&node, "node has no pure operation");
        }
        evalMark[nodeIndex] = 0;
        evalStamp[nodeIndex] = fireSerial;
    }

    void GraphRuntime::runFrom(uint32_t nodeIndex) {
        uint32_t current = nodeIndex;

        while (!failed && !stopped) {
            if (current >= nodes.size()) {
                reportError(nullptr, "execution reached an invalid node");
                return;
            }

            const BoundNode& node = nodes[current];
            if (node.kind != BoundOpKind::Exec) {
                reportError(&node, "execution reached a data node");
                return;
            }

            if (budget == 0) {
                reportError(&node, "execution budget exceeded (possible infinite loop)");
                return;
            }
            budget--;

            int pin = node.exec ? node.exec(*this, node) : -1;

            if (pin >= 0) {
                if (node.sequence) {
                    for (int p = static_cast<int>(node.execTargets.size()) - 1; p > pin; --p) {
                        pushExecTargets(node, static_cast<size_t>(p));
                    }
                }
                if (static_cast<size_t>(pin) < node.execTargets.size() && !node.execTargets[static_cast<size_t>(pin)].empty()) {
                    const std::vector<uint32_t>& targets = node.execTargets[static_cast<size_t>(pin)];
                    for (size_t t = targets.size(); t-- > 1;) {
                        continuations.push_back(targets[t]);
                    }
                    current = targets[0];
                    continue;
                }
            }

            current = popContinuation();
            if (current == INVALID_NODE) return;
        }
    }

    void GraphRuntime::pushExecTargets(const BoundNode& node, size_t pin) {
        if (pin >= node.execTargets.size()) return;
        for (size_t t = node.execTargets[pin].size(); t-- > 0;) {
            continuations.push_back(node.execTargets[pin][t]);
        }
    }

    uint32_t GraphRuntime::popContinuation() {
        if (continuations.empty()) return INVALID_NODE;
        const uint32_t next = continuations.back();
        continuations.pop_back();
        return next;
    }

    void GraphRuntime::scheduleDelay(const BoundNode& node, double seconds) {
        scheduleResume(node, seconds, false);
    }

    void GraphRuntime::scheduleResume(const BoundNode& node, double seconds, bool self) {
        GraphPendingDelay pending;
        pending.remaining = seconds > 0.0 ? seconds : 0.0;

        if (self) {
            // Re-entering the node re-reads its inputs, which is what a wait/until wants.
            pending.resume.push_back(node.index);
        } else if (!node.execTargets.empty()) {
            pending.resume = node.execTargets[0];
        }

        if (pending.remaining <= 0.0) {
            pending.remaining = 0.000001;
        }
        instance.delays.push_back(std::move(pending));
    }

    void GraphRuntime::runExecOutput(const BoundNode& node, size_t pin) {
        if (failed || pin >= node.execTargets.size()) return;

        // Copied: the body may add nothing to the graph, but a nested loop reuses this
        // function and must not read a vector that the outer frame still holds a view of.
        const std::vector<uint32_t> targets = node.execTargets[pin];

        std::vector<uint32_t> outer;
        outer.swap(continuations);

        for (size_t t = 0; t < targets.size() && !failed; ++t) {
            // Each iteration is a fresh evaluation window: a pure chain hanging off the
            // loop index has to be recomputed, not served from this fire's stamp.
            if (++fireSerial == 0) {
                std::fill(evalStamp.begin(), evalStamp.end(), 0);
                fireSerial = 1;
            }
            continuations.clear();
            runFrom(targets[t]);
        }

        continuations.swap(outer);
    }

    void GraphRuntime::requestBreak() {
        breakRequests++;
    }

    bool GraphRuntime::consumeBreak() {
        if (breakRequests == 0) return false;
        breakRequests--;
        return true;
    }

    GraphValue GraphRuntime::scratchValue(const BoundNode& node) const {
        if (node.index >= instance.nodeScratch.size()) return GraphValue();
        return instance.nodeScratch[node.index];
    }

    void GraphRuntime::writeScratch(const BoundNode& node, const GraphValue& value) {
        if (node.index >= instance.nodeScratch.size()) return;
        instance.nodeScratch[node.index] = value;
    }

    size_t GraphRuntime::argCount() const {
        return instance.eventArgs.size();
    }

    GraphValue GraphRuntime::readArg(size_t index) const {
        if (index >= instance.eventArgs.size()) return GraphValue();
        return instance.eventArgs[index];
    }

    void GraphRuntime::reportError(const BoundNode* node, const std::string& message) {
        if (failed) return;
        failed = true;
        if (node) {
            Log::error("graph '%s': node %u (%s): %s",
                       graph.getAsset().name.c_str(), node->id,
                       node->source ? node->source->kind.c_str() : "?",
                       message.c_str());
        } else {
            Log::error("graph '%s': %s", graph.getAsset().name.c_str(), message.c_str());
        }
    }

    namespace GraphBinding {

        void initializeGraphScripts(Scene* scene) {
            if (!scene) return;

            std::vector<std::pair<Entity, size_t>> pending;
            auto components = scene->getComponentArray<ScriptComponent>();
            if (components) {
                for (size_t c = 0; c < components->size(); ++c) {
                    const ScriptComponent& component = components->getComponentFromIndex(c);
                    for (size_t s = 0; s < component.scripts.size(); ++s) {
                        const ScriptEntry& entry = component.scripts[s];
                        if (!entry.enabled || entry.type != ScriptType::GRAPH || entry.instance != nullptr) {
                            continue;
                        }
                        pending.push_back(std::make_pair(components->getEntity(c), s));
                    }
                }
            }
            if (pending.empty()) return;

            ensureTickSubscription();

            for (size_t p = 0; p < pending.size(); ++p) {
                const Entity entity = pending[p].first;
                const size_t scriptIndex = pending[p].second;

                ScriptComponent* component = scene->findComponent<ScriptComponent>(entity);
                if (!component || scriptIndex >= component->scripts.size()) continue;
                ScriptEntry& entry = component->scripts[scriptIndex];
                if (!entry.enabled || entry.type != ScriptType::GRAPH || entry.instance != nullptr) continue;

                std::string error;
                std::shared_ptr<ExecutableGraph> graph = loadGraph(entry.path, error);
                if (!graph) {
                    Log::error("graph script '%s': %s", entry.path.c_str(), error.c_str());
                    continue;
                }

                std::unique_ptr<GraphInstance> instance(new GraphInstance());
                instance->scene = scene;
                instance->entity = entity;
                instance->scriptIndex = scriptIndex;
                instance->graph = graph;
                instance->registers = graph->getRegisterDefaults();
                instance->variables.reserve(graph->getAsset().variables.size());
                for (size_t v = 0; v < graph->getAsset().variables.size(); ++v) {
                    instance->variables.push_back(graph->getAsset().variables[v].defaultValue);
                }
                applyPropertyOverrides(*instance, entry);
                instance->nodeState.assign(graph->getNodes().size(), 0);
                instance->nodeScratch.assign(graph->getNodes().size(), GraphValue());
                instance->tag = makeInstanceTag(instance.get());

                GraphInstance* raw = instance.get();
                entry.instance = raw;
                graphInstances.push_back(std::move(instance));

                GraphRuntime runtime(*raw, *graph);
                runtime.fire("start");

                for (size_t e = 0; e < graph->getAsset().events.size(); ++e) {
                    const std::string& eventId = graph->getAsset().events[e].id;
                    if (eventId != "start") {
                        subscribeGraphEvent(*raw, eventId);
                    }
                }
            }
        }

        void cleanupGraphScripts(Scene* scene) {
            for (size_t i = 0; i < graphInstances.size();) {
                GraphInstance* instance = graphInstances[i].get();
                if (instance && instance->scene == scene) {
                    destroyInstance(instance);
                    graphInstances.erase(graphInstances.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            maybeStopTickSubscription();
        }

        void cleanupGraphScripts(Scene* scene, Entity entity) {
            for (size_t i = 0; i < graphInstances.size();) {
                GraphInstance* instance = graphInstances[i].get();
                if (instance && instance->scene == scene && instance->entity == entity) {
                    destroyInstance(instance);
                    graphInstances.erase(graphInstances.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            maybeStopTickSubscription();
        }

        void cleanupAll() {
            for (size_t i = 0; i < graphInstances.size(); ++i) {
                destroyInstance(graphInstances[i].get());
            }
            graphInstances.clear();
            graphCache.clear();
            stopTickSubscription();
        }

        size_t getInstanceCount() {
            size_t count = 0;
            for (size_t i = 0; i < graphInstances.size(); ++i) {
                if (graphInstances[i] && !graphInstances[i]->dead) {
                    count++;
                }
            }
            return count;
        }

        size_t dispatchEvent(Scene* scene, Entity entity, const std::string& eventId,
                             const std::vector<GraphValue>& args) {
            size_t fired = 0;
            for (size_t i = 0; i < graphInstances.size(); ++i) {
                GraphInstance* instance = graphInstances[i].get();
                if (!instance || instance->dead || !instance->graph) continue;
                if (instance->scene != scene || instance->entity != entity) continue;
                if (!instance->graph->findEventEntries(eventId)) continue;
                GraphRuntime runtime(*instance, *instance->graph);
                runtime.fire(eventId, args);
                fired++;
            }
            return fired;
        }

        size_t dispatchEventAll(Scene* scene, const std::string& eventId,
                                const std::vector<GraphValue>& args) {
            size_t fired = 0;
            // Instances are appended while a graph runs, so iterate by index over a
            // snapshot of the count: a fire that attaches another script must not be
            // skipped forever, nor reach an entry this pass has already passed.
            const size_t limit = graphInstances.size();
            for (size_t i = 0; i < limit; ++i) {
                GraphInstance* instance = graphInstances[i].get();
                if (!instance || instance->dead || !instance->graph) continue;
                if (scene && instance->scene != scene) continue;
                if (!instance->graph->findEventEntries(eventId)) continue;
                GraphRuntime runtime(*instance, *instance->graph);
                runtime.fire(eventId, args);
                fired++;
            }
            return fired;
        }

    }

}
}
