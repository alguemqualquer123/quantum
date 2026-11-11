// (c) SR VINIX
// SPDX-License-Identifier: MIT

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    pragma comment(lib, "Ws2_32.lib")
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/select.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

#include "window/ScriptDebuggerWindow.h"

#include "LuaDebugger.h"
#include "Project.h"
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace quantum::editor {

namespace {

#if defined(_WIN32)
using DebugSocket = SOCKET;
constexpr DebugSocket invalidDebugSocket = INVALID_SOCKET;
#else
using DebugSocket = int;
constexpr DebugSocket invalidDebugSocket = -1;
#endif

void closeDebugSocket(DebugSocket socket) {
    if (socket == invalidDebugSocket) return;
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

int debugSocketError() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool debugWouldBlock(int error) {
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
#else
    return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS ||
        error == EALREADY || error == EINTR;
#endif
}

class DebugSocketRuntime {
public:
    bool start(std::string& error) {
#if defined(_WIN32)
        WSADATA data{};
        const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            error = "WSAStartup failed (" + std::to_string(result) + ")";
            return false;
        }
        initialized = true;
#else
        (void)error;
#endif
        return true;
    }

    ~DebugSocketRuntime() {
#if defined(_WIN32)
        if (initialized) ::WSACleanup();
#endif
    }

private:
#if defined(_WIN32)
    bool initialized = false;
#endif
};

bool setDebugNonBlocking(DebugSocket socket) {
#if defined(_WIN32)
    u_long enabled = 1;
    return ::ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool jsonStringField(const std::string& json, const char* key, std::string& value) {
    const std::string marker = std::string("\"") + key + "\":\"";
    const size_t begin = json.find(marker);
    if (begin == std::string::npos) return false;

    value.clear();
    for (size_t index = begin + marker.size(); index < json.size(); ++index) {
        const char character = json[index];
        if (character == '"') return true;
        if (character != '\\' || index + 1 >= json.size()) {
            value.push_back(character);
            continue;
        }
        const char escaped = json[++index];
        switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(escaped); break;
        }
    }
    return false;
}

bool jsonBoolField(const std::string& json, const char* key, bool& value) {
    const std::string marker = std::string("\"") + key + "\":";
    const size_t begin = json.find(marker);
    if (begin == std::string::npos) return false;
    const size_t valueBegin = begin + marker.size();
    if (json.compare(valueBegin, 4, "true") == 0) {
        value = true;
        return true;
    }
    if (json.compare(valueBegin, 5, "false") == 0) {
        value = false;
        return true;
    }
    return false;
}

bool jsonIntField(const std::string& json, const char* key, int& value) {
    const std::string marker = std::string("\"") + key + "\":";
    const size_t begin = json.find(marker);
    if (begin == std::string::npos) return false;
    const size_t valueBegin = begin + marker.size();
    char* end = nullptr;
    const long parsed = std::strtol(json.c_str() + valueBegin, &end, 10);
    if (end == json.c_str() + valueBegin) return false;
    value = static_cast<int>(parsed);
    return true;
}

std::string jsonEventName(const std::string& json) {
    std::string event;
    jsonStringField(json, "event", event);
    return event;
}

struct RemoteFrame {
    int level = 0;
    std::string source;
    std::string name;
    int line = 0;
};

struct RemoteBreakpoint {
    int id = 0;
    std::string specification;
};

}

class RemoteLuaDebuggerClient {
public:
    enum class State {
        Disconnected,
        Connecting,
        Connected,
        Ready
    };

    ~RemoteLuaDebuggerClient() { close(); }

    bool start(const std::string& host, int port, const std::string& token, int timeoutMs) {
        close();
        error.clear();
        tokenValue = token;
        timeoutMilliseconds = std::max(1, timeoutMs);
        connectDeadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeoutMilliseconds);

        if (host != "127.0.0.1" && host != "localhost") {
            error = "Remote debugger is restricted to loopback (127.0.0.1)";
            return false;
        }
        if (port < 1 || port > 65535) {
            error = "Port must be between 1 and 65535";
            return false;
        }
        if (token.find_first_of("\r\n") != std::string::npos) {
            error = "Token cannot contain a newline";
            return false;
        }
        if (!runtime.start(error)) return false;

        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == invalidDebugSocket) {
            error = "Could not create debugger socket (" + std::to_string(debugSocketError()) + ")";
            return false;
        }
        if (!setDebugNonBlocking(socket)) {
            error = "Could not make debugger socket nonblocking (" + std::to_string(debugSocketError()) + ")";
            close();
            return false;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        const int result = ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (result == 0) {
            state = State::Connected;
        } else if (debugWouldBlock(debugSocketError())) {
            state = State::Connecting;
        } else {
            error = "Could not connect to debugger (" + std::to_string(debugSocketError()) + ")";
            close();
            return false;
        }
        return true;
    }

    void close() {
        closeDebugSocket(socket);
        socket = invalidDebugSocket;
        state = State::Disconnected;
        receiveBuffer.clear();
        outgoing.clear();
        outgoingOffset = 0;
        authenticated = false;
        lastResponseRequestId.clear();
    }

    void poll() {
        if (socket == invalidDebugSocket) return;
        if (state == State::Connecting) {
            if (std::chrono::steady_clock::now() >= connectDeadline) {
                error = "Debugger connection timed out";
                close();
                return;
            }
            fd_set writable;
            fd_set exceptional;
            FD_ZERO(&writable);
            FD_ZERO(&exceptional);
            FD_SET(socket, &writable);
            FD_SET(socket, &exceptional);
            timeval timeout{};
#if defined(_WIN32)
            const int result = ::select(0, nullptr, &writable, &exceptional, &timeout);
#else
            const int result = ::select(socket + 1, nullptr, &writable, &exceptional, &timeout);
#endif
            if (result <= 0) return;
            int socketError = 0;
#if defined(_WIN32)
            int length = sizeof(socketError);
#else
            socklen_t length = static_cast<socklen_t>(sizeof(socketError));
#endif
            if (::getsockopt(socket, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char*>(&socketError), &length) != 0 || socketError != 0) {
                error = "Debugger connection failed (" + std::to_string(socketError ? socketError : debugSocketError()) + ")";
                close();
                return;
            }
            state = State::Connected;
        }
        if (state == State::Connected || state == State::Ready) {
            receive();
            flush();
        }
    }

    State getState() const { return state; }
    bool isReady() const { return state == State::Ready; }
    bool isConnecting() const { return state == State::Connecting; }
    const std::string& getError() const { return error; }
    const std::string& getStateName() const { return remoteState; }
    const std::string& getStopReason() const { return stopReason; }
    bool getStopOnError() const { return stopOnError; }
    const std::string& getErrorFilter() const { return errorFilter; }
    bool hasErrorCategory(const char* category) const {
        const std::string token = category ? category : "";
        size_t begin = 0;
        while (begin <= errorFilter.size()) {
            const size_t separator = errorFilter.find(',', begin);
            if (errorFilter.substr(begin, separator == std::string::npos
                                   ? std::string::npos : separator - begin) == token) return true;
            if (separator == std::string::npos) break;
            begin = separator + 1;
        }
        return false;
    }

    bool setErrorFilter(bool runtime, bool callback, bool syntax) {
        std::string categories;
        const auto append = [&categories](const char* category) {
            if (!categories.empty()) categories += ',';
            categories += category;
        };
        if (runtime) append("runtime");
        if (callback) append("callback");
        if (syntax) append("syntax");
        if (categories.empty()) categories = "none";
        return command("errors " + categories);
    }
    const std::string& getSource() const { return source; }
    int getLine() const { return line; }
    const std::string& getWatchValue() const { return watchValue; }
    const std::string& getWatchError() const { return watchError; }
    const std::vector<RemoteFrame>& getFrames() const { return frames; }
    const std::vector<RemoteBreakpoint>& getBreakpoints() const { return breakpoints; }
    const std::string& getLastResponseRequestId() const { return lastResponseRequestId; }

    bool command(const std::string& commandText) {
        if (!isReady() || commandText.find_first_of("\r\n") != std::string::npos) return false;
        outgoing += "@" + std::to_string(nextRequestId++) + " ";
        outgoing += commandText;
        outgoing.push_back('\n');
        return true;
    }

    void requestStatus() { command("status"); }

    bool deleteBreakpoint(int id) {
        if (id == 0 || !command("delete " + std::to_string(id))) return false;
        pendingDeleteId = id;
        return true;
    }

    void rememberBreakpoint(const std::string& specification) {
        pendingBreakpointSpecifications.push_back(specification);
    }

private:
    void receive() {
        char buffer[4096];
        for (;;) {
            const auto received = ::recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received == 0) {
                error = "Remote debugger closed the connection";
                close();
                return;
            }
            if (received < 0) {
                if (debugWouldBlock(debugSocketError())) break;
                error = "Debugger receive failed (" + std::to_string(debugSocketError()) + ")";
                close();
                return;
            }
            receiveBuffer.append(buffer, static_cast<std::size_t>(received));
            if (receiveBuffer.size() > maxLineLength) {
                error = "Remote debugger event exceeds 65536 bytes";
                close();
                return;
            }
            if (received < static_cast<int>(sizeof(buffer))) break;
        }

        for (;;) {
            const size_t newline = receiveBuffer.find('\n');
            if (newline == std::string::npos) break;
            std::string event = receiveBuffer.substr(0, newline);
            receiveBuffer.erase(0, newline + 1);
            if (!event.empty() && event.back() == '\r') event.pop_back();
            processEvent(event);
        }
    }

    void processEvent(const std::string& json) {
        lastResponseRequestId.clear();
        jsonStringField(json, "requestId", lastResponseRequestId);
        const std::string event = jsonEventName(json);
        if (event == "hello") {
            bool authRequired = false;
            jsonBoolField(json, "authRequired", authRequired);
            if (authRequired) {
                if (tokenValue.empty()) {
                    error = "Remote debugger requires a token";
                    close();
                    return;
                }
                outgoing += "auth " + tokenValue + '\n';
            } else {
                authenticated = true;
                state = State::Ready;
                requestStatus();
            }
            return;
        }
        if (event == "ready") {
            authenticated = true;
            state = State::Ready;
            requestStatus();
            return;
        }
        if (event == "status" || event == "command") {
            jsonStringField(json, "state", remoteState);
            jsonStringField(json, "stopReason", stopReason);
            jsonStringField(json, "source", source);
            jsonIntField(json, "line", line);
            jsonBoolField(json, "stopOnError", stopOnError);
            jsonStringField(json, "errorFilter", errorFilter);
            return;
        }
        if (event == "errorFilter") {
            jsonBoolField(json, "stopOnError", stopOnError);
            jsonStringField(json, "errorFilter", errorFilter);
            return;
        }
        if (event == "watch") {
            bool ok = false;
            jsonBoolField(json, "ok", ok);
            if (ok) {
                jsonStringField(json, "value", watchValue);
                watchError.clear();
            } else {
                jsonStringField(json, "error", watchError);
                watchValue.clear();
            }
            return;
        }
        if (event == "stack") {
            parseFrames(json);
            return;
        }
        if (event == "breakpoint") {
            int id = 0;
            bool removed = false;
            if (jsonIntField(json, "id", id)) {
                RemoteBreakpoint breakpoint;
                breakpoint.id = id;
                if (!pendingBreakpointSpecifications.empty()) {
                    breakpoint.specification = pendingBreakpointSpecifications.front();
                    pendingBreakpointSpecifications.erase(pendingBreakpointSpecifications.begin());
                }
                breakpoints.push_back(std::move(breakpoint));
            } else if (jsonBoolField(json, "removed", removed) && removed && pendingDeleteId != 0) {
                breakpoints.erase(std::remove_if(breakpoints.begin(), breakpoints.end(),
                    [this](const RemoteBreakpoint& breakpoint) { return breakpoint.id == pendingDeleteId; }),
                    breakpoints.end());
                pendingDeleteId = 0;
            }
            return;
        }
        if (event == "breakpoints") {
            bool cleared = false;
            if (jsonBoolField(json, "cleared", cleared) && cleared) breakpoints.clear();
            return;
        }
        if (event == "error") {
            jsonStringField(json, "message", error);
            close();
            return;
        }
        if (event == "bye") close();
    }

    void parseFrames(const std::string& json) {
        frames.clear();
        const size_t arrayBegin = json.find("\"frames\":[");
        if (arrayBegin == std::string::npos) return;
        size_t cursor = arrayBegin + 10;
        while (cursor < json.size()) {
            const size_t objectBegin = json.find('{', cursor);
            if (objectBegin == std::string::npos) break;
            const size_t objectEnd = json.find('}', objectBegin);
            if (objectEnd == std::string::npos) break;
            const std::string object = json.substr(objectBegin, objectEnd - objectBegin + 1);
            RemoteFrame frame;
            jsonIntField(object, "level", frame.level);
            jsonStringField(object, "source", frame.source);
            jsonStringField(object, "name", frame.name);
            jsonIntField(object, "line", frame.line);
            frames.push_back(std::move(frame));
            cursor = objectEnd + 1;
        }
    }

    void flush() {
        while (socket != invalidDebugSocket && outgoingOffset < outgoing.size()) {
#if defined(MSG_NOSIGNAL)
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const auto sent = ::send(socket, outgoing.data() + outgoingOffset,
                                     static_cast<int>(std::min<std::size_t>(outgoing.size() - outgoingOffset, 4096)),
                                     flags);
            if (sent <= 0) {
                if (sent < 0 && debugWouldBlock(debugSocketError())) return;
                error = "Debugger send failed (" + std::to_string(debugSocketError()) + ")";
                close();
                return;
            }
            outgoingOffset += static_cast<std::size_t>(sent);
        }
        if (outgoingOffset == outgoing.size()) {
            outgoing.clear();
            outgoingOffset = 0;
        }
    }

    static constexpr std::size_t maxLineLength = 64 * 1024;
    DebugSocketRuntime runtime;
    DebugSocket socket = invalidDebugSocket;
    State state = State::Disconnected;
    bool authenticated = false;
    int timeoutMilliseconds = 1000;
    std::chrono::steady_clock::time_point connectDeadline{};
    int pendingDeleteId = 0;
    std::string tokenValue;
    std::string error;
    std::string receiveBuffer;
    std::string outgoing;
    std::size_t outgoingOffset = 0;
    std::uint64_t nextRequestId = 1;
    std::string lastResponseRequestId;
    std::string remoteState = "idle";
    std::string stopReason = "none";
    bool stopOnError = true;
    std::string errorFilter = "runtime,callback";
    std::string source;
    int line = 0;
    std::string watchValue;
    std::string watchError;
    std::vector<RemoteFrame> frames;
    std::vector<RemoteBreakpoint> breakpoints;
    std::vector<std::string> pendingBreakpointSpecifications;
};

ScriptDebuggerWindow::ScriptDebuggerWindow() = default;

ScriptDebuggerWindow::~ScriptDebuggerWindow() = default;

void ScriptDebuggerWindow::open(Project* project) {
    targetProject = project;
    openState = true;
}

void ScriptDebuggerWindow::show() {
    if (!openState) return;

    if (remoteClient) {
        remoteClient->poll();
        static double lastStatusRequest = 0.0;
        if (remoteClient->isReady() && ImGui::GetTime() - lastStatusRequest >= 0.25) {
            remoteClient->requestStatus();
            lastStatusRequest = ImGui::GetTime();
        }
    }

    ImGui::SetNextWindowSize(ImVec2(620.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &openState)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##scriptDebuggerTabs")) {
        if (ImGui::BeginTabItem("Play (in-process)")) {
            drawLocalDebugger();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Remote TCP")) {
            drawRemoteDebugger();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void ScriptDebuggerWindow::drawLocalDebugger() {
    LuaDebugger* debugger = targetProject ? targetProject->getPlayScriptDebugger() : nullptr;
    if (!debugger) {
        ImGui::TextWrapped("Attach to the live Lua VM used by the current Play session. "
                          "This debugger remains in-process and uses the existing Play lifecycle.");
        ImGui::BeginDisabled(!targetProject || !targetProject->isPlaySessionActive());
        if (ImGui::Button("Attach to Play")) {
            targetProject->attachPlayScriptDebugger();
            debugger = targetProject->getPlayScriptDebugger();
        }
        ImGui::EndDisabled();
        if (!targetProject || !targetProject->isPlaySessionActive()) {
            ImGui::SameLine();
            ImGui::TextDisabled("Start Play first");
        }
        return;
    }

    ImGui::Text("State: %s  |  stop: %s", LuaDebugger::stateName(debugger->getState()),
        LuaDebugger::stopReasonName(debugger->getStopReason()));
    if (debugger->getState() == LuaDebugger::State::Paused) {
        if (ImGui::Button("Continue")) debugger->resume();
        ImGui::SameLine();
        if (ImGui::Button("Step Into")) debugger->stepInto();
        ImGui::SameLine();
        if (ImGui::Button("Step Over")) debugger->stepOver();
        ImGui::SameLine();
        if (ImGui::Button("Step Out")) debugger->stepOut();
    } else if (debugger->getState() == LuaDebugger::State::Running) {
        if (ImGui::Button("Pause")) debugger->pause();
    }
    ImGui::SameLine();
    if (ImGui::Button("Detach")) {
        targetProject->detachPlayScriptDebugger();
        return;
    }

    ImGui::SeparatorText("Breakpoint");
    ImGui::InputText("Source", source, sizeof(source));
    ImGui::InputInt("Line", &line);
    if (line < 1) line = 1;
    if (ImGui::Button("Add breakpoint") && source[0] != '\0') debugger->addBreakpoint(source, line);
    ImGui::SameLine();
    if (ImGui::Button("Clear all")) debugger->clearBreakpoints();
    int removeBreakpointId = 0;
    for (const LuaDebugger::Breakpoint& breakpoint : debugger->getBreakpoints()) {
        ImGui::PushID(breakpoint.id);
        ImGui::Text("#%d  %s:%d", breakpoint.id, breakpoint.source.c_str(), breakpoint.line);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) removeBreakpointId = breakpoint.id;
        ImGui::PopID();
    }
    if (removeBreakpointId != 0) debugger->removeBreakpoint(removeBreakpointId);

    ImGui::SeparatorText("Watch");
    ImGui::InputText("Expression", watchExpression, sizeof(watchExpression));
    if (ImGui::Button("Evaluate")) {
        std::string value;
        std::string error;
        if (debugger->evaluateWatch(watchExpression, value, error)) {
            std::snprintf(watchResult, sizeof(watchResult), "%s", value.c_str());
            watchError[0] = '\0';
        } else {
            watchResult[0] = '\0';
            std::snprintf(watchError, sizeof(watchError), "%s", error.c_str());
        }
    }
    if (watchResult[0] != '\0') ImGui::Text("Result: %s", watchResult);
    if (watchError[0] != '\0') ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
        "Watch error: %s", watchError);

    ImGui::SeparatorText("Call stack snapshot");
    for (const LuaDebugger::Frame& frame : debugger->getCallStack()) {
        ImGui::Text("%d  %s  %s:%d", frame.level, frame.name.c_str(), frame.source.c_str(), frame.currentLine);
    }
    if (debugger->getCallStack().empty()) ImGui::TextDisabled("No captured Lua frames");
}

void ScriptDebuggerWindow::drawRemoteDebugger() {
    if (!remoteClient || !remoteClient->isReady()) {
        drawRemoteConnection();
        return;
    }

    ImGui::Text("Connected to 127.0.0.1:%d  |  State: %s  |  stop: %s",
        remotePort, remoteClient->getStateName().c_str(), remoteClient->getStopReason().c_str());
    if (!remoteClient->getSource().empty()) {
        ImGui::Text("Location: %s:%d", remoteClient->getSource().c_str(), remoteClient->getLine());
    }
    ImGui::Text("Lua error filter: %s", remoteClient->getErrorFilter().c_str());
    bool runtimeErrors = remoteClient->hasErrorCategory("runtime");
    bool callbackErrors = remoteClient->hasErrorCategory("callback");
    bool syntaxErrors = remoteClient->hasErrorCategory("syntax");
    bool errorFilterChanged = false;
    errorFilterChanged |= ImGui::Checkbox("Runtime", &runtimeErrors);
    ImGui::SameLine();
    errorFilterChanged |= ImGui::Checkbox("Callbacks", &callbackErrors);
    ImGui::SameLine();
    errorFilterChanged |= ImGui::Checkbox("Syntax", &syntaxErrors);
    if (errorFilterChanged) remoteClient->setErrorFilter(runtimeErrors, callbackErrors, syntaxErrors);

    const bool paused = remoteClient->getStateName() == "paused";
    const bool running = remoteClient->getStateName() == "running";
    if (paused) {
        if (ImGui::Button("Continue")) remoteClient->command("continue");
        ImGui::SameLine();
        if (ImGui::Button("Step Into")) remoteClient->command("step");
        ImGui::SameLine();
        if (ImGui::Button("Step Over")) remoteClient->command("next");
        ImGui::SameLine();
        if (ImGui::Button("Step Out")) remoteClient->command("out");
    } else if (running && ImGui::Button("Pause")) {
        remoteClient->command("pause");
    }
    ImGui::SameLine();
    if (ImGui::Button("Disconnect")) {
        remoteClient->close();
        return;
    }

    ImGui::SeparatorText("Breakpoint");
    ImGui::InputText("Source", source, sizeof(source));
    ImGui::InputInt("Line", &line);
    if (line < 1) line = 1;
    ImGui::InputText("Condition (optional)", breakpointCondition, sizeof(breakpointCondition));
    if (ImGui::Button("Add breakpoint") && source[0] != '\0') {
        std::ostringstream specification;
        specification << source << ':' << line;
        if (breakpointCondition[0] != '\0') specification << " if " << breakpointCondition;
        remoteClient->rememberBreakpoint(specification.str());
        remoteClient->command("break " + specification.str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear all")) remoteClient->command("clear");
    int removeBreakpointId = 0;
    for (const RemoteBreakpoint& breakpoint : remoteClient->getBreakpoints()) {
        ImGui::PushID(breakpoint.id);
        ImGui::Text("#%d  %s", breakpoint.id, breakpoint.specification.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) removeBreakpointId = breakpoint.id;
        ImGui::PopID();
    }
    if (removeBreakpointId != 0) {
        remoteClient->deleteBreakpoint(removeBreakpointId);
    }

    ImGui::SeparatorText("Watch");
    ImGui::InputText("Expression", watchExpression, sizeof(watchExpression));
    if (ImGui::Button("Evaluate")) remoteClient->command("watch " + std::string(watchExpression));
    if (!remoteClient->getWatchValue().empty()) ImGui::Text("Result: %s", remoteClient->getWatchValue().c_str());
    if (!remoteClient->getWatchError().empty()) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
        "Watch error: %s", remoteClient->getWatchError().c_str());

    ImGui::SeparatorText("Call stack snapshot");
    if (ImGui::Button("Refresh stack")) remoteClient->command("stack");
    for (const RemoteFrame& frame : remoteClient->getFrames()) {
        ImGui::Text("%d  %s  %s:%d", frame.level, frame.name.c_str(), frame.source.c_str(), frame.line);
    }
    if (remoteClient->getFrames().empty()) ImGui::TextDisabled("No captured Lua frames");
}

void ScriptDebuggerWindow::drawRemoteConnection() {
    ImGui::TextWrapped("Connect to the quantum-engine Lua debugger. The client only accepts loopback endpoints.");
    ImGui::InputText("Host (loopback)", remoteHost, sizeof(remoteHost));
    ImGui::InputInt("Port", &remotePort);
    if (remotePort < 1) remotePort = 1;
    if (remotePort > 65535) remotePort = 65535;
    ImGui::InputText("Token", remoteToken, sizeof(remoteToken), ImGuiInputTextFlags_Password);

    if (remoteClient && remoteClient->isConnecting()) {
        ImGui::TextDisabled("Connecting...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) remoteClient->close();
    } else if (!remoteClient || remoteClient->getState() == RemoteLuaDebuggerClient::State::Disconnected) {
        if (ImGui::Button("Connect")) {
            remoteClient = std::make_unique<RemoteLuaDebuggerClient>();
            remoteClient->start(remoteHost, remotePort, remoteToken, 1000);
        }
    }
    if (remoteClient && !remoteClient->getError().empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", remoteClient->getError().c_str());
    }
}

}


