// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

// ResourceServerClient.cpp - see ResourceServerClient.h.

#include "ResourceServerClient.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
    // <winsock2.h> BEFORE <windows.h> and WIN32_LEAN_AND_MEAN defined before
    // any include: otherwise <windows.h> pulls <winsock.h> and the sockaddr
    // redefinition cascade this repo already hit fires in the editor TU too.
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <csignal>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <poll.h>
    #include <spawn.h>
    #include <sys/socket.h>
    #include <sys/wait.h>
    #include <unistd.h>
    extern char** environ;
#endif

namespace doriax::editor::cli {

namespace {

constexpr int kSpawnWaitMs = 8000;
constexpr char kListeningPrefix[] = "doriax-resource-server: listening on 127.0.0.1:";

// ---------------------------------------------------------------------------
// Socket library lifetime: refcounted so any number of clients can coexist
// and the last one tears the library down (WSAStartup/WSACleanup pairing).
// ---------------------------------------------------------------------------
std::mutex g_netMutex;
int g_netRefs = 0;

bool netRetain(std::string& errorOut) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_netMutex);
    if (g_netRefs == 0) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            errorOut = "WSAStartup failed";
            return false;
        }
    }
    ++g_netRefs;
#else
    (void)errorOut;
#endif
    return true;
}

void netRelease() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_netMutex);
    if (g_netRefs > 0 && --g_netRefs == 0)
        WSACleanup();
#endif
}

struct NetSession {
    std::string error;
    bool ok = false;
    NetSession() { ok = netRetain(error); }
    ~NetSession() { if (ok) netRelease(); }
};

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void closeSocket(SocketHandle s) { closesocket(s); }
bool socketWouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void closeSocket(SocketHandle s) { ::close(s); }
bool socketWouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

bool setNonBlocking(SocketHandle s) {
#ifdef _WIN32
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// Waits until the socket is readable/writable or the deadline passes.
// Returns 1 ready, 0 timeout, -1 error.
int waitSocket(SocketHandle s, bool forRead, int timeoutMs) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int r = forRead ? select(0, &set, nullptr, nullptr, &tv)
                    : select(0, nullptr, &set, nullptr, &tv);
#else
    pollfd pfd{};
    pfd.fd = s;
    pfd.events = forRead ? POLLIN : POLLOUT;
    int r = ::poll(&pfd, 1, timeoutMs);
    if (r > 0 && (pfd.revents & (POLLERR | POLLNVAL))) r = -1;
#endif
    if (r > 0) return 1;
    return (r == 0) ? 0 : -1;
}

// The server only binds 127.0.0.1, so the client only accepts dotted IPv4
// (no getaddrinfo dependency on the MinGW/MSVC matrix).
bool connectHost(const std::string& host) {
    return !host.empty() && host.size() < 64 &&
           std::all_of(host.begin(), host.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; });
}

// Parses a dotted IPv4 quad without sscanf (MSVC warns C4996 on it).
bool parseDottedIpv4(const std::string& host, unsigned long out[4]) {
    int idx = 0;
    bool anyDigit = false;
    out[0] = out[1] = out[2] = out[3] = 0;
    for (char c : host) {
        if (c == '.') {
            if (idx >= 3 || !anyDigit) return false;
            ++idx;
            anyDigit = false;
            continue;
        }
        if (c < '0' || c > '9') return false;
        out[idx] = out[idx] * 10 + static_cast<unsigned long>(c - '0');
        if (out[idx] > 255) return false;
        anyDigit = true;
    }
    return idx == 3 && anyDigit;
}

// Parses the trailing port out of the server's stdout banner line.
bool parseListeningLine(const std::string& line, int& portOut) {
    size_t at = line.find(kListeningPrefix);
    if (at == std::string::npos) return false;
    at += sizeof(kListeningPrefix) - 1;
    unsigned long v = strtoul(line.c_str() + at, nullptr, 10);
    if (v == 0 || v > 65535) return false;
    portOut = int(v);
    return true;
}

std::string absoluteOr(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    return (ec ? p : a).generic_u8string();
}

#ifdef _WIN32
std::string windowsExeDirectory() {
    char buf[4096];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return "";
    std::string full(buf, n);
    size_t slash = full.find_last_of("\\/");
    return slash == std::string::npos ? "." : full.substr(0, slash);
}
#endif

}

// ---------------------------------------------------------------------------

bool findResourceServerExecutable(fs::path& outPath) {
#ifdef _WIN32
    const char* exeName = "doriax-resource-server.exe";
    const std::string exeDir = windowsExeDirectory();
#else
    const char* exeName = "doriax-resource-server";
    std::string exeDir;
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string full(buf);
        size_t slash = full.find_last_of('/');
        if (slash != std::string::npos) exeDir = full.substr(0, slash);
    }
#endif
    std::vector<std::string> dirs;
    if (!exeDir.empty()) {
        dirs.push_back(exeDir);
        // Common single-config generator layouts under the executable dir's
        // parent (build/Release/, build/Debug/, build/bin/).
        fs::path parent = fs::path(exeDir).parent_path();
        std::string ps = parent.string();
        dirs.push_back((fs::path(ps) / "Release").string());
        dirs.push_back((fs::path(ps) / "Debug").string());
        dirs.push_back((fs::path(ps) / "bin").string());
    }
    dirs.push_back(".");
    for (const auto& d : dirs) {
        fs::path candidate = fs::path(d) / exeName;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            outPath = candidate;
            return true;
        }
    }
    return false;
}

bool requestServerLine(const std::string& host, int port, const std::string& requestLine,
                       std::string& responseOut, std::string& errorOut, int timeoutMs) {
    responseOut.clear();
    errorOut.clear();
    if (port <= 0 || port > 65535) { errorOut = "invalid port"; return false; }
    if (!connectHost(host)) { errorOut = "invalid host"; return false; }

    NetSession net;
    if (!net.ok) { errorOut = net.error; return false; }

    SocketHandle s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) { errorOut = "socket() failed"; return false; }

    bool ok = false;
    do {
        if (!setNonBlocking(s)) break;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        // Dotted-quad without inet_pton dependencies on every toolchain.
        unsigned long parts[4];
        if (!parseDottedIpv4(host, parts)) {
            errorOut = "host must be a dotted IPv4 address";
            break;
        }
        addr.sin_addr.s_addr = htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);

        int r = ::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (r != 0) {
#ifdef _WIN32
            bool inProgress = WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == 10036 /*WSAEINPROGRESS*/;
#else
            bool inProgress = errno == EINPROGRESS || errno == EINTR;
#endif
            if (!inProgress) { errorOut = "connect failed"; break; }
            if (waitSocket(s, false, timeoutMs) != 1) { errorOut = "connect timed out"; break; }
            int soErr = 0;
#ifdef _WIN32
            int len = sizeof(soErr);
#else
            socklen_t len = sizeof(soErr);
#endif
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len) != 0 ||
                soErr != 0) {
                errorOut = "connection refused";
                break;
            }
        }

        std::string payload = requestLine;
        if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
        size_t sent = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        bool sendOk = true;
        while (sent < payload.size()) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) { sendOk = false; break; }
            int n = ::send(s, payload.data() + sent, int(payload.size() - sent), 0);
            if (n > 0) { sent += size_t(n); continue; }
            if (n < 0 && socketWouldBlock()) { waitSocket(s, false, int(left)); continue; }
            sendOk = false;
            break;
        }
        if (!sendOk) { errorOut = "send failed or timed out"; break; }

        // Read until the first newline (the server answers with exactly one
        // JSON line) or the deadline.
        std::string acc;
        char buf[2048];
        bool gotLine = false;
        while (!gotLine && acc.size() < (1u << 20)) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) break;
            int ready = waitSocket(s, true, int(left));
            if (ready != 1) break;
#ifdef _WIN32
            int n = recv(s, buf, sizeof(buf), 0);
#else
            ssize_t n = ::recv(s, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            // size_t loop counter: works for both `int` (Winsock) and
            // `ssize_t` (POSIX) recv() return types without needing ssize_t
            // on MSVC.
            for (size_t i = 0; i < static_cast<size_t>(n); ++i) {
                if (buf[i] == '\n') { gotLine = true; break; }
                acc.push_back(buf[i]);
            }
        }
        if (!gotLine) { errorOut = "no response (timed out or closed)"; break; }
        responseOut = acc;
        ok = true;
    } while (false);

    closeSocket(s);
    if (!ok && errorOut.empty()) errorOut = "request failed";
    return ok;
}

bool probeServer(const std::string& host, int port, std::string& statusJsonOut,
                 std::string& errorOut) {
    return requestServerLine(host, port, "{\"cmd\":\"status\"}", statusJsonOut, errorOut);
}

bool requestStatus(int port, std::string& statusJsonOut, std::string& errorOut) {
    return probeServer("127.0.0.1", port, statusJsonOut, errorOut);
}

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Windows spawn: CreateProcessA with an inherited stdout/stderr pipe, exactly
// the pipe/no-window technique editor/util/CommandRunner.cpp uses, but
// detached (no wait, handles closed after the port line arrives).
// ---------------------------------------------------------------------------
bool spawnResourceServer(const fs::path& projectDir, const fs::path& serverExe, int& portOut,
                         unsigned long long& pidOut, std::string& errorOut) {
    portOut = 0;
    pidOut = 0;
    errorOut.clear();

    fs::path exe = serverExe;
    if (exe.empty() && !findResourceServerExecutable(exe)) {
        errorOut = "doriax-resource-server executable not found beside the editor";
        return false;
    }

    std::string cmd = "\"" + absoluteOr(exe) + "\" --project \"" + absoluteOr(projectDir) +
                      "\" --port 0";

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        errorOut = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    if (!CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        errorOut = "CreateProcess failed (code " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(hWrite);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSpawnWaitMs);
    bool found = false;
    std::string acc, pending;
    char buf[1024];
    while (!found && std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available > 0) {
            DWORD got = 0;
            if (!ReadFile(hRead, buf, sizeof(buf), &got, nullptr) || got == 0) break;
            acc.append(buf, got);
            size_t pos = 0, nl;
            while ((nl = acc.find('\n', pos)) != std::string::npos) {
                pending = acc.substr(pos, nl - pos);
                pos = nl + 1;
                if (!pending.empty() && pending.back() == '\r') pending.pop_back();
                if (parseListeningLine(pending, portOut)) found = true;
            }
            acc.erase(0, pos);
        } else {
            DWORD exitCode = 0;
            if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    if (!found) {
        // A server we cannot reach is useless: reap it.
        TerminateProcess(pi.hProcess, 1);
        portOut = 0;
        errorOut = "doriax-resource-server did not report a listening port";
        CloseHandle(hRead);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    pidOut = pi.dwProcessId;
    // Detach: closing the process/thread handles leaves the child running.
    CloseHandle(hRead);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool stopResourceServer(unsigned long long pid, std::string& errorOut) {
    errorOut.clear();
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE,
                           static_cast<DWORD>(pid));
    if (!h) {
        // Already gone is success for a best-effort stop.
        return true;
    }
    bool ok = TerminateProcess(h, 0) != FALSE;
    DWORD check = 0;
    if (GetExitCodeProcess(h, &check) && check == STILL_ACTIVE && !ok)
        errorOut = "TerminateProcess failed";
    CloseHandle(h);
    return ok || errorOut.empty();
}
#else
// ---------------------------------------------------------------------------
// POSIX spawn: posix_spawn (the CommandRunner technique) with an output pipe;
// the child is left in its own process group and never reaped here.
// ---------------------------------------------------------------------------
bool spawnResourceServer(const fs::path& projectDir, const fs::path& serverExe, int& portOut,
                         unsigned long long& pidOut, std::string& errorOut) {
    portOut = 0;
    pidOut = 0;
    errorOut.clear();

    fs::path exe = serverExe;
    if (exe.empty() && !findResourceServerExecutable(exe)) {
        errorOut = "doriax-resource-server executable not found beside the editor";
        return false;
    }

    std::string exeStr = absoluteOr(exe);
    std::string projStr = absoluteOr(projectDir);
    std::string portStr = "0";
    char* argv[] = { const_cast<char*>(exeStr.c_str()), const_cast<char*>("--project"),
                     const_cast<char*>(projStr.c_str()), const_cast<char*>("--port"),
                     portStr.data(), nullptr };

    int pipefd[2];
    if (pipe(pipefd) != 0) { errorOut = "pipe() failed"; return false; }
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    pid_t pid = 0;
    int rc = posix_spawn(&pid, exeStr.c_str(), &actions, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        errorOut = "posix_spawn failed: " + std::string(strerror(rc));
        return false;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSpawnWaitMs);
    bool found = false;
    std::string acc;
    char buf[1024];
    while (!found && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{};
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        int ready = ::poll(&pfd, 1, 20);
        if (ready > 0) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n <= 0) break;  // EOF: the server exited before reporting
            acc.append(buf, n);
            size_t pos = 0, nl;
            std::string line;
            while ((nl = acc.find('\n', pos)) != std::string::npos) {
                line = acc.substr(pos, nl - pos);
                pos = nl + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (parseListeningLine(line, portOut)) found = true;
            }
            acc.erase(0, pos);
        } else {
            int status = 0;
            if (waitpid(pid, &status, WNOHANG) == pid) break;  // died without a port
        }
    }
    close(pipefd[0]);

    if (!found) {
        kill(-pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
        portOut = 0;
        errorOut = "doriax-resource-server did not report a listening port";
        return false;
    }

    pidOut = static_cast<unsigned long long>(pid);
    // Detached: intentionally NOT waitpid()ing. The entry lingers as a zombie
    // until this process exits (single known cost; see report).
    return true;
}

bool stopResourceServer(unsigned long long pid, std::string& errorOut) {
    errorOut.clear();
    if (kill(static_cast<pid_t>(pid), SIGTERM) != 0 && errno != ESRCH) {
        errorOut = "kill failed";
        return false;
    }
    return true;
}
#endif

bool spawnResourceServer(const fs::path& projectDir, int& portOut, unsigned long long& pidOut,
                         std::string& errorOut) {
    return spawnResourceServer(projectDir, fs::path(), portOut, pidOut, errorOut);
}

}
