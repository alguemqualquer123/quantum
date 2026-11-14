// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "NodeOps.h"

#include "../LuaBinding.h"
#include "lua.hpp"
#include "LuaBridge.h"

#include <algorithm>
#include <string>

namespace quantum {
namespace graph {

    namespace {

        bool splitMethodKind(const BoundNode& node, std::string& className, std::string& methodName) {
            className.clear();
            methodName.clear();
            if (!node.source) return false;
            const std::string& kind = node.source->kind;
            if (kind == "method.call") {
                className = node.source->getStringParam("class", "Object");
                methodName = node.source->getStringParam("method");
            } else if (kind.compare(0, 7, "method.") == 0) {
                const size_t separator = kind.find('.', 7);
                if (separator == std::string::npos) return false;
                className = kind.substr(7, separator - 7);
                methodName = kind.substr(separator + 1);
            }
            return !className.empty() && !methodName.empty();
        }

        bool pushGraphValue(lua_State* state, const GraphValue& value) {
            switch (value.type) {
                case GraphValueType::None:
                    lua_pushnil(state);
                    return true;
                case GraphValueType::Bool:
                    lua_pushboolean(state, value.asBool() ? 1 : 0);
                    return true;
                case GraphValueType::Int:
                case GraphValueType::Entity:
                    lua_pushinteger(state, static_cast<lua_Integer>(value.asInt()));
                    return true;
                case GraphValueType::Float:
                    lua_pushnumber(state, static_cast<lua_Number>(value.asFloat()));
                    return true;
                case GraphValueType::String: {
                    const std::string text = value.asString();
                    lua_pushlstring(state, text.data(), text.size());
                    return true;
                }
                case GraphValueType::Vector2:
                    return static_cast<bool>(luabridge::push<Vector2>(state, value.asVector2()));
                case GraphValueType::Vector3:
                case GraphValueType::Color3:
                    return static_cast<bool>(luabridge::push<Vector3>(state, value.asVector3()));
                case GraphValueType::Vector4:
                case GraphValueType::Color4:
                    return static_cast<bool>(luabridge::push<Vector4>(state, value.asVector4()));
                case GraphValueType::Quaternion:
                    return static_cast<bool>(luabridge::push<Quaternion>(state, value.asQuaternion()));
                case GraphValueType::Rect:
                    return static_cast<bool>(luabridge::push<Rect>(state, value.asRect()));
                case GraphValueType::Any:
                    lua_pushnil(state);
                    return true;
            }
            lua_pushnil(state);
            return false;
        }

        GraphValue readReturnValue(lua_State* state, const GraphNode& source) {
            GraphValueType expected = GraphValueType::Any;
            const std::string returnType = source.getStringParam("returnType", "any");
            if (!returnType.empty() && returnType != "any") {
                graphValueTypeFromName(returnType, expected);
            }
            if (expected == GraphValueType::Bool && lua_isboolean(state, -1))
                return GraphValue::makeBool(lua_toboolean(state, -1) != 0);
            if (expected == GraphValueType::Int && lua_isnumber(state, -1))
                return GraphValue::makeInt(static_cast<int64_t>(lua_tointeger(state, -1)));
            if (expected == GraphValueType::Float && lua_isnumber(state, -1))
                return GraphValue::makeFloat(static_cast<double>(lua_tonumber(state, -1)));
            if (expected == GraphValueType::String && lua_isstring(state, -1))
                return GraphValue::makeString(lua_tostring(state, -1));
            if (expected == GraphValueType::Entity && lua_isnumber(state, -1))
                return GraphValue::makeEntity(static_cast<Entity>(lua_tointeger(state, -1)));
            if (expected == GraphValueType::Any) {
                if (lua_isboolean(state, -1)) return GraphValue::makeBool(lua_toboolean(state, -1) != 0);
                if (lua_isinteger(state, -1)) return GraphValue::makeInt(static_cast<int64_t>(lua_tointeger(state, -1)));
                if (lua_isnumber(state, -1)) return GraphValue::makeFloat(static_cast<double>(lua_tonumber(state, -1)));
                if (lua_isstring(state, -1)) return GraphValue::makeString(lua_tostring(state, -1));
            }
            return GraphValue::makeNone();
        }

        int reflectedMethod(GraphRuntime& runtime, const BoundNode& node) {
            std::string className;
            std::string methodName;
            if (!splitMethodKind(node, className, methodName)) {
                op::warn(runtime, node, "reflected method needs class and method names");
                return 0;
            }
            lua_State* state = LuaBinding::getLuaState();
            if (!state) {
                op::warn(runtime, node, "Lua VM is not initialized");
                return 0;
            }

            const int top = lua_gettop(state);
            const Entity target = op::target(runtime, node, 0);
            if (!LuaBinding::pushEntityHandleForGraph(state, runtime.getInstance().scene,
                    target, className)) {
                op::warn(runtime, node, "target could not be wrapped as an entity handle");
                lua_settop(state, top);
                return 0;
            }
            lua_getfield(state, -1, methodName.c_str());
            if (!lua_isfunction(state, -1)) {
                op::warn(runtime, node, "reflected method is not registered in Lua bindings");
                lua_settop(state, top);
                return 0;
            }
            lua_insert(state, -2); // method, target

            const int argumentCount = std::clamp(static_cast<int>(node.source->getFloatParam("args", 0.0)), 0, 8);
            for (int i = 0; i < argumentCount; ++i) {
                if (!pushGraphValue(state, op::in(runtime, node, static_cast<size_t>(i + 1)))) {
                    op::warn(runtime, node, "unable to convert reflected method argument");
                    lua_settop(state, top);
                    return 0;
                }
            }
            if (LuaBinding::pcallWithTraceback(state, argumentCount + 1, 1) != 0) {
                const std::string error = LuaBinding::getLuaStackErrorString(state, -1);
                op::warn(runtime, node, "reflected method failed: " + error);
                lua_settop(state, top);
                return 0;
            }
            if (node.outputs.size() > 0) {
                runtime.writeOutput(node, 0, readReturnValue(state, *node.source));
            }
            lua_settop(state, top);
            return 0;
        }

    }

    void registerReflectionOps() {
        registerGraphOp("method.call", BoundOpKind::Exec, &reflectedMethod, nullptr);
    }

}
}
