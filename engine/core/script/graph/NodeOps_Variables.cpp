// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "NodeOps.h"

#include <string>

namespace quantum {
namespace graph {

    namespace {

        enum class Accumulate { Add, Subtract, Multiply, Divide };

        GraphValue* variable(GraphRuntime& runtime, const BoundNode& node) {
            std::vector<GraphValue>& variables = op::instance(runtime).variables;
            if (node.dataIndex < 0 || static_cast<size_t>(node.dataIndex) >= variables.size()) {
                op::warn(runtime, node, "the variable parameter does not name a graph variable");
                return nullptr;
            }
            return &variables[static_cast<size_t>(node.dataIndex)];
        }

        const GraphValue* variableDefault(GraphRuntime& runtime, const BoundNode& node) {
            const std::vector<GraphVariable>& variables = runtime.getGraph().getAsset().variables;
            if (node.dataIndex < 0 || static_cast<size_t>(node.dataIndex) >= variables.size()) {
                return nullptr;
            }
            return &variables[static_cast<size_t>(node.dataIndex)].defaultValue;
        }

        void varGet(GraphRuntime& runtime, const BoundNode& node) {
            std::vector<GraphValue>& variables = op::instance(runtime).variables;
            if (node.dataIndex >= 0 && static_cast<size_t>(node.dataIndex) < variables.size()) {
                op::out(runtime, node, 0, variables[static_cast<size_t>(node.dataIndex)]);
                return;
            }
            op::out(runtime, node, 0, GraphValue::makeNone());
        }

        // A set of an unwired pin is the variable's own default, not None: the graph
        // author sees a pin typed like the variable, and writing <none> into a Float
        // would leave the storage tagged wrongly for every later reader.
        int varSet(GraphRuntime& runtime, const BoundNode& node) {
            GraphValue* target = variable(runtime, node);
            if (!target) return -1;

            GraphValue value = op::in(runtime, node, 0);
            if (value.isNone()) {
                const GraphValue* def = variableDefault(runtime, node);
                value = def ? *def : GraphValue::makeNone();
            }

            GraphValue converted;
            if (target->type != GraphValueType::None && target->type != GraphValueType::Any &&
                value.type != target->type && value.convertTo(target->type, converted)) {
                *target = converted;
            } else {
                *target = value;
            }
            return 0;
        }

        bool accumulateScalar(GraphValue& target, double operand, Accumulate mode, bool& changed) {
            changed = true;
            switch (target.type) {
                case GraphValueType::Bool:
                    if (mode == Accumulate::Add) {
                        target = GraphValue::makeBool(operand != 0.0);
                    } else {
                        changed = false;
                    }
                    return true;
                case GraphValueType::Int: {
                    const int64_t asInt = static_cast<int64_t>(operand);
                    if (mode == Accumulate::Divide && asInt == 0) {
                        return false;      // a zero divisor leaves the value alone
                    }
                    const int64_t base = target.asInt();
                    int64_t result = base;
                    switch (mode) {
                        case Accumulate::Add: result = base + asInt; break;
                        case Accumulate::Subtract: result = base - asInt; break;
                        case Accumulate::Multiply: result = base * asInt; break;
                        case Accumulate::Divide: result = base / asInt; break;
                    }
                    target = GraphValue::makeInt(result);
                    return true;
                }
                case GraphValueType::Float: {
                    const double base = target.asFloat();
                    double result = base;
                    switch (mode) {
                        case Accumulate::Add: result = base + operand; break;
                        case Accumulate::Subtract: result = base - operand; break;
                        case Accumulate::Multiply: result = base * operand; break;
                        case Accumulate::Divide: if (operand == 0.0) return false; result = base / operand; break;
                    }
                    target = GraphValue::makeFloat(result);
                    return true;
                }
                case GraphValueType::Vector2: {
                    Vector2 v = target.asVector2();
                    switch (mode) {
                        case Accumulate::Add: v = v + Vector2(static_cast<float>(operand), static_cast<float>(operand)); break;
                        case Accumulate::Subtract: v = v - Vector2(static_cast<float>(operand), static_cast<float>(operand)); break;
                        case Accumulate::Multiply: v = v * static_cast<float>(operand); break;
                        case Accumulate::Divide: if (operand == 0.0) return false; v = v / static_cast<float>(operand); break;
                    }
                    target = GraphValue::makeVector2(v);
                    return true;
                }
                case GraphValueType::Vector3:
                case GraphValueType::Color3: {
                    const bool color = target.type == GraphValueType::Color3;
                    Vector3 v = target.asVector3();
                    switch (mode) {
                        case Accumulate::Add: v = v + Vector3(static_cast<float>(operand), static_cast<float>(operand), static_cast<float>(operand)); break;
                        case Accumulate::Subtract: v = v - Vector3(static_cast<float>(operand), static_cast<float>(operand), static_cast<float>(operand)); break;
                        case Accumulate::Multiply: v = v * static_cast<float>(operand); break;
                        case Accumulate::Divide: if (operand == 0.0) return false; v = v / static_cast<float>(operand); break;
                    }
                    target = color ? GraphValue::makeColor3(v) : GraphValue::makeVector3(v);
                    return true;
                }
                case GraphValueType::Vector4:
                case GraphValueType::Color4: {
                    const bool color = target.type == GraphValueType::Color4;
                    Vector4 v = target.asVector4();
                    switch (mode) {
                        case Accumulate::Add: v = v + Vector4(static_cast<float>(operand), static_cast<float>(operand), static_cast<float>(operand), 0.0f); break;
                        case Accumulate::Subtract: v = v - Vector4(static_cast<float>(operand), static_cast<float>(operand), static_cast<float>(operand), 0.0f); break;
                        case Accumulate::Multiply: v = v * static_cast<float>(operand); break;
                        case Accumulate::Divide: if (operand == 0.0) return false; v = v / static_cast<float>(operand); break;
                    }
                    target = color ? GraphValue::makeColor4(v) : GraphValue::makeVector4(v);
                    return true;
                }
                case GraphValueType::String:
                    if (mode == Accumulate::Add) {
                        target = GraphValue::makeString(target.asString() + GraphValue::makeFloat(operand).asString());
                        return true;
                    }
                    changed = false;
                    return true;
                default:
                    changed = false;
                    return true;
            }
        }

        int varAccumulate(GraphRuntime& runtime, const BoundNode& node, Accumulate mode) {
            GraphValue* target = variable(runtime, node);
            if (!target) return -1;

            bool changed = false;
            if (!accumulateScalar(*target, op::number(runtime, node, 0), mode, changed) || !changed) {
                op::warn(runtime, node, "this variable type does not support the operation");
            }
            return 0;
        }

        int varIncrement(GraphRuntime& runtime, const BoundNode& node) {
            return varAccumulate(runtime, node, Accumulate::Add);
        }

        int varDecrement(GraphRuntime& runtime, const BoundNode& node) {
            return varAccumulate(runtime, node, Accumulate::Subtract);
        }

        int varToggle(GraphRuntime& runtime, const BoundNode& node) {
            GraphValue* target = variable(runtime, node);
            if (!target) return -1;

            switch (target->type) {
                case GraphValueType::Bool:
                    *target = GraphValue::makeBool(!target->asBool());
                    break;
                case GraphValueType::Int:
                    *target = GraphValue::makeInt(target->asInt() == 0 ? 1 : 0);
                    break;
                case GraphValueType::Float:
                    *target = GraphValue::makeFloat(target->asFloat() == 0.0 ? 1.0 : 0.0);
                    break;
                default:
                    *target = GraphValue::makeBool(!target->asBool());
                    break;
            }
            return 0;
        }

        int varReset(GraphRuntime& runtime, const BoundNode& node) {
            GraphValue* target = variable(runtime, node);
            if (!target) return -1;
            const GraphValue* def = variableDefault(runtime, node);
            *target = def ? *def : GraphValue::makeNone();
            return 0;
        }

    }

    void registerVariableOps() {
        registerGraphOp("var.get", BoundOpKind::Pure, nullptr, &varGet);
        registerGraphOp("var.set", BoundOpKind::Exec, &varSet, nullptr);
        registerGraphOp("var.add", BoundOpKind::Exec,
                        [](GraphRuntime& rt, const BoundNode& n) { return varAccumulate(rt, n, Accumulate::Add); }, nullptr);
        registerGraphOp("var.subtract", BoundOpKind::Exec,
                        [](GraphRuntime& rt, const BoundNode& n) { return varAccumulate(rt, n, Accumulate::Subtract); }, nullptr);
        registerGraphOp("var.multiply", BoundOpKind::Exec,
                        [](GraphRuntime& rt, const BoundNode& n) { return varAccumulate(rt, n, Accumulate::Multiply); }, nullptr);
        registerGraphOp("var.divide", BoundOpKind::Exec,
                        [](GraphRuntime& rt, const BoundNode& n) { return varAccumulate(rt, n, Accumulate::Divide); }, nullptr);
        registerGraphOp("var.increment", BoundOpKind::Exec, &varIncrement, nullptr);
        registerGraphOp("var.decrement", BoundOpKind::Exec, &varDecrement, nullptr);
        registerGraphOp("var.toggle", BoundOpKind::Exec, &varToggle, nullptr);
        registerGraphOp("var.reset", BoundOpKind::Exec, &varReset, nullptr);
    }

}
}
