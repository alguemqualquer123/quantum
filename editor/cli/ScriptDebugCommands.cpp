// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ScriptDebugCommands.h"

#include "graph/GraphAsset.h"
#include "LuaDebugger.h"
#include "lua.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    pragma comment(lib, "Ws2_32.lib")
#else
#    include <arpa/inet.h>
#    include <cerrno>
#    include <netinet/in.h>
#    include <sys/select.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

namespace quantum::editor::cli {

namespace {

#if defined(_WIN32)
using TcpSocket = SOCKET;
constexpr TcpSocket invalidTcpSocket = INVALID_SOCKET;
#else
using TcpSocket = int;
constexpr TcpSocket invalidTcpSocket = -1;
#endif

void closeTcpSocket(TcpSocket socket) {
    if (socket == invalidTcpSocket) return;
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

int tcpSocketError() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

class TcpSocketRuntime {
public:
    bool start(std::string& error) {
#if defined(_WIN32)
        WSADATA data{};
        const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            error = "WSAStartup failed (" + std::to_string(result) + ")";
            return false;
        }
        initialized_ = true;
#else
        (void)error;
#endif
        return true;
    }

    ~TcpSocketRuntime() {
#if defined(_WIN32)
        if (initialized_) ::WSACleanup();
#endif
    }

private:
#if defined(_WIN32)
    bool initialized_ = false;
#endif
};

class TcpLoopbackTransport {
public:
    TcpLoopbackTransport(std::uint16_t port, int timeoutMs)
        : timeoutMs_(timeoutMs) {
        open(port);
    }

    ~TcpLoopbackTransport() {
        closeClient();
        closeListener();
    }

    TcpLoopbackTransport(const TcpLoopbackTransport&) = delete;
    TcpLoopbackTransport& operator=(const TcpLoopbackTransport&) = delete;

    bool isOpen() const { return listener_ != invalidTcpSocket; }
    bool hasClient() const { return client_ != invalidTcpSocket; }
    bool isTimedOut() const { return timedOut_; }
    std::uint16_t port() const { return port_; }
    const std::string& error() const { return error_; }

    bool acceptClient() {
        if (!isOpen()) return false;
        const int waitResult = waitFor(listener_, false);
        if (waitResult <= 0) {
            timedOut_ = waitResult == 0;
            if (waitResult < 0) setError("TCP accept wait failed");
            return false;
        }

#if defined(_WIN32)
        int addressLength = static_cast<int>(sizeof(sockaddr_in));
#else
        socklen_t addressLength = static_cast<socklen_t>(sizeof(sockaddr_in));
#endif
        sockaddr_in address{};
        client_ = ::accept(listener_, reinterpret_cast<sockaddr*>(&address), &addressLength);
        if (client_ == invalidTcpSocket) {
            setError("TCP accept failed");
            return false;
        }
        closeListener();
        return true;
    }

    bool readLine(std::string& line) {
        line.clear();
        if (!hasClient()) return false;

        for (;;) {
            const std::size_t newline = receiveBuffer_.find('\n');
            if (newline != std::string::npos) {
                line = receiveBuffer_.substr(0, newline);
                receiveBuffer_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            if (receiveBuffer_.size() > maxLineLength) {
                setError("TCP debug command exceeds 65536 bytes");
                return false;
            }

            const int waitResult = waitFor(client_, false);
            if (waitResult == 0) {
                timedOut_ = true;
                setError("TCP debug session timed out");
                return false;
            }
            if (waitResult < 0) {
                setError("TCP receive wait failed");
                return false;
            }

            char chunk[4096];
            const auto received = ::recv(client_, chunk, static_cast<int>(sizeof(chunk)), 0);
            if (received == 0) {
                if (!receiveBuffer_.empty()) {
                    line = std::move(receiveBuffer_);
                    receiveBuffer_.clear();
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    return true;
                }
                setError("TCP debug client closed the connection");
                return false;
            }
            if (received < 0) {
                setError("TCP receive failed");
                return false;
            }
            receiveBuffer_.append(chunk, static_cast<std::size_t>(received));
        }
    }

    bool sendLine(const std::string& line) {
        std::string payload = line;
        if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
        return sendBytes(payload.data(), payload.size());
    }

private:
    static constexpr std::size_t maxLineLength = 64 * 1024;

    void setError(const std::string& prefix) {
        if (!error_.empty()) return;
        error_ = prefix + " (" + std::to_string(tcpSocketError()) + ")";
    }

    void open(std::uint16_t port) {
        if (!runtime_.start(error_)) return;

        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == invalidTcpSocket) {
            setError("TCP socket creation failed");
            return;
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
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1
            || ::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
            || ::listen(listener_, 1) != 0) {
            setError("TCP loopback listener setup failed");
            closeListener();
            return;
        }

#if defined(_WIN32)
        int addressLength = static_cast<int>(sizeof(address));
#else
        socklen_t addressLength = static_cast<socklen_t>(sizeof(address));
#endif
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
            setError("TCP listener port query failed");
            closeListener();
            return;
        }
        port_ = ntohs(address.sin_port);
    }

    int waitFor(TcpSocket socket, bool write) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket, &set);
        timeval timeout{};
        timeout.tv_sec = timeoutMs_ / 1000;
        timeout.tv_usec = (timeoutMs_ % 1000) * 1000;
#if defined(_WIN32)
        const int result = ::select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &timeout);
#else
        const int result = ::select(socket + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &timeout);
#endif
        if (result < 0) return -1;
        return result > 0 ? 1 : 0;
    }

    bool sendBytes(const char* data, std::size_t size) {
        if (!hasClient()) return false;
        std::size_t offset = 0;
        while (offset < size) {
            const int waitResult = waitFor(client_, true);
            if (waitResult == 0) {
                timedOut_ = true;
                setError("TCP send timed out");
                return false;
            }
            if (waitResult < 0) {
                setError("TCP send wait failed");
                return false;
            }
#if defined(MSG_NOSIGNAL)
            constexpr int sendFlags = MSG_NOSIGNAL;
#else
            constexpr int sendFlags = 0;
#endif
            const auto sent = ::send(client_, data + offset,
                                     static_cast<int>(std::min<std::size_t>(size - offset, 4096)),
                                     sendFlags);
            if (sent <= 0) {
                setError("TCP send failed");
                return false;
            }
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }

    void closeClient() {
        closeTcpSocket(client_);
        client_ = invalidTcpSocket;
    }

    void closeListener() {
        closeTcpSocket(listener_);
        listener_ = invalidTcpSocket;
    }

    TcpSocketRuntime runtime_;
    TcpSocket listener_ = invalidTcpSocket;
    TcpSocket client_ = invalidTcpSocket;
    int timeoutMs_ = 30000;
    std::uint16_t port_ = 0;
    bool timedOut_ = false;
    std::string receiveBuffer_;
    std::string error_;
};

class DebugOutput {
public:
    explicit DebugOutput(TcpLoopbackTransport* tcp = nullptr)
        : tcp_(tcp) {}

    void setTransport(TcpLoopbackTransport* tcp) { tcp_ = tcp; }

    bool line(const std::string& value) {
        std::cout << value << '\n';
        std::cout.flush();
        return !tcp_ || tcp_->sendLine(value);
    }

    bool healthy() const { return !tcp_ || (!tcp_->isTimedOut() && tcp_->error().empty()); }

private:
    TcpLoopbackTransport* tcp_ = nullptr;
};

void printUsage(const char* executableName) {
    std::cout
        << "Usage:\n"
        << "  " << executableName << " script debug --file <script.lua> [options]\n"
        << "  " << executableName << " script lint --file <script.lua> [--json] [--strict]\n"
        << "  " << executableName << " script graph-validate --file <graph.yaml>\n"
        << "  " << executableName << " script graph-roundtrip --file <graph.yaml> --out <graph.yaml>\n\n"
        << "Debug options:\n"
        << "  --breakpoint <source:line>  Add a source breakpoint (repeatable).\n"
        << "  --condition <expression>   Condition for the most recent breakpoint.\n"
        << "  --commands <list>           Auto commands separated by ';'.\n"
        << "  --protocol                  Read continue/step/next/out/watch/stack from stdin.\n"
        << "  --tcp-loopback <port>       With --protocol, accept the same line protocol on 127.0.0.1.\n"
        << "  --tcp-timeout-ms <ms>       TCP accept/read/write timeout (default: 30000).\n"
        << "  --json                      Emit one final JSON object instead of text events.\n";
}

bool valueAfter(int argc, char** argv, int& index, std::string& value) {
    if (index + 1 >= argc) return false;
    value = argv[++index];
    return true;
}

bool parseTcpPort(const std::string& text, std::uint16_t& port) {
    try {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(text, &consumed, 10);
        if (consumed != text.size() || parsed > (std::numeric_limits<std::uint16_t>::max)()) return false;
        port = static_cast<std::uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseTcpTimeout(const std::string& text, int& timeoutMs) {
    try {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(text, &consumed, 10);
        if (consumed != text.size() || parsed == 0 || parsed > 3600000UL) return false;
        timeoutMs = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseBreakpoint(const std::string& text, std::string& source, int& line) {
    const size_t separator = text.rfind(':');
    if (separator == std::string::npos) return false;
    source = text.substr(0, separator);
    try {
        line = std::stoi(text.substr(separator + 1));
    } catch (...) {
        return false;
    }
    return !source.empty() && line > 0;
}

struct BreakpointOption {
    std::string specification;
    std::string condition;
};

std::vector<std::string> splitCommands(const std::string& text) {
    std::vector<std::string> result;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = text.find(';', begin);
        const std::string part = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!part.empty()) result.push_back(part);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

std::string trimText(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::string jsonEscape(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20) {
                    result += "\\u00";
                    result.push_back(hex[(character >> 4) & 0x0f]);
                    result.push_back(hex[character & 0x0f]);
                } else {
                    result.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    return result;
}

struct ScriptLintDiagnostic {
    std::string severity;
    std::string code;
    std::string message;
    int line = 0;
    int column = 0;
};

bool isIdentifierCharacter(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
}

size_t longBracketOpenLength(const std::string& source, size_t offset) {
    if (offset >= source.size() || source[offset] != '[') return 0;
    size_t cursor = offset + 1;
    while (cursor < source.size() && source[cursor] == '=') ++cursor;
    if (cursor >= source.size() || source[cursor] != '[') return 0;
    return cursor - offset + 1;
}

size_t skipLongBracket(const std::string& source, size_t offset, size_t openLength) {
    const size_t equalsCount = openLength - 2;
    const std::string close = "]" + std::string(equalsCount, '=') + "]";
    const size_t end = source.find(close, offset + openLength);
    return end == std::string::npos ? source.size() : end + close.size();
}

void lineColumnAt(const std::string& source, size_t offset, int& line, int& column) {
    line = 1;
    column = 1;
    const size_t end = std::min(offset, source.size());
    for (size_t index = 0; index < end; ++index) {
        if (source[index] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
}

void addLintDiagnostic(std::vector<ScriptLintDiagnostic>& diagnostics,
                       const std::string& severity,
                       const std::string& code,
                       const std::string& message,
                       const std::string& source,
                       size_t offset) {
    ScriptLintDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.code = code;
    diagnostic.message = message;
    lineColumnAt(source, offset, diagnostic.line, diagnostic.column);
    diagnostics.push_back(std::move(diagnostic));
}

void collectUnsupportedApiDiagnostics(const std::string& source,
                                      std::vector<ScriptLintDiagnostic>& diagnostics) {
    struct UnsupportedApi {
        const char* expression;
        const char* code;
        const char* message;
    };
    static constexpr UnsupportedApi unsupportedApis[] = {
        {"math.atan2", "QLUA001", "math.atan2 is unavailable in engine Lua; use math.atan(y, x)"},
        {"math.pow", "QLUA002", "math.pow is unavailable in engine Lua; use the ^ operator"},
        {"loadstring", "QLUA003", "loadstring is unavailable in engine Lua; use load"},
        {"setfenv", "QLUA004", "setfenv is unavailable in engine Lua; use lexical _ENV"},
        {"getfenv", "QLUA005", "getfenv is unavailable in engine Lua; use lexical _ENV"},
        {"table.getn", "QLUA006", "table.getn is unavailable in engine Lua; use the # operator"},
        {"table.foreach", "QLUA007", "table.foreach is unavailable in engine Lua; use pairs"},
        {"table.foreachi", "QLUA008", "table.foreachi is unavailable in engine Lua; use ipairs"},
        {"math.cosh", "QLUA009", "math.cosh is unavailable in engine Lua"},
        {"math.sinh", "QLUA010", "math.sinh is unavailable in engine Lua"},
        {"math.tanh", "QLUA011", "math.tanh is unavailable in engine Lua"},
        {"math.frexp", "QLUA012", "math.frexp is unavailable in engine Lua"},
        {"math.ldexp", "QLUA013", "math.ldexp is unavailable in engine Lua"},
        {"math.log10", "QLUA014", "math.log10 is unavailable in engine Lua; use math.log(value, 10)"},
    };

    size_t cursor = 0;
    while (cursor < source.size()) {
        if (source[cursor] == '-' && cursor + 1 < source.size() && source[cursor + 1] == '-') {
            const size_t longLength = longBracketOpenLength(source, cursor + 2);
            if (longLength > 0) {
                cursor = skipLongBracket(source, cursor + 2, longLength);
            } else {
                const size_t newline = source.find('\n', cursor + 2);
                cursor = newline == std::string::npos ? source.size() : newline + 1;
            }
            continue;
        }

        if (source[cursor] == '\'' || source[cursor] == '"') {
            const char quote = source[cursor++];
            while (cursor < source.size()) {
                if (source[cursor] == '\\') {
                    cursor = std::min(cursor + 2, source.size());
                } else if (source[cursor++] == quote) {
                    break;
                }
            }
            continue;
        }

        const size_t longLength = longBracketOpenLength(source, cursor);
        if (longLength > 0) {
            cursor = skipLongBracket(source, cursor, longLength);
            continue;
        }

        bool matched = false;
        for (const UnsupportedApi& api : unsupportedApis) {
            const std::string expression(api.expression);
            if (source.compare(cursor, expression.size(), expression) != 0) continue;
            const bool validBefore = cursor == 0 || !isIdentifierCharacter(source[cursor - 1]);
            const size_t end = cursor + expression.size();
            const bool validAfter = end >= source.size() || !isIdentifierCharacter(source[end]);
            if (!validBefore || !validAfter) continue;
            addLintDiagnostic(diagnostics, "warning", api.code, api.message, source, cursor);
            cursor = end;
            matched = true;
            break;
        }
        if (!matched) ++cursor;
    }
}

void sortLintDiagnostics(std::vector<ScriptLintDiagnostic>& diagnostics) {
    std::sort(diagnostics.begin(), diagnostics.end(), [](const auto& left, const auto& right) {
        if (left.line != right.line) return left.line < right.line;
        if (left.column != right.column) return left.column < right.column;
        if (left.severity != right.severity) return left.severity < right.severity;
        if (left.code != right.code) return left.code < right.code;
        return left.message < right.message;
    });
}

void printJsonLintResult(const std::string& file,
                         bool strict,
                         bool ok,
                         const std::vector<ScriptLintDiagnostic>& diagnostics) {
    size_t errorCount = 0;
    size_t warningCount = 0;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == "error") ++errorCount;
        else if (diagnostic.severity == "warning") ++warningCount;
    }

    std::cout << "{\"command\":\"script lint\",\"file\":\"" << jsonEscape(file)
              << "\",\"strict\":" << (strict ? "true" : "false")
              << ",\"ok\":" << (ok ? "true" : "false")
              << ",\"errorCount\":" << errorCount
              << ",\"warningCount\":" << warningCount
              << ",\"diagnostics\":[";
    for (size_t index = 0; index < diagnostics.size(); ++index) {
        if (index > 0) std::cout << ",";
        const auto& diagnostic = diagnostics[index];
        std::cout << "{\"severity\":\"" << jsonEscape(diagnostic.severity)
                  << "\",\"code\":\"" << jsonEscape(diagnostic.code)
                  << "\",\"line\":" << diagnostic.line
                  << ",\"column\":" << diagnostic.column
                  << ",\"message\":\"" << jsonEscape(diagnostic.message) << "\"}";
    }
    std::cout << "]}\n";
}

void printJsonError(const std::string& reason, const std::string& message);

int runLint(int argc, char** argv) {
    std::string file;
    bool json = false;
    bool strict = false;
    for (int index = 0; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--file") {
            if (!valueAfter(argc, argv, index, file)) {
                if (json) printJsonError("invalid_arguments", "script lint: --file requires a value");
                else std::cerr << "script lint: --file requires a value\n";
                return 2;
            }
        } else if (argument == "--json") {
            json = true;
        } else if (argument == "--strict") {
            strict = true;
        } else if (argument == "--help" || argument == "-h") {
            if (json) printJsonError("help", "Use --file <script.lua> and optional --json or --strict");
            else printUsage("quantum-editor-cmd");
            return 0;
        } else {
            if (json) printJsonError("invalid_arguments", "script lint: unknown option " + argument);
            else std::cerr << "script lint: unknown option " << argument << "\n";
            return 2;
        }
    }
    if (file.empty()) {
        if (json) printJsonError("invalid_arguments", "script lint: --file is required");
        else std::cerr << "script lint: --file is required\n";
        return 2;
    }

    std::ifstream input(file, std::ios::binary);
    if (!input) {
        const std::string message = "Could not open Lua file: " + file;
        if (json) printJsonLintResult(file, strict, false, {{"error", "QLINT000", message, 0, 0}});
        else std::cerr << "script lint: " << message << "\n";
        return 1;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string source = contents.str();
    std::vector<ScriptLintDiagnostic> diagnostics;
    if (source.empty()) {
        addLintDiagnostic(diagnostics, "warning", "QLINT001", "script is empty", source, 0);
    }

    lua_State* lua = luaL_newstate();
    if (!lua) {
        const std::string message = "could not create Lua state";
        if (json) printJsonLintResult(file, strict, false, {{"error", "QLINT002", message, 0, 0}});
        else std::cerr << "script lint: " << message << "\n";
        return 1;
    }
    luaL_openlibs(lua);
    quantum::LuaDebugger debugger(lua);
    if (!source.empty() && !debugger.startString(file, source)) {
        const auto& diagnostic = debugger.getDiagnostic();
        ScriptLintDiagnostic syntax;
        syntax.severity = "error";
        syntax.code = "QLINT100";
        syntax.message = diagnostic.message.empty() ? "Lua syntax error" : diagnostic.message;
        syntax.line = diagnostic.line;
        if (syntax.line <= 0) {
            const size_t marker = syntax.message.find(':');
            if (marker != std::string::npos) {
                size_t digit = marker + 1;
                while (digit < syntax.message.size() && std::isdigit(static_cast<unsigned char>(syntax.message[digit]))) ++digit;
                if (digit > marker + 1) syntax.line = std::stoi(syntax.message.substr(marker + 1, digit - marker - 1));
            }
        }
        diagnostics.push_back(std::move(syntax));
    } else {
        collectUnsupportedApiDiagnostics(source, diagnostics);
    }
    sortLintDiagnostics(diagnostics);
    debugger.shutdown();
    lua_close(lua);

    bool hasError = false;
    bool hasWarning = false;
    for (const auto& diagnostic : diagnostics) {
        hasError = hasError || diagnostic.severity == "error";
        hasWarning = hasWarning || diagnostic.severity == "warning";
        if (!json) {
            std::cout << "script-lint: " << diagnostic.severity << " " << diagnostic.code << " "
                      << file << ":" << diagnostic.line << ":" << diagnostic.column << " "
                      << diagnostic.message << "\n";
        }
    }
    const bool ok = !hasError && (!strict || !hasWarning);
    if (json) {
        printJsonLintResult(file, strict, ok, diagnostics);
    } else {
        std::cout << "script-lint: " << (ok ? "ok" : "failed")
                  << " diagnostics=" << diagnostics.size() << "\n";
    }
    return ok ? 0 : 1;
}

struct JsonWatch {
    std::string expression;
    bool ok = false;
    std::string value;
    std::string error;
};

struct JsonSession {
    std::vector<JsonWatch> watches;
    std::vector<std::string> commandErrors;
    std::string reason;
    std::string diagnosticOverride;
};

void printStack(const quantum::LuaDebugger& debugger, DebugOutput& output) {
    for (const auto& frame : debugger.getCallStack()) {
        std::ostringstream line;
        line << "frame " << frame.level << " " << frame.source << ":"
             << frame.currentLine << " " << frame.name << " [" << frame.what << "]";
        output.line(line.str());
    }
}

void printStop(const quantum::LuaDebugger& debugger, DebugOutput& output) {
    std::ostringstream stop;
    stop << "stop state=" << quantum::LuaDebugger::stateName(debugger.getState())
         << " reason=" << quantum::LuaDebugger::stopReasonName(debugger.getStopReason());
    output.line(stop.str());
    if (!debugger.getCallStack().empty()) {
        const auto& frame = debugger.getCallStack().front();
        std::ostringstream location;
        location << "location " << frame.source << ":" << frame.currentLine << " " << frame.name;
        output.line(location.str());
    }
}

void printJsonCallStack(std::ostream& output, const std::vector<quantum::LuaDebugger::Frame>& callStack) {
    output << "[";
    for (size_t index = 0; index < callStack.size(); ++index) {
        if (index > 0) output << ",";
        const auto& frame = callStack[index];
        output << "{\"level\":" << frame.level
               << ",\"source\":\"" << jsonEscape(frame.source)
               << "\",\"name\":\"" << jsonEscape(frame.name)
               << "\",\"what\":\"" << jsonEscape(frame.what)
               << "\",\"line\":" << frame.line
               << ",\"currentLine\":" << frame.currentLine << "}";
    }
    output << "]";
}

void printJsonResult(const quantum::LuaDebugger& debugger,
                     const JsonSession& session,
                     DebugOutput& output) {
    const auto& diagnostic = debugger.getDiagnostic();
    const std::string diagnosticMessage = session.diagnosticOverride.empty()
        ? diagnostic.message
        : session.diagnosticOverride;
    const std::string reason = session.reason.empty()
        ? quantum::LuaDebugger::stateName(debugger.getState())
        : session.reason;

    std::ostringstream result;
    result << "{\"state\":\"" << jsonEscape(quantum::LuaDebugger::stateName(debugger.getState()))
           << "\",\"stopReason\":\"" << jsonEscape(quantum::LuaDebugger::stopReasonName(debugger.getStopReason()))
           << "\",\"reason\":\"" << jsonEscape(reason)
           << "\",\"diagnostic\":{\"message\":\"" << jsonEscape(diagnosticMessage)
           << "\",\"source\":\"" << jsonEscape(diagnostic.source)
           << "\",\"line\":" << diagnostic.line
           << ",\"traceback\":\"" << jsonEscape(diagnostic.traceback) << "\"}"
           << ",\"watches\":[";
    for (size_t index = 0; index < session.watches.size(); ++index) {
        if (index > 0) result << ",";
        const auto& watch = session.watches[index];
        result << "{\"expression\":\"" << jsonEscape(watch.expression)
               << "\",\"ok\":" << (watch.ok ? "true" : "false");
        if (watch.ok) {
            result << ",\"value\":\"" << jsonEscape(watch.value) << "\"";
        } else {
            result << ",\"error\":\"" << jsonEscape(watch.error) << "\"";
        }
        result << "}";
    }
    result << "],\"callStack\":";
    printJsonCallStack(result, debugger.getCallStack());
    result << ",\"commandErrors\":[";
    for (size_t index = 0; index < session.commandErrors.size(); ++index) {
        if (index > 0) result << ",";
        result << "\"" << jsonEscape(session.commandErrors[index]) << "\"";
    }
    result << "]}";
    output.line(result.str());
}

void printJsonError(const std::string& reason, const std::string& message) {
    std::cout << "{\"state\":\"error\",\"stopReason\":\"error\",\"reason\":\""
              << jsonEscape(reason)
              << "\",\"diagnostic\":{\"message\":\"" << jsonEscape(message)
              << "\",\"source\":\"\",\"line\":0,\"traceback\":\"\"}"
              << ",\"watches\":[],\"callStack\":[],\"commandErrors\":[]}\n";
}

bool executeDebuggerCommand(quantum::LuaDebugger& debugger,
                            const std::string& raw,
                            JsonSession* jsonSession,
                            DebugOutput& output) {
    std::istringstream input(raw);
    std::string operation;
    input >> operation;
    if (operation == "continue" || operation == "c") return debugger.resume();
    if (operation == "step" || operation == "s") return debugger.stepInto();
    if (operation == "next" || operation == "n") return debugger.stepOver();
    if (operation == "out" || operation == "o") return debugger.stepOut();
    if (operation == "stack") {
        if (!jsonSession) printStack(debugger, output);
        return true;
    }
    if (operation == "watch" || operation == "w") {
        std::string expression;
        std::getline(input, expression);
        const std::string trimmedExpression = trimText(expression);
        std::string value;
        std::string error;
        const bool ok = debugger.evaluateWatch(trimmedExpression, value, error);
        if (jsonSession) {
            JsonWatch watch;
            watch.expression = trimmedExpression;
            watch.ok = ok;
            watch.value = value;
            watch.error = error;
            jsonSession->watches.push_back(std::move(watch));
        } else if (ok) {
            output.line("watch " + value);
        } else {
            output.line("watch-error " + error);
        }
        return true;
    }
    if (operation == "quit" || operation == "q") {
        if (jsonSession) jsonSession->reason = "quit";
        return false;
    }
    if (jsonSession) {
        jsonSession->commandErrors.push_back("unknown debugger command: " + operation);
    } else {
        output.line("unknown debugger command: " + operation);
    }
    return true;
}

int runDebug(int argc, char** argv, const char* executableName) {
    std::string file;
    std::string commandText;
    std::vector<BreakpointOption> breakpoints;
    bool protocol = false;
    bool json = false;
    bool tcpLoopback = false;
    std::uint16_t tcpPort = 0;
    int tcpTimeoutMs = 30000;
    for (int i = 0; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--file") {
            if (!valueAfter(argc, argv, i, file)) {
                if (json) printJsonError("invalid_arguments", "script debug: --file requires a value");
                return 2;
            }
        } else if (argument == "--breakpoint") {
            std::string value;
            if (!valueAfter(argc, argv, i, value)) {
                if (json) printJsonError("invalid_arguments", "script debug: --breakpoint requires a value");
                return 2;
            }
            breakpoints.push_back({value, {}});
        } else if (argument == "--condition") {
            std::string value;
            if (!valueAfter(argc, argv, i, value) || breakpoints.empty()) {
                if (json) printJsonError("invalid_arguments", "script debug: --condition requires a preceding --breakpoint and an expression");
                else std::cerr << "script debug: --condition requires a preceding --breakpoint and an expression\n";
                return 2;
            }
            breakpoints.back().condition = value;
        } else if (argument == "--commands") {
            if (!valueAfter(argc, argv, i, commandText)) {
                if (json) printJsonError("invalid_arguments", "script debug: --commands requires a value");
                return 2;
            }
        } else if (argument == "--protocol") {
            protocol = true;
        } else if (argument == "--tcp-loopback") {
            std::string value;
            if (!valueAfter(argc, argv, i, value) || !parseTcpPort(value, tcpPort)) {
                if (json) printJsonError("invalid_arguments", "script debug: --tcp-loopback requires a port from 0 to 65535");
                else std::cerr << "script debug: --tcp-loopback requires a port from 0 to 65535\n";
                return 2;
            }
            tcpLoopback = true;
        } else if (argument == "--tcp-timeout-ms") {
            std::string value;
            if (!valueAfter(argc, argv, i, value) || !parseTcpTimeout(value, tcpTimeoutMs)) {
                if (json) printJsonError("invalid_arguments", "script debug: --tcp-timeout-ms must be between 1 and 3600000");
                else std::cerr << "script debug: --tcp-timeout-ms must be between 1 and 3600000\n";
                return 2;
            }
        } else if (argument == "--json") {
            json = true;
        } else if (argument == "--help" || argument == "-h") {
            if (json) {
                printJsonError("help", "Use --file <script.lua> and optional breakpoints, commands or protocol");
            } else {
                printUsage(executableName);
            }
            return 0;
        } else {
            if (json) printJsonError("invalid_arguments", "script debug: unknown option " + argument);
            else std::cerr << "script debug: unknown option " << argument << "\n";
            return 2;
        }
    }
    if (file.empty()) {
        if (json) printJsonError("invalid_arguments", "script debug: --file is required");
        else std::cerr << "script debug: --file is required\n";
        return 2;
    }
    if (tcpLoopback && !protocol) {
        if (json) printJsonError("invalid_arguments", "script debug: --tcp-loopback requires --protocol");
        else std::cerr << "script debug: --tcp-loopback requires --protocol\n";
        return 2;
    }

    lua_State* lua = luaL_newstate();
    if (!lua) {
        if (json) printJsonError("lua_state_creation", "script debug: could not create Lua state");
        else std::cerr << "script debug: could not create Lua state\n";
        return 1;
    }
    luaL_openlibs(lua);
    quantum::LuaDebugger debugger(lua);
    JsonSession jsonSession;
    JsonSession* jsonOutput = json ? &jsonSession : nullptr;
    DebugOutput output;
    for (const BreakpointOption& option : breakpoints) {
        const std::string& text = option.specification;
        std::string source;
        int line = 0;
        if (!parseBreakpoint(text, source, line)) {
            if (json) {
                jsonSession.reason = "invalid_breakpoint";
                jsonSession.diagnosticOverride = "script debug: bad breakpoint " + text;
                printJsonResult(debugger, jsonSession, output);
            } else {
                std::cerr << "script debug: bad breakpoint " << text << "\n";
            }
            debugger.shutdown();
            lua_close(lua);
            return 2;
        }
        if (debugger.addBreakpoint(source, line, option.condition) == 0) {
            if (json) {
                jsonSession.reason = "invalid_breakpoint";
                jsonSession.diagnosticOverride = "script debug: could not add breakpoint " + text;
                printJsonResult(debugger, jsonSession, output);
            } else {
                std::cerr << "script debug: could not add breakpoint " << text << "\n";
            }
            debugger.shutdown();
            lua_close(lua);
            return 2;
        }
    }
    if (!debugger.startFile(file)) {
        if (json) {
            jsonSession.reason = "startup_error";
            printJsonResult(debugger, jsonSession, output);
        } else {
            std::cerr << "script debug: " << debugger.getDiagnostic().message << "\n";
        }
        debugger.shutdown();
        lua_close(lua);
        return 1;
    }

    std::unique_ptr<TcpLoopbackTransport> tcp;
    if (tcpLoopback) {
        tcp = std::make_unique<TcpLoopbackTransport>(tcpPort, tcpTimeoutMs);
        if (!tcp->isOpen()) {
            const std::string message = tcp->error().empty()
                ? "TCP loopback listener could not be opened"
                : tcp->error();
            if (json) {
                jsonSession.reason = "tcp_error";
                jsonSession.diagnosticOverride = "script debug: " + message;
                printJsonResult(debugger, jsonSession, output);
            } else {
                std::cerr << "script debug: " << message << "\n";
            }
            debugger.shutdown();
            lua_close(lua);
            return 1;
        }
        std::cerr << "script debug: tcp-listening host=127.0.0.1 port=" << tcp->port()
                  << " timeoutMs=" << tcpTimeoutMs << "\n";
        if (!tcp->acceptClient()) {
            const bool timedOut = tcp->isTimedOut();
            const std::string message = tcp->error().empty()
                ? (timedOut ? "TCP debug client accept timed out" : "TCP debug client was not accepted")
                : tcp->error();
            if (json) {
                jsonSession.reason = timedOut ? "tcp_timeout" : "tcp_error";
                jsonSession.diagnosticOverride = "script debug: " + message;
                printJsonResult(debugger, jsonSession, output);
            } else {
                std::cerr << "script debug: " << message << "\n";
            }
            debugger.shutdown();
            lua_close(lua);
            return 1;
        }
        output.setTransport(tcp.get());
    }

    if (protocol) {
        if (!json) output.line("ready state=idle");
        std::string line;
        if (tcpLoopback) {
            while (tcp->readLine(line)) {
                if (!executeDebuggerCommand(debugger, line, jsonOutput, output)) break;
                if (!output.healthy()) {
                    jsonSession.reason = "tcp_error";
                    break;
                }
                if (!json) printStop(debugger, output);
                if (debugger.getState() == quantum::LuaDebugger::State::Completed ||
                    debugger.getState() == quantum::LuaDebugger::State::Error) break;
            }
        } else {
            while (std::getline(std::cin, line)) {
                if (!executeDebuggerCommand(debugger, line, jsonOutput, output)) break;
                if (!json) printStop(debugger, output);
                if (debugger.getState() == quantum::LuaDebugger::State::Completed ||
                    debugger.getState() == quantum::LuaDebugger::State::Error) break;
            }
        }
        if (jsonSession.reason.empty()) {
            if (tcpLoopback && tcp->isTimedOut()) jsonSession.reason = "tcp_timeout";
            else if (tcpLoopback && !tcp->error().empty()) jsonSession.reason = "tcp_disconnect";
            else if (debugger.getState() == quantum::LuaDebugger::State::Completed) jsonSession.reason = "completed";
            else if (debugger.getState() == quantum::LuaDebugger::State::Error) jsonSession.reason = "error";
            else jsonSession.reason = "input_eof";
        }
    } else {
        const std::vector<std::string> commands = splitCommands(commandText);
        size_t commandIndex = 0;
        debugger.resume();
        while (debugger.getState() == quantum::LuaDebugger::State::Paused) {
            if (commandIndex < commands.size()) {
                if (!executeDebuggerCommand(debugger, commands[commandIndex++], jsonOutput, output)) break;
            } else {
                debugger.resume();
            }
            if (!json && debugger.getState() == quantum::LuaDebugger::State::Paused) printStop(debugger, output);
        }
        if (jsonSession.reason.empty()) {
            if (debugger.getState() == quantum::LuaDebugger::State::Completed) jsonSession.reason = "completed";
            else if (debugger.getState() == quantum::LuaDebugger::State::Error) jsonSession.reason = "error";
            else if (debugger.getState() == quantum::LuaDebugger::State::Paused) jsonSession.reason = "paused";
            else jsonSession.reason = "stopped";
        }
    }

    if (debugger.getState() == quantum::LuaDebugger::State::Error) {
        const auto& diagnostic = debugger.getDiagnostic();
        if (json) {
            printJsonResult(debugger, jsonSession, output);
        } else {
            std::cerr << "script error: " << diagnostic.message << "\n";
            if (!diagnostic.traceback.empty()) std::cerr << diagnostic.traceback << "\n";
            if (tcpLoopback) output.line("script error: " + diagnostic.message);
        }
        debugger.shutdown();
        lua_close(lua);
        return 1;
    }
    if (json) {
        printJsonResult(debugger, jsonSession, output);
    } else {
        output.line("script-debug: " + std::string(quantum::LuaDebugger::stateName(debugger.getState())));
    }
    debugger.shutdown();
    lua_close(lua);
    return 0;
}

int runGraph(int argc, char** argv, bool roundTrip) {
    std::string file;
    std::string output;
    for (int i = 0; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--file") {
            if (!valueAfter(argc, argv, i, file)) return 2;
        } else if (argument == "--out") {
            if (!valueAfter(argc, argv, i, output)) return 2;
        } else if (argument == "--help" || argument == "-h") {
            printUsage("quantum-editor-cmd");
            return 0;
        } else {
            std::cerr << "script graph: unknown option " << argument << "\n";
            return 2;
        }
    }
    if (file.empty() || (roundTrip && output.empty())) {
        std::cerr << "script graph: --file is required, and roundtrip needs --out\n";
        return 2;
    }
        quantum::editor::graph::GraphAsset asset;
    std::string error;
    if (!asset.load(file, error)) {
        std::cerr << "graph load error: " << error << "\n";
        return 1;
    }
    std::vector<std::string> errors;
    if (!asset.validate(errors)) {
        for (const std::string& message : errors) std::cerr << "graph error: " << message << "\n";
        return 1;
    }
    if (roundTrip) {
        if (!asset.save(output, error)) {
            std::cerr << "graph save error: " << error << "\n";
            return 1;
        }
        quantum::editor::graph::GraphAsset reloaded;
        if (!reloaded.load(output, error) || !reloaded.validate(errors)) {
            std::cerr << "graph roundtrip error: " << (error.empty() ? "reloaded graph is invalid" : error) << "\n";
            return 1;
        }
        std::cout << "graph-roundtrip: ok " << output << "\n";
    } else {
        std::cout << "graph-validate: ok nodes=" << asset.nodes.size() << " edges=" << asset.edges.size() << "\n";
    }
    return 0;
}

}

int runScriptDebugCommand(int argc, char** argv, const char* executableName) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        printUsage(executableName);
        return argc < 2 ? 2 : 0;
    }
    const std::string subcommand = argv[1];
    if (subcommand == "debug") return runDebug(argc - 2, argv + 2, executableName);
    if (subcommand == "lint") return runLint(argc - 2, argv + 2);
    if (subcommand == "graph-validate") return runGraph(argc - 2, argv + 2, false);
    if (subcommand == "graph-roundtrip") return runGraph(argc - 2, argv + 2, true);
    std::cerr << "script: unknown subcommand " << subcommand << "\n";
    printUsage(executableName);
    return 2;
}

}
