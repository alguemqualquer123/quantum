// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "ResourceServer.h"
#include "JsonMini.h"
#include "ResourceIndex.h"
#include "TrayIcon.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef _WIN32
    // winsock2.h MUST come before windows.h; the target also defines
    // WIN32_LEAN_AND_MEAN (see doriax_resource_server.cmake) so windows.h
    // does not pull winsock.h and redefine sockaddr.
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
#else
    #include <arpa/inet.h>
    #include <csignal>
    #include <errno.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace doriax::rs {

namespace {

constexpr int kWatchTickMs = 1000;
constexpr int kSelectSliceMs = 200;
constexpr int kParentCheckSeconds = 2;
constexpr int kRequestTimeoutMs = 3000;
constexpr size_t kMaxRequestBytes = 65536;
constexpr size_t kMaxIndexEntriesInResponse = 5000;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void closeSocket(SocketHandle s) { closesocket(s); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void closeSocket(SocketHandle s) { ::close(s); }
#endif

std::atomic<bool> g_stopRequested{ false };

#ifdef _WIN32
BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stopRequested.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
void ctrlHandler(int) { g_stopRequested.store(true); }
#endif

std::string errorResponse(const std::string& message) {
    return "{\"ok\":false,\"error\":\"" + jsonEscape(message) + "\"}";
}

std::string okResponse(const std::string& fields) {
    std::string out = "{\"ok\":true";
    if (!fields.empty()) out += "," + fields;
    out += "}";
    return out;
}

void jsonPutArray(std::string& body, const std::string& key, const std::vector<std::string>& items) {
    if (!body.empty()) body += ',';
    body += '"' + jsonEscape(key) + "\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) body += ',';
        body += '"' + jsonEscape(items[i]) + '"';
    }
    body += ']';
}

std::string entryObject(const std::string& path, const IndexEntry& e) {
    std::string body;
    jsonPutString(body, "path", path);
    jsonPutNumber(body, "size", static_cast<double>(e.size));
    jsonPutNumber(body, "mtime", static_cast<double>(e.mtimeRaw));
    jsonPutString(body, "sha1", e.sha1);
    return "{" + body + "}";
}

// Extracts a scalar string member from an already-parsed field map.
std::string fieldStr(const std::map<std::string, JsonField>& f, const std::string& key) {
    auto it = f.find(key);
    return it == f.end() ? std::string() : it->second.text;
}

}

std::string handleRequestLine(const std::string& requestLine, ResourceIndex& index,
                              const std::string& token, long long startUnix) {
    std::map<std::string, JsonField> fields;
    std::string parseErr;
    if (!parseFlatJsonObject(requestLine, fields, parseErr))
        return errorResponse("bad request json: " + parseErr);

    std::string cmd = fieldStr(fields, "cmd");
    if (cmd.empty())
        return errorResponse("missing cmd");

    // Shared-secret gate: when the server was started with --token, every
    // request (including ping) must echo it back.
    if (!token.empty()) {
        std::string got = fieldStr(fields, "token");
        if (got != token)
            return errorResponse("bad token");
    }

    if (cmd == "ping")
        return okResponse("\"pong\":true");

    if (cmd == "status") {
        std::string body;
        jsonPutString(body, "server", "doriax-resource-server");
        jsonPutString(body, "project", index.projectDir().generic_u8string());
        jsonPutNumber(body, "scanId", static_cast<double>(index.scanId()));
        jsonPutNumber(body, "watchedFiles", static_cast<double>(index.fileCount()));
        jsonPutNumber(body, "lastScanUnix", static_cast<double>(index.lastScanUnix()));
        long long now = static_cast<long long>(std::time(nullptr));
        jsonPutNumber(body, "uptimeSeconds", double(std::max<long long>(0, now - startUnix)));
        jsonPutString(body, "manifestSha1", index.manifestSha1());
#ifdef _WIN32
        jsonPutNumber(body, "pid", static_cast<double>(GetCurrentProcessId()));
#elif defined(__unix__) || defined(__APPLE__)
        jsonPutNumber(body, "pid", static_cast<double>(getpid()));
#endif
        jsonPutArray(body, "roots", index.foundRoots());
        return okResponse(body);
    }

    if (cmd == "index") {
        auto snap = index.snapshot();
        std::string body;
        jsonPutNumber(body, "scanId", static_cast<double>(index.scanId()));
        if (!body.empty()) body += ',';
        body += "\"entries\":[";
        size_t limit = snap.size() < kMaxIndexEntriesInResponse ? snap.size() : kMaxIndexEntriesInResponse;
        for (size_t i = 0; i < limit; ++i) {
            if (i) body += ',';
            body += entryObject(snap[i].first, snap[i].second);
        }
        body += ']';
        if (snap.size() > limit)
            jsonPutString(body, "note", "truncated; query single entries with cmd=entry");
        return okResponse(body);
    }

    if (cmd == "entry") {
        std::string requestPath = fieldStr(fields, "path");
        std::string rel;
        if (!index.resolveInside(requestPath, rel))
            return errorResponse("invalid path");
        IndexEntry e;
        if (!index.lookup(rel, e))
            return errorResponse("no such entry");
        std::string body;
        jsonPutNumber(body, "scanId", static_cast<double>(index.scanId()));
        body += ",\"entry\":" + entryObject(rel, e);
        return okResponse(body);
    }

    if (cmd == "changes") {
        auto it = fields.find("since");
        if (it == fields.end())
            return errorResponse("since required");
        uint64_t since = it->second.asUInt();
        uint64_t current = 0;
        std::vector<std::string> added, removed, modified;
        bool expired = false;
        index.changesSince(since, current, added, removed, modified, expired);
        std::string body;
        jsonPutNumber(body, "since", static_cast<double>(since));
        jsonPutNumber(body, "scanId", static_cast<double>(current));
        if (expired) {
            jsonPutNumber(body, "expired", 1);
            return okResponse(body);
        }
        jsonPutArray(body, "added", added);
        jsonPutArray(body, "removed", removed);
        jsonPutArray(body, "modified", modified);
        return okResponse(body);
    }

    return errorResponse("unknown cmd: " + cmd);
}

namespace {

// Reads one newline-terminated request from a freshly accepted connection.
// Returns false on timeout/EOF/oversize; on true, lineOut holds the request
// without the newline.
bool readLine(SocketHandle s, std::string& lineOut) {
    lineOut.clear();
    char buf[1024];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
    for (;;) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) return false;
#ifdef _WIN32
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        timeval tv;
        tv.tv_sec = long(remaining.count() / 1000);
        tv.tv_usec = long((remaining.count() % 1000) * 1000);
        int ready = select(0, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0) return false;
        int got = recv(s, buf, int(sizeof(buf)), 0);
#else
        pollfd pfd{};
        pfd.fd = s;
        pfd.events = POLLIN;
        int ready = ::poll(&pfd, 1, int(remaining.count()));
        if (ready <= 0) return false;
        ssize_t got = ::recv(s, buf, sizeof(buf), 0);
#endif
        if (got <= 0) return false;  // EOF or error
        for (int i = 0; i < got; ++i) {
            if (buf[i] == '\n') return true;
            lineOut.push_back(buf[i]);
            if (lineOut.size() > kMaxRequestBytes) return false;
        }
        if (got < int(sizeof(buf))) continue;
    }
}

bool writeAll(SocketHandle s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        int n = send(s, data.data() + sent, int(data.size() - sent), 0);
        if (n == SOCKET_ERROR) return false;
#else
        ssize_t n = ::send(s, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
#endif
        sent += size_t(n);
    }
    return true;
}

void serveConnection(SocketHandle client, ResourceIndex& index, const std::string& token,
                     long long startUnix, bool verbose) {
    std::string line;
    if (!readLine(client, line)) {
        closeSocket(client);
        return;
    }
    if (verbose) {
        std::fprintf(stderr, "request: %s\n", line.c_str());
    }
    std::string response = handleRequestLine(line, index, token, startUnix);
    response.push_back('\n');
    writeAll(client, response);
    closeSocket(client);
}

int lastErrorValue() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// Tray/menu text for the current state. `editorRunning` drives whether
// "Close server and editor" is offered.
TraySnapshot makeTraySnapshot(const ServerOptions& options, const ResourceIndex& index,
                              int port, bool editorRunning) {
    TraySnapshot snap;
    std::string name = options.projectDir.filename().generic_u8string();
    if (name.empty()) name = options.projectDir.parent_path().filename().generic_u8string();
    snap.projectName = name;
    snap.projectPath = options.projectDir.generic_u8string();
    snap.address = "127.0.0.1:" + std::to_string(port);
    snap.watchedLine = std::to_string(index.fileCount()) + " files (scan " +
                       std::to_string(static_cast<unsigned long long>(index.scanId())) + ")";
    snap.editorRunning = editorRunning;
    return snap;
}

}

int runOnceSummary(const ServerOptions& options) {
    ResourceIndex index(options.projectDir);
    ResourceIndex::ChangeSet changes;
    index.scanOnce(&changes);
    std::printf("doriax-resource-server: project %s\n",
                index.projectDir().generic_u8string().c_str());
    std::printf("doriax-resource-server: roots:");
    if (index.foundRoots().empty()) std::printf(" (none found)");
    for (const auto& r : index.foundRoots()) std::printf(" %s", r.c_str());
    std::printf("\n");
    std::printf("doriax-resource-server: %zu files indexed, scanId=%llu\n",
                index.fileCount(), static_cast<unsigned long long>(index.scanId()));
    std::printf("doriax-resource-server: manifest %s\n", index.manifestSha1().c_str());
    std::printf("doriax-resource-server: added=%zu removed=%zu modified=%zu\n",
                changes.added.size(), changes.removed.size(), changes.modified.size());
    return 0;
}

int runResourceServer(const ServerOptions& options) {
    if (options.once) return runOnceSummary(options);

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "doriax-resource-server: WSAStartup failed\n");
        return 2;
    }
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
#else
    std::signal(SIGINT, ctrlHandler);
    std::signal(SIGTERM, ctrlHandler);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    long long startUnix = static_cast<long long>(std::time(nullptr));
    ResourceIndex index(options.projectDir);
    index.scanOnce();  // initial index before the first request can arrive

    SocketHandle listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == kInvalidSocket) {
        std::fprintf(stderr, "doriax-resource-server: socket() failed (%d)\n", lastErrorValue());
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);  // 127.0.0.1 only: localhost
    addr.sin_port = htons(static_cast<uint16_t>(options.port));

    if (bind(listenSock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listenSock, 16) != 0) {
        std::fprintf(stderr, "doriax-resource-server: bind/listen failed (%d)\n", lastErrorValue());
        closeSocket(listenSock);
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }

    // With --port 0 the OS picked the port; report the real one so the editor
    // (which greps for this exact line) can connect.
    sockaddr_in bound{};
#ifdef _WIN32
    int boundLen = sizeof(bound);
#else
    socklen_t boundLen = sizeof(bound);
#endif
    int actualPort = options.port;
    if (getsockname(listenSock, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0)
        actualPort = ntohs(bound.sin_port);
    std::printf("doriax-resource-server: listening on 127.0.0.1:%d\n", actualPort);
    std::fflush(stdout);

    if (options.verbose) {
        std::fprintf(stderr, "doriax-resource-server: watching %s (%zu files, scanId %llu)\n",
                     index.projectDir().generic_u8string().c_str(), index.fileCount(),
                     static_cast<unsigned long long>(index.scanId()));
    }

    // The server outlives the editor on purpose, so the notification-area icon
    // is how the user reaches it. Failing to add one is not an error: the
    // service still answers, and POSIX has no tray at all.
    TrayIcon tray;
    bool editorRunning = options.parentPid != 0 && processIsRunning(options.parentPid);
    TraySnapshot traySnapshot = makeTraySnapshot(options, index, actualPort, editorRunning);
    if (!tray.create(traySnapshot) && options.verbose) {
        std::fprintf(stderr, "doriax-resource-server: no tray icon (system tray unavailable)\n");
    }

    auto nextScan = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWatchTickMs);
    auto nextParentCheck = nextScan;
    while (!g_stopRequested.load()) {
        auto now = std::chrono::steady_clock::now();
        auto msToScan = std::chrono::duration_cast<std::chrono::milliseconds>(nextScan - now).count();
        if (msToScan <= 0) {
            ResourceIndex::ChangeSet changes;
            if (index.scanOnce(&changes)) {
                if (options.verbose) {
                    std::fprintf(stderr,
                        "scan %llu: +%zu -%zu ~%zu (files %zu)\n",
                        static_cast<unsigned long long>(index.scanId()),
                        changes.added.size(), changes.removed.size(), changes.modified.size(),
                        index.fileCount());
                }
            }
            nextScan = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWatchTickMs);
            msToScan = kWatchTickMs;
        }
        int waitMs = int(std::min<long long>(msToScan, kSelectSliceMs));

        // A pid can be recycled once its process is gone, so the editor is only
        // ever considered running while the one we started with is alive.
        if (options.parentPid != 0 && editorRunning && now >= nextParentCheck) {
            editorRunning = processIsRunning(options.parentPid);
            nextParentCheck = now + std::chrono::seconds(kParentCheckSeconds);
        }
        traySnapshot = makeTraySnapshot(options, index, actualPort, editorRunning);
        tray.setSnapshot(traySnapshot);

#ifdef _WIN32
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listenSock, &rfds);
        timeval tv{};
        tv.tv_sec = waitMs / 1000;
        tv.tv_usec = (waitMs % 1000) * 1000;
        int ready = select(0, &rfds, nullptr, nullptr, &tv);
        if (ready > 0) {
            SocketHandle client = accept(listenSock, nullptr, nullptr);
#else
        pollfd pfd{};
        pfd.fd = listenSock;
        pfd.events = POLLIN;
        int ready = ::poll(&pfd, 1, waitMs);
        if (ready > 0) {
            SocketHandle client = ::accept(listenSock, nullptr, nullptr);
#endif
            if (client != kInvalidSocket)
                serveConnection(client, index, options.token, startUnix, options.verbose);
        }

        // Shell messages are pumped right after the socket wait so a tray click
        // is seen within one slice (200 ms) without its own blocking wait.
        const TrayChoice choice = tray.pump();
        if (choice == TrayChoice::QuitServer) break;
        if (choice == TrayChoice::QuitServerAndEditor) {
            // WM_CLOSE, never TerminateProcess: the editor flushes the scene it
            // has open and gets to ask about unsaved work first.
            if (options.parentPid != 0 && requestProcessCloseWindow(options.parentPid) &&
                options.verbose) {
                std::fprintf(stderr, "doriax-resource-server: asked editor %llu to close\n",
                             options.parentPid);
            }
            break;
        }
    }

    tray.destroy();
    closeSocket(listenSock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

}
