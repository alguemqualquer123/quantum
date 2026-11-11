// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "NodeCatalog.h"

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace quantum {
namespace graph {

    namespace {

        // Type and literal shorthands: the catalog is a table of ~300 rows and the
        // full spelling of every pin would make it unreadable.
        const GraphValueType kBool = GraphValueType::Bool;
        const GraphValueType kInt = GraphValueType::Int;
        const GraphValueType kFloat = GraphValueType::Float;
        const GraphValueType kString = GraphValueType::String;
        const GraphValueType kVec2 = GraphValueType::Vector2;
        const GraphValueType kVec3 = GraphValueType::Vector3;
        const GraphValueType kVec4 = GraphValueType::Vector4;
        const GraphValueType kQuat = GraphValueType::Quaternion;
        const GraphValueType kRect = GraphValueType::Rect;
        const GraphValueType kCol3 = GraphValueType::Color3;
        const GraphValueType kCol4 = GraphValueType::Color4;
        const GraphValueType kEntity = GraphValueType::Entity;
        const GraphValueType kAny = GraphValueType::Any;

        GraphValue f(double v) { return GraphValue::makeFloat(v); }
        GraphValue i(int64_t v) { return GraphValue::makeInt(v); }
        GraphValue bl(bool v) { return GraphValue::makeBool(v); }
        GraphValue s(const char* v) { return GraphValue::makeString(v); }
        GraphValue v2(float x, float y) { return GraphValue::makeVector2(Vector2(x, y)); }
        GraphValue v3(float x, float y, float z) { return GraphValue::makeVector3(Vector3(x, y, z)); }

        GraphPinDef execPinIn() {
            GraphPinDef pin;
            pin.name = "in";
            pin.kind = GraphPinKind::Exec;
            pin.dir = GraphPinDir::Input;
            return pin;
        }

        GraphPinDef xin() {
            return execPinIn();
        }

        static GraphPinDef execPinOut(const std::string& name = "out") {
            GraphPinDef pin;
            pin.name = name;
            pin.kind = GraphPinKind::Exec;
            pin.dir = GraphPinDir::Output;
            return pin;
        }

        GraphPinDef xout(const std::string& name = "out") {
            return execPinOut(name);
        }

        static GraphPinDef dataPinIn(const std::string& name, GraphValueType type, const GraphValue& defaultValue = GraphValue()) {
            GraphPinDef pin;
            pin.name = name;
            pin.type = type;
            pin.kind = GraphPinKind::Data;
            pin.dir = GraphPinDir::Input;
            pin.defaultValue = defaultValue;
            return pin;
        }

        GraphPinDef din(const std::string& name, GraphValueType type, const GraphValue& defaultValue = GraphValue()) {
            return dataPinIn(name, type, defaultValue);
        }

        static GraphPinDef dataPinOut(const std::string& name, GraphValueType type) {
            GraphPinDef pin;
            pin.name = name;
            pin.type = type;
            pin.kind = GraphPinKind::Data;
            pin.dir = GraphPinDir::Output;
            return pin;
        }

        GraphPinDef dout(const std::string& name, GraphValueType type) {
            return dataPinOut(name, type);
        }

        // The entity a node acts on. Left unconnected it means "the entity running
        // this graph", which is what most gameplay graphs want.
        GraphPinDef targetPin() {
            return din("target", kEntity);
        }

        std::vector<GraphPinDef> binaryMathPins() {
            return {
                din("a", kFloat, f(0.0)),
                din("b", kFloat, f(0.0)),
                dout("out", kFloat)
            };
        }

        std::vector<GraphPinDef> unaryMathPins() {
            return {
                din("a", kFloat, f(0.0)),
                dout("out", kFloat)
            };
        }

        std::vector<GraphPinDef> binaryBoolPins() {
            return {
                din("a", kBool, bl(false)),
                din("b", kBool, bl(false)),
                dout("out", kBool)
            };
        }

        std::vector<GraphPinDef> stringUnaryPins() {
            return {
                din("value", kString, s("")),
                dout("out", kString)
            };
        }

        int clampCount(double raw, int low, int high) {
            if (!(raw == raw)) return low;              // NaN guard
            const double rounded = raw < 0.0 ? std::ceil(raw - 0.5) : std::floor(raw + 0.5);
            return static_cast<int>(std::max(static_cast<double>(low), std::min(static_cast<double>(high), rounded)));
        }

    }

    NodeCatalog::NodeCatalog() {
        const std::string eventCategory = "Event";

        // ---- Event entry nodes. The data outputs are filled from the event payload
        // by NodeDefs' eventNode, whose order matches the engine signal arguments.
        add("event.start", "Start", eventCategory, false, {xout()});
        add("event.update", "Update", eventCategory, false, {xout()});
        add("event.fixedUpdate", "Fixed Update", eventCategory, false, {xout()});
        add("event.postUpdate", "Post Update", eventCategory, false, {xout()});
        add("event.draw", "Draw", eventCategory, false, {xout()});
        add("event.pause", "Pause", eventCategory, false, {xout()});
        add("event.resume", "Resume", eventCategory, false, {xout()});
        add("event.shutdown", "Shutdown", eventCategory, false, {xout()});
        add("event.sceneLoaded", "Scene Loaded", eventCategory, false, {xout()});

        add("event.keyDown", "Key Down", eventCategory, false, {
            xout(), dout("key", kInt), dout("repeat", kBool), dout("modifiers", kInt)});
        add("event.keyUp", "Key Up", eventCategory, false, {
            xout(), dout("key", kInt), dout("repeat", kBool), dout("modifiers", kInt)});
        add("event.charInput", "Character Input", eventCategory, false, {xout(), dout("code", kInt)});

        add("event.mouseDown", "Mouse Down", eventCategory, false, {
            xout(), dout("button", kInt), dout("position", kVec2), dout("modifiers", kInt)});
        add("event.mouseUp", "Mouse Up", eventCategory, false, {
            xout(), dout("button", kInt), dout("position", kVec2), dout("modifiers", kInt)});
        add("event.mouseMove", "Mouse Move", eventCategory, false, {
            xout(), dout("position", kVec2), dout("buttons", kInt)});
        add("event.mouseScroll", "Mouse Scroll", eventCategory, false, {
            xout(), dout("offset", kVec2), dout("buttons", kInt)});
        add("event.mouseEnter", "Mouse Enter", eventCategory, false, {xout()});
        add("event.mouseLeave", "Mouse Leave", eventCategory, false, {xout()});

        add("event.touchStart", "Touch Start", eventCategory, false, {
            xout(), dout("pointer", kInt), dout("position", kVec2)});
        add("event.touchEnd", "Touch End", eventCategory, false, {
            xout(), dout("pointer", kInt), dout("position", kVec2)});
        add("event.touchMove", "Touch Move", eventCategory, false, {
            xout(), dout("pointer", kInt), dout("position", kVec2)});
        add("event.touchCancel", "Touch Cancel", eventCategory, false, {
            xout(), dout("pointer", kInt), dout("position", kVec2)});

        add("event.gamepadButtonDown", "Gamepad Button Down", eventCategory, false, {
            xout(), dout("pad", kInt), dout("button", kInt)});
        add("event.gamepadButtonUp", "Gamepad Button Up", eventCategory, false, {
            xout(), dout("pad", kInt), dout("button", kInt)});
        add("event.gamepadAxisMove", "Gamepad Axis", eventCategory, false, {
            xout(), dout("pad", kInt), dout("axis", kInt), dout("value", kFloat)});
        add("event.gamepadConnect", "Gamepad Connected", eventCategory, false, {xout(), dout("pad", kInt)});
        add("event.gamepadDisconnect", "Gamepad Disconnected", eventCategory, false, {xout(), dout("pad", kInt)});

        // Graph-to-graph messaging: the id is "custom:<name>" and the payload arrives
        // through the arg pins (their count comes from the "args" parameter).
        add("event.custom", "Custom Event", eventCategory, false, {xout(), dout("arg0", kAny), dout("arg1", kAny)});
        add("event.dispatch", "Dispatch Event", eventCategory, false, {
            xin(), din("name", kString, s("hit")), targetPin(),
            din("a", kAny), din("b", kAny), din("c", kAny), xout()});
        add("event.dispatchAll", "Dispatch To Scene", eventCategory, false, {
            xin(), din("name", kString, s("hit")),
            din("a", kAny), din("b", kAny), din("c", kAny), xout()});

        // ---- Flow
        add("flow.branch", "Branch", "Flow", false, {
            xin(), din("condition", kBool, bl(false)), xout("true"), xout("false")});
        add("flow.sequence", "Sequence", "Flow", false, {
            xin(), xout("then0"), xout("then1")}, true);
        add("flow.doOnce", "Do Once", "Flow", false, {xin(), xout()});
        add("flow.delay", "Delay", "Flow", false, {
            xin(), din("duration", kFloat, f(0.2)), xout()});
        add("flow.nextFrame", "Next Frame", "Flow", false, {xin(), xout()});
        add("flow.forLoop", "For Loop", "Flow", false, {
            xin(), din("first", kInt, i(0)), din("last", kInt, i(0)), din("step", kInt, i(1)),
            dout("index", kInt), xout("loop"), xout("completed")});
        add("flow.repeat", "Repeat", "Flow", false, {
            xin(), din("count", kInt, i(1)), dout("index", kInt), xout("loop"), xout("completed")});
        add("flow.whileLoop", "While Loop", "Flow", false, {
            xin(), din("condition", kBool, bl(false)), xout("loop"), xout("completed")});
        add("flow.break", "Break Loop", "Flow", false, {xin()});
        add("flow.gate", "Gate", "Flow", false, {
            xin(), din("open", kBool, bl(true)), xout()});
        add("flow.flipFlop", "Flip Flop", "Flow", false, {
            xin(), din("reset", kBool, bl(false)), xout("a"), xout("b")});
        add("flow.cooldown", "Cooldown", "Flow", false, {
            xin(), din("seconds", kFloat, f(0.5)), xout("ready"), xout("blocked")});
        add("flow.wait", "Wait Until", "Flow", false, {
            xin(), din("condition", kBool, bl(false)), din("timeout", kFloat, f(0.0)),
            din("interval", kFloat, f(0.05)), xout("satisfied"), xout("timedOut")});
        add("flow.switchInt", "Switch Int", "Flow", false, {
            xin(), din("value", kInt, i(0)), xout("case0"), xout("case1"), xout("case2"), xout("default")});
        add("flow.switchString", "Switch String", "Flow", false, {
            xin(), din("value", kString, s("")), xout("case0"), xout("case1"), xout("case2"), xout("default")});

        // ---- Variables. Every "var." kind resolves its "variable" parameter to a
        // variable index, so the runtime can reach the storage without a name lookup.
        add("var.get", "Get Variable", "Variables", true, {dout("value", kFloat)});
        add("var.set", "Set Variable", "Variables", false, {
            xin(), din("value", kFloat), xout()});
        add("var.add", "Add To Variable", "Variables", false, {
            xin(), din("value", kFloat, f(0)), xout()});
        add("var.subtract", "Subtract From Variable", "Variables", false, {
            xin(), din("value", kFloat, f(0)), xout()});
        add("var.multiply", "Multiply Variable", "Variables", false, {
            xin(), din("value", kFloat, f(1)), xout()});
        add("var.divide", "Divide Variable", "Variables", false, {
            xin(), din("value", kFloat, f(1)), xout()});
        add("var.increment", "Increment Variable", "Variables", false, {xin(), xout()});
        add("var.decrement", "Decrement Variable", "Variables", false, {xin(), xout()});
        add("var.toggle", "Toggle Variable", "Variables", false, {xin(), xout()});
        add("var.reset", "Reset Variable", "Variables", false, {xin(), xout()});

        // ---- Math
        add("math.add", "Add", "Math", true, binaryMathPins());
        add("math.sub", "Subtract", "Math", true, binaryMathPins());
        add("math.mul", "Multiply", "Math", true, binaryMathPins());
        add("math.div", "Divide", "Math", true, binaryMathPins());
        add("math.min", "Min", "Math", true, binaryMathPins());
        add("math.max", "Max", "Math", true, binaryMathPins());
        add("math.abs", "Absolute", "Math", true, unaryMathPins());
        add("math.negate", "Negate", "Math", true, unaryMathPins());
        add("math.sign", "Sign", "Math", true, unaryMathPins());
        add("math.sqrt", "Square Root", "Math", true, unaryMathPins());
        add("math.cbrt", "Cube Root", "Math", true, unaryMathPins());
        add("math.exp", "Exp", "Math", true, unaryMathPins());
        add("math.log", "Log", "Math", true, unaryMathPins());
        add("math.log2", "Log2", "Math", true, unaryMathPins());
        add("math.floor", "Floor", "Math", true, unaryMathPins());
        add("math.ceil", "Ceil", "Math", true, unaryMathPins());
        add("math.round", "Round", "Math", true, unaryMathPins());
        add("math.trunc", "Truncate", "Math", true, unaryMathPins());
        add("math.fract", "Fractional", "Math", true, unaryMathPins());
        add("math.sin", "Sine", "Math", true, unaryMathPins());
        add("math.cos", "Cosine", "Math", true, unaryMathPins());
        add("math.tan", "Tangent", "Math", true, unaryMathPins());
        add("math.asin", "Asin", "Math", true, unaryMathPins());
        add("math.acos", "Acos", "Math", true, unaryMathPins());
        add("math.atan", "Atan", "Math", true, unaryMathPins());
        add("math.degrees", "To Degrees", "Math", true, unaryMathPins());
        add("math.radians", "To Radians", "Math", true, unaryMathPins());
        add("math.pow", "Power", "Math", true, binaryMathPins());
        add("math.mod", "Modulo", "Math", true, binaryMathPins());
        add("math.atan2", "Atan2", "Math", true, binaryMathPins());
        add("math.average", "Average", "Math", true, binaryMathPins());
        add("math.lerp", "Lerp", "Math", true, {
            din("a", kFloat, f(0.0)), din("b", kFloat, f(1.0)), din("t", kFloat, f(0.5)),
            dout("out", kFloat)});
        add("math.clamp", "Clamp", "Math", true, {
            din("value", kFloat, f(0.0)), din("min", kFloat, f(0.0)), din("max", kFloat, f(1.0)),
            dout("out", kFloat)});
        add("math.normalize", "Normalize", "Math", true, {
            din("value", kFloat, f(0.0)), din("min", kFloat, f(0.0)), din("max", kFloat, f(1.0)),
            dout("out", kFloat)});
        add("math.remap", "Remap", "Math", true, {
            din("value", kFloat, f(0.0)), din("inMin", kFloat, f(0.0)), din("inMax", kFloat, f(1.0)),
            din("outMin", kFloat, f(0.0)), din("outMax", kFloat, f(1.0)), dout("out", kFloat)});
        add("math.smoothstep", "Smoothstep", "Math", true, {
            din("edge0", kFloat, f(0.0)), din("edge1", kFloat, f(1.0)), din("value", kFloat, f(0.5)),
            dout("out", kFloat)});
        add("math.pingpong", "PingPong", "Math", true, {
            din("value", kFloat, f(0.0)), din("min", kFloat, f(0.0)), din("max", kFloat, f(1.0)),
            dout("out", kFloat)});
        add("math.wrap", "Wrap", "Math", true, {
            din("value", kFloat, f(0.0)), din("min", kFloat, f(0.0)), din("max", kFloat, f(1.0)),
            dout("out", kFloat)});
        add("math.random", "Random Float", "Math", true, {
            din("min", kFloat, f(0.0)), din("max", kFloat, f(1.0)), dout("out", kFloat)});
        add("math.randomInt", "Random Int", "Math", true, {
            din("min", kInt, i(0)), din("max", kInt, i(1)), dout("out", kInt)});
        add("math.randomBool", "Random Bool", "Math", true, {
            din("chance", kFloat, f(0.5)), dout("out", kBool)});

        // ---- Vector2
        add("vector2.make", "Make Vector2", "Vector2", true, {
            din("x", kFloat, f(0)), din("y", kFloat, f(0)), dout("out", kVec2)});
        add("vector2.split", "Split Vector2", "Vector2", true, {
            din("value", kVec2, v2(0, 0)), dout("x", kFloat), dout("y", kFloat)});
        add("vector2.add", "Add Vector2", "Vector2", true, {
            din("a", kVec2, v2(0, 0)), din("b", kVec2, v2(0, 0)), dout("out", kVec2)});
        add("vector2.sub", "Subtract Vector2", "Vector2", true, {
            din("a", kVec2, v2(0, 0)), din("b", kVec2, v2(0, 0)), dout("out", kVec2)});
        add("vector2.scale", "Scale Vector2", "Vector2", true, {
            din("value", kVec2, v2(0, 0)), din("factor", kFloat, f(1)), dout("out", kVec2)});
        add("vector2.dot", "Dot Vector2", "Vector2", true, {
            din("a", kVec2, v2(0, 0)), din("b", kVec2, v2(0, 0)), dout("out", kFloat)});
        add("vector2.length", "Length Vector2", "Vector2", true, {
            din("value", kVec2, v2(0, 0)), dout("out", kFloat)});
        add("vector2.distance", "Distance Vector2", "Vector2", true, {
            din("a", kVec2, v2(0, 0)), din("b", kVec2, v2(0, 0)), dout("out", kFloat)});
        add("vector2.normalized", "Normalize Vector2", "Vector2", true, {
            din("value", kVec2, v2(0, 0)), dout("out", kVec2)});
        add("vector2.lerp", "Lerp Vector2", "Vector2", true, {
            din("a", kVec2, v2(0, 0)), din("b", kVec2, v2(0, 0)), din("t", kFloat, f(0.5)),
            dout("out", kVec2)});

        // ---- Vector3
        add("vector3.make", "Make Vector3", "Vector3", true, {
            din("x", kFloat, f(0)), din("y", kFloat, f(0)), din("z", kFloat, f(0)), dout("out", kVec3)});
        add("vector3.split", "Split Vector3", "Vector3", true, {
            din("value", kVec3, v3(0, 0, 0)), dout("x", kFloat), dout("y", kFloat), dout("z", kFloat)});
        add("vector3.add", "Add Vector3", "Vector3", true, {
            din("a", kVec3, v3(0, 0, 0)), din("b", kVec3, v3(0, 0, 0)), dout("out", kVec3)});
        add("vector3.sub", "Subtract Vector3", "Vector3", true, {
            din("a", kVec3, v3(0, 0, 0)), din("b", kVec3, v3(0, 0, 0)), dout("out", kVec3)});
        add("vector3.scale", "Scale Vector3", "Vector3", true, {
            din("value", kVec3, v3(0, 0, 0)), din("factor", kFloat, f(1)), dout("out", kVec3)});
        add("vector3.negate", "Negate Vector3", "Vector3", true, {
            din("value", kVec3, v3(0, 0, 0)), dout("out", kVec3)});
        add("vector3.min", "Min Vector3", "Vector3", true, {
            din("a", kVec3, v3(0, 0, 0)), din("b", kVec3, v3(0, 0, 0)), dout("out", kVec3)});
        add("vector3.max", "Max Vector3", "Vector3", true, {
            din("a", kVec3, v3(0, 0, 0)), din("b", kVec3, v3(0, 0, 0)), dout("out", kVec3)});
        add("vector3.dot", "Dot Vector3", "Vector3", true, {
            din("a", kVec3, v3(0, 0, 0)), din("b", kVec3, v3(0, 0, 0)), dout("out", kFloat)});
        add("vector3.cross", "Cross Vector3", "Vector3", true, {
            din("a", kVec3, v3(0, 0, 0)), din("b", kVec3, v3(0, 0, 0)), dout("out", kVec3)});
        add("vector3.length", "Length Vector3", "Vector3", true, {
            din("value", kVec3, v3(0, 0, 0)), dout("out", kFloat)});
        add("vector3.squaredLength", "Squared Length", "Vector3", true, {
            din("value", kVec3, v3(0, 0, 0)), dout("out", kFloat)});
        add("vector3.distance", "Distance Vector3", "Vector3", true, {
            din("a", kVec3, v3(0, 0, 0)), din("b", kVec3, v3(0, 0, 0)), dout("out", kFloat)});
        add("vector3.normalized", "Normalize Vector3", "Vector3", true, {
            din("value", kVec3, v3(0, 0, 0)), dout("out", kVec3)});
        add("vector3.lerp", "Lerp Vector3", "Vector3", true, {
            din("a", kVec3, v3(0, 0, 0)), din("b", kVec3, v3(0, 0, 0)), din("t", kFloat, f(0.5)),
            dout("out", kVec3)});
        add("vector3.moveTowards", "Move Towards", "Vector3", true, {
            din("current", kVec3, v3(0, 0, 0)), din("target", kVec3, v3(0, 0, 0)),
            din("maxDelta", kFloat, f(0.1)), dout("out", kVec3)});
        add("vector3.reflect", "Reflect", "Vector3", true, {
            din("incident", kVec3, v3(0, 0, 0)), din("normal", kVec3, v3(0, 1, 0)), dout("out", kVec3)});
        add("vector3.angle", "Angle Vector3", "Vector3", true, {
            din("a", kVec3, v3(0, 0, 0)), din("b", kVec3, v3(0, 0, 0)), dout("out", kFloat)});
        add("vector3.rotate", "Rotate Vector3", "Vector3", true, {
            din("rotation", kQuat), din("value", kVec3, v3(0, 0, 0)), dout("out", kVec3)});

        // ---- Vector4
        add("vector4.make", "Make Vector4", "Vector4", true, {
            din("x", kFloat, f(0)), din("y", kFloat, f(0)), din("z", kFloat, f(0)), din("w", kFloat, f(0)),
            dout("out", kVec4)});
        add("vector4.split", "Split Vector4", "Vector4", true, {
            din("value", kVec4), dout("x", kFloat), dout("y", kFloat), dout("z", kFloat), dout("w", kFloat)});
        add("vector4.lerp", "Lerp Vector4", "Vector4", true, {
            din("a", kVec4), din("b", kVec4), din("t", kFloat, f(0.5)), dout("out", kVec4)});

        // ---- Quaternion
        add("quaternion.identity", "Identity Rotation", "Rotation", true, {dout("out", kQuat)});
        add("quaternion.make", "Make Quaternion", "Rotation", true, {
            din("w", kFloat, f(1)), din("x", kFloat, f(0)), din("y", kFloat, f(0)), din("z", kFloat, f(0)),
            dout("out", kQuat)});
        add("quaternion.fromEuler", "From Euler Angles", "Rotation", true, {
            din("euler", kVec3, v3(0, 0, 0)), din("order", kInt, i(0)), dout("out", kQuat)});
        add("quaternion.toEuler", "To Euler Angles", "Rotation", true, {
            din("value", kQuat), din("order", kInt, i(0)), dout("out", kVec3)});
        add("quaternion.multiply", "Multiply Rotations", "Rotation", true, {
            din("a", kQuat), din("b", kQuat), dout("out", kQuat)});
        add("quaternion.inverse", "Invert Rotation", "Rotation", true, {
            din("value", kQuat), dout("out", kQuat)});
        add("quaternion.slerp", "Slerp Rotations", "Rotation", true, {
            din("a", kQuat), din("b", kQuat), din("t", kFloat, f(0.5)), dout("out", kQuat)});
        add("quaternion.lookRotation", "Look Rotation", "Rotation", true, {
            din("forward", kVec3, v3(0, 0, 1)), din("up", kVec3, v3(0, 1, 0)), dout("out", kQuat)});
        add("quaternion.rotateVector", "Rotate Point", "Rotation", true, {
            din("rotation", kQuat), din("value", kVec3, v3(0, 0, 0)), dout("out", kVec3)});

        // ---- Rect
        add("rect.make", "Make Rect", "Rect", true, {
            din("x", kFloat, f(0)), din("y", kFloat, f(0)), din("width", kFloat, f(1)),
            din("height", kFloat, f(1)), dout("out", kRect)});
        add("rect.split", "Split Rect", "Rect", true, {
            din("value", kRect), dout("x", kFloat), dout("y", kFloat), dout("width", kFloat),
            dout("height", kFloat)});
        add("rect.contains", "Rect Contains", "Rect", true, {
            din("value", kRect), din("point", kVec2, v2(0, 0)), dout("out", kBool)});

        // ---- Color
        add("color3.make", "Make Color", "Color", true, {
            din("r", kFloat, f(1)), din("g", kFloat, f(1)), din("b", kFloat, f(1)), dout("out", kCol3)});
        add("color3.split", "Split Color", "Color", true, {
            din("value", kCol3), dout("r", kFloat), dout("g", kFloat), dout("b", kFloat)});
        add("color4.make", "Make Color With Alpha", "Color", true, {
            din("r", kFloat, f(1)), din("g", kFloat, f(1)), din("b", kFloat, f(1)), din("a", kFloat, f(1)),
            dout("out", kCol4)});
        add("color4.withAlpha", "Color With Alpha", "Color", true, {
            din("color", kCol3), din("alpha", kFloat, f(1)), dout("out", kCol4)});
        add("color.fromHex", "Color From Hex", "Color", true, {
            din("text", kString, s("#ffffff")), dout("out", kCol4)});
        add("color.toHex", "Color To Hex", "Color", true, {
            din("value", kCol4), dout("out", kString)});

        // ---- Logic and validation
        add("logic.and", "And", "Logic", true, binaryBoolPins());
        add("logic.or", "Or", "Logic", true, binaryBoolPins());
        add("logic.xor", "Xor", "Logic", true, binaryBoolPins());
        add("logic.not", "Not", "Logic", true, {
            din("a", kBool, bl(false)), dout("out", kBool)});
        add("logic.greater", "Greater", "Logic", true, binaryMathPins());
        add("logic.less", "Less", "Logic", true, binaryMathPins());
        add("logic.greaterEqual", "Greater Or Equal", "Logic", true, binaryMathPins());
        add("logic.lessEqual", "Less Or Equal", "Logic", true, binaryMathPins());
        add("logic.equal", "Equal", "Logic", true, binaryMathPins());
        add("logic.notEqual", "Not Equal", "Logic", true, binaryMathPins());
        add("logic.between", "Between", "Logic", true, {
            din("value", kFloat, f(0)), din("min", kFloat, f(0)), din("max", kFloat, f(1)), dout("out", kBool)});
        add("logic.approximately", "Approximately Equal", "Logic", true, {
            din("a", kFloat, f(0)), din("b", kFloat, f(0)), din("tolerance", kFloat, f(0.0001)),
            dout("out", kBool)});
        add("logic.isNil", "Is Nil", "Logic", true, {
            din("value", kAny), dout("out", kBool)});
        add("logic.isNotNil", "Is Not Nil", "Logic", true, {
            din("value", kAny), dout("out", kBool)});
        add("logic.select", "Select", "Logic", true, {
            din("condition", kBool, bl(false)), din("ifTrue", kAny), din("ifFalse", kAny), dout("out", kAny)});
        add("logic.typeOf", "Type Of", "Logic", true, {
            din("value", kAny), dout("out", kString)});
        add("logic.validate", "Validate Number", "Logic", true, {
            din("value", kFloat, f(0)), din("min", kFloat, f(0)), din("max", kFloat, f(1)),
            dout("out", kFloat), dout("clamped", kBool)});

        // ---- String
        add("string.concat", "Concatenate", "String", true, {
            din("a", kString, s("")), din("b", kString, s("")), dout("out", kString)});
        add("string.length", "Length", "String", true, {
            din("value", kString, s("")), dout("out", kInt)});
        add("string.upper", "Upper Case", "String", true, stringUnaryPins());
        add("string.lower", "Lower Case", "String", true, stringUnaryPins());
        add("string.trim", "Trim", "String", true, stringUnaryPins());
        add("string.contains", "Contains", "String", true, {
            din("value", kString, s("")), din("needle", kString, s("")), dout("out", kBool)});
        add("string.startsWith", "Starts With", "String", true, {
            din("value", kString, s("")), din("prefix", kString, s("")), dout("out", kBool)});
        add("string.endsWith", "Ends With", "String", true, {
            din("value", kString, s("")), din("suffix", kString, s("")), dout("out", kBool)});
        add("string.equals", "Text Equals", "String", true, {
            din("a", kString, s("")), din("b", kString, s("")), dout("out", kBool)});
        add("string.isEmpty", "Text Is Empty", "String", true, {
            din("value", kString, s("")), dout("out", kBool)});
        add("string.replace", "Replace", "String", true, {
            din("value", kString, s("")), din("from", kString, s("")), din("to", kString, s("")),
            dout("out", kString)});
        add("string.substring", "Substring", "String", true, {
            din("value", kString, s("")), din("start", kInt, i(0)), din("length", kInt, i(1)),
            dout("out", kString)});
        add("string.repeat", "Repeat", "String", true, {
            din("value", kString, s("")), din("count", kInt, i(2)), dout("out", kString)});
        // {0} {1} {2} are replaced by the a/b/c arguments.
        add("string.format", "Format", "String", true, {
            din("template", kString, s("Score: {0}")), din("a", kAny), din("b", kAny), din("c", kAny),
            dout("out", kString)});

        // ---- Casts
        add("cast.toBool", "To Bool", "Cast", true, {din("value", kAny), dout("out", kBool)});
        add("cast.toInt", "To Int", "Cast", true, {din("value", kAny), dout("out", kInt)});
        add("cast.toFloat", "To Float", "Cast", true, {din("value", kAny), dout("out", kFloat)});
        add("cast.toString", "To String", "Cast", true, {din("value", kAny), dout("out", kString)});
        add("cast.toVector2", "To Vector2", "Cast", true, {din("value", kAny), dout("out", kVec2)});
        add("cast.toVector3", "To Vector3", "Cast", true, {din("value", kAny), dout("out", kVec3)});
        add("cast.toVector4", "To Vector4", "Cast", true, {din("value", kAny), dout("out", kVec4)});
        add("cast.toColor3", "To Color", "Cast", true, {din("value", kAny), dout("out", kCol3)});
        add("cast.toColor4", "To Color With Alpha", "Cast", true, {din("value", kAny), dout("out", kCol4)});
        add("cast.toQuaternion", "To Quaternion", "Cast", true, {din("value", kAny), dout("out", kQuat)});
        add("cast.toEntity", "To Entity", "Cast", true, {din("value", kAny), dout("out", kEntity)});

        // ---- Entity. Every "target" pin left unconnected means the entity running
        // this graph, so a self-contained script never has to wire one.
        add("entity.self", "This Entity", "Entity", true, {dout("out", kEntity)});
        add("entity.getPosition", "Get Position", "Entity", true, {
            targetPin(), dout("position", kVec3)});
        add("entity.setPosition", "Set Position", "Entity", false, {
            xin(), din("position", kVec3, v3(0, 0, 0)), targetPin(), xout()});
        add("entity.translate", "Add Translation", "Entity", false, {
            xin(), din("delta", kVec3, v3(0, 0, 0)), targetPin(), xout()});
        add("entity.move", "Move By Speed", "Entity", false, {
            xin(), din("direction", kVec3, v3(0, 0, 0)), din("speed", kFloat, f(1)), targetPin(), xout()});
        add("entity.getRotation", "Get Rotation", "Entity", true, {
            targetPin(), dout("rotation", kQuat)});
        add("entity.setRotation", "Set Rotation", "Entity", false, {
            xin(), din("rotation", kQuat), targetPin(), xout()});
        add("entity.setEulerRotation", "Set Euler Rotation", "Entity", false, {
            xin(), din("euler", kVec3, v3(0, 0, 0)), din("order", kInt, i(0)), targetPin(), xout()});
        add("entity.rotate", "Add Rotation", "Entity", false, {
            xin(), din("delta", kVec3, v3(0, 0, 0)), targetPin(), xout()});
        add("entity.lookAt", "Look At Point", "Entity", false, {
            xin(), din("point", kVec3, v3(0, 0, 0)), din("up", kVec3, v3(0, 1, 0)), targetPin(), xout()});
        add("entity.getScale", "Get Scale", "Entity", true, {
            targetPin(), dout("scale", kVec3)});
        add("entity.setScale", "Set Scale", "Entity", false, {
            xin(), din("scale", kVec3, v3(1, 1, 1)), targetPin(), xout()});
        add("entity.scaleBy", "Multiply Scale", "Entity", false, {
            xin(), din("factor", kVec3, v3(1, 1, 1)), targetPin(), xout()});
        add("entity.getParent", "Get Parent", "Entity", true, {
            targetPin(), dout("parent", kEntity)});
        add("entity.setParent", "Set Parent", "Entity", false, {
            xin(), din("parent", kEntity), din("keepTransform", kBool, bl(true)), targetPin(), xout()});
        add("entity.setVisible", "Set Visible", "Entity", false, {
            xin(), din("visible", kBool, bl(true)), targetPin(), xout()});
        add("entity.isVisible", "Is Visible", "Entity", true, {
            targetPin(), dout("visible", kBool)});
        add("entity.getName", "Get Name", "Entity", true, {
            targetPin(), dout("name", kString)});
        add("entity.setName", "Set Name", "Entity", false, {
            xin(), din("name", kString, s("")), targetPin(), xout()});
        add("entity.findByName", "Find By Name", "Entity", true, {
            din("name", kString, s("")), dout("entity", kEntity)});
        add("entity.exists", "Entity Exists", "Entity", true, {
            targetPin(), dout("exists", kBool)});
        add("entity.count", "Entity Count", "Entity", true, {dout("count", kInt)});
        add("entity.destroy", "Destroy Entity", "Entity", false, {xin(), targetPin()});
        add("entity.spawn", "Spawn Bundle", "Entity", false, {
            xin(), din("bundle", kString, s("")), din("position", kVec3, v3(0, 0, 0)),
            din("rotation", kQuat), din("parent", kEntity), dout("entity", kEntity), xout()});
        add("entity.distanceTo", "Distance Between", "Entity", true, {
            din("a", kEntity), din("b", kEntity), dout("out", kFloat)});
        add("entity.directionTo", "Direction To", "Entity", true, {
            din("from", kEntity), din("to", kEntity), dout("out", kVec3)});
        add("entity.hasComponent", "Has Component", "Entity", true, {
            din("name", kString, s("Transform")), targetPin(), dout("has", kBool)});
        add("entity.setTag", "Set Tag", "Entity", false, {
            xin(), din("tag", kString, s("")), targetPin(), xout()});
        add("entity.getTag", "Get Tag", "Entity", true, {
            targetPin(), dout("tag", kString)});
        add("entity.hasTag", "Has Tag", "Entity", true, {
            din("tag", kString, s("")), targetPin(), dout("has", kBool)});
        add("entity.findByTag", "Find By Tag", "Entity", true, {
            din("tag", kString, s("")), dout("entity", kEntity)});
        add("entity.countTag", "Count By Tag", "Entity", true, {
            din("tag", kString, s("")), dout("count", kInt)});

        // ---- Components that gameplay graphs reach for constantly
        add("text.get", "Get Text", "UI", true, {
            targetPin(), dout("text", kString)});
        add("text.set", "Set Text", "UI", false, {
            xin(), din("text", kString, s("")), targetPin(), xout()});
        add("text.setFontSize", "Set Font Size", "UI", false, {
            xin(), din("size", kInt, i(20)), targetPin(), xout()});
        add("camera.get", "Get Scene Camera", "UI", true, {dout("camera", kEntity)});
        add("camera.setActive", "Set Scene Camera", "UI", false, {
            xin(), din("camera", kEntity), xout()});

        add("light.getIntensity", "Get Light Intensity", "Light", true, {
            targetPin(), dout("intensity", kFloat)});
        add("light.setIntensity", "Set Light Intensity", "Light", false, {
            xin(), din("intensity", kFloat, f(1)), targetPin(), xout()});
        add("light.setColor", "Set Light Color", "Light", false, {
            xin(), din("color", kCol3), targetPin(), xout()});
        add("light.setRange", "Set Light Range", "Light", false, {
            xin(), din("range", kFloat, f(10)), targetPin(), xout()});
        add("light.setShadows", "Set Light Shadows", "Light", false, {
            xin(), din("shadows", kBool, bl(true)), targetPin(), xout()});

        add("particles.play", "Play Particles", "Particles", false, {xin(), targetPin(), xout()});
        add("particles.stop", "Stop Particles", "Particles", false, {xin(), targetPin(), xout()});
        add("particles.reset", "Reset Particles", "Particles", false, {xin(), targetPin(), xout()});
        add("particles.setRate", "Set Particle Rate", "Particles", false, {
            xin(), din("rate", kInt, i(10)), targetPin(), xout()});

        // ---- Skeletal animation. "target" is the model entity; the nodes that take
        // an "animation" pin want one of its animation entities (anim.entity).
        add("anim.play", "Play Animation", "Animation", false, {
            xin(), din("name", kString, s("idle")), din("fade", kFloat, f(0.15)), targetPin(), xout()});
        add("anim.playIndex", "Play Animation Index", "Animation", false, {
            xin(), din("index", kInt, i(0)), din("fade", kFloat, f(0.15)), targetPin(), xout()});
        add("anim.stopAll", "Stop Animations", "Animation", false, {
            xin(), din("fade", kFloat, f(0.15)), targetPin(), xout()});
        add("anim.count", "Animation Count", "Animation", true, {
            targetPin(), dout("count", kInt)});
        add("anim.name", "Animation Name", "Animation", true, {
            din("index", kInt, i(0)), targetPin(), dout("name", kString)});
        add("anim.entity", "Animation Entity", "Animation", true, {
            din("index", kInt, i(0)), targetPin(), dout("entity", kEntity)});
        add("anim.findByName", "Find Animation", "Animation", true, {
            din("name", kString, s("idle")), targetPin(), dout("entity", kEntity)});
        add("anim.isPlaying", "Is Animation Playing", "Animation", true, {
            din("name", kString, s("idle")), targetPin(), dout("playing", kBool)});
        add("anim.start", "Start Animation", "Animation", false, {
            xin(), din("animation", kEntity), xout()});
        add("anim.setState", "Set Animation State", "Animation", false, {
            xin(), din("animation", kEntity), din("state", kInt, i(0)), xout()});
        add("anim.setLoop", "Set Animation Loop", "Animation", false, {
            xin(), din("animation", kEntity), din("loop", kBool, bl(true)), xout()});
        add("anim.isLoop", "Is Animation Looping", "Animation", true, {
            din("animation", kEntity), dout("loop", kBool)});
        add("anim.setSpeed", "Set Animation Speed", "Animation", false, {
            xin(), din("animation", kEntity), din("speed", kFloat, f(1)), xout()});
        add("anim.setWeight", "Set Animation Weight", "Animation", false, {
            xin(), din("animation", kEntity), din("weight", kFloat, f(1)), xout()});
        add("anim.setDuration", "Set Animation Duration", "Animation", false, {
            xin(), din("animation", kEntity), din("duration", kFloat, f(1)), xout()});
        add("anim.progress", "Animation Progress", "Animation", true, {
            din("animation", kEntity), dout("progress", kFloat)});
        add("anim.morphSet", "Set Morph Weight", "Animation", false, {
            xin(), din("morph", kString, s("")), din("weight", kFloat, f(0)), targetPin(), xout()});
        add("anim.morphGet", "Get Morph Weight", "Animation", true, {
            din("morph", kString, s("")), targetPin(), dout("weight", kFloat)});
        add("anim.boneEntity", "Bone Entity", "Animation", true, {
            din("bone", kString, s("")), targetPin(), dout("entity", kEntity)});
        add("anim.bonePosition", "Bone World Position", "Animation", true, {
            din("bone", kString, s("")), targetPin(), dout("position", kVec3)});
        add("anim.resetBindPose", "Reset To Bind Pose", "Animation", false, {xin(), targetPin(), xout()});

        // ---- Tween actions. Each create node starts the action and outputs its
        // entity, which is what the control nodes below take.
        add("action.moveTo", "Move To", "Action", false, {
            xin(), din("start", kVec3, v3(0, 0, 0)), din("end", kVec3, v3(1, 0, 0)),
            din("duration", kFloat, f(1)), din("loop", kBool, bl(false)),
            din("fromCurrent", kBool, bl(true)), din("ease", kInt, i(0)), targetPin(),
            dout("action", kEntity), xout()});
        add("action.rotateTo", "Rotate To", "Action", false, {
            xin(), din("start", kQuat), din("end", kQuat), din("duration", kFloat, f(1)),
            din("loop", kBool, bl(false)), din("ease", kInt, i(0)), targetPin(),
            dout("action", kEntity), xout()});
        add("action.spin", "Spin Around Axis", "Action", false, {
            xin(), din("axis", kVec3, v3(0, 1, 0)), din("angle", kFloat, f(360)),
            din("duration", kFloat, f(1)), din("loop", kBool, bl(true)), din("ease", kInt, i(0)),
            targetPin(), dout("action", kEntity), xout()});
        add("action.scaleTo", "Scale To", "Action", false, {
            xin(), din("start", kVec3, v3(1, 1, 1)), din("end", kVec3, v3(1, 1, 1)),
            din("duration", kFloat, f(1)), din("loop", kBool, bl(false)), din("ease", kInt, i(0)),
            targetPin(), dout("action", kEntity), xout()});
        add("action.fadeAlpha", "Fade Alpha", "Action", false, {
            xin(), din("start", kFloat, f(0)), din("end", kFloat, f(1)), din("duration", kFloat, f(1)),
            din("loop", kBool, bl(false)), din("ease", kInt, i(0)), targetPin(),
            dout("action", kEntity), xout()});
        add("action.colorTo", "Color To", "Action", false, {
            xin(), din("start", kCol3), din("end", kCol3), din("duration", kFloat, f(1)),
            din("loop", kBool, bl(false)), din("ease", kInt, i(0)), targetPin(),
            dout("action", kEntity), xout()});
        add("action.start", "Start Action", "Action", false, {xin(), din("action", kEntity), xout()});
        add("action.pause", "Pause Action", "Action", false, {xin(), din("action", kEntity), xout()});
        add("action.stop", "Stop Action", "Action", false, {xin(), din("action", kEntity), xout()});
        add("action.restart", "Restart Action", "Action", false, {xin(), din("action", kEntity), xout()});
        add("action.setSpeed", "Set Action Speed", "Action", false, {
            xin(), din("action", kEntity), din("speed", kFloat, f(1)), xout()});
        add("action.setWeight", "Set Action Weight", "Action", false, {
            xin(), din("action", kEntity), din("weight", kFloat, f(1)), xout()});
        add("action.setTarget", "Set Action Target", "Action", false, {
            xin(), din("action", kEntity), din("target", kEntity), xout()});
        add("action.isRunning", "Is Action Running", "Action", true, {
            din("action", kEntity), dout("running", kBool)});
        add("action.isPaused", "Is Action Paused", "Action", true, {
            din("action", kEntity), dout("paused", kBool)});
        add("action.time", "Action Time", "Action", true, {
            din("action", kEntity), dout("seconds", kFloat)});
        add("action.value", "Action Value", "Action", true, {
            din("action", kEntity), dout("value", kFloat)});
        add("action.stopAllOnTarget", "Stop Actions On Target", "Action", false, {
            xin(), targetPin(), xout()});

        // ---- Raycasts
        add("ray.cast", "Ray Cast", "Physics", true, {
            din("origin", kVec3, v3(0, 0, 0)), din("direction", kVec3, v3(0, 0, 1)),
            din("filter", kInt, i(1)), din("ignore", kEntity),
            dout("hit", kBool), dout("distance", kFloat), dout("point", kVec3), dout("normal", kVec3),
            dout("entity", kEntity)});
        add("ray.fromScreen", "Ray From Screen", "Physics", true, {
            din("screen", kVec2, v2(0, 0)), din("filter", kInt, i(1)), din("ignore", kEntity),
            dout("hit", kBool), dout("distance", kFloat), dout("point", kVec3), dout("normal", kVec3),
            dout("entity", kEntity)});

        // ---- 3D physics. Needs the entity to carry a loaded Body3D; the nodes warn
        // instead of touching a body the physics backend has not built.
        add("phys3d.applyImpulse", "Apply Impulse", "Physics", false, {
            xin(), din("impulse", kVec3, v3(0, 0, 0)), targetPin(), xout()});
        add("phys3d.applyImpulseAt", "Apply Impulse At Point", "Physics", false, {
            xin(), din("impulse", kVec3, v3(0, 0, 0)), din("point", kVec3, v3(0, 0, 0)), targetPin(), xout()});
        add("phys3d.applyForce", "Apply Force", "Physics", false, {
            xin(), din("force", kVec3, v3(0, 0, 0)), targetPin(), xout()});
        add("phys3d.applyTorque", "Apply Torque", "Physics", false, {
            xin(), din("torque", kVec3, v3(0, 0, 0)), targetPin(), xout()});
        add("phys3d.setLinearVelocity", "Set Linear Velocity", "Physics", false, {
            xin(), din("velocity", kVec3, v3(0, 0, 0)), targetPin(), xout()});
        add("phys3d.getLinearVelocity", "Get Linear Velocity", "Physics", true, {
            targetPin(), dout("velocity", kVec3)});
        add("phys3d.setAngularVelocity", "Set Angular Velocity", "Physics", false, {
            xin(), din("velocity", kVec3, v3(0, 0, 0)), targetPin(), xout()});
        add("phys3d.setPosition", "Set Body Position", "Physics", false, {
            xin(), din("position", kVec3, v3(0, 0, 0)), targetPin(), xout()});
        add("phys3d.getPosition", "Get Body Position", "Physics", true, {
            targetPin(), dout("position", kVec3)});
        add("phys3d.setGravityFactor", "Set Gravity Factor", "Physics", false, {
            xin(), din("factor", kFloat, f(1)), targetPin(), xout()});
        add("phys3d.getGravityFactor", "Get Gravity Factor", "Physics", true, {
            targetPin(), dout("factor", kFloat)});
        add("phys3d.setSensor", "Set Sensor", "Physics", false, {
            xin(), din("sensor", kBool, bl(true)), targetPin(), xout()});
        add("phys3d.isSensor", "Is Sensor", "Physics", true, {
            targetPin(), dout("sensor", kBool)});
        add("phys3d.setType", "Set Body Type", "Physics", false, {
            xin(), din("type", kInt, i(2)), targetPin(), xout()});
        add("phys3d.setMass", "Set Mass", "Physics", false, {
            xin(), din("mass", kFloat, f(1)), targetPin(), xout()});
        add("phys3d.activate", "Wake Body", "Physics", false, {xin(), targetPin(), xout()});
        add("phys3d.deactivate", "Sleep Body", "Physics", false, {xin(), targetPin(), xout()});

        // ---- 2D physics
        add("phys2d.applyImpulse", "Apply Impulse 2D", "Physics", false, {
            xin(), din("impulse", kVec2, v2(0, 0)), targetPin(), xout()});
        add("phys2d.setLinearVelocity", "Set Linear Velocity 2D", "Physics", false, {
            xin(), din("velocity", kVec2, v2(0, 0)), targetPin(), xout()});
        add("phys2d.getLinearVelocity", "Get Linear Velocity 2D", "Physics", true, {
            targetPin(), dout("velocity", kVec2)});
        add("phys2d.setAngularVelocity", "Set Angular Velocity 2D", "Physics", false, {
            xin(), din("velocity", kFloat, f(0)), targetPin(), xout()});
        add("phys2d.setGravityScale", "Set Gravity Scale 2D", "Physics", false, {
            xin(), din("scale", kFloat, f(1)), targetPin(), xout()});
        add("phys2d.setType", "Set Body Type 2D", "Physics", false, {
            xin(), din("type", kInt, i(2)), targetPin(), xout()});
        add("phys2d.setPosition", "Set Body Position 2D", "Physics", false, {
            xin(), din("position", kVec2, v2(0, 0)), targetPin(), xout()});

        // ---- Audio. A graph never creates a sound entity by itself except through
        // audio.playNew, whose output must be given back to audio.destroy.
        add("audio.play", "Play Sound", "Audio", false, {xin(), targetPin(), xout()});
        add("audio.playPath", "Load And Play Sound", "Audio", false, {
            xin(), din("path", kString, s("assets/sound/blank.ogg")), din("volume", kFloat, f(1)),
            din("loop", kBool, bl(false)), targetPin(), xout()});
        add("audio.playNew", "Play Sound On New Entity", "Audio", false, {
            xin(), din("path", kString, s("assets/sound/blank.ogg")), din("volume", kFloat, f(1)),
            din("loop", kBool, bl(false)), dout("entity", kEntity), xout()});
        add("audio.stop", "Stop Sound", "Audio", false, {xin(), targetPin(), xout()});
        add("audio.pause", "Pause Sound", "Audio", false, {xin(), targetPin(), xout()});
        add("audio.destroy", "Destroy Sound", "Audio", false, {xin(), targetPin(), xout()});
        add("audio.isPlaying", "Is Playing", "Audio", true, {
            targetPin(), dout("playing", kBool)});
        add("audio.setVolume", "Set Volume", "Audio", false, {
            xin(), din("volume", kFloat, f(1)), targetPin(), xout()});
        add("audio.getVolume", "Get Volume", "Audio", true, {
            targetPin(), dout("volume", kFloat)});
        add("audio.setLooping", "Set Looping", "Audio", false, {
            xin(), din("looping", kBool, bl(false)), targetPin(), xout()});
        add("audio.setSpeed", "Set Speed", "Audio", false, {
            xin(), din("speed", kFloat, f(1)), targetPin(), xout()});
        add("audio.setPan", "Set Pan", "Audio", false, {
            xin(), din("pan", kFloat, f(0)), targetPin(), xout()});
        add("audio.set3D", "Set 3D Sound", "Audio", false, {
            xin(), din("is3D", kBool, bl(true)), targetPin(), xout()});
        add("audio.setGlobalVolume", "Set Global Volume", "Audio", false, {
            xin(), din("volume", kFloat, f(1)), xout()});
        add("audio.setMuted", "Set Muted", "Audio", false, {
            xin(), din("muted", kBool, bl(false)), xout()});
        add("audio.stopAll", "Stop All Sounds", "Audio", false, {xin(), xout()});
        add("audio.pauseAll", "Pause All Sounds", "Audio", false, {xin(), xout()});
        add("audio.resumeAll", "Resume All Sounds", "Audio", false, {xin(), xout()});

        // ---- Input polling
        add("input.isKeyDown", "Is Key Down", "Input", true, {
            din("key", kInt, i(0)), dout("down", kBool)});
        add("input.isMouseDown", "Is Mouse Down", "Input", true, {
            din("button", kInt, i(0)), dout("down", kBool)});
        add("input.mousePosition", "Mouse Position", "Input", true, {dout("position", kVec2)});
        add("input.mouseScroll", "Mouse Scroll", "Input", true, {dout("offset", kVec2)});
        add("input.isTouch", "Is Touching", "Input", true, {dout("touching", kBool)});
        add("input.touchCount", "Touch Count", "Input", true, {dout("count", kInt)});
        add("input.touchPosition", "Touch Position", "Input", true, {
            din("pointer", kInt, i(0)), dout("position", kVec2)});
        add("input.modifiers", "Key Modifiers", "Input", true, {dout("modifiers", kInt)});
        add("input.gamepadConnected", "Gamepad Connected", "Input", true, {
            din("pad", kInt, i(0)), dout("connected", kBool)});
        add("input.gamepadButton", "Gamepad Button Down", "Input", true, {
            din("pad", kInt, i(0)), din("button", kInt, i(0)), dout("down", kBool)});
        add("input.gamepadAxis", "Gamepad Axis", "Input", true, {
            din("pad", kInt, i(0)), din("axis", kInt, i(0)), dout("value", kFloat)});
        add("input.gamepadCount", "Gamepad Count", "Input", true, {dout("count", kInt)});

        // ---- Time
        add("time.delta", "Delta Seconds", "Time", true, {dout("seconds", kFloat)});
        add("time.elapsed", "Elapsed Seconds", "Time", true, {dout("seconds", kFloat)});
        add("time.systemTime", "System Time", "Time", true, {dout("seconds", kFloat)});
        add("time.framerate", "Measured Framerate", "Time", true, {dout("fps", kFloat)});
        add("time.setUpdateMS", "Set Update Interval", "Time", false, {
            xin(), din("milliseconds", kInt, i(16)), xout()});
        add("time.setMaxDelta", "Set Max Delta", "Time", false, {
            xin(), din("seconds", kFloat, f(0.25)), xout()});

        // ---- Scene
        add("scene.load", "Load Scene By Name", "Scene", false, {
            xin(), din("name", kString, s("")), xout()});
        add("scene.loadById", "Load Scene By Id", "Scene", false, {
            xin(), din("id", kInt, i(0)), xout()});
        add("scene.quit", "Quit Application", "Scene", false, {xin(), xout()});
        add("scene.pause", "Pause Events", "Scene", false, {xin(), xout()});
        add("scene.resume", "Resume Events", "Scene", false, {xin(), xout()});
        add("scene.canvasWidth", "Canvas Width", "Scene", true, {dout("pixels", kInt)});
        add("scene.canvasHeight", "Canvas Height", "Scene", true, {dout("pixels", kInt)});
        add("scene.setBackground", "Set Background Color", "Scene", false, {
            xin(), din("color", kCol4), xout()});
        add("scene.setRenderDistance", "Set Render Distance", "Scene", false, {
            xin(), din("metres", kFloat, f(0)), xout()});

        // ---- Debug
        add("debug.print", "Print String", "Debug", false, {
            xin(), din("text", kString, s("")), xout()});
        add("debug.printValue", "Print Value", "Debug", false, {
            xin(), din("value", kAny), xout()});
        add("debug.warn", "Warn", "Debug", false, {
            xin(), din("text", kString, s("")), xout()});
        add("debug.error", "Error", "Debug", false, {
            xin(), din("text", kString, s("")), xout()});

        // Reflection-backed calls are intentionally represented by one generic
        // authoring row. A generated kind such as method.Object.setPosition is
        // resolved to this row by find(), while the node parameters retain the
        // reflected class/method name and argument count.
        std::vector<GraphPinDef> reflectedPins = {xin(), din("target", kEntity)};
        for (int i = 0; i < 8; ++i) reflectedPins.push_back(din("arg" + std::to_string(i), kAny));
        reflectedPins.push_back(dout("result", kAny));
        reflectedPins.push_back(xout());
        add("method.call", "Call Reflected Method", "Reflection", false,
            std::move(reflectedPins));
        add("debug.assert", "Assert", "Debug", false, {
            xin(), din("condition", kBool, bl(true)), din("text", kString, s("")), xout()});
    }

    void NodeCatalog::add(const std::string& kind, const std::string& title, const std::string& category,
                          bool pure, std::vector<GraphPinDef> pins, bool sequence) {
        GraphNodeDef def;
        def.kind = kind;
        def.title = title;
        def.category = category;
        def.pins = pins;
        def.pure = pure;
        def.sequence = sequence;

        int dataInputs = 0;
        int dataOutputs = 0;
        for (const GraphPinDef& pin : pins) {
            if (pin.kind != GraphPinKind::Data) continue;
            if (pin.dir == GraphPinDir::Input) {
                inputOrdinals[kind][pin.name] = dataInputs++;
            } else {
                outputOrdinals[kind][pin.name] = dataOutputs++;
            }
        }

        index[kind] = defs.size();
        defs.push_back(std::move(def));
    }

    const NodeCatalog& NodeCatalog::core() {
        static NodeCatalog catalog;
        return catalog;
    }

    const GraphNodeDef* NodeCatalog::find(const std::string& kind) const {
        std::map<std::string, size_t>::const_iterator it = index.find(kind);
        if (it == index.end()) {
            if (kind.compare(0, 7, "method.") == 0) {
                it = index.find("method.call");
                if (it != index.end()) return &defs[it->second];
            }
            return nullptr;
        }
        return &defs[it->second];
    }

    int NodeCatalog::dataInputOrdinal(const std::string& kind, const std::string& pin) const {
        std::map<std::string, std::map<std::string, int> >::const_iterator byKind =
            inputOrdinals.find(kind);
        if (byKind == inputOrdinals.end()) {
            return -1;
        }
        std::map<std::string, int>::const_iterator byName = byKind->second.find(pin);
        return byName == byKind->second.end() ? -1 : byName->second;
    }

    int NodeCatalog::dataOutputOrdinal(const std::string& kind, const std::string& pin) const {
        std::map<std::string, std::map<std::string, int> >::const_iterator byKind =
            outputOrdinals.find(kind);
        if (byKind == outputOrdinals.end()) {
            return -1;
        }
        std::map<std::string, int>::const_iterator byName = byKind->second.find(pin);
        return byName == byKind->second.end() ? -1 : byName->second;
    }

    std::vector<GraphPinDef> NodeCatalog::resolvePins(const GraphNode& node, const GraphAsset* asset) const {
        const GraphNodeDef* def = find(node.kind);
        if (!def) {
            return {};
        }

        std::vector<GraphPinDef> pins = def->pins;

        if (node.kind.compare(0, 4, "var.") == 0) {
            GraphValueType type = GraphValueType::Any;
            std::string variableName = node.getStringParam("variable");
            if (asset && !variableName.empty()) {
                const GraphVariable* variable = asset->findVariable(variableName);
                if (variable) {
                    type = variable->type;
                }
            }
            for (GraphPinDef& pin : pins) {
                // Only the pins that carry the variable itself take its type; the
                // operand of var.add and friends stays a plain number.
                const bool carriesVariable = node.kind == "var.get" ||
                                             (node.kind == "var.set" && pin.name == "value");
                if (carriesVariable && pin.kind == GraphPinKind::Data) {
                    pin.type = type;
                }
            }
        } else if (node.kind == "flow.sequence") {
            const int count = clampCount(node.getFloatParam("count", 2.0), 2, 16);
            pins.erase(std::remove_if(pins.begin(), pins.end(), [](const GraphPinDef& pin) {
                return pin.kind == GraphPinKind::Exec && pin.dir == GraphPinDir::Output;
            }), pins.end());
            for (int c = 0; c < count; ++c) {
                pins.push_back(execPinOut("then" + std::to_string(c)));
            }
        } else if (node.kind == "flow.switchInt" || node.kind == "flow.switchString") {
            const int cases = clampCount(node.getFloatParam("cases", 3.0), 1, 16);
            pins.erase(std::remove_if(pins.begin(), pins.end(), [](const GraphPinDef& pin) {
                return pin.kind == GraphPinKind::Exec && pin.dir == GraphPinDir::Output;
            }), pins.end());
            for (int c = 0; c < cases; ++c) {
                pins.push_back(execPinOut("case" + std::to_string(c)));
            }
            pins.push_back(execPinOut("default"));
        } else if (node.kind == "event.custom") {
            const int args = clampCount(node.getFloatParam("args", 2.0), 0, 8);
            pins.erase(std::remove_if(pins.begin(), pins.end(), [](const GraphPinDef& pin) {
                return pin.kind == GraphPinKind::Data && pin.dir == GraphPinDir::Output;
            }), pins.end());
            for (int a = 0; a < args; ++a) {
                pins.push_back(dataPinOut("arg" + std::to_string(a), GraphValueType::Any));
            }
        } else if (node.kind == "method.call" || node.kind.compare(0, 7, "method.") == 0) {
            const int args = clampCount(node.getFloatParam("args", 0.0), 0, 8);
            pins.erase(std::remove_if(pins.begin(), pins.end(), [](const GraphPinDef& pin) {
                return pin.kind == GraphPinKind::Data && pin.dir == GraphPinDir::Input
                    && pin.name.compare(0, 3, "arg") == 0;
            }), pins.end());
            std::vector<GraphPinDef>::iterator result = std::find_if(pins.begin(), pins.end(),
                [](const GraphPinDef& pin) { return pin.name == "result"; });
            const size_t resultPosition = result == pins.end()
                ? pins.size() : static_cast<size_t>(result - pins.begin());
            for (int a = 0; a < args; ++a) {
                pins.insert(pins.begin() + static_cast<std::ptrdiff_t>(resultPosition + a),
                    dataPinIn("arg" + std::to_string(a), GraphValueType::Any));
            }
        }

        return pins;
    }

    bool graphEventIdForNode(const GraphNode& node, std::string& id) {
        id.clear();
        if (node.kind.compare(0, 6, "event.") != 0) {
            return false;
        }

        static const struct { const char* kind; const char* eventId; const char* filter; } kEvents[] = {
            {"event.start", "start", nullptr},
            {"event.update", "update", nullptr},
            {"event.fixedUpdate", "fixedUpdate", nullptr},
            {"event.postUpdate", "postUpdate", nullptr},
            {"event.draw", "draw", nullptr},
            {"event.pause", "pause", nullptr},
            {"event.resume", "resume", nullptr},
            {"event.shutdown", "shutdown", nullptr},
            {"event.sceneLoaded", "sceneLoaded", nullptr},
            {"event.keyDown", "keydown", "key"},
            {"event.keyUp", "keyup", "key"},
            {"event.charInput", "char", nullptr},
            {"event.mouseDown", "mousedown", "button"},
            {"event.mouseUp", "mouseup", "button"},
            {"event.mouseMove", "mousemove", nullptr},
            {"event.mouseScroll", "mousescroll", nullptr},
            {"event.mouseEnter", "mouseEnter", nullptr},
            {"event.mouseLeave", "mouseLeave", nullptr},
            {"event.touchStart", "touchstart", nullptr},
            {"event.touchEnd", "touchend", nullptr},
            {"event.touchMove", "touchmove", nullptr},
            {"event.touchCancel", "touchcancel", nullptr},
            {"event.gamepadButtonDown", "gamepaddown", "button"},
            {"event.gamepadButtonUp", "gamepadup", "button"},
            {"event.gamepadAxisMove", "gamepadaxis", "axis"},
            {"event.gamepadConnect", "gamepadconnect", nullptr},
            {"event.gamepadDisconnect", "gamepaddisconnect", nullptr},
            {"event.dispatch", nullptr, nullptr},
            {"event.dispatchAll", nullptr, nullptr},
        };

        for (const auto& entry : kEvents) {
            if (node.kind == entry.kind) {
                if (!entry.eventId) {
                    return false;          // dispatch nodes fire, they do not bind
                }
                id = entry.eventId;
                if (entry.filter) {
                    const GraphParam* param = node.findParam(entry.filter);
                    if (param && param->value.type == GraphValueType::Int) {
                        const int64_t value = param->value.asInt();
                        if (value >= 0) {
                            id += ":" + std::to_string(value);
                        }
                    }
                }
                return true;
            }
        }

        if (node.kind == "event.custom") {
            id = std::string(GRAPH_CUSTOM_EVENT_PREFIX) + node.getStringParam("name", "hit");
            return true;
        }
        return false;
    }

}
}
