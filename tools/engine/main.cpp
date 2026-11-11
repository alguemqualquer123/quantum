// (c) SR VINIX
// (c) SR VINIX
// SPDX-License-Identifier: MIT
//
// quantum-engine: standalone game runtime.
//
// Boots the quantum engine without the editor, following the same sequence an
// exported game uses:
//   1. main() -> <platform>::init (engine/platform/win/main.cpp:6)
//   2. QuantumWin::init -> Engine::systemInit(argc, argv, new SystemWin)
//      (engine/platform/win/QuantumWin.cpp:304-376)
//   3. Engine::systemInit fires the QUANTUM_INIT bootstrap through
//      getOnInit().call() (engine/core/Engine.cpp:903-933, engine/core/Engine.h:135-144)
//   4. window + GL/Vulkan/D3D context, then systemViewLoaded/systemViewChanged
//      and the draw loop until WM_CLOSE.
// The editor-configured build of the quantum library is compiled with
// NO_LUA_INIT (root CMakeLists.txt:321), so the automatic `main.lua` load in
// LuaBinding::init (engine/core/script/LuaBinding.cpp:429) is skipped inside
// the dll; this runner replays it with the same lookup through the public
// LuaBinding API, then falls back to a built-in demo scene so something always
// launches. Headless project probes without a Lua entry use an empty scene
// instead of constructing the visual demo.
//
// CLI:
//   quantum-engine [--project <path-to-project.yaml-or-export-dir>]
//                 [--scene <name>]
//                 [--window WxH]
//                 [--fullscreen]
//                 [--log-level N]
//
// --project resolves the resource roots the same way an exported game does:
// assets/, lua/ and shaders/ next to the working directory are read relative
// to it (engine/core/io/FileData.cpp:143-167 maps the asset://, lua:// and
// shader:// schemes onto SystemWin::getAssetPath, which is "assets" when no
// QUANTUM_ASSET_PATH was baked in, engine/platform/win/SystemWin.cpp:113-139),
// so the runner simply makes the project directory the current one before the
// engine starts. A resources.pak in the project directory is only read when
// the library was built without QUANTUM_EDITOR (engine/core/io/ResourcePack.cpp:214-225);
// in the editor-configuration build it warns that packed resources are not
// available.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "Quantum.h"
#include "util/FrameCapture.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/NetworkSystem.h"
#include "component/FogComponent.h"
#include "component/LightComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/ScriptComponent.h"
#include "script/LuaBinding.h"
#include "script/LuaDebugger.h"
#include "script/graph/GraphCompiler.h"
#include "script/graph/ExecutableGraph.h"
#include "script/graph/GraphRuntime.h"
#include "script/graph/GraphSchema.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "lua.hpp"
#include "sokol_gfx.h"
#include "stb_image.h"
#include "stb_image_write.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#if defined(_DEBUG)
#include <crtdbg.h>
#endif
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "Ws2_32.lib")
#include "QuantumWin.h"
#include "SystemWin.h"
#elif defined(__APPLE__)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "QuantumMac.h"
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "QuantumLinux.h"
#endif

using namespace quantum;

namespace {

// ---- CLI state -------------------------------------------------------------

struct Options {
    std::filesystem::path projectPath; // project.yaml file or project/export dir
    std::filesystem::path projectDir;  // resolved directory of the above, set after --project parsing
    std::string luaDir;                // project.yaml's luaDir, the editor's script root ("." for most templates)
    std::string sceneName;             // lua entry override (or scene name), optional
    std::string title;                 // window title (project name when known)
    std::filesystem::path statsOut;    // where the --frames report is written
    std::string label;                 // report name (project name when absent)
    int windowWidth = 0;               // 0 = keep the platform default
    int windowHeight = 0;
    bool fullscreen = false;
    bool headless = false;
    bool scriptProbe = false;
    bool scriptSmoke = false;
    bool graphSmoke = false;
    bool graphProjectSmoke = false;
    bool scriptReloadSmoke = false;
    bool scriptHotReload = false;
    std::vector<std::string> renderFeatures;
    std::string renderLut;
    int logLevel = 1;                  // 0 quiet, 1 normal, 2 verbose
    // Measurement run: 0 keeps the ordinary "run until the window closes" behaviour.
    int frames = 0;
    int warmupFrames = -1;             // -1 = a quarter of --frames, so a load settles
    std::filesystem::path screenshot;  // PNG written at --capture-frame; empty = no capture
    std::filesystem::path compareScreenshot; // baseline PNG for an automated pixel diff
    std::filesystem::path compareDiff; // optional false-colour absolute-difference PNG
    std::filesystem::path compareReport; // optional machine-readable comparison report
    float compareThreshold = 0.01f;          // mean RGB error in [0,1]
    int captureFrame = 60;             // 1-based frame the screenshot is taken on
    std::string audioProbeWav;
    std::string audioProbeOgg;
    enum class NetworkRole { None, Server, Client } networkRole = NetworkRole::None;
    std::string networkAddress = "127.0.0.1";
    std::string networkToken;
    bool networkRelay = false;
    std::string relayAddress = "127.0.0.1";
    std::string relayRoom;
    std::string relayToken;
    int relayPort = 0;
    int networkPort = 39179;
    bool debugPortEnabled = false;
    std::uint16_t debugPort = 0;
    std::string debugToken;
};

Options g_options;
std::filesystem::path g_originalCwd;
bool g_audioProbeFailed = false;
bool g_scriptSmokeFailed = false;
bool g_graphSmokeFailed = false;
bool g_graphProjectSmokeFailed = false;
bool g_scriptReloadSmokeFailed = false;
bool g_helpRequested = false;
std::unique_ptr<class RemoteDebugServer> g_remoteDebugServer;
std::unique_ptr<LuaDebugger> g_remoteDebugger;

struct ScriptReloadProbeState {
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    std::filesystem::path file;
};

ScriptReloadProbeState g_scriptReloadProbe;

// Compiled-in fallback so a build-tree exe still finds engine/ sources and the
// demo logo before the engine/ dir is copied next to it.
#ifndef QUANTUM_ENGINE_DIR_FALLBACK
#define QUANTUM_ENGINE_DIR_FALLBACK ""
#endif

// ---- logging ---------------------------------------------------------------

void say(int level, const char* fmt, ...) {
    if (level > g_options.logLevel) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    va_end(args);
    std::fputc('\n', stdout);
    // Runs that abort or are killed mid-frame lose the stdout buffer, and the last line printed
    // is the only witness of where they stopped, so every log line goes out the door at once.
    std::fflush(stdout);
}

// ---- remote Lua debugger --------------------------------------------------

#if defined(_WIN32)
using RemoteDebugSocket = SOCKET;
constexpr RemoteDebugSocket invalidRemoteDebugSocket = INVALID_SOCKET;
#else
using RemoteDebugSocket = int;
constexpr RemoteDebugSocket invalidRemoteDebugSocket = -1;
#endif

void closeRemoteDebugSocket(RemoteDebugSocket socket) {
    if (socket == invalidRemoteDebugSocket) return;
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

int remoteDebugSocketError() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool remoteDebugWouldBlock(int error) {
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

bool setRemoteDebugNonBlocking(RemoteDebugSocket socket) {
#if defined(_WIN32)
    u_long mode = 1;
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

class RemoteDebugSocketRuntime {
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

    ~RemoteDebugSocketRuntime() {
#if defined(_WIN32)
        if (initialized) ::WSACleanup();
#endif
    }

private:
#if defined(_WIN32)
    bool initialized = false;
#endif
};

std::string remoteDebugTrim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::string remoteDebugJsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += character; break;
        }
    }
    return escaped;
}

bool remoteDebugTokenMatches(const std::string& expected, const std::string& actual) {
    if (expected.size() != actual.size()) return false;
    unsigned char difference = 0;
    for (size_t index = 0; index < expected.size(); ++index) {
        difference |= static_cast<unsigned char>(expected[index] ^ actual[index]);
    }
    return difference == 0;
}

std::string remoteDebugLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool remoteDebugParseErrorCategories(const std::string& value,
                                     LuaDebugger::ErrorCategoryMask& result) {
    result = 0;
    const std::string input = remoteDebugTrim(value);
    if (input.empty()) return false;
    size_t begin = 0;
    while (begin <= input.size()) {
        const size_t separator = input.find(',', begin);
        const std::string token = remoteDebugLower(remoteDebugTrim(
            input.substr(begin, separator == std::string::npos ? std::string::npos : separator - begin)));
        if (token == "runtime") {
            result |= LuaDebugger::kRuntimeErrorFilter;
        } else if (token == "callback" || token == "callbacks") {
            result |= LuaDebugger::kCallbackErrorFilter;
        } else if (token == "syntax") {
            result |= LuaDebugger::kSyntaxErrorFilter;
        } else if (token == "all") {
            result |= LuaDebugger::kAllErrorFilter;
        } else {
            return false;
        }
        if (separator == std::string::npos) break;
        begin = separator + 1;
    }
    return result != 0;
}

std::string remoteDebugErrorFilterText(LuaDebugger::ErrorCategoryMask filter) {
    if (filter == 0) return "none";
    std::string result;
    const auto append = [&result](const char* name) {
        if (!result.empty()) result += ',';
        result += name;
    };
    if ((filter & LuaDebugger::kRuntimeErrorFilter) != 0) append("runtime");
    if ((filter & LuaDebugger::kCallbackErrorFilter) != 0) append("callback");
    if ((filter & LuaDebugger::kSyntaxErrorFilter) != 0) append("syntax");
    return result;
}

class RemoteDebugServer {
public:
    RemoteDebugServer() = default;
    ~RemoteDebugServer() {
        closeClient();
        closeListener();
    }

    RemoteDebugServer(const RemoteDebugServer&) = delete;
    RemoteDebugServer& operator=(const RemoteDebugServer&) = delete;

    bool open(std::uint16_t requestedPort, std::string token, std::string& error) {
        token_ = std::move(token);
        if (!runtime_.start(error)) return false;

        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == invalidRemoteDebugSocket) {
            error = "remote debugger socket creation failed (" + std::to_string(remoteDebugSocketError()) + ")";
            return false;
        }
        int reuseAddress = 1;
#if defined(_WIN32)
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress));
#else
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));
#if defined(SO_NOSIGPIPE)
        ::setsockopt(listener_, SOL_SOCKET, SO_NOSIGPIPE, &reuseAddress, sizeof(reuseAddress));
#endif
#endif

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(requestedPort);
        if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1
            || ::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
            || ::listen(listener_, 1) != 0
            || !setRemoteDebugNonBlocking(listener_)) {
            error = "remote debugger loopback listener setup failed ("
                + std::to_string(remoteDebugSocketError()) + ")";
            closeListener();
            return false;
        }

#if defined(_WIN32)
        int addressLength = static_cast<int>(sizeof(address));
#else
        socklen_t addressLength = static_cast<socklen_t>(sizeof(address));
#endif
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
            error = "remote debugger listener port query failed ("
                + std::to_string(remoteDebugSocketError()) + ")";
            closeListener();
            return false;
        }
        port_ = ntohs(address.sin_port);
        return true;
    }

    std::uint16_t port() const { return port_; }

    void service(LuaDebugger* debugger) {
        acceptClient();
        if (client_ == invalidRemoteDebugSocket) return;
        receiveLines();
        if (client_ != invalidRemoteDebugSocket && debugger) processLines(*debugger);
        flush();
    }

private:
    static constexpr size_t maxLineLength = 64 * 1024;

    void acceptClient() {
        if (listener_ == invalidRemoteDebugSocket || client_ != invalidRemoteDebugSocket) return;
#if defined(_WIN32)
        int addressLength = static_cast<int>(sizeof(sockaddr_in));
#else
        socklen_t addressLength = static_cast<socklen_t>(sizeof(sockaddr_in));
#endif
        sockaddr_in address{};
        const RemoteDebugSocket accepted = ::accept(
            listener_, reinterpret_cast<sockaddr*>(&address), &addressLength);
        if (accepted == invalidRemoteDebugSocket) {
            if (!remoteDebugWouldBlock(remoteDebugSocketError())) closeListener();
            return;
        }
        if (!setRemoteDebugNonBlocking(accepted)) {
            closeRemoteDebugSocket(accepted);
            return;
        }
        client_ = accepted;
        authenticated_ = token_.empty();
        receiveBuffer_.clear();
        outgoing_.clear();
        outgoingOffset_ = 0;
        closeAfterFlush_ = false;
        queue("{\"event\":\"hello\",\"protocol\":2,\"authRequired\":"
              + std::string(authenticated_ ? "false" : "true") + "}");
    }

    void receiveLines() {
        char buffer[4096];
        for (;;) {
            const int received = ::recv(client_, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received == 0) {
                closeClient();
                return;
            }
            if (received < 0) {
                if (remoteDebugWouldBlock(remoteDebugSocketError())) break;
                closeClient();
                return;
            }
            receiveBuffer_.append(buffer, static_cast<size_t>(received));
            if (receiveBuffer_.size() > maxLineLength) {
                queue("{\"event\":\"error\",\"message\":\"debug command exceeds 65536 bytes\"}");
                closeAfterFlush_ = true;
                return;
            }
            if (received < static_cast<int>(sizeof(buffer))) break;
        }
    }

    void processLines(LuaDebugger& debugger) {
        for (;;) {
            const size_t newline = receiveBuffer_.find('\n');
            if (newline == std::string::npos) return;
            std::string line = receiveBuffer_.substr(0, newline);
            receiveBuffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            processLine(remoteDebugTrim(line), debugger);
            if (client_ == invalidRemoteDebugSocket) return;
        }
    }

    void processLine(const std::string& line, LuaDebugger& debugger) {
        if (line.empty()) return;
        if (!authenticated_) {
            if (line.rfind("auth ", 0) == 0
                && remoteDebugTokenMatches(token_, remoteDebugTrim(line.substr(5)))) {
                authenticated_ = true;
                queue("{\"event\":\"ready\",\"state\":\""
                      + std::string(LuaDebugger::stateName(debugger.getState())) + "\"}");
            } else {
                queue("{\"event\":\"error\",\"message\":\"authentication failed\"}");
                closeAfterFlush_ = true;
            }
            return;
        }

        std::string requestId;
        std::string commandLine = line;
        if (commandLine.front() == '@') {
            const size_t separator = commandLine.find(' ');
            if (separator == std::string::npos || separator <= 1 || separator > 65) {
                queue("{\"event\":\"error\",\"message\":\"request id must be @<id> followed by a command\"}");
                return;
            }
            requestId = commandLine.substr(1, separator - 1);
            if (!std::all_of(requestId.begin(), requestId.end(), [](unsigned char character) {
                    return (character >= 'a' && character <= 'z')
                        || (character >= 'A' && character <= 'Z')
                        || (character >= '0' && character <= '9')
                        || character == '-' || character == '_' || character == '.';
                })) {
                queue("{\"event\":\"error\",\"message\":\"request id contains invalid characters\"}");
                return;
            }
            commandLine = remoteDebugTrim(commandLine.substr(separator + 1));
        }

        const size_t separator = commandLine.find(' ');
        const std::string command = separator == std::string::npos ? commandLine : commandLine.substr(0, separator);
        const std::string arguments = separator == std::string::npos
            ? std::string() : remoteDebugTrim(commandLine.substr(separator + 1));

        if (command == "status") {
            sendStatus(debugger, requestId);
        } else if (command == "continue" || command == "c") {
            queueCommandResult("continue", debugger.resume(), debugger, requestId);
        } else if (command == "pause") {
            debugger.pause();
            queueCommandResult("pause", true, debugger, requestId);
        } else if (command == "step" || command == "s") {
            queueCommandResult("step", debugger.stepInto(), debugger, requestId);
        } else if (command == "next" || command == "n") {
            queueCommandResult("next", debugger.stepOver(), debugger, requestId);
        } else if (command == "out" || command == "o") {
            queueCommandResult("out", debugger.stepOut(), debugger, requestId);
        } else if (command == "watch" || command == "w") {
            std::string value;
            std::string error;
            const bool ok = debugger.evaluateWatch(arguments, value, error);
            queue("{\"event\":\"watch\",\"ok\":" + std::string(ok ? "true" : "false")
                  + (ok ? ",\"value\":\"" + remoteDebugJsonEscape(value) + "\""
                        : ",\"error\":\"" + remoteDebugJsonEscape(error) + "\"") + "}", requestId);
        } else if (command == "stack") {
            std::ostringstream response;
            response << "{\"event\":\"stack\",\"frames\":[";
            const auto& frames = debugger.getCallStack();
            for (size_t index = 0; index < frames.size(); ++index) {
                if (index > 0) response << ',';
                const auto& frame = frames[index];
                response << "{\"level\":" << frame.level
                         << ",\"source\":\"" << remoteDebugJsonEscape(frame.source)
                         << "\",\"name\":\"" << remoteDebugJsonEscape(frame.name)
                         << "\",\"line\":" << frame.currentLine << '}';
            }
            response << "]}";
            queue(response.str(), requestId);
        } else if (command == "break") {
            addBreakpoint(arguments, debugger, requestId);
        } else if (command == "clear") {
            debugger.clearBreakpoints();
            queue("{\"event\":\"breakpoints\",\"cleared\":true}", requestId);
        } else if (command == "delete") {
            char* end = nullptr;
            const long id = std::strtol(arguments.c_str(), &end, 10);
            const bool removed = end != arguments.c_str() && end && *end == '\0'
                && debugger.removeBreakpoint(static_cast<int>(id));
            queue("{\"event\":\"breakpoint\",\"removed\":"
                  + std::string(removed ? "true" : "false") + "}", requestId);
        } else if (command == "errors") {
            const std::string filterArgument = remoteDebugLower(remoteDebugTrim(arguments));
            if (filterArgument == "on" || filterArgument == "true" || filterArgument == "1") {
                debugger.setStopOnError(true);
            } else if (filterArgument == "off" || filterArgument == "false" || filterArgument == "0"
                       || filterArgument == "none") {
                debugger.setErrorFilter(0);
            } else if (filterArgument == "all") {
                debugger.setErrorFilter(LuaDebugger::kAllErrorFilter);
            } else {
                LuaDebugger::ErrorCategoryMask parsed = 0;
                bool relative = false;
                bool add = true;
                std::string categories = filterArgument;
                if (!categories.empty() && (categories.front() == '+' || categories.front() == '-')) {
                    relative = true;
                    add = categories.front() == '+';
                    categories.erase(categories.begin());
                }
                if (!remoteDebugParseErrorCategories(categories, parsed)) {
                    queue("{\"event\":\"error\",\"message\":\"errors expects on, off, all, none, or a comma-separated category list (runtime,callback,syntax)\"}", requestId);
                    return;
                }
                if (relative) {
                    const auto current = debugger.getErrorFilter();
                    debugger.setErrorFilter(add ? static_cast<LuaDebugger::ErrorCategoryMask>(current | parsed)
                                                : static_cast<LuaDebugger::ErrorCategoryMask>(current & ~parsed));
                } else {
                    debugger.setErrorFilter(parsed);
                }
            }
            std::ostringstream response;
            response << "{\"event\":\"errorFilter\",\"stopOnError\":"
                     << (debugger.getStopOnError() ? "true" : "false")
                     << ",\"errorMask\":" << static_cast<unsigned int>(debugger.getErrorFilter())
                     << ",\"errorFilter\":\"" << remoteDebugErrorFilterText(debugger.getErrorFilter())
                     << "\"}";
            queue(response.str(), requestId);
        } else if (command == "ping") {
            queue("{\"event\":\"pong\"}", requestId);
        } else if (command == "quit" || command == "q") {
            queue("{\"event\":\"bye\"}", requestId);
            closeAfterFlush_ = true;
        } else {
            queue("{\"event\":\"error\",\"message\":\"unknown debugger command\"}", requestId);
        }
    }

    void addBreakpoint(const std::string& specification, LuaDebugger& debugger,
                       const std::string& requestId) {
        std::string location = specification;
        std::string condition;
        const size_t conditionOffset = location.find(" if ");
        if (conditionOffset != std::string::npos) {
            condition = remoteDebugTrim(location.substr(conditionOffset + 4));
            location = remoteDebugTrim(location.substr(0, conditionOffset));
        }
        const size_t separator = location.rfind(':');
        if (separator == std::string::npos) {
            queue("{\"event\":\"error\",\"message\":\"break expects source:line\"}", requestId);
            return;
        }
        const std::string lineText = location.substr(separator + 1);
        char* end = nullptr;
        const long line = std::strtol(lineText.c_str(), &end, 10);
        if (end == lineText.c_str() || !end || *end != '\0' || line <= 0) {
            queue("{\"event\":\"error\",\"message\":\"break line is invalid\"}", requestId);
            return;
        }
        const int id = debugger.addBreakpoint(location.substr(0, separator), static_cast<int>(line), condition);
        if (id == 0) {
            queue("{\"event\":\"error\",\"message\":\"breakpoint was rejected\"}", requestId);
            return;
        }
        queue("{\"event\":\"breakpoint\",\"id\":" + std::to_string(id) + "}", requestId);
    }

    void queueCommandResult(const std::string& command, bool ok, const LuaDebugger& debugger,
                            const std::string& requestId) {
        queue("{\"event\":\"command\",\"command\":\"" + command
              + "\",\"ok\":" + std::string(ok ? "true" : "false")
              + ",\"state\":\"" + LuaDebugger::stateName(debugger.getState()) + "\"}", requestId);
    }

    void sendStatus(const LuaDebugger& debugger, const std::string& requestId) {
        std::ostringstream response;
        response << "{\"event\":\"status\",\"state\":\""
                 << LuaDebugger::stateName(debugger.getState())
                 << "\",\"stopReason\":\""
                 << LuaDebugger::stopReasonName(debugger.getStopReason())
                 << "\",\"stopOnError\":" << (debugger.getStopOnError() ? "true" : "false")
                 << ",\"errorMask\":" << static_cast<unsigned int>(debugger.getErrorFilter())
                 << ",\"errorFilter\":\"" << remoteDebugErrorFilterText(debugger.getErrorFilter()) << "\"";
        const auto& frames = debugger.getCallStack();
        if (!frames.empty()) {
            response << ",\"source\":\"" << remoteDebugJsonEscape(frames.front().source)
                     << "\",\"line\":" << frames.front().currentLine;
        }
        const auto& diagnostic = debugger.getDiagnostic();
        if (!diagnostic.message.empty()) {
            response << ",\"diagnostic\":\"" << remoteDebugJsonEscape(diagnostic.message) << "\"";
        }
        response << '}';
        queue(response.str(), requestId);
    }

    void queue(const std::string& line, const std::string& requestId = {}) {
        if (client_ == invalidRemoteDebugSocket) return;
        if (outgoing_.empty()) outgoingOffset_ = 0;
        if (!requestId.empty() && line.size() >= 2 && line.front() == '{') {
            outgoing_ += "{\"requestId\":\"" + remoteDebugJsonEscape(requestId) + "\",";
            outgoing_ += line.substr(1);
        } else {
            outgoing_ += line;
        }
        outgoing_ += '\n';
    }

    void flush() {
        while (client_ != invalidRemoteDebugSocket && outgoingOffset_ < outgoing_.size()) {
#if defined(MSG_NOSIGNAL)
            constexpr int sendFlags = MSG_NOSIGNAL;
#else
            constexpr int sendFlags = 0;
#endif
            const auto sent = ::send(client_, outgoing_.data() + outgoingOffset_,
                                     static_cast<int>(std::min<size_t>(outgoing_.size() - outgoingOffset_, 4096)),
                                     sendFlags);
            if (sent <= 0) {
                if (sent < 0 && remoteDebugWouldBlock(remoteDebugSocketError())) return;
                closeClient();
                return;
            }
            outgoingOffset_ += static_cast<size_t>(sent);
        }
        if (outgoingOffset_ == outgoing_.size()) {
            outgoing_.clear();
            outgoingOffset_ = 0;
            if (closeAfterFlush_) closeClient();
        }
    }

    void closeClient() {
        closeRemoteDebugSocket(client_);
        client_ = invalidRemoteDebugSocket;
        authenticated_ = false;
        receiveBuffer_.clear();
        outgoing_.clear();
        outgoingOffset_ = 0;
        closeAfterFlush_ = false;
    }

    void closeListener() {
        closeRemoteDebugSocket(listener_);
        listener_ = invalidRemoteDebugSocket;
    }

    RemoteDebugSocketRuntime runtime_;
    RemoteDebugSocket listener_ = invalidRemoteDebugSocket;
    RemoteDebugSocket client_ = invalidRemoteDebugSocket;
    std::uint16_t port_ = 0;
    std::string token_;
    std::string receiveBuffer_;
    std::string outgoing_;
    size_t outgoingOffset_ = 0;
    bool authenticated_ = false;
    bool closeAfterFlush_ = false;
};

void serviceRemoteDebugger() {
    if (g_remoteDebugServer && g_remoteDebugger) g_remoteDebugServer->service(g_remoteDebugger.get());
}

// ---- engine dir resolution (mirrors editor/util/FileUtils.h:96-108) --------

std::filesystem::path getExecutableDir() {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    wchar_t buffer[4096];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, 4096);
    if (length == 0 || length >= 4096) return {};
    std::error_code ec;
    return fs::weakly_canonical(fs::path(buffer), ec).parent_path();
#elif defined(__linux__)
    std::error_code ec;
    const fs::path executablePath = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return fs::weakly_canonical(executablePath, ec).parent_path();
#else
    return fs::current_path();
#endif
}

std::filesystem::path getEngineDir() {
    namespace fs = std::filesystem;

    const fs::path exeDir = getExecutableDir();
#if defined(__APPLE__)
    std::error_code ec;
    const fs::path bundled = exeDir.parent_path() / "Resources" / "engine";
    if (fs::exists(bundled, ec)) {
        return bundled;
    }
#endif
    std::error_code ec;
    const fs::path nextToExe = exeDir.empty() ? fs::path() : exeDir / "engine";
    if (!nextToExe.empty() && fs::exists(nextToExe, ec)) {
        return nextToExe;
    }
    const std::string fallback = QUANTUM_ENGINE_DIR_FALLBACK;
    if (!fallback.empty() && fs::exists(fallback, ec)) {
        return fallback;
    }
    return nextToExe;
}

// ---- project resolution ----------------------------------------------------

// project.yaml holds "name: <title>" near the top (templates/fps-fps/project.yaml).
// A plain scan is enough here; yaml-cpp is an editor-target dependency and the
// runner keeps its link surface minimal.
std::string readProjectField(const std::filesystem::path& projectDir, const std::string& key) {
    const std::filesystem::path yaml = projectDir / "project.yaml";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(yaml, ec)) return {};

    std::FILE* file = std::fopen(yaml.string().c_str(), "rb");
    if (!file) return {};

    const std::string prefix = key + ":";
    char line[512];
    std::string value;
    for (int i = 0; i < 32 && std::fgets(line, sizeof(line), file); i++) {
        const std::string text(line);
        if (text.rfind(prefix, 0) == 0) {
            value = text.substr(prefix.size());
            const size_t begin = value.find_first_not_of(" \t\"'");
            const size_t end = value.find_last_not_of(" \t\"'\r\n");
            if (begin == std::string::npos) value.clear();
            else value = value.substr(begin, end - begin + 1);
            break;
        }
    }
    std::fclose(file);
    return value;
}

std::string readProjectName(const std::filesystem::path& projectDir) {
    return readProjectField(projectDir, "name");
}

// Accepts a path to project.yaml or to the project/export directory itself and
// makes it the process working directory, exactly where an exported game's
// assets/, lua/ and resources.pak live next to its executable. Returns the
// resolved project directory (empty on failure).
std::filesystem::path resolveAndEnterProject(const std::filesystem::path& given) {
    namespace fs = std::filesystem;

    fs::path path = given;
    if (path.is_relative()) path = g_originalCwd / path;
    path = path.lexically_normal();

    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        // project.yaml (or any file inside the project): the dir is the answer
        path = path.parent_path();
    }
    if (!fs::is_directory(path, ec)) {
        say(0, "error: project path not found: %s", given.string().c_str());
        return {};
    }

    fs::path pak = path / "resources.pak";
    if (fs::is_regular_file(pak, ec)) {
        // The editor-configuration quantum library compiles the pack loader out
        // (engine/core/io/ResourcePack.cpp:214-225), so packed resources stay
        // invisible to this runner. Loose assets/lua/shaders still work.
        say(1, "warning: resources.pak found but packed resources need a non-editor engine build; using loose files");
    }

    fs::current_path(path, ec);
    if (ec) {
        say(0, "error: could not enter project directory: %s", path.string().c_str());
        return {};
    }
    return path;
}

// ---- lua project boot ------------------------------------------------------

// Same lookup LuaBinding::init performs (engine/core/script/LuaBinding.cpp:429-458):
// "lua://main.lua" at the asset root, then "lua://lua/main.lua". It is dead
// code in the editor-configuration dll (NO_LUA_INIT), so the runner replays it
// through the public LuaBinding API. `baseName` defaults to "main".
bool runLuaEntry(const std::string& baseName) {
    lua_State* L = LuaBinding::getLuaState();
    if (!L) return false;

    const std::string luadir = std::string("lua") + System::instance().getDirSeparator();
    const std::string suffix = baseName + ".lua";
    std::vector<std::string> candidates;
    candidates.push_back("lua://" + suffix);
    candidates.push_back("lua://" + luadir + suffix);
    // The editor reads the script root from project.yaml's luaDir and most templates set it
    // to "." (scripts at the project root), so a template the editor opens has always been
    // unbootable here. An absolute path needs no VFS prefix: FileData::getSystemPath
    // (engine/core/io/FileData.cpp:145) passes it through untouched.
    if (!g_options.projectDir.empty()) {
        const std::filesystem::path root = g_options.luaDir.empty()
            ? g_options.projectDir : g_options.projectDir / g_options.luaDir;
        candidates.push_back((root / (baseName + ".lua")).lexically_normal().generic_string());
    }

    for (const std::string& luafile : candidates) {
        Data filedata;
        if (filedata.open(luafile.c_str()) == FileErrors::FILEDATA_OK && filedata.length() > 0) {
            say(2, "boot: running %s", luafile.c_str());
            if (luaL_loadbuffer(L, (const char*)filedata.getMemPtr(), filedata.length(), luafile.c_str()) == 0) {
                if (LuaBinding::pcallWithTraceback(L, 0, LUA_MULTRET) != 0) {
                    say(0, "error: Lua error in %s: %s", luafile.c_str(),
                        LuaBinding::getLuaStackErrorString(L, -1).c_str());
                    lua_pop(L, 1);
                }
                return true; // entry exists even when it errored: do not fall back
            }
            say(0, "error: Lua load error in %s: %s", luafile.c_str(),
                LuaBinding::getLuaStackErrorString(L, -1).c_str());
            lua_pop(L, 1);
            return true;
        }
    }
    return false;
}

// ---- built-in demo scene ---------------------------------------------------

// The demo mirrors the standalone sample project engine/project/main.cpp:9-16.
// The demo scene and its UI root are function-local statics so that nothing is
// constructed before main() parses the command line: a namespace-scope Scene
// would bulk-register components, and Image's ctor would query them, during
// static initialization -- before Engine::systemInit can answer those queries.
// They are still constructed in Scene-before-Image order on first use, and the
// storage is static, so the pointer passed to Engine::setScene stays valid.
Scene& demoScene() { static Scene scene; return scene; }
Image& demoImage() { static Image image(&demoScene()); return image; }
Scene& headlessFallbackScene() { static Scene scene; return scene; }

// Headless probes must not depend on a desktop display, X11, Cocoa or a GPU
// context. The platform game loops still use their native System subclasses;
// this small implementation only supplies the resource roots and dimensions
// needed during Engine::systemInit/systemUpdate.
class HeadlessSystem final : public System {
public:
    int getScreenWidth() override { return 1; }
    int getScreenHeight() override { return 1; }
    std::string getAssetPath() override { return "assets"; }
    std::string getUserDataPath() override { return "."; }
    std::string getLuaPath() override { return "lua"; }
    std::string getShaderPath() override { return "shaders"; }
};

bool installScriptProbe() {
    lua_State* L = LuaBinding::getLuaState();
    if (!L) return false;
    const char* source =
        "Engine.onUpdate = function() "
        "local accumulator = 0 "
        "for i = 1, 16 do accumulator = accumulator + i end "
        "end";
    if (luaL_loadbuffer(L, source, std::strlen(source), "quantum-engine://script-probe") != 0) {
        say(0, "error: script probe load failed: %s",
            LuaBinding::getLuaStackErrorString(L, -1).c_str());
        lua_pop(L, 1);
        return false;
    }
    if (LuaBinding::pcallWithTraceback(L, 0, LUA_MULTRET) != 0) {
        say(0, "error: script probe install failed: %s",
            LuaBinding::getLuaStackErrorString(L, -1).c_str());
        lua_pop(L, 1);
        return false;
    }
    say(1, "quantum-engine: Lua script profiling probe installed");
    return true;
}

bool runScriptSmoke() {
    lua_State* L = LuaBinding::getLuaState();
    if (!L) return false;
    const char* source = R"lua(
        local scene = Scene()
        local object = Object(scene)
        local entity = object.entity
        local generation = scene:getEntityGeneration(entity)
        object.name = "lua-script-smoke"
        object:setPosition(3.0, 4.0, 5.0)
        local position = object.position
        assert(scene:isEntityCreated(entity))
        assert(object.name == "lua-script-smoke")
        assert(math.abs(position.x - 3.0) < 0.0001)
        assert(math.abs(position.y - 4.0) < 0.0001)
        object.entityOwned = false
        scene:destroyEntity(entity)
        assert(not scene:isEntityCreated(entity))
        assert(scene:getEntityGeneration(entity) == generation + 1)
        local recycled = scene:createEntity()
        assert(recycled == entity)
        assert(scene:isEntityCreated(recycled))
        scene:destroyEntity(recycled)

        local voxels = VoxelWorld(305419896)
        voxels:update(0, 0, 1)
        assert(voxels:setBlock(0, 0, 0, 7))
        assert(voxels:getBlock(0, 0, 0) == 7)
        assert(voxels.dirtyChunkCount > 0)
        voxels:rebuildLighting()
    )lua";
    if (luaL_loadbuffer(L, source, std::strlen(source), "quantum-engine://script-smoke") != 0) {
        say(0, "error: script smoke load failed: %s",
            LuaBinding::getLuaStackErrorString(L, -1).c_str());
        lua_pop(L, 1);
        return false;
    }
    if (LuaBinding::pcallWithTraceback(L, 0, LUA_MULTRET) != 0) {
        say(0, "error: script smoke failed: %s",
            LuaBinding::getLuaStackErrorString(L, -1).c_str());
        lua_pop(L, 1);
        return false;
    }
    say(1, "quantum-engine: Lua scripting smoke passed");
    return true;
}

graph::GraphAsset makeGraphSmokeAsset() {
    graph::GraphAsset asset;
    asset.name = "graph-runtime-smoke";

    graph::GraphVariable result;
    result.name = "result";
    result.displayName = "result";
    result.type = graph::GraphValueType::Float;
    result.defaultValue = graph::GraphValue::makeFloat(0.0);
    asset.variables.push_back(result);

    graph::GraphNode start;
    start.id = 1;
    start.kind = "event.start";

    graph::GraphNode set;
    set.id = 2;
    set.kind = "var.set";
    set.setParam("variable", graph::GraphValue::makeString("result"));

    graph::GraphNode add;
    add.id = 3;
    add.kind = "math.add";
    add.setParam("a", graph::GraphValue::makeFloat(2.0));
    add.setParam("b", graph::GraphValue::makeFloat(5.5));

    graph::GraphNode reflected;
    reflected.id = 4;
    reflected.kind = "method.Object.setVisibleOnly";
    reflected.setParam("args", graph::GraphValue::makeInt(1));
    reflected.setParam("arg0", graph::GraphValue::makeBool(false));

    asset.nodes.push_back(start);
    asset.nodes.push_back(set);
    asset.nodes.push_back(add);
    asset.nodes.push_back(reflected);

    graph::GraphEdge execEdge;
    execEdge.fromNode = 1;
    execEdge.fromPin = "out";
    execEdge.toNode = 2;
    execEdge.toPin = "in";
    asset.edges.push_back(execEdge);

    graph::GraphEdge reflectedExecEdge;
    reflectedExecEdge.fromNode = 2;
    reflectedExecEdge.fromPin = "out";
    reflectedExecEdge.toNode = 4;
    reflectedExecEdge.toPin = "in";
    asset.edges.push_back(reflectedExecEdge);

    graph::GraphEdge valueEdge;
    valueEdge.fromNode = 3;
    valueEdge.fromPin = "out";
    valueEdge.toNode = 2;
    valueEdge.toPin = "value";
    asset.edges.push_back(valueEdge);

    graph::GraphEventBinding startEvent;
    startEvent.id = "start";
    startEvent.entryNode = 1;
    asset.events.push_back(startEvent);

    return asset;
}

bool runGraphSmoke() {
    graph::GraphAsset asset = makeGraphSmokeAsset();

    std::vector<graph::GraphDiagnostic> diagnostics;
    graph::GraphSchema::validate(asset, diagnostics);
    if (graph::GraphSchema::hasErrors(diagnostics)) {
        say(0, "error: graph smoke schema failed: %s",
            graph::GraphSchema::format(diagnostics).c_str());
        return false;
    }

    std::shared_ptr<graph::ExecutableGraph> executable =
        std::make_shared<graph::ExecutableGraph>();
    std::string error;
    if (!executable->prepare(asset, error)) {
        say(0, "error: graph smoke prepare failed: %s", error.c_str());
        return false;
    }

    Scene scene;
    const Entity entity = scene.createEntity();
    scene.addComponent<Transform>(entity);
    graph::GraphInstance instance;
    instance.scene = &scene;
    instance.entity = entity;
    instance.graph = executable;
    instance.registers = executable->getRegisterDefaults();
    for (const graph::GraphVariable& variable : asset.variables) {
        instance.variables.push_back(variable.defaultValue);
    }
    instance.nodeState.assign(executable->getNodes().size(), 0);
    instance.nodeScratch.assign(executable->getNodes().size(), graph::GraphValue());

    graph::GraphRuntime runtime(instance, *executable);
    runtime.fire("start");
    const bool passed = !runtime.hasFailed() && instance.variables.size() == 1 &&
        instance.variables[0].type == graph::GraphValueType::Float &&
        std::fabs(instance.variables[0].asFloat() - 7.5) < 0.0001
        && !scene.getComponent<Transform>(entity).visible;
    say(passed ? 1 : 0,
        "quantum-engine: visual graph runtime smoke %s (result=%.3f)",
        passed ? "passed" : "FAILED",
        instance.variables.empty() ? 0.0 : instance.variables[0].asFloat());
    return passed;
}

bool runGraphProjectSmoke() {
    if (g_options.projectPath.empty()) {
        say(0, "error: graph project smoke requires --project");
        return false;
    }

    // Compile into the selected project's asset root, then let GraphBinding load
    // it through asset:// exactly as an exported project does. The temporary file
    // is removed before this probe returns and no source scene is touched.
    const std::filesystem::path graphPath =
        std::filesystem::path(System::instance().getAssetPath()) /
        ".quantum-graph-runtime-smoke.gdat";
    std::error_code fileError;
    std::filesystem::remove(graphPath, fileError);

    const graph::GraphAsset asset = makeGraphSmokeAsset();
    std::string error;
    if (!graph::GraphCompiler::writeFile(graphPath.string(), asset,
            0x475241504852554Cull, error)) {
        say(0, "error: graph project smoke compile failed: %s", error.c_str());
        return false;
    }

    bool passed = false;
    {
        Scene scene;
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity);
        scene.addComponent<ScriptComponent>(entity);
        ScriptEntry entry;
        entry.type = ScriptType::GRAPH;
        entry.path = ".quantum-graph-runtime-smoke.gdat";
        entry.className = "graph-runtime-smoke";
        entry.enabled = true;
        scene.getComponent<ScriptComponent>(entity).scripts.push_back(entry);

        graph::GraphBinding::initializeGraphScripts(&scene);
        ScriptEntry& loaded = scene.getComponent<ScriptComponent>(entity).scripts[0];
        graph::GraphInstance* instance =
            static_cast<graph::GraphInstance*>(loaded.instance);
        const double result = instance && instance->variables.size() == 1
            ? instance->variables[0].asFloat() : 0.0;
        passed = instance && loaded.instance != nullptr
            && instance->variables.size() == 1
            && instance->variables[0].type == graph::GraphValueType::Float
            && std::fabs(result - 7.5) < 0.0001
            && !scene.getComponent<Transform>(entity).visible
            && graph::GraphBinding::getInstanceCount() == 1;
        say(passed ? 1 : 0,
            "quantum-engine: exported graph project smoke %s (asset:// load, reflected method, result=%.3f)",
            passed ? "passed" : "FAILED", result);
        graph::GraphBinding::cleanupGraphScripts(&scene);
    }

    std::filesystem::remove(graphPath, fileError);
    if (fileError) {
        say(0, "error: graph project smoke cleanup failed: %s", fileError.message().c_str());
        passed = false;
    }
    return passed;
}

bool writeScriptReloadProbe(const std::filesystem::path& file, int version) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    if (version == 1) {
        output << "return {\n"
               << "  init = function(self) self.state = (self.state or 0) + 1; RegisterEngineEvent(self, 'onUpdate') end,\n"
               << "  onUpdate = function(self) self.state = self.state + 1 end,\n"
               << "  version = function() return 1 end,\n"
               << "  onReload = function(self) self.reloads = (self.reloads or 0) + 1; RegisterEngineEvent(self, 'onUpdate') end\n"
               << "}\n";
    } else {
        output << "return {\n"
               << "  init = function(self) RegisterEngineEvent(self, 'onUpdate') end,\n"
               << "  onUpdate = function(self) self.state = self.state + 10 end,\n"
               << "  version = function() return 2 end,\n"
               << "  onReload = function(self) self.reloads = (self.reloads or 0) + 1; RegisterEngineEvent(self, 'onUpdate') end\n"
               << "}\n";
    }
    return output.good();
}

bool readLuaIntegerField(lua_State* L, int ref, const char* field, lua_Integer& value) {
    const int base = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_getfield(L, -1, field);
    const bool valid = lua_isinteger(L, -1);
    if (valid) value = lua_tointeger(L, -1);
    lua_settop(L, base);
    return valid;
}

bool prepareScriptReloadSmoke() {
    if (g_options.projectDir.empty()) {
        say(0, "error: --script-reload-smoke requires --project");
        return false;
    }
    const std::filesystem::path root = g_options.projectDir
        / (g_options.luaDir.empty() ? "lua" : g_options.luaDir);
    g_scriptReloadProbe.file = root / "_quantum_script_reload_smoke.lua";
    if (!writeScriptReloadProbe(g_scriptReloadProbe.file, 1)) return false;

    static Scene scene;
    g_scriptReloadProbe.scene = &scene;
    g_scriptReloadProbe.entity = scene.createEntity();
    scene.addComponent<ScriptComponent>(g_scriptReloadProbe.entity);
    ScriptComponent* component = scene.findComponent<ScriptComponent>(g_scriptReloadProbe.entity);
    if (!component) return false;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.path = "_quantum_script_reload_smoke.lua";
    entry.className = "ScriptReloadSmoke";
    entry.enabled = true;
    component->scripts.push_back(std::move(entry));
    LuaBinding::initializeLuaScripts(&scene);
    if (!component->scripts[0].instance) return false;
    Engine::setScene(&scene);
    return true;
}

bool finishScriptReloadSmoke() {
    Scene* scene = g_scriptReloadProbe.scene;
    if (!scene || !scene->isEntityCreated(g_scriptReloadProbe.entity)) return false;
    ScriptComponent* component = scene->findComponent<ScriptComponent>(g_scriptReloadProbe.entity);
    if (!component || component->scripts.empty() || !component->scripts[0].instance) return false;
    const int ref = static_cast<int>(reinterpret_cast<intptr_t>(component->scripts[0].instance));
    lua_State* L = LuaBinding::getLuaState();
    lua_Integer beforeReload = 0;
    if (!L || !readLuaIntegerField(L, ref, "state", beforeReload) || beforeReload != 2) {
        say(0, "script reload smoke: state before reload=%lld", static_cast<long long>(beforeReload));
        return false;
    }
    if (g_options.scriptHotReload)
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    if (!writeScriptReloadProbe(g_scriptReloadProbe.file, 2)) return false;
    bool callbackRanDuringHotReload = false;
    if (g_options.scriptHotReload) {
        // Let the polling interval elapse so this probe exercises the mtime path,
        // rather than the explicit reload API used by the baseline smoke.
        Engine::systemUpdate();
        callbackRanDuringHotReload = true;
    } else if (!LuaBinding::reloadLuaScripts(scene)) {
        return false;
    }
    lua_Integer reloads = 0;
    if (!readLuaIntegerField(L, ref, "reloads", reloads) || reloads != 1) {
        say(0, "script reload smoke: reload callback count=%lld", static_cast<long long>(reloads));
        return false;
    }
    lua_settop(L, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_getfield(L, -1, "version");
    if (!lua_isfunction(L, -1)) {
        say(0, "script reload smoke: version field type is %s", luaL_typename(L, -1));
        lua_settop(L, 0);
        return false;
    }
    lua_pushvalue(L, -2);
    if (LuaBinding::pcallWithTraceback(L, 1, 1) != LUA_OK || !lua_isinteger(L, -1)
            || lua_tointeger(L, -1) != 2) {
        lua_settop(L, 0);
        return false;
    }
    lua_settop(L, 0);
    if (!callbackRanDuringHotReload) Engine::systemUpdate();
    lua_Integer afterReload = 0;
    const bool statePreservedAndRebound = readLuaIntegerField(L, ref, "state", afterReload)
        && afterReload == 12;
    if (!statePreservedAndRebound)
        say(0, "script reload smoke: state after reload=%lld", static_cast<long long>(afterReload));
    std::error_code ec;
    std::filesystem::remove(g_scriptReloadProbe.file, ec);
    return statePreservedAndRebound;
}

void bootDemoScene() {
    say(1, "boot: no project entry found, running the built-in demo scene");

    demoImage().setAnchorPreset(AnchorPreset::CENTER);

    // The logo is the sample project's own asset; when running loose it is
    // looked up in the working directory, then in engine/project/assets of
    // the engine dir copied next to the executable (the editor ships one).
    const std::string candidates[3] = {
        "quantum.png",
        (getEngineDir() / "project" / "assets" / "quantum.png").generic_string(),
        (std::filesystem::path(QUANTUM_ENGINE_DIR_FALLBACK).empty()
             ? std::string()
             : (std::filesystem::path(QUANTUM_ENGINE_DIR_FALLBACK) / "project" / "assets" / "quantum.png").generic_string()),
    };
    for (const std::string& path : candidates) {
        if (path.empty()) continue;
        Data probe;
        if (probe.open(path.c_str()) == FileErrors::FILEDATA_OK && probe.length() > 0) {
            demoImage().setTexture(path);
            break;
        }
    }

    demoScene().setBackgroundColor(0.10f, 0.12f, 0.16f);

    Engine::setScalingMode(Scaling::FITWIDTH);
    Engine::setCanvasSize(1000, 480);
    Engine::setScene(&demoScene());

    // A slow alpha pulse keeps the frame loop visible without assets.
    Engine::onUpdate.add("quantum-engine-demo-pulse", []() {
        const double t = Engine::getSystemTime();
        demoImage().setAlpha(0.8f + 0.2f * (float)std::sin(t * 1.5));
    });
}

void applyRenderFeatureOptions(Scene* scene) {
    if (!scene || g_options.renderFeatures.empty()) return;

    for (std::string feature : g_options.renderFeatures) {
        std::transform(feature.begin(), feature.end(), feature.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (feature == "taa") {
            scene->setTAAEnabled(true);
        } else if (feature == "motionblur" || feature == "motion-blur") {
            scene->setMotionBlurEnabled(true);
        } else if (feature == "hdr") {
            scene->setTonemapEnabled(true);
            scene->setBloomEnabled(true);
        } else if (feature == "lut" || feature == "lutcinematic"
                || feature == "colorgrading" || feature == "color-grading") {
            scene->setColorGradingEnabled(true);
            if (!g_options.renderLut.empty()) scene->setColorGradingLUTPath(g_options.renderLut);
        } else if (feature == "ssgi") {
            scene->setSSGIEnabled(true);
        } else if (feature == "ssr" || feature == "ssrfallback") {
            scene->setSSREnabled(true);
            if (feature == "ssrfallback") {
                // The validation fixture deliberately adds a runtime-captured local
                // probe. SSR can then replace confident screen-space hits while the
                // scene keeps the probe's forward reflection when a ray misses.
                scene->setSSRMaxDistance(48.0f);
                scene->setSSRThickness(0.18f);
                scene->setSSRMaxSteps(64);
                scene->setSSRIntensity(1.0f);
                scene->setSSRBlur(0.08f);

                Entity probeEntity = scene->createEntity();
                scene->addComponent<Transform>(probeEntity);
                scene->addComponent<ReflectionProbeComponent>(probeEntity);
                Transform& probeTransform = scene->getComponent<Transform>(probeEntity);
                probeTransform.position = Vector3(0.0f, 0.0f, 0.0f);
                probeTransform.worldPosition = probeTransform.position;
                ReflectionProbeComponent& probe = scene->getComponent<ReflectionProbeComponent>(probeEntity);
                probe.mode = ReflectionProbeMode::DYNAMIC;
                probe.updateMode = ReflectionProbeUpdateMode::ON_LOAD;
                probe.boxSize = Vector3(100.0f, 100.0f, 100.0f);
                probe.blendDistance = 4.0f;
                probe.resolution = 32;
                probe.nearClip = 0.05f;
                probe.farClip = 200.0f;
                probe.includeSky = true;
                scene->setEntityName(probeEntity, "phase3-ssr-fallback-probe");
                say(1, "quantum-engine: SSR fallback fixture added a dynamic reflection probe");
            }
        } else if (feature == "atmosphere" || feature == "fog") {
            FogComponent* fog = scene->findComponentFromIndex<FogComponent>(0);
            if (!fog) {
                const Entity fogEntity = scene->createEntity();
                scene->addComponent<FogComponent>(fogEntity);
                fog = scene->findComponentFromIndex<FogComponent>(0);
            }
            if (fog) {
                fog->density = 0.018f;
                fog->heightFog = true;
                fog->fogHeightFalloff = 0.08f;
                fog->fogGroundHeight = 0.0f;
                fog->volumetricFog = feature == "atmosphere";
                fog->godRays = feature == "atmosphere";
            }
        } else if (feature == "shadows") {
            auto lights = scene->getComponentArray<LightComponent>();
            for (int i = 0; lights && i < lights->size(); i++)
                lights->getComponentFromIndex(i).shadows = true;
        } else if (feature == "clustered" || feature == "tiled") {
            say(1, "quantum-engine: clustered/tiled lighting is selected by the compiled graphics backend");
        } else if (feature == "decals") {
            say(1, "quantum-engine: decals require a DecalComponent in the loaded scene");
        } else {
            say(0, "warning: unknown --render-feature '%s'", feature.c_str());
        }
    }
    if (!g_options.renderLut.empty() && !scene->isColorGradingEnabled()) {
        scene->setColorGradingEnabled(true);
        scene->setColorGradingLUTPath(g_options.renderLut);
    }
    say(1, "quantum-engine: render feature overrides applied (%d)",
        static_cast<int>(g_options.renderFeatures.size()));
}

void attachRemoteDebugger() {
    if (!g_options.debugPortEnabled || !g_remoteDebugServer) return;
    lua_State* state = LuaBinding::getLuaState();
    if (!state) {
        say(0, "error: remote debugger requested but Lua is not initialized");
        return;
    }

    g_remoteDebugger = std::make_unique<LuaDebugger>(state);
    g_remoteDebugger->setPauseCallback([](bool paused) {
        // Drawing and the remote service continue while game events are paused,
        // so a debugger client can always issue the command that resumes Play.
        Engine::pauseGameEvents(paused);
    });
    if (!g_remoteDebugger->attach()) {
        say(0, "error: remote debugger could not attach to the Lua VM");
        g_remoteDebugger.reset();
        return;
    }
    Engine::onPostDraw.add("quantum-engine-remote-debug", []() {
        serviceRemoteDebugger();
    });
    Engine::onShutdown.add("quantum-engine-remote-debug-shutdown", []() {
        if (g_remoteDebugger) {
            g_remoteDebugger->shutdown();
            g_remoteDebugger.reset();
        }
    });
    say(1, "quantum-engine: remote Lua debugger attached on 127.0.0.1:%u",
        static_cast<unsigned>(g_remoteDebugServer->port()));
}

// ---- bootstrap + window adjustments ----------------------------------------

// Registered exactly like the exported-game main does with QUANTUM_INIT
// (engine/core/Engine.h:135-144): the engine fires it inside
// Engine::systemInit via getOnInit().call().
QUANTUM_INIT void init() {
    // Attach before loading the project entry so a client can install a
    // breakpoint before the first callback is dispatched.
    attachRemoteDebugger();
    bool booted = false;
    if (!g_options.projectPath.empty()) {
        if (!g_options.sceneName.empty()) booted = runLuaEntry(g_options.sceneName);
        if (!booted) booted = runLuaEntry("main");
    }
    if (!booted) {
        if (g_options.headless) {
            say(1, "boot: no Lua entry found, running an empty headless scene");
            Engine::setScene(&headlessFallbackScene());
        } else {
            bootDemoScene();
        }
    }

    if (g_options.scriptProbe && !installScriptProbe())
        say(0, "warning: Lua script profiling probe was not installed");
    if (g_options.scriptSmoke)
        g_scriptSmokeFailed = !runScriptSmoke();
    if (g_options.graphSmoke)
        g_graphSmokeFailed = !runGraphSmoke();
    if (g_options.graphProjectSmoke)
        g_graphProjectSmokeFailed = !runGraphProjectSmoke();
    if (g_options.scriptReloadSmoke)
        g_scriptReloadSmokeFailed = !prepareScriptReloadSmoke();

    if (!g_options.audioProbeWav.empty()) {
        const bool wavLoaded = AudioSystem::loadMusicStream(g_options.audioProbeWav);
        const bool wavStarted = AudioSystem::playMusicStream(g_options.audioProbeWav, 0.0f);
        const bool oggLoaded = AudioSystem::loadMusicStream(g_options.audioProbeOgg);
        const bool oggStarted = AudioSystem::playMusicStream(g_options.audioProbeOgg, 0.1f);
        const bool playing = AudioSystem::isMusicPlaying();
        AudioSystem::stopMusic();
        const bool ambienceWav = AudioSystem::setAmbienceLayer(
            "probe-wav", g_options.audioProbeWav, 0.5f, 0.0f);
        const bool ambienceOgg = AudioSystem::setAmbienceLayer(
            "probe-ogg", g_options.audioProbeOgg, 0.25f, 0.0f);
        const bool ambienceLayers = ambienceWav && ambienceOgg
            && AudioSystem::getAmbienceLayerCount() == 2;
        AudioSystem::stopAmbienceLayer("probe-wav", 0.0f);
        const bool ambienceStoppedOne = AudioSystem::getAmbienceLayerCount() == 1;
        AudioSystem::stopAmbience();
        const bool ambienceStopped = AudioSystem::getAmbienceLayerCount() == 0;
        g_audioProbeFailed = !(wavLoaded && wavStarted && oggLoaded && oggStarted && playing
            && ambienceLayers && ambienceStoppedOne && ambienceStopped);
        say(g_audioProbeFailed ? 0 : 1,
            "quantum-engine: audio probe wav=%s ogg=%s wavPlay=%s oggPlay=%s playing=%s ambience=%s",
            wavLoaded ? "ok" : "failed", oggLoaded ? "ok" : "failed",
            wavStarted ? "ok" : "failed", oggStarted ? "ok" : "failed",
            playing ? "yes" : "no", ambienceLayers && ambienceStopped ? "ok" : "failed");
    }

    Scene* scene = Engine::getMainScene();
    applyRenderFeatureOptions(scene);
    if (g_options.networkRole != Options::NetworkRole::None) {
        if (!scene) {
            // A headless project may intentionally have no view lifecycle. Keep
            // the network CLI useful by giving it the same empty Scene used by
            // the runner's fallback path.
            Engine::setScene(&headlessFallbackScene());
        }
        // The project entry has already run above, so start networking against
        // its final scene now. Deferring this to onUpdate made headless runs
        // silently skip the CLI role because the Lua event table can be rebound
        // by a project before the first frame.
        const std::shared_ptr<NetworkSystem> network = scene->getSystem<NetworkSystem>();
        if (!network) {
            say(0, "error: selected scene has no NetworkSystem");
            return;
        }
        network->setAuthToken(g_options.networkToken);
        if (g_options.networkRelay && g_options.networkRole == Options::NetworkRole::Server) {
            network->startRelayServer(g_options.relayAddress,
                static_cast<uint16_t>(g_options.relayPort), g_options.relayRoom,
                g_options.relayToken.empty() ? g_options.networkToken : g_options.relayToken);
            say(1, "quantum-engine: network relay server room=%s relay=%s:%d",
                g_options.relayRoom.c_str(), g_options.relayAddress.c_str(), g_options.relayPort);
        } else if (g_options.networkRelay && g_options.networkRole == Options::NetworkRole::Client) {
            network->startRelayClient(g_options.relayAddress,
                static_cast<uint16_t>(g_options.relayPort), g_options.relayRoom,
                g_options.relayToken.empty() ? g_options.networkToken : g_options.relayToken);
            say(1, "quantum-engine: network relay client room=%s relay=%s:%d",
                g_options.relayRoom.c_str(), g_options.relayAddress.c_str(), g_options.relayPort);
        } else if (g_options.networkRole == Options::NetworkRole::Server) {
            network->startServer(static_cast<uint16_t>(g_options.networkPort));
            say(1, "quantum-engine: network server listening on %d", g_options.networkPort);
        } else {
            network->startClient(g_options.networkAddress,
                static_cast<uint16_t>(g_options.networkPort));
            say(1, "quantum-engine: network client connecting to %s:%d",
                g_options.networkAddress.c_str(), g_options.networkPort);
        }
    }
}

// QuantumWin::init uses compile-time window settings (engine/platform/win/
// QuantumWin.cpp:312-326); runtime overrides from the CLI are applied as soon
// as the view exists, through the public System API.
void subscribeWindowOptions() {
    Engine::onViewLoaded.add("quantum-engine-window", []() {
        System& sys = System::instance();
        if (g_options.windowWidth > 0 && g_options.windowHeight > 0) {
            sys.setWindowSize(g_options.windowWidth, g_options.windowHeight);
        }
        if (!g_options.title.empty()) {
            sys.setWindowTitle(g_options.title);
        }
        if (g_options.fullscreen) {
            sys.requestFullscreen();
        }
    });
}

// ---- headless measurement run ----------------------------------------------

// --frames turns the runner into a benchmark: Engine::FrameStats is sampled once
// per presented frame, the first --warmup frames are dropped so async loads
// settle, and when the requested count is reached the mean is printed as JSON and
// the window is closed. It measures the loop the shipped game runs, which the
// editor cannot show (ImGui chrome, outliner and gizmos are outside it).
struct Measure {
    static const int MAX_SYSTEMS = 64;

    int warmup = 0;
    int total = 0;
    int frame = 0;
    int measured = 0;
    bool done = false;

    double fpsSum = 0.0;
    double fpsMin = 1e30;
    double fpsMax = 0.0;
    double fixedMs = 0.0;
    double updateMs = 0.0;
    double drawMs = 0.0;
    double commitMs = 0.0;
    double frameMs = 0.0;
    unsigned long long drawCalls = 0;
    unsigned long long instances = 0;
    unsigned long long triangles = 0;
    unsigned long long entitiesSum = 0;
    unsigned long long peakEntities = 0;
    unsigned long long componentsSum = 0;
    unsigned long long peakComponents = 0;
    unsigned long long registeredComponents = 0;
    unsigned long long entityStorageSlots = 0;
    unsigned long long entityStorageCapacity = 0;
    unsigned long long entityFreeList = 0;
    unsigned long long componentRegistrySlots = 0;
    unsigned long long componentRegistryCapacity = 0;
    unsigned long long meshLoads = 0;
    unsigned long long meshDrops = 0;
    unsigned long long residentBytesSum = 0;
    unsigned long long peakResidentBytes = 0;
    unsigned long long gpuDrawCalls = 0;
    unsigned long long gpuPasses = 0;
    unsigned long long gpuUpdates = 0;
    unsigned long long gpuAliveBuffers = 0;
    unsigned long long gpuAliveImages = 0;
    unsigned long long gpuAliveSamplers = 0;
    unsigned long long gpuAliveShaders = 0;
    unsigned long long gpuAlivePipelines = 0;
    unsigned long long gpuAliveViews = 0;
    bool gpuStatsAvailable = false;
    unsigned long long scriptCalls = 0;
    unsigned long long scriptErrors = 0;
    double scriptMs = 0.0;
    unsigned long long privateBytesSum = 0;
    unsigned long long peakPrivateBytes = 0;
    unsigned long long activeHeapAllocationsSum = 0;
    unsigned long long peakActiveHeapAllocations = 0;
    unsigned long long activeHeapBytesSum = 0;
    unsigned long long peakActiveHeapBytes = 0;
    bool heapTrackingAvailable = false;

    std::string systemNames[MAX_SYSTEMS];
    double systemMs[MAX_SYSTEMS] = {0.0};
    int systemCount = 0;

    struct ScriptMeasure {
        std::string name;
        double milliseconds = 0.0;
        unsigned long long calls = 0;
        unsigned long long errors = 0;
    };
    std::vector<ScriptMeasure> scripts;

    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point measuring{};
};

Measure g_measure;

uint64_t processResidentBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<uint64_t>(counters.WorkingSetSize);
#endif
    return 0;
}

uint64_t processPrivateBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        return static_cast<uint64_t>(counters.PrivateUsage);
#endif
    return 0;
}

struct HeapSample {
    uint64_t allocations = 0;
    uint64_t bytes = 0;
    bool available = false;
};

HeapSample processHeapSample() {
#if defined(_WIN32) && defined(_DEBUG)
    _CrtMemState state{};
    _CrtMemCheckpoint(&state);
    uint64_t bytes = 0;
    for (size_t i = 0; i < _MAX_BLOCKS; ++i) bytes += state.lSizes[i];
    return {
        static_cast<uint64_t>(state.lTotalCount),
        bytes,
        true
    };
#endif
    return {};
}

// FrameStats keeps at most 32 named systems; the runner merges frames, so the
// table is keyed by name here rather than by slot (a slot can be reused).
void addMeasuredSystem(const char* name, double ms) {
    for (int i = 0; i < g_measure.systemCount; i++) {
        if (g_measure.systemNames[i] == name) {
            g_measure.systemMs[i] += ms;
            return;
        }
    }
    if (g_measure.systemCount < Measure::MAX_SYSTEMS) {
        g_measure.systemNames[g_measure.systemCount] = name;
        g_measure.systemMs[g_measure.systemCount] = ms;
        g_measure.systemCount++;
    }
}

void addMeasuredScript(const Engine::FrameStats::ScriptTime& script) {
    if (!script.name[0]) return;
    for (auto& measured : g_measure.scripts) {
        if (measured.name == script.name) {
            measured.milliseconds += script.milliseconds;
            measured.calls += script.calls;
            measured.errors += script.errors;
            return;
        }
    }
    if (g_measure.scripts.size() < Engine::FrameStats::MAX_SCRIPT_FUNCTIONS) {
        g_measure.scripts.push_back({script.name, script.milliseconds, script.calls, script.errors});
    }
}

std::string jsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '\n' || c == '\r' || c == '\t') { out.push_back('_'); continue; }
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string number(double value, int precision = 3) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

bool compareScreenshots(const std::filesystem::path& currentPath,
    const std::filesystem::path& baselinePath, float threshold,
    const std::filesystem::path& diffPath, const std::filesystem::path& reportPath) {
    int currentWidth = 0;
    int currentHeight = 0;
    int baselineWidth = 0;
    int baselineHeight = 0;
    int currentChannels = 0;
    int baselineChannels = 0;
    unsigned char* current = stbi_load(currentPath.string().c_str(), &currentWidth,
        &currentHeight, &currentChannels, 4);
    unsigned char* baseline = stbi_load(baselinePath.string().c_str(), &baselineWidth,
        &baselineHeight, &baselineChannels, 4);
    if (!current || !baseline) {
        say(0, "error: screenshot compare could not load '%s' or '%s'",
            currentPath.string().c_str(), baselinePath.string().c_str());
        if (current) stbi_image_free(current);
        if (baseline) stbi_image_free(baseline);
        return false;
    }
    if (currentWidth != baselineWidth || currentHeight != baselineHeight) {
        say(0, "error: screenshot dimensions differ: current=%dx%d baseline=%dx%d",
            currentWidth, currentHeight, baselineWidth, baselineHeight);
        stbi_image_free(current);
        stbi_image_free(baseline);
        return false;
    }

    const size_t pixels = static_cast<size_t>(currentWidth) * static_cast<size_t>(currentHeight);
    double totalError = 0.0;
    double maximumError = 0.0;
    size_t differingPixels = 0;
    const double limit = std::clamp(static_cast<double>(threshold), 0.0, 1.0);
    std::vector<unsigned char> diff;
    if (!diffPath.empty())
        diff.resize(pixels * 4, 0);
    for (size_t i = 0; i < pixels; ++i) {
        double pixelError = 0.0;
        for (int channel = 0; channel < 3; ++channel) {
            const int delta = std::abs(static_cast<int>(current[i * 4 + channel])
                - static_cast<int>(baseline[i * 4 + channel]));
            pixelError += delta / 255.0;
            if (!diff.empty())
                diff[i * 4 + channel] = static_cast<unsigned char>(std::min(255, delta * 4));
        }
        pixelError /= 3.0;
        totalError += pixelError;
        maximumError = std::max(maximumError, pixelError);
        if (pixelError > limit) ++differingPixels;
        if (!diff.empty()) diff[i * 4 + 3] = 255;
    }

    const double meanError = pixels > 0 ? totalError / static_cast<double>(pixels) : 0.0;
    const double differingRatio = pixels > 0
        ? static_cast<double>(differingPixels) / static_cast<double>(pixels) : 0.0;
    const bool passed = meanError <= limit;

    if (!diff.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(diffPath.parent_path(), ec);
        if (!stbi_write_png(diffPath.string().c_str(), currentWidth, currentHeight, 4,
                diff.data(), currentWidth * 4)) {
            say(0, "error: failed to write screenshot diff '%s'", diffPath.string().c_str());
        }
    }

    if (!reportPath.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(reportPath.parent_path(), ec);
        std::FILE* report = std::fopen(reportPath.string().c_str(), "wb");
        if (report) {
            std::fprintf(report,
                "{\n"
                "  \"current\": \"%s\",\n"
                "  \"baseline\": \"%s\",\n"
                "  \"width\": %d,\n"
                "  \"height\": %d,\n"
                "  \"meanError\": %.9f,\n"
                "  \"maximumError\": %.9f,\n"
                "  \"differingPixels\": %llu,\n"
                "  \"differingRatio\": %.9f,\n"
                "  \"threshold\": %.9f,\n"
                "  \"passed\": %s\n"
                "}\n",
                jsonEscape(currentPath.string()).c_str(),
                jsonEscape(baselinePath.string()).c_str(), currentWidth, currentHeight,
                meanError, maximumError,
                static_cast<unsigned long long>(differingPixels), differingRatio, limit,
                passed ? "true" : "false");
            std::fclose(report);
        } else {
            say(0, "error: failed to write screenshot compare report '%s'", reportPath.string().c_str());
        }
    }

    stbi_image_free(current);
    stbi_image_free(baseline);
    say(passed ? 1 : 0,
        "quantum-engine: screenshot compare mean=%.6f max=%.6f differing=%.2f%% threshold=%.6f%s",
        meanError, maximumError, differingRatio * 100.0, limit,
        passed ? " PASS" : " FAIL");
    return passed;
}

// The measured system costs summed, in descending order, as JSON array rows.
std::string slowestSystemsJson(int limit) {
    std::string out;
    const double frames = g_measure.measured > 0 ? (double)g_measure.measured : 1.0;
    for (int rank = 0; rank < limit; rank++) {
        int best = -1;
        double bestMs = -1.0;
        for (int i = 0; i < g_measure.systemCount; i++) {
            if (g_measure.systemMs[i] > bestMs) { bestMs = g_measure.systemMs[i]; best = i; }
        }
        if (best < 0 || bestMs <= 0.0) break;
        if (!out.empty()) out += ",\n";
        out += "      {\"name\": \"" + jsonEscape(g_measure.systemNames[best]) +
               "\", \"ms\": " + number(bestMs / frames) + "}";
        g_measure.systemMs[best] = -1.0;   // consumed
    }
    return out;
}

std::string slowestScriptsJson(int limit) {
    std::vector<size_t> order(g_measure.scripts.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [](size_t left, size_t right) {
        return g_measure.scripts[left].milliseconds > g_measure.scripts[right].milliseconds;
    });
    const double frames = g_measure.measured > 0 ? static_cast<double>(g_measure.measured) : 1.0;
    std::string out;
    const size_t count = std::min(static_cast<size_t>(limit), order.size());
    for (size_t rank = 0; rank < count; rank++) {
        const auto& script = g_measure.scripts[order[rank]];
        if (!out.empty()) out += ",\n";
        out += "      {\"name\": \"" + jsonEscape(script.name)
            + "\", \"ms\": " + number(script.milliseconds / frames)
            + ", \"callsPerFrame\": " + number(static_cast<double>(script.calls) / frames, 2)
            + ", \"errorsPerFrame\": " + number(static_cast<double>(script.errors) / frames, 2) + "}";
    }
    return out;
}

std::string buildReport(const std::string& label) {
    const double frames = g_measure.measured > 0 ? (double)g_measure.measured : 1.0;
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_measure.measuring).count();
    const double wallFps = elapsed > 0.0 ? frames / elapsed : 0.0;
    const double avgFps = g_measure.measured > 0 ? g_measure.fpsSum / frames : 0.0;
    const size_t entities = Engine::getMainScene() ? Engine::getMainScene()->getEntityCount() : 0;
    const size_t components = Engine::getMainScene() ? Engine::getMainScene()->getComponentInstanceCount() : 0;

    std::string json;
    json += "{\n";
    json += "  \"name\": \"" + jsonEscape(label) + "\",\n";
    json += "  \"source\": \"quantum-engine\",\n";
    json += "  \"entities\": " + std::to_string(entities) + ",\n";
    json += "  \"avgEntities\": " + number((double)g_measure.entitiesSum / frames, 2) + ",\n";
    json += "  \"peakEntities\": " + std::to_string(g_measure.peakEntities) + ",\n";
    json += "  \"components\": " + std::to_string(components) + ",\n";
    json += "  \"avgComponents\": " + number((double)g_measure.componentsSum / frames, 2) + ",\n";
    json += "  \"peakComponents\": " + std::to_string(g_measure.peakComponents) + ",\n";
    json += "  \"registeredComponents\": " + std::to_string(g_measure.registeredComponents) + ",\n";
    json += "  \"entityIdLimit\": " + std::to_string(EntityManager::maxStoredEntity()) + ",\n";
    json += "  \"entityStorageSlots\": " + std::to_string(g_measure.entityStorageSlots) + ",\n";
    json += "  \"entityStorageCapacity\": " + std::to_string(g_measure.entityStorageCapacity) + ",\n";
    json += "  \"entityStorageBytes\": " + std::to_string(g_measure.entityStorageCapacity * sizeof(EntityMetadata)) + ",\n";
    json += "  \"entityFreeList\": " + std::to_string(g_measure.entityFreeList) + ",\n";
    json += "  \"componentSignatureLimit\": 64,\n";
    json += "  \"componentRegistrySlots\": " + std::to_string(g_measure.componentRegistrySlots) + ",\n";
    json += "  \"componentRegistryCapacity\": " + std::to_string(g_measure.componentRegistryCapacity) + ",\n";
    json += "  \"frames\": " + std::to_string(g_measure.total) + ",\n";
    json += "  \"warmupFrames\": " + std::to_string(g_measure.warmup) + ",\n";
    json += "  \"measuredFrames\": " + std::to_string(g_measure.measured) + ",\n";
    json += "  \"elapsedSeconds\": " + number(elapsed, 2) + ",\n";
    json += "  \"wallFps\": " + number(wallFps, 2) + ",\n";
    json += "  \"avgFps\": " + number(avgFps, 2) + ",\n";
    json += "  \"minFps\": " + number(g_measure.measured > 0 ? g_measure.fpsMin : 0.0, 2) + ",\n";
    json += "  \"maxFps\": " + number(g_measure.fpsMax, 2) + ",\n";
    json += "  \"avgMs\": " + number(wallFps > 0.0 ? 1000.0 / wallFps : 0.0) + ",\n";
    json += "  \"avgDrawCalls\": " + number((double)g_measure.drawCalls / frames, 2) + ",\n";
    json += "  \"avgInstances\": " + number((double)g_measure.instances / frames, 2) + ",\n";
    json += "  \"avgTriangles\": " + number((double)g_measure.triangles / frames, 1) + ",\n";
    json += "  \"meshLoadsPerFrame\": " + number((double)g_measure.meshLoads / frames, 2) + ",\n";
    json += "  \"meshDropsPerFrame\": " + number((double)g_measure.meshDrops / frames, 2) + ",\n";
    json += "  \"avgResidentBytes\": " + number((double)g_measure.residentBytesSum / frames, 0) + ",\n";
    json += "  \"peakResidentBytes\": " + std::to_string(g_measure.peakResidentBytes) + ",\n";
    json += "  \"avgPrivateBytes\": " + number((double)g_measure.privateBytesSum / frames, 0) + ",\n";
    json += "  \"peakPrivateBytes\": " + std::to_string(g_measure.peakPrivateBytes) + ",\n";
    json += "  \"heapTrackingAvailable\": " + std::string(g_measure.heapTrackingAvailable ? "true" : "false") + ",\n";
    json += "  \"avgActiveHeapAllocations\": " + number((double)g_measure.activeHeapAllocationsSum / frames, 0) + ",\n";
    json += "  \"peakActiveHeapAllocations\": " + std::to_string(g_measure.peakActiveHeapAllocations) + ",\n";
    json += "  \"avgActiveHeapBytes\": " + number((double)g_measure.activeHeapBytesSum / frames, 0) + ",\n";
    json += "  \"peakActiveHeapBytes\": " + std::to_string(g_measure.peakActiveHeapBytes) + ",\n";
    json += "  \"gpuStatsAvailable\": " + std::string(g_measure.gpuStatsAvailable ? "true" : "false") + ",\n";
    json += "  \"avgGpuDrawCalls\": " + number((double)g_measure.gpuDrawCalls / frames, 2) + ",\n";
    json += "  \"avgGpuPasses\": " + number((double)g_measure.gpuPasses / frames, 2) + ",\n";
    json += "  \"avgGpuUpdates\": " + number((double)g_measure.gpuUpdates / frames, 2) + ",\n";
    json += "  \"avgGpuAliveBuffers\": " + number((double)g_measure.gpuAliveBuffers / frames, 0) + ",\n";
    json += "  \"avgGpuAliveImages\": " + number((double)g_measure.gpuAliveImages / frames, 0) + ",\n";
    json += "  \"avgGpuAliveSamplers\": " + number((double)g_measure.gpuAliveSamplers / frames, 0) + ",\n";
    json += "  \"avgGpuAliveShaders\": " + number((double)g_measure.gpuAliveShaders / frames, 0) + ",\n";
    json += "  \"avgGpuAlivePipelines\": " + number((double)g_measure.gpuAlivePipelines / frames, 0) + ",\n";
    json += "  \"avgGpuAliveViews\": " + number((double)g_measure.gpuAliveViews / frames, 0) + ",\n";
    json += "  \"fixedUpdateMs\": " + number(g_measure.fixedMs / frames) + ",\n";
    json += "  \"updateMs\": " + number(g_measure.updateMs / frames) + ",\n";
    json += "  \"drawSceneMs\": " + number(g_measure.drawMs / frames) + ",\n";
    json += "  \"commitMs\": " + number(g_measure.commitMs / frames) + ",\n";
    json += "  \"engineFrameMs\": " + number(g_measure.frameMs / frames) + ",\n";
    json += "  \"scriptMs\": " + number(g_measure.scriptMs / frames) + ",\n";
    json += "  \"scriptCallsPerFrame\": " + number((double)g_measure.scriptCalls / frames, 2) + ",\n";
    json += "  \"scriptErrorsPerFrame\": " + number((double)g_measure.scriptErrors / frames, 2) + ",\n";
    json += "  \"slowestScripts\": [\n" + slowestScriptsJson(8) + "\n  ],\n";
    // Everything the loop spends outside its own phases: present, wait, OS pump.
    json += "  \"outsideLoopMs\": " + number(
        wallFps > 0.0 ? 1000.0 / wallFps - g_measure.frameMs / frames : 0.0) + ",\n";
    json += "  \"slowestSystems\": [\n" + slowestSystemsJson(8) + "\n  ]\n";
    json += "}\n";
    return json;
}

void recordMeasuredFrame() {
    if (g_measure.measured == 0)
        g_measure.measuring = std::chrono::steady_clock::now();

    const Engine::FrameStats& stats = Engine::getFrameStats();
    const double fps = Engine::getFramerate();
    g_measure.measured++;
    g_measure.fpsSum += fps;
    if (fps < g_measure.fpsMin) g_measure.fpsMin = fps;
    if (fps > g_measure.fpsMax) g_measure.fpsMax = fps;
    g_measure.fixedMs += stats.fixedUpdateMs;
    g_measure.updateMs += stats.updateMs;
    g_measure.drawMs += stats.drawSceneMs;
    g_measure.commitMs += stats.drawCommitMs;
    g_measure.frameMs += stats.frameMs;
    g_measure.scriptMs += stats.scriptMs;
    g_measure.scriptCalls += stats.scriptCalls;
    g_measure.scriptErrors += stats.scriptErrors;
    for (int i = 0; i < stats.scriptFunctionCount; i++)
        addMeasuredScript(stats.scriptFunctions[i]);
    g_measure.drawCalls += stats.drawCalls;
    g_measure.instances += stats.instances;
    g_measure.triangles += stats.triangles;
    const size_t entities = Engine::getMainScene() ? Engine::getMainScene()->getEntityCount() : 0;
    const size_t components = Engine::getMainScene() ? Engine::getMainScene()->getComponentInstanceCount() : 0;
    g_measure.entitiesSum += entities;
    g_measure.peakEntities = std::max(g_measure.peakEntities, (unsigned long long)entities);
    g_measure.componentsSum += components;
    g_measure.peakComponents = std::max(g_measure.peakComponents, (unsigned long long)components);
    g_measure.registeredComponents = Engine::getMainScene() ?
        Engine::getMainScene()->getRegisteredComponentCount() : 0;
    if (Engine::getMainScene()) {
        g_measure.entityStorageSlots = Engine::getMainScene()->getEntityStorageSlotCount();
        g_measure.entityStorageCapacity = Engine::getMainScene()->getEntityStorageCapacity();
        g_measure.entityFreeList = Engine::getMainScene()->getEntityFreeListSize();
        g_measure.componentRegistrySlots = Engine::getMainScene()->getComponentRegistrySlotCount();
        g_measure.componentRegistryCapacity = Engine::getMainScene()->getComponentRegistryCapacity();
    }
    g_measure.meshLoads += stats.meshLoads;
    g_measure.meshDrops += stats.meshDrops;
    const uint64_t residentBytes = processResidentBytes();
    g_measure.residentBytesSum += residentBytes;
    g_measure.peakResidentBytes = std::max(g_measure.peakResidentBytes, residentBytes);
    const uint64_t privateBytes = processPrivateBytes();
    g_measure.privateBytesSum += privateBytes;
    g_measure.peakPrivateBytes = std::max(g_measure.peakPrivateBytes, privateBytes);
    const HeapSample heap = processHeapSample();
    if (heap.available) {
        g_measure.heapTrackingAvailable = true;
        g_measure.activeHeapAllocationsSum += heap.allocations;
        g_measure.peakActiveHeapAllocations = std::max(g_measure.peakActiveHeapAllocations, heap.allocations);
        g_measure.activeHeapBytesSum += heap.bytes;
        g_measure.peakActiveHeapBytes = std::max(g_measure.peakActiveHeapBytes, heap.bytes);
    }
    // A platform view can be marked loaded while the backend setup failed or is
    // already tearing down. sg_query_stats() asserts in both cases, so keep the
    // profiling runner alive and report GPU data only for a valid Sokol context.
    if (Engine::isViewLoaded() && sg_isvalid()) {
        const sg_stats gpu = sg_query_stats();
        const sg_frame_stats& frame = gpu.cur_frame;
        g_measure.gpuStatsAvailable = true;
        g_measure.gpuDrawCalls += frame.num_draw + frame.num_draw_ex;
        g_measure.gpuPasses += frame.num_passes;
        g_measure.gpuUpdates += frame.num_update_buffer + frame.num_append_buffer + frame.num_update_image;
        g_measure.gpuAliveBuffers += gpu.total.buffers.alive;
        g_measure.gpuAliveImages += gpu.total.images.alive;
        g_measure.gpuAliveSamplers += gpu.total.samplers.alive;
        g_measure.gpuAliveShaders += gpu.total.shaders.alive;
        g_measure.gpuAlivePipelines += gpu.total.pipelines.alive;
        g_measure.gpuAliveViews += gpu.total.views.alive;
    }
    for (int i = 0; i < stats.timedSystemCount; i++) {
        const Engine::FrameStats::SystemTime& timed = stats.timedSystems[i];
        addMeasuredSystem(timed.name, timed.fixedMs + timed.updateMs + timed.drawMs);
    }
}

void emitMeasureReport(const std::string& label) {
    const std::string report = buildReport(label);
    std::fputs(report.c_str(), stdout);
    std::fflush(stdout);
    if (!g_options.statsOut.empty()) {
        std::FILE* file = std::fopen(g_options.statsOut.string().c_str(), "wb");
        if (file) { std::fputs(report.c_str(), file); std::fclose(file); }
        else say(0, "error: failed to write %s", g_options.statsOut.string().c_str());
    }
}

void subscribeMeasure(const std::string& label) {
    g_measure.started = std::chrono::steady_clock::now();
    g_measure.measuring = g_measure.started;

    Engine::onPostDraw.add("quantum-engine-measure", [label]() {
        if (g_measure.done) return;
        g_measure.frame++;

        // FrameStats is published when a frame finishes, so this reads the last full
        // frame rather than the one still drawing. One frame of lag, and every field
        // of it present - which is what the editor benchmark could not promise while
        // the counters were sampled mid-frame.
        if (g_measure.frame <= g_measure.warmup) return;
        recordMeasuredFrame();

        if (g_measure.measured < g_measure.total) return;
        g_measure.done = true;

        emitMeasureReport(label);
        // WM_CLOSE through the same path a user clicking the X takes, so the
        // platform shutdown and engine teardown still run in order.
        System::instance().quit();
    });
}

int runHeadlessMeasurement(int argc, char** argv, const std::string& label) {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    if (g_options.frames <= 0) {
        say(0, "error: --headless requires --frames N");
        return 2;
    }

    say(1, "quantum-engine: headless init");
#if defined(_WIN32)
    Engine::systemInit(argc, argv, new SystemWin());
#else
    Engine::systemInit(argc, argv, new HeadlessSystem());
#endif
    say(1, "quantum-engine: headless init complete");
    if (g_options.scriptReloadSmoke) {
        Engine::systemUpdate();
        g_scriptReloadSmokeFailed = !finishScriptReloadSmoke();
        say(g_scriptReloadSmokeFailed ? 0 : 1,
            "quantum-engine: Lua script reload smoke %s",
            g_scriptReloadSmokeFailed ? "FAILED" : "passed");
    }
    const int totalFrames = g_options.warmupFrames + g_options.frames;
    for (int frame = 0; frame < totalFrames; frame++) {
        say(2, "quantum-engine: headless frame %d", frame + 1);
        Engine::systemUpdate();
        serviceRemoteDebugger();
        if (frame < g_options.warmupFrames) continue;
        g_measure.frame++;
        recordMeasuredFrame();
    }
    say(1, "quantum-engine: headless frames complete");
    emitMeasureReport(label);
    say(1, "quantum-engine: headless view teardown");
    Engine::systemViewDestroyed();
    say(1, "quantum-engine: headless engine shutdown");
    Engine::systemShutdown();
    say(1, "quantum-engine: headless exit");
    return (g_audioProbeFailed || g_scriptSmokeFailed || g_graphSmokeFailed
        || g_graphProjectSmokeFailed) ? 1 : 0;
#else
    (void)argc;
    (void)argv;
    (void)label;
    say(0, "error: --headless profiling is not implemented for this platform yet");
    return 2;
#endif
}

// ---- argument parsing ------------------------------------------------------

bool parseInto(const char* value, int& out) {
    if (!value || !*value) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) return false;
    out = (int)parsed;
    return true;
}

bool parseWindow(const char* value, int& w, int& h) {
    if (!value) return false;
    const std::string text(value);
    const size_t sep = text.find_first_of("xX");
    if (sep == std::string::npos) return false;
    return parseInto(text.substr(0, sep).c_str(), w) &&
           parseInto(text.substr(sep + 1).c_str(), h) && w > 0 && h > 0;
}

void printUsage(const char* argv0) {
    std::printf(
        "usage: %s [--project <path-to-project.yaml-or-export-dir>] [--scene <name>]\n"
        "          [--window WxH] [--fullscreen] [--headless] [--log-level N]\n"
        "          [--window-title TITLE]\n"
        "          [--network-server PORT | --network-client HOST PORT]\n"
        "          [--network-relay-server HOST PORT ROOM | --network-relay-client HOST PORT ROOM]\n"
        "          [--network-token TOKEN | --network-token-env NAME]\n"
        "          [--network-relay-token TOKEN | --network-relay-token-env NAME]\n"
        "          [--debug-port PORT | --debug-token-env NAME]\n"
        "          [--frames N] [--warmup N] [--name LABEL] [--stats-out PATH]\n"
        "          [--screenshot PATH] [--capture-frame N]\n"
        "\n"
        "  --project   Project or exported-game directory. Its assets/, lua/ and\n"
        "              shaders/ become the resource roots for the run.\n"
        "  --scene     Lua entry script (name.lua) inside the project; defaults to\n"
        "              main.lua. Without a runnable project entry, the built-in\n"
        "              demo scene is launched instead.\n"
        "  --window    Initial window client size, e.g. 1280x720.\n"
        "  --window-title TITLE  Override the runtime window title.\n"
        "  --fullscreen  Start in fullscreen.\n"
        "  --headless   Run update frames without creating a window or graphics context.\n"
        "  --script-probe  Install a tiny Lua onUpdate callback for profiling smoke tests.\n"
        "  --script-smoke  Execute a headless Lua/ECS API smoke test and fail on error.\n"
        "  --graph-smoke  Execute a built-in visual graph and verify its variable output.\n"
        "  --graph-project-smoke  Compile/load a temporary .gdat through asset:// and run it via GraphBinding (requires --project).\n"
        "  --script-reload-smoke  Reload a Lua module in place and verify state/event preservation.\n"
        "  --script-hot-reload  Poll Lua source mtimes and reload changed modules (development mode).\n"
        "  --render-feature NAME  Enable a render path for visual validation; repeatable:\n"
        "                         taa, motionblur, hdr, lut, ssgi, ssr, ssrfallback, atmosphere,\n"
        "                         shadows, clustered, decals.\n"
        "  --render-lut PATH  LUT path used by --render-feature lut.\n"
        "  --log-level 0 quiet, 1 normal (default), 2 verbose.\n"
        "  --network-server PORT  Start the loaded scene as the authoritative peer.\n"
        "  --network-client HOST PORT  Connect the loaded scene to a server.\n"
        "  --network-relay-server HOST PORT ROOM  Register a server through an authenticated UDP relay.\n"
        "  --network-relay-client HOST PORT ROOM  Join a server through an authenticated UDP relay.\n"
        "  --network-token TOKEN  Optional pre-shared HMAC-SHA256 token for all UDP datagrams.\n"
        "  --network-token-env NAME\n"
        "                         Read the pre-shared token from an environment variable.\n"
        "  --network-relay-token TOKEN  HMAC token for relay room control (defaults to network token).\n"
        "  --network-relay-token-env NAME  Read the relay control token from an environment variable.\n"
        "  --debug-port PORT     Expose a loopback Lua debugger (0 selects a free port).\n"
        "  --debug-token TOKEN   Require `auth TOKEN` before accepting debugger commands.\n"
        "  --debug-token-env NAME\n"
        "                         Read the debugger token from an environment variable.\n"
        "\n"
        "Measurement run (exits by itself once the frames are counted):\n"
        "  --frames N    Average Engine::FrameStats over N presented frames.\n"
        "  --warmup N    Drop the first N frames; defaults to a quarter of --frames.\n"
        "  --name LABEL  Label for the report.\n"
        "  --stats-out PATH  Also write the JSON report to PATH.\n"
        "\n"
        "Image capture (the only way to see a headless run):\n"
        "  --screenshot PATH  Write the picked frame as a PNG, at the window size.\n"
        "                     Alone, it also ends the run on that frame.\n"
        "  --capture-frame N  1-based frame to capture; defaults to 60, so async\n"
        "                     loads and shader compiles have settled.\n"
        "  --compare-screenshot PATH  Compare the captured PNG with a baseline.\n"
        "  --compare-threshold N  Mean RGB error threshold in [0,1] (default 0.01).\n"
        "  --compare-diff PATH  Write a false-colour absolute-difference PNG.\n"
        "  --compare-report PATH  Write comparison metrics as JSON.\n"
        "  --audio-probe WAV OGG  Load, play, crossfade and stop both files in the initialized runtime.\n",
        argv0);
}

bool parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const std::string arg(argv[i]);
        const auto next = [&](std::string& store) -> bool {
            if (i + 1 >= argc) {
                say(0, "error: %s requires a value", arg.c_str());
                return false;
            }
            store = argv[++i];
            return true;
        };

        if (arg == "--project") {
            std::string value;
            if (!next(value)) return false;
            g_options.projectPath = value;
        } else if (arg == "--scene") {
            if (!next(g_options.sceneName)) return false;
        } else if (arg == "--window") {
            std::string value;
            if (!next(value)) return false;
            if (!parseWindow(value.c_str(), g_options.windowWidth, g_options.windowHeight)) {
                say(0, "error: bad --window value: %s (expected WxH)", value.c_str());
                return false;
            }
        } else if (arg == "--window-title") {
            if (!next(g_options.title)) return false;
        } else if (arg == "--fullscreen") {
            g_options.fullscreen = true;
        } else if (arg == "--headless") {
            g_options.headless = true;
        } else if (arg == "--script-probe") {
            g_options.scriptProbe = true;
        } else if (arg == "--script-smoke") {
            g_options.scriptSmoke = true;
        } else if (arg == "--graph-smoke") {
            g_options.graphSmoke = true;
        } else if (arg == "--graph-project-smoke") {
            g_options.graphProjectSmoke = true;
        } else if (arg == "--script-reload-smoke") {
            g_options.scriptReloadSmoke = true;
        } else if (arg == "--script-hot-reload") {
            g_options.scriptHotReload = true;
        } else if (arg == "--render-feature") {
            std::string value;
            if (!next(value)) return false;
            g_options.renderFeatures.push_back(value);
        } else if (arg == "--render-lut") {
            if (!next(g_options.renderLut)) return false;
        } else if (arg == "--network-server") {
            if (!parseInto(i + 1 < argc ? argv[i + 1] : nullptr, g_options.networkPort)) {
                say(0, "error: --network-server needs a port");
                return false;
            }
            i++;
            if (g_options.networkPort < 1 || g_options.networkPort > 65535) {
                say(0, "error: network port must be between 1 and 65535");
                return false;
            }
            g_options.networkRole = Options::NetworkRole::Server;
            g_options.networkRelay = false;
        } else if (arg == "--network-client") {
            if (!next(g_options.networkAddress)) return false;
            if (!parseInto(i + 1 < argc ? argv[i + 1] : nullptr, g_options.networkPort)) {
                say(0, "error: --network-client needs a port after the host");
                return false;
            }
            i++;
            if (g_options.networkPort < 1 || g_options.networkPort > 65535) {
                say(0, "error: network port must be between 1 and 65535");
                return false;
            }
            g_options.networkRole = Options::NetworkRole::Client;
            g_options.networkRelay = false;
        } else if (arg == "--network-relay-server") {
            if (!next(g_options.relayAddress)
                    || !parseInto(i + 1 < argc ? argv[i + 1] : nullptr, g_options.relayPort)) {
                say(0, "error: --network-relay-server needs HOST PORT ROOM");
                return false;
            }
            i++;
            if (!next(g_options.relayRoom) || g_options.relayPort < 1
                    || g_options.relayPort > 65535 || g_options.relayRoom.empty()) {
                say(0, "error: invalid relay server endpoint or room");
                return false;
            }
            g_options.networkRole = Options::NetworkRole::Server;
            g_options.networkRelay = true;
        } else if (arg == "--network-relay-client") {
            if (!next(g_options.relayAddress)
                    || !parseInto(i + 1 < argc ? argv[i + 1] : nullptr, g_options.relayPort)) {
                say(0, "error: --network-relay-client needs HOST PORT ROOM");
                return false;
            }
            i++;
            if (!next(g_options.relayRoom) || g_options.relayPort < 1
                    || g_options.relayPort > 65535 || g_options.relayRoom.empty()) {
                say(0, "error: invalid relay client endpoint or room");
                return false;
            }
            g_options.networkRole = Options::NetworkRole::Client;
            g_options.networkRelay = true;
        } else if (arg == "--network-token") {
            if (!next(g_options.networkToken)) {
                say(0, "error: --network-token needs a token");
                return false;
            }
        } else if (arg == "--network-token-env") {
            std::string environmentName;
            if (!next(environmentName)) {
                say(0, "error: --network-token-env needs a variable name");
                return false;
            }
            const char* token = std::getenv(environmentName.c_str());
            if (!token) {
                say(0, "error: network token environment variable is not set: %s",
                    environmentName.c_str());
                return false;
            }
            g_options.networkToken = token;
        } else if (arg == "--network-relay-token") {
            if (!next(g_options.relayToken)) {
                say(0, "error: --network-relay-token needs a token");
                return false;
            }
        } else if (arg == "--network-relay-token-env") {
            std::string environmentName;
            if (!next(environmentName)) {
                say(0, "error: --network-relay-token-env needs a variable name");
                return false;
            }
            const char* token = std::getenv(environmentName.c_str());
            if (!token) {
                say(0, "error: relay token environment variable is not set: %s",
                    environmentName.c_str());
                return false;
            }
            g_options.relayToken = token;
        } else if (arg == "--debug-port") {
            int port = 0;
            if (!parseInto(i + 1 < argc ? argv[i + 1] : nullptr, port)) {
                say(0, "error: --debug-port needs a port");
                return false;
            }
            i++;
            if (port < 0 || port > 65535) {
                say(0, "error: debug port must be between 0 and 65535");
                return false;
            }
            g_options.debugPort = static_cast<std::uint16_t>(port);
            g_options.debugPortEnabled = true;
        } else if (arg == "--debug-token") {
            if (!next(g_options.debugToken)) {
                say(0, "error: --debug-token needs a token");
                return false;
            }
        } else if (arg == "--debug-token-env") {
            std::string environmentName;
            if (!next(environmentName)) {
                say(0, "error: --debug-token-env needs a variable name");
                return false;
            }
            const char* token = std::getenv(environmentName.c_str());
            if (!token) {
                say(0, "error: debugger token environment variable is not set: %s",
                    environmentName.c_str());
                return false;
            }
            g_options.debugToken = token;
        } else if (arg == "--log-level") {
            if (!parseInto(i + 1 < argc ? argv[i + 1] : nullptr, g_options.logLevel)) return false;
            i++;
            if (g_options.logLevel < 0 || g_options.logLevel > 2) {
                say(0, "error: --log-level must be 0, 1 or 2");
                return false;
            }
        } else if (arg == "--frames") {
            if (!parseInto(i + 1 < argc ? argv[i + 1] : nullptr, g_options.frames)) {
                say(0, "error: --frames needs a frame count");
                return false;
            }
            i++;
            if (g_options.frames <= 0) {
                say(0, "error: --frames must be greater than 0");
                return false;
            }
        } else if (arg == "--warmup") {
            if (!parseInto(i + 1 < argc ? argv[i + 1] : nullptr, g_options.warmupFrames)) {
                say(0, "error: --warmup needs a frame count");
                return false;
            }
            i++;
            if (g_options.warmupFrames < 0) {
                say(0, "error: --warmup must be 0 or greater");
                return false;
            }
        } else if (arg == "--name") {
            if (!next(g_options.label)) return false;
        } else if (arg == "--stats-out") {
            std::string value;
            if (!next(value)) return false;
            g_options.statsOut = value;
        } else if (arg == "--screenshot") {
            std::string value;
            if (!next(value)) return false;
            g_options.screenshot = value;
        } else if (arg == "--compare-screenshot") {
            std::string value;
            if (!next(value)) return false;
            g_options.compareScreenshot = value;
        } else if (arg == "--compare-threshold") {
            std::string value;
            if (!next(value)) return false;
            char* end = nullptr;
            const float threshold = std::strtof(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0' || threshold < 0.0f || threshold > 1.0f) {
                say(0, "error: --compare-threshold must be between 0 and 1");
                return false;
            }
            g_options.compareThreshold = threshold;
        } else if (arg == "--compare-diff") {
            std::string value;
            if (!next(value)) return false;
            g_options.compareDiff = value;
        } else if (arg == "--compare-report") {
            std::string value;
            if (!next(value)) return false;
            g_options.compareReport = value;
        } else if (arg == "--capture-frame") {
            if (!parseInto(i + 1 < argc ? argv[i + 1] : nullptr, g_options.captureFrame)) {
                say(0, "error: --capture-frame needs a frame number");
                return false;
            }
            i++;
            if (g_options.captureFrame < 1) {
                say(0, "error: --capture-frame must be 1 or greater");
                return false;
            }
        } else if (arg == "--audio-probe") {
            if (!next(g_options.audioProbeWav)) return false;
            if (i + 1 >= argc) {
                say(0, "error: --audio-probe needs WAV and OGG paths");
                return false;
            }
            g_options.audioProbeOgg = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            g_helpRequested = true;
            return true;
        } else {
            say(0, "error: unknown argument: %s", arg.c_str());
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (!parseArgs(argc, argv)) return 2;
    if (g_helpRequested) return 0;

    LuaBinding::setLuaHotReloadEnabled(g_options.scriptHotReload);

    std::error_code ec;
    g_originalCwd = std::filesystem::current_path(ec);
    if (!g_options.statsOut.empty() && g_options.statsOut.is_relative())
        g_options.statsOut = g_originalCwd / g_options.statsOut;

    if (!g_options.projectPath.empty()) {
        const std::filesystem::path projectDir = resolveAndEnterProject(g_options.projectPath);
        if (projectDir.empty()) return 1;
        g_options.projectDir = projectDir;
        g_options.luaDir = readProjectField(projectDir, "luaDir");
        if (g_options.title.empty()) g_options.title = readProjectName(projectDir);
        say(1, "quantum-engine: project %s", projectDir.string().c_str());
    } else {
        if (g_options.title.empty()) g_options.title = "quantum-engine";
        say(1, "quantum-engine: no --project, running the built-in demo scene");
    }

    if (g_options.debugPortEnabled) {
        g_remoteDebugServer = std::make_unique<RemoteDebugServer>();
        std::string error;
        if (!g_remoteDebugServer->open(g_options.debugPort, g_options.debugToken, error)) {
            say(0, "error: %s", error.c_str());
            g_remoteDebugServer.reset();
            return 1;
        }
        say(1, "quantum-engine: remote Lua debugger listening on 127.0.0.1:%u auth=%s",
            static_cast<unsigned>(g_remoteDebugServer->port()),
            g_options.debugToken.empty() ? "off" : "on");
    }

    subscribeWindowOptions();

    // A capture run has no reason to keep going after the picture is written, so --screenshot on
    // its own also sets the frame count and drops the warmup: the run ends on the captured frame.
    if (!g_options.screenshot.empty() && g_options.frames == 0) {
        g_options.frames = g_options.captureFrame;
        g_options.warmupFrames = 0;
    }

    if (g_options.frames > 0) {
        g_measure.total = g_options.frames;
        g_measure.warmup = g_options.warmupFrames >= 0
            ? g_options.warmupFrames : g_options.frames / 4;
        std::string label = g_options.label;
        if (label.empty()) label = g_options.title.empty() ? "quantum-engine" : g_options.title;
        if (!g_options.headless) subscribeMeasure(label);
        say(1, "quantum-engine: measuring %d frames after %d warmup", g_measure.total, g_measure.warmup);
    }

    if (g_options.headless) {
        std::string label = g_options.label;
        if (label.empty()) label = g_options.title.empty() ? "quantum-engine" : g_options.title;
        g_options.warmupFrames = g_options.warmupFrames >= 0
            ? g_options.warmupFrames : g_options.frames / 4;
        const int result = runHeadlessMeasurement(argc, argv, label);
        if (!g_options.compareScreenshot.empty()) {
            say(0, "error: screenshot comparison requires a real graphics run; --headless has no framebuffer");
            return result == 0 ? 2 : result;
        }
        return result;
    }

    if (!g_options.screenshot.empty()) {
        namespace fs = std::filesystem;
        // The project directory becomes the working directory before the engine starts, so a
        // relative --screenshot path is read against the shell the runner was called from.
        fs::path target = g_options.screenshot;
        if (target.is_relative()) target = g_originalCwd / target;
        target = target.lexically_normal();
        std::error_code made;
        fs::create_directories(target.parent_path(), made);

        // The countdown runs once per finished frame, so frame N is reached with N-1 still to serve.
        FrameCapture::request(target.string(), g_options.captureFrame - 1);
        say(1, "quantum-engine: screenshot %s on frame %d", target.string().c_str(), g_options.captureFrame);
    }

    // The platform init owns the frame loop and shuts the engine down on
    // window close (QuantumWin::init, engine/platform/win/QuantumWin.cpp:304-376).
#if defined(_WIN32)
    const int result = QuantumWin::init(argc, argv);
#elif defined(__APPLE__)
    const int result = QuantumMac::init(argc, argv);
#else
    const int result = QuantumLinux::init(argc, argv);
#endif

    say(1, "quantum-engine: exited with %d", result);
    if (result != 0 || g_options.compareScreenshot.empty()) return result;
    if (g_options.screenshot.empty()) {
        say(0, "error: --compare-screenshot requires --screenshot");
        return 2;
    }
    std::filesystem::path current = g_options.screenshot;
    std::filesystem::path baseline = g_options.compareScreenshot;
    std::filesystem::path diff = g_options.compareDiff;
    std::filesystem::path report = g_options.compareReport;
    if (current.is_relative()) current = g_originalCwd / current;
    if (baseline.is_relative()) baseline = g_originalCwd / baseline;
    if (!diff.empty() && diff.is_relative()) diff = g_originalCwd / diff;
    if (!report.empty() && report.is_relative()) report = g_originalCwd / report;
    return compareScreenshots(current.lexically_normal(), baseline.lexically_normal(),
        g_options.compareThreshold, diff.lexically_normal(), report.lexically_normal()) ? 0 : 1;
}
