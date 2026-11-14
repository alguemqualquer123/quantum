// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef luadebugger_h
#define luadebugger_h

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

#include "Export.h"

struct lua_State;
struct lua_Debug;

namespace quantum {

    class QUANTUM_API LuaDebugger {
    public:
        enum class State {
            Idle,
            Running,
            Paused,
            Completed,
            Error
        };

        enum class StopReason {
            None,
            Breakpoint,
            Step,
            Paused,
            Error
        };

        // Protected Lua failures are classified at the boundary that invokes
        // them so tooling can ignore noisy callback failures without hiding
        // errors from ordinary script execution.
        enum class ErrorCategory : std::uint8_t {
            Runtime = 1u,
            Callback = 2u,
            Syntax = 4u
        };
        using ErrorCategoryMask = std::uint8_t;
        static constexpr ErrorCategoryMask kRuntimeErrorFilter =
            static_cast<ErrorCategoryMask>(ErrorCategory::Runtime);
        static constexpr ErrorCategoryMask kCallbackErrorFilter =
            static_cast<ErrorCategoryMask>(ErrorCategory::Callback);
        static constexpr ErrorCategoryMask kSyntaxErrorFilter =
            static_cast<ErrorCategoryMask>(ErrorCategory::Syntax);
        static constexpr ErrorCategoryMask kDefaultErrorFilter =
            kRuntimeErrorFilter | kCallbackErrorFilter;
        static constexpr ErrorCategoryMask kAllErrorFilter =
            kDefaultErrorFilter | kSyntaxErrorFilter;

        struct Breakpoint {
            int id = 0;
            std::string source;
            int line = 0;
            std::string condition;
            bool enabled = true;
        };

        struct Frame {
            int level = 0;
            std::string source;
            std::string name;
            std::string what;
            int line = 0;
            int currentLine = 0;
        };

        struct Diagnostic {
            std::string message;
            std::string source;
            int line = 0;
            std::string traceback;
        };

        explicit LuaDebugger(lua_State* mainState);
        ~LuaDebugger();

        bool startString(const std::string& source, const std::string& code);
        bool startFile(const std::string& path);

        // Hooks the debugger to the live Lua VM used by Play. Unlike a standalone
        // coroutine session, attached mode observes callbacks already running in
        // the engine and pauses game events cooperatively at the next safe frame.
        bool attach();
        bool isAttached() const { return attached; }
        void setPauseCallback(std::function<void(bool)> callback);

        bool getStopOnError() const { return errorFilter != 0; }
        void setStopOnError(bool enabled) { errorFilter = enabled ? kDefaultErrorFilter : 0; }
        ErrorCategoryMask getErrorFilter() const { return errorFilter; }
        void setErrorFilter(ErrorCategoryMask filter) { errorFilter = filter & kAllErrorFilter; }
        // Called by LuaBinding immediately after a protected Lua call fails.
        // The active debugger is looked up by lua_State, so no global engine
        // callback or second exception channel is required.
        static void notifyRuntimeError(lua_State* state,
                                       ErrorCategory category = ErrorCategory::Runtime);

        int addBreakpoint(const std::string& source, int line, const std::string& condition = {});
        bool removeBreakpoint(int id);
        void clearBreakpoints();
        const std::vector<Breakpoint>& getBreakpoints() const { return breakpoints; }

        bool resume();
        bool stepInto();
        bool stepOver();
        bool stepOut();
        void pause();
        // Releases the debug thread before the owning lua_State is closed.
        void shutdown();

        State getState() const { return state; }
        StopReason getStopReason() const { return stopReason; }
        const std::vector<Frame>& getCallStack() const { return callStack; }
        const Diagnostic& getDiagnostic() const { return diagnostic; }

        // Evaluates an expression while paused. The current frame's locals are
        // copied into a read-only-by-convention environment with _G fallback.
        bool evaluateWatch(const std::string& expression, std::string& value, std::string& error);

        static const char* stateName(State value);
        static const char* stopReasonName(StopReason value);

    private:
        enum class StepMode {
            None,
            Into,
            Over,
            Out
        };

        lua_State* mainState = nullptr;
        lua_State* thread = nullptr;
        int threadReference = -1;
        State state = State::Idle;
        StopReason stopReason = StopReason::None;
        StepMode stepMode = StepMode::None;
        int stepDepth = 0;
        std::string stepSource;
        int stepLine = 0;
        bool skipCurrentLocation = false;
        bool attached = false;
        ErrorCategoryMask errorFilter = kDefaultErrorFilter;
        int evaluationContextReference = -1;
        lua_State* evaluationContextState = nullptr;
        int nextBreakpointId = 1;
        std::vector<Breakpoint> breakpoints;
        std::vector<Frame> callStack;
        Diagnostic diagnostic;
        std::function<void(bool)> pauseCallback;

        static void hook(lua_State* state, lua_Debug* debug);
        void onHook(lua_State* state, lua_Debug* debug);
        bool shouldStop(lua_State* stateValue, const std::string& source, int line, int depth);
        int getDepth(lua_State* state) const;
        void captureCallStack();
        void captureEvaluationContext(lua_State* stateValue, lua_Debug* debug);
        void clearEvaluationContext();
        void captureError();
        void captureRuntimeError(lua_State* stateValue);
        void onRuntimeError(lua_State* stateValue, ErrorCategory category);
        void resetSession();
        lua_State* debugState() const;
        void notifyPause(bool paused);
        static std::string normalizeSource(const std::string& source);
        static std::string formatValue(lua_State* state, int index);
        bool evaluateExpression(lua_State* stateValue, const std::string& expression,
                                std::string& value, std::string& error, bool* truthy = nullptr);
    };

}

#endif /* luadebugger_h */
