// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "PluginManager.h"

using namespace quantum;
using namespace quantum::plugin;

namespace {

    // Raw C functions, not bound C++ signatures: LuaBridge satisfies a declared `lua_State*` by
    // consuming stack slot 1, so a function that wants both the VM and its own arguments has to read
    // the stack itself. The engine already binds this way for Vector3.__mul
    // (binding/MathClassesLua.cpp:111) and for RegisterEvent (LuaBinding.cpp:504).

    void pushValue(lua_State* L, const UiValue& value) {
        switch (value.kind) {
            case UiValue::Kind::Bool: lua_pushboolean(L, value.toBool() ? 1 : 0); break;
            case UiValue::Kind::Int: lua_pushinteger(L, static_cast<lua_Integer>(value.toInt())); break;
            case UiValue::Kind::Float: lua_pushnumber(L, static_cast<lua_Number>(value.toFloat())); break;
            case UiValue::Kind::String: lua_pushstring(L, value.toString().c_str()); break;
            default: lua_pushnil(L); break;
        }
    }

    // A Lua number is one type and UiValue keeps int and float apart, so a whole number becomes an
    // Int: a plugin reading getInt("repeat") out of `repeat = 3` would otherwise get a Float back and
    // fall through to its own default.
    UiValue valueFromStack(lua_State* L, int index) {
        switch (lua_type(L, index)) {
            case LUA_TBOOLEAN: return UiValue::fromBool(lua_toboolean(L, index) != 0);
            case LUA_TSTRING: return UiValue::fromString(lua_tostring(L, index));
            case LUA_TNUMBER: {
                const double number = static_cast<double>(lua_tonumber(L, index));
                if (number == static_cast<double>(static_cast<long long>(number))) {
                    return UiValue::fromInt(static_cast<long long>(number));
                }
                return UiValue::fromFloat(number);
            }
            default: return UiValue();
        }
    }

    void pushStringArray(lua_State* L, const std::vector<std::string>& items) {
        lua_createtable(L, static_cast<int>(items.size()), 0);
        for (size_t i = 0; i < items.size(); i++) {
            lua_pushstring(L, items[i].c_str());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
    }

    // The table is just below the value the caller pushed.
    void assignField(lua_State* L, const char* key) { lua_setfield(L, -2, key); }

    int listPlugins(lua_State* L) {
        std::vector<std::string> ids;
        for (const PluginRecord* record : PluginManager::instance().records()) {
            ids.push_back(record->manifest.id);
        }
        pushStringArray(L, ids);
        return 1;
    }

    int listCommands(lua_State* L) {
        std::vector<std::string> names;
        for (const PluginCommandInfo& command : PluginManager::instance().registry().commands()) {
            names.push_back(command.name);
        }
        pushStringArray(L, names);
        return 1;
    }

    int pluginState(lua_State* L) {
        const char* id = lua_tostring(L, 1);
        const PluginRecord* record = id ? PluginManager::instance().find(id) : nullptr;
        // "missing" rather than nil: a script comparing states wants one string type back.
        lua_pushstring(L, record ? pluginStateName(record->state) : "missing");
        return 1;
    }

    int isPluginEnabled(lua_State* L) {
        const char* id = lua_tostring(L, 1);
        lua_pushboolean(L, id && PluginManager::instance().isEnabled(id) ? 1 : 0);
        return 1;
    }

    int setPluginEnabled(lua_State* L) {
        const char* id = lua_tostring(L, 1);
        const bool enabled = lua_toboolean(L, 2) != 0;
        lua_pushboolean(L, id && PluginManager::instance().setEnabled(id, enabled) ? 1 : 0);
        return 1;
    }

    int describePlugin(lua_State* L) {
        const char* id = lua_tostring(L, 1);
        const PluginRecord* record = id ? PluginManager::instance().find(id) : nullptr;
        if (!record) {
            // An empty table rather than nil: `Plugins.describe("x").state` reads "nil" either way,
            // and a script that walks the fields never indexes into nil.
            lua_createtable(L, 0, 0);
            return 1;
        }

        const PluginManifest& manifest = record->manifest;
        lua_createtable(L, 0, 12);
        lua_pushstring(L, manifest.id.c_str()); assignField(L, "id");
        lua_pushstring(L, manifest.name.c_str()); assignField(L, "name");
        lua_pushstring(L, manifest.version.c_str()); assignField(L, "version");
        lua_pushstring(L, manifest.author.c_str()); assignField(L, "author");
        lua_pushstring(L, manifest.description.c_str()); assignField(L, "description");
        lua_pushstring(L, manifest.type.c_str()); assignField(L, "type");
        lua_pushinteger(L, static_cast<lua_Integer>(manifest.abiVersion)); assignField(L, "abi");
        lua_pushstring(L, manifest.directory.c_str()); assignField(L, "folder");
        lua_pushstring(L, manifest.panelTitle.c_str()); assignField(L, "panel");
        lua_pushstring(L, pluginStateName(record->state)); assignField(L, "state");
        lua_pushstring(L, record->error.c_str()); assignField(L, "error");
        lua_pushboolean(L, PluginManager::instance().isEnabled(manifest.id) ? 1 : 0);
        assignField(L, "enabled");
        pushStringArray(L, manifest.permissions); assignField(L, "permissions");
        pushStringArray(L, manifest.commands); assignField(L, "commands");
        return 1;
    }

    // The one call a shipped game makes: {ok, value, error} instead of an exception, because
    // _HAS_EXCEPTIONS=0 means nothing may throw across this boundary anyway, and a script that
    // forgets to check still gets a readable table rather than a stopped VM.
    int callPlugin(lua_State* L) {
        const char* command = lua_tostring(L, 1);

        UiValueMap values;
        if (lua_istable(L, 2)) {
            lua_pushnil(L);  // first key
            while (lua_next(L, 2) != 0) {
                // key at -2, value at -1. Only the string keys are arguments; the array part of a
                // table is positional and the boundary has no positional form.
                if (lua_type(L, -2) == LUA_TSTRING) {
                    values[lua_tostring(L, -2)] = valueFromStack(L, -1);
                }
                lua_pop(L, 1);
            }
        }

        UiValue returned;
        std::string error;
        bool ok = false;
        if (command) {
            ok = PluginManager::instance().executeCommand(command, values, returned, error);
        } else {
            error = "Plugins.call needs a command name as a string";
        }

        lua_createtable(L, 0, 3);
        lua_pushboolean(L, ok ? 1 : 0); assignField(L, "ok");
        pushValue(L, ok ? returned : UiValue()); assignField(L, "value");
        lua_pushstring(L, error.c_str()); assignField(L, "error");
        return 1;
    }

}

void LuaBinding::registerPluginClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    // A namespace of functions, not a class: PluginManager is a process-wide singleton and there is
    // no plugin object to hand a script.
    luabridge::getGlobalNamespace(L)
        .beginNamespace("Plugins")
        .addFunction("list", &listPlugins)
        .addFunction("commands", &listCommands)
        .addFunction("state", &pluginState)
        .addFunction("describe", &describePlugin)
        .addFunction("isEnabled", &isPluginEnabled)
        .addFunction("setEnabled", &setPluginEnabled)
        .addFunction("call", &callPlugin)
        .endNamespace();
#endif //DISABLE_LUA_BINDINGS
}
