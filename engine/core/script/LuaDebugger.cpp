// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "LuaDebugger.h"

#include "lua.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace quantum {

namespace {
    std::map<lua_State*, LuaDebugger*> activeDebuggers;

    std::string trim(const std::string& value) {
        size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
        size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
        return value.substr(begin, end - begin);
    }
}

LuaDebugger::LuaDebugger(lua_State* state) : mainState(state) {
}

LuaDebugger::~LuaDebugger() {
    resetSession();
}

const char* LuaDebugger::stateName(State value) {
    switch (value) {
        case State::Idle: return "idle";
        case State::Running: return "running";
        case State::Paused: return "paused";
        case State::Completed: return "completed";
        case State::Error: return "error";
    }
    return "unknown";
}

const char* LuaDebugger::stopReasonName(StopReason value) {
    switch (value) {
        case StopReason::None: return "none";
        case StopReason::Breakpoint: return "breakpoint";
        case StopReason::Step: return "step";
        case StopReason::Paused: return "pause";
        case StopReason::Error: return "error";
    }
    return "unknown";
}

std::string LuaDebugger::normalizeSource(const std::string& source) {
    std::string value = source;
    if (!value.empty() && value[0] == '@') value.erase(value.begin());
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.rfind("lua://", 0) == 0) value.erase(0, 6);
    return value;
}

void LuaDebugger::resetSession() {
    clearEvaluationContext();
    if (thread) {
        activeDebuggers.erase(thread);
        lua_sethook(thread, nullptr, 0, 0);
        if (mainState && threadReference >= 0) {
            luaL_unref(mainState, LUA_REGISTRYINDEX, threadReference);
        }
        thread = nullptr;
        threadReference = -1;
    }
    if (attached && mainState) {
        const auto it = activeDebuggers.find(mainState);
        if (it != activeDebuggers.end() && it->second == this) activeDebuggers.erase(it);
        lua_sethook(mainState, nullptr, 0, 0);
        attached = false;
        notifyPause(false);
    }
    state = State::Idle;
    stopReason = StopReason::None;
    stepMode = StepMode::None;
    callStack.clear();
}

bool LuaDebugger::startString(const std::string& source, const std::string& code) {
    if (!mainState || code.empty()) return false;
    resetSession();
    diagnostic = Diagnostic();

    thread = lua_newthread(mainState);
    threadReference = luaL_ref(mainState, LUA_REGISTRYINDEX);
    activeDebuggers[thread] = this;

    const std::string chunkName = "@" + source;
    if (luaL_loadbuffer(thread, code.data(), code.size(), chunkName.c_str()) != LUA_OK) {
        captureError();
        state = State::Error;
        return false;
    }

    lua_sethook(thread, &LuaDebugger::hook, LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, 0);
    state = State::Idle;
    stopReason = StopReason::None;
    return true;
}

bool LuaDebugger::startFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        diagnostic.message = "Could not open Lua file: " + path;
        diagnostic.source = path;
        state = State::Error;
        return false;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return startString(path, contents.str());
}

bool LuaDebugger::attach() {
    if (!mainState) return false;
    resetSession();
    diagnostic = Diagnostic();
    attached = true;
    activeDebuggers[mainState] = this;
    lua_sethook(mainState, &LuaDebugger::hook, LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET, 0);
    state = State::Running;
    stopReason = StopReason::None;
    return true;
}

void LuaDebugger::setPauseCallback(std::function<void(bool)> callback) {
    pauseCallback = std::move(callback);
    if (pauseCallback && state == State::Paused) pauseCallback(true);
}

void LuaDebugger::notifyRuntimeError(lua_State* stateValue, ErrorCategory category) {
    const auto it = activeDebuggers.find(stateValue);
    if (it != activeDebuggers.end() && it->second) it->second->onRuntimeError(stateValue, category);
}

lua_State* LuaDebugger::debugState() const {
    return thread ? thread : (attached ? mainState : nullptr);
}

void LuaDebugger::notifyPause(bool paused) {
    if (pauseCallback) pauseCallback(paused);
}

int LuaDebugger::addBreakpoint(const std::string& source, int line, const std::string& condition) {
    if (line <= 0) return 0;
    Breakpoint breakpoint;
    breakpoint.id = nextBreakpointId++;
    breakpoint.source = normalizeSource(source);
    breakpoint.line = line;
    breakpoint.condition = trim(condition);
    breakpoints.push_back(breakpoint);
    return breakpoint.id;
}

bool LuaDebugger::removeBreakpoint(int id) {
    const auto it = std::find_if(breakpoints.begin(), breakpoints.end(),
        [id](const Breakpoint& breakpoint) { return breakpoint.id == id; });
    if (it == breakpoints.end()) return false;
    breakpoints.erase(it);
    return true;
}

void LuaDebugger::clearBreakpoints() {
    breakpoints.clear();
}

int LuaDebugger::getDepth(lua_State* stateValue) const {
    int depth = 0;
    lua_Debug debug;
    while (lua_getstack(stateValue, depth, &debug)) ++depth;
    return depth;
}

bool LuaDebugger::shouldStop(lua_State* stateValue, const std::string& source, int line, int depth) {
    const std::string normalized = normalizeSource(source);
    if (stepMode == StepMode::Into) {
        return !(normalized == stepSource && line == stepLine);
    }
    if (stepMode == StepMode::Over) {
        return depth <= stepDepth && !(normalized == stepSource && line == stepLine);
    }
    if (stepMode == StepMode::Out) {
        return depth < stepDepth;
    }
    for (const Breakpoint& breakpoint : breakpoints) {
        if (!breakpoint.enabled || breakpoint.line != line) continue;
        bool sourceMatches = breakpoint.source == normalized;
        const std::string suffix = "/" + breakpoint.source;
        if (!sourceMatches && normalized.size() > suffix.size()
                && normalized.compare(normalized.size() - suffix.size(), suffix.size(), suffix) == 0) {
            sourceMatches = true;
        }
        if (!sourceMatches) continue;
        if (breakpoint.condition.empty()) return true;

        std::string value;
        std::string error;
        bool truthy = false;
        if (!evaluateExpression(stateValue, breakpoint.condition, value, error, &truthy)) {
            diagnostic.message = "Conditional breakpoint failed: " + error;
            diagnostic.source = normalized;
            diagnostic.line = line;
            return true;
        }
        if (truthy) return true;
    }
    return false;
}

void LuaDebugger::hook(lua_State* stateValue, lua_Debug* debug) {
    const auto it = activeDebuggers.find(stateValue);
    if (it != activeDebuggers.end()) it->second->onHook(stateValue, debug);
}

void LuaDebugger::onHook(lua_State* stateValue, lua_Debug* debug) {
    if (state == State::Error || state == State::Completed) return;
    if (debug->event != LUA_HOOKLINE && debug->event != LUA_HOOKCALL && debug->event != LUA_HOOKRET) return;

    lua_getinfo(stateValue, "Sln", debug);
    const std::string source = debug->source ? debug->source : "";
    const int line = debug->currentline;
    const int depth = getDepth(stateValue);
    if (line <= 0) return;

    if (skipCurrentLocation && normalizeSource(source) == stepSource && line == stepLine) {
        skipCurrentLocation = false;
        return;
    }

    if (!shouldStop(stateValue, source, line, depth)) return;

    if (stepMode == StepMode::None) stopReason = StopReason::Breakpoint;
    else stopReason = StopReason::Step;
    stepMode = StepMode::None;
    stepSource = normalizeSource(source);
    stepLine = line;
    captureCallStack();
    captureEvaluationContext(stateValue, debug);
    state = State::Paused;
    notifyPause(true);
    if (!attached && lua_isyieldable(stateValue)) lua_yield(stateValue, 0);
}

void LuaDebugger::captureCallStack() {
    callStack.clear();
    lua_State* stateValue = debugState();
    if (!stateValue) return;
    for (int level = 0; ; ++level) {
        lua_Debug debug;
        if (!lua_getstack(stateValue, level, &debug)) break;
        if (!lua_getinfo(stateValue, "Snl", &debug)) continue;
        Frame frame;
        frame.level = level;
        frame.source = debug.source ? normalizeSource(debug.source) : std::string();
        frame.name = debug.name ? debug.name : "?";
        frame.what = debug.what ? debug.what : "?";
        frame.line = debug.linedefined;
        frame.currentLine = debug.currentline;
        callStack.push_back(frame);
    }
}

void LuaDebugger::clearEvaluationContext() {
    if (evaluationContextState && evaluationContextReference >= 0) {
        luaL_unref(evaluationContextState, LUA_REGISTRYINDEX, evaluationContextReference);
    }
    evaluationContextState = nullptr;
    evaluationContextReference = -1;
}

void LuaDebugger::captureEvaluationContext(lua_State* stateValue, lua_Debug* debug) {
    clearEvaluationContext();
    if (!stateValue || !debug) return;

    lua_newtable(stateValue);
    const int context = lua_gettop(stateValue);
    for (int localIndex = 1; ; ++localIndex) {
        const char* name = lua_getlocal(stateValue, debug, localIndex);
        if (!name) break;
        lua_setfield(stateValue, context, name);
    }
    if (lua_getinfo(stateValue, "f", debug)) {
        const int functionIndex = lua_gettop(stateValue);
        for (int upvalueIndex = 1; ; ++upvalueIndex) {
            const char* name = lua_getupvalue(stateValue, functionIndex, upvalueIndex);
            if (!name) break;
            lua_setfield(stateValue, context, name);
        }
        lua_pop(stateValue, 1);
    }

    // The snapshot gets the same global fallback as a live frame, while its
    // values remain stable after the attached hook returns to C++.
    lua_newtable(stateValue);
    lua_pushglobaltable(stateValue);
    lua_setfield(stateValue, -2, "__index");
    lua_setmetatable(stateValue, context);
    evaluationContextReference = luaL_ref(stateValue, LUA_REGISTRYINDEX);
    evaluationContextState = stateValue;
}

void LuaDebugger::captureError() {
    diagnostic = Diagnostic();
    if (!thread) return;
    const int top = lua_gettop(thread);
    if (top > 0 && lua_isstring(thread, -1)) diagnostic.message = lua_tostring(thread, -1);
    if (diagnostic.message.empty()) diagnostic.message = "Lua execution failed";
    lua_Debug debug;
    if (lua_getstack(thread, 0, &debug) && lua_getinfo(thread, "Sl", &debug)) {
        diagnostic.source = debug.source ? normalizeSource(debug.source) : std::string();
        diagnostic.line = debug.currentline;
    }
    luaL_traceback(thread, thread, diagnostic.message.c_str(), 0);
    if (lua_isstring(thread, -1)) diagnostic.traceback = lua_tostring(thread, -1);
    lua_pop(thread, 1);
    captureCallStack();
}

void LuaDebugger::captureRuntimeError(lua_State* stateValue) {
    diagnostic = Diagnostic();
    if (!stateValue) return;
    const int top = lua_gettop(stateValue);
    if (top > 0 && lua_isstring(stateValue, -1)) {
        diagnostic.message = lua_tostring(stateValue, -1);
    }
    if (diagnostic.message.empty()) diagnostic.message = "Lua runtime error";
    luaL_traceback(stateValue, stateValue, diagnostic.message.c_str(), 0);
    if (lua_isstring(stateValue, -1)) diagnostic.traceback = lua_tostring(stateValue, -1);
    lua_pop(stateValue, 1);
    captureCallStack();
}

void LuaDebugger::onRuntimeError(lua_State* stateValue, ErrorCategory category) {
    if (!attached || state == State::Error || state == State::Completed) return;
    captureRuntimeError(stateValue);
    const ErrorCategoryMask categoryMask = static_cast<ErrorCategoryMask>(category);
    if ((errorFilter & categoryMask) == 0 || state != State::Running) return;
    stopReason = StopReason::Error;
    stepMode = StepMode::None;
    state = State::Paused;
    notifyPause(true);
}

bool LuaDebugger::resume() {
    lua_State* stateValue = debugState();
    if (!stateValue || state == State::Completed || state == State::Error) return false;
    if (attached) {
        if (state == State::Paused) skipCurrentLocation = true;
        state = State::Running;
        stopReason = StopReason::None;
        notifyPause(false);
        return true;
    }
    if (state == State::Paused) skipCurrentLocation = true;
    state = State::Running;
    stopReason = StopReason::None;
    int results = 0;
    const int status = lua_resume(thread, nullptr, 0, &results);
    if (status == LUA_YIELD) {
        state = State::Paused;
        return true;
    }
    if (status == LUA_OK) {
        if (results > 0) lua_pop(thread, results);
        state = State::Completed;
        stopReason = StopReason::None;
        return true;
    }
    captureError();
    state = State::Error;
    stopReason = StopReason::Error;
    return false;
}

void LuaDebugger::pause() {
    if (state == State::Running) {
        state = State::Paused;
        stopReason = StopReason::Paused;
        notifyPause(true);
    }
}

void LuaDebugger::shutdown() {
    resetSession();
}

bool LuaDebugger::stepInto() {
    if (state != State::Paused) return false;
    stepMode = StepMode::Into;
    stepDepth = getDepth(debugState());
    if (!callStack.empty()) {
        stepSource = callStack.front().source;
        stepLine = callStack.front().currentLine;
    }
    return resume();
}

bool LuaDebugger::stepOver() {
    if (state != State::Paused) return false;
    stepMode = StepMode::Over;
    stepDepth = getDepth(debugState());
    if (!callStack.empty()) {
        stepSource = callStack.front().source;
        stepLine = callStack.front().currentLine;
    }
    return resume();
}

bool LuaDebugger::stepOut() {
    if (state != State::Paused) return false;
    stepMode = StepMode::Out;
    stepDepth = getDepth(debugState());
    if (!callStack.empty()) {
        stepSource = callStack.front().source;
        stepLine = callStack.front().currentLine;
    }
    return resume();
}

std::string LuaDebugger::formatValue(lua_State* stateValue, int index) {
    if (lua_isnil(stateValue, index)) return "nil";
    if (lua_isboolean(stateValue, index)) return lua_toboolean(stateValue, index) ? "true" : "false";
    if (lua_isinteger(stateValue, index)) return std::to_string(lua_tointeger(stateValue, index));
    if (lua_isnumber(stateValue, index)) return std::to_string(lua_tonumber(stateValue, index));
    if (lua_isstring(stateValue, index)) return std::string("\"") + lua_tostring(stateValue, index) + "\"";
    return std::string("<") + luaL_typename(stateValue, index) + ">";
}

bool LuaDebugger::evaluateExpression(lua_State* stateValue, const std::string& expression,
                                     std::string& value, std::string& error, bool* truthy) {
    value.clear();
    error.clear();
    if (truthy) *truthy = false;
    if (!stateValue) {
        error = "no Lua state is available";
        return false;
    }

    const std::string trimmed = trim(expression);
    if (trimmed.empty()) {
        error = "expression is empty";
        return false;
    }

    lua_Debug frame;
    const bool hasFrame = lua_getstack(stateValue, 0, &frame) != 0;
    if (hasFrame) {
        for (int localIndex = 1; ; ++localIndex) {
            const char* name = lua_getlocal(stateValue, &frame, localIndex);
            if (!name) break;
            if (trimmed == name) {
                if (truthy) *truthy = !lua_isnil(stateValue, -1) &&
                    (!lua_isboolean(stateValue, -1) || lua_toboolean(stateValue, -1) != 0);
                value = formatValue(stateValue, -1);
                lua_pop(stateValue, 1);
                return true;
            }
            lua_pop(stateValue, 1);
        }

        // A callback's captured variables live in upvalues rather than locals.
        // Include them so watches and conditional breakpoints observe the same
        // lexical scope that the callback itself sees.
        if (lua_getinfo(stateValue, "f", &frame)) {
            const int functionIndex = lua_gettop(stateValue);
            for (int upvalueIndex = 1; ; ++upvalueIndex) {
                const char* name = lua_getupvalue(stateValue, functionIndex, upvalueIndex);
                if (!name) break;
                if (trimmed == name) {
                    if (truthy) *truthy = !lua_isnil(stateValue, -1) &&
                        (!lua_isboolean(stateValue, -1) || lua_toboolean(stateValue, -1) != 0);
                    value = formatValue(stateValue, -1);
                    lua_pop(stateValue, 2); // upvalue value and function
                    return true;
                }
                lua_pop(stateValue, 1);
            }
            lua_pop(stateValue, 1);
        }
    }

    // Give expressions access to the current frame's locals while retaining the
    // normal global environment. In attached mode the hook returns before the
    // editor/remote client can issue a watch, so use the immutable snapshot that
    // was captured at the stop point instead of assuming the Lua stack remains.
    const bool hasSnapshot = evaluationContextState == stateValue
        && evaluationContextReference >= 0;
    int environment = 0;
    if (hasSnapshot) {
        lua_rawgeti(stateValue, LUA_REGISTRYINDEX, evaluationContextReference);
        const int context = lua_gettop(stateValue);
        lua_newtable(stateValue);
        environment = lua_gettop(stateValue);
        lua_newtable(stateValue);
        lua_pushvalue(stateValue, context);
        lua_setfield(stateValue, -2, "__index");
        lua_setmetatable(stateValue, environment);
        lua_remove(stateValue, context);
        environment = context;
    } else {
        lua_newtable(stateValue);
        environment = lua_gettop(stateValue);
    }
    if (!hasSnapshot && hasFrame) {
        if (lua_getinfo(stateValue, "f", &frame)) {
            const int functionIndex = lua_gettop(stateValue);
            for (int upvalueIndex = 1; ; ++upvalueIndex) {
                const char* name = lua_getupvalue(stateValue, functionIndex, upvalueIndex);
                if (!name) break;
                lua_setfield(stateValue, environment, name);
            }
            lua_pop(stateValue, 1); // function pushed by lua_getinfo
        }
        for (int localIndex = 1; ; ++localIndex) {
            const char* name = lua_getlocal(stateValue, &frame, localIndex);
            if (!name) break;
            lua_setfield(stateValue, environment, name);
        }
    }
    if (!hasSnapshot) {
        lua_newtable(stateValue);
        lua_pushglobaltable(stateValue);
        lua_setfield(stateValue, -2, "__index");
        lua_setmetatable(stateValue, environment);
    }

    const std::string chunk = "return (" + trimmed + ")";
    if (luaL_loadbuffer(stateValue, chunk.data(), chunk.size(), "=(debug-expression)") != LUA_OK) {
        error = lua_tostring(stateValue, -1) ? lua_tostring(stateValue, -1) : "invalid expression";
        lua_pop(stateValue, 1);
        lua_remove(stateValue, environment);
        return false;
    }
    lua_pushvalue(stateValue, environment);
    if (!lua_setupvalue(stateValue, -2, 1)) {
        lua_pop(stateValue, 1);
    }
    lua_remove(stateValue, environment);

    if (lua_pcall(stateValue, 0, 1, 0) != LUA_OK) {
        error = lua_tostring(stateValue, -1) ? lua_tostring(stateValue, -1) : "expression evaluation failed";
        lua_pop(stateValue, 1);
        return false;
    }
    if (truthy) *truthy = !lua_isnil(stateValue, -1) &&
        (!lua_isboolean(stateValue, -1) || lua_toboolean(stateValue, -1) != 0);
    value = formatValue(stateValue, -1);
    lua_pop(stateValue, 1);
    return true;
}

bool LuaDebugger::evaluateWatch(const std::string& expression, std::string& value, std::string& error) {
    lua_State* stateValue = debugState();
    if (!stateValue || state != State::Paused) {
        value.clear();
        error = "watch requires a paused debug session";
        return false;
    }
    return evaluateExpression(stateValue, expression, value, error);
}

}
