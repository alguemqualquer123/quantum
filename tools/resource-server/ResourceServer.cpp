// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "ResourceServer.h"
#include "JsonMini.h"
#include "ResourceIndex.h"
#include "ServerWindow.h"
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
// The connections command is a human-facing diagnostic, so it answers with the
// tail of the log rather than the whole ring.
constexpr size_t kMaxConnectionsInResponse = 128;

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

// Same, for members that are already JSON (the connection and client objects).
void jsonPutRawArray(std::string& body, const std::string& key,
                     const std::vector<std::string>& items) {
    if (!body.empty()) body += ',';
    body += '"' + jsonEscape(key) + "\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) body += ',';
        body += items[i];
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

std::string connectionObject(const ConnectionRecord& r) {
    std::string body;
    jsonPutNumber(body, "id", static_cast<double>(r.id));
    jsonPutString(body, "peer", r.peer);
    jsonPutString(body, "command", r.command);
    jsonPutString(body, "client", r.client);
    jsonPutString(body, "process", r.processName);
    jsonPutNumber(body, "pid", static_cast<double>(r.ownerPid));
    jsonPutNumber(body, "unixTime", static_cast<double>(r.unixTime));
    jsonPutNumber(body, "durationMs", r.durationMs);
    jsonPutNumber(body, "requestBytes", static_cast<double>(r.requestBytes));
    jsonPutNumber(body, "responseBytes", static_cast<double>(r.responseBytes));
    jsonPutNumber(body, "ok", r.ok ? 1 : 0);
    jsonPutString(body, "detail", r.detail);
    return "{" + body + "}";
}

std::string clientObject(const ClientGroup& g) {
    std::string body;
    jsonPutString(body, "process", g.processName);
    jsonPutString(body, "pid", g.pidText);
    jsonPutString(body, "peerHost", g.peerHost);
    jsonPutString(body, "client", g.client);
    jsonPutNumber(body, "requests", static_cast<double>(g.requests));
    jsonPutNumber(body, "failures", static_cast<double>(g.failures));
    jsonPutNumber(body, "firstSeenUnix", static_cast<double>(g.firstSeenUnix));
    jsonPutNumber(body, "lastSeenUnix", static_cast<double>(g.lastSeenUnix));
    jsonPutString(body, "lastCommand", g.lastCommand);
    return "{" + body + "}";
}

// Extracts a scalar string member from an already-parsed field map.
std::string fieldStr(const std::map<std::string, JsonField>& f, const std::string& key) {
    auto it = f.find(key);
    return it == f.end() ? std::string() : it->second.text;
}

}

std::string handleRequestLine(const std::string& requestLine, ResourceIndex& index,
                              const std::string& token, long long startUnix,
                              const ConnectionLog* log) {
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

    if (cmd == "connections") {
        if (!log) return errorResponse("connections unavailable");
        const std::vector<ConnectionRecord>& records = log->records();
        const size_t first = records.size() > kMaxConnectionsInResponse
                                 ? records.size() - kMaxConnectionsInResponse
                                 : 0;
        std::vector<std::string> recent;
        recent.reserve(records.size() - first);
        // Newest first, matching what the window shows.
        for (size_t i = records.size(); i-- > first;) recent.push_back(connectionObject(records[i]));
        std::vector<std::string> clients;
        std::vector<ClientGroup> groups = log->clients();
        clients.reserve(groups.size());
        for (const ClientGroup& g : groups) clients.push_back(clientObject(g));

        std::string body;
        jsonPutNumber(body, "accepted", static_cast<double>(log->accepted()));
        jsonPutNumber(body, "failed", static_cast<double>(log->failures()));
        jsonPutNumber(body, "lastRequestUnix", static_cast<double>(log->lastRequestUnix()));
        jsonPutNumber(body, "retained", static_cast<double>(records.size()));
        jsonPutNumber(body, "capacity", static_cast<double>(ConnectionLog::kCapacity));
        jsonPutRawArray(body, "clients", clients);
        jsonPutRawArray(body, "recent", recent);
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

double elapsedMs(std::chrono::steady_clock::time_point started) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    return static_cast<double>(us) / 1000.0;
}

// "127.0.0.1:53112" for an accepted socket, empty when the peer is gone.
std::string peerName(SocketHandle client) {
    sockaddr_in peer{};
#ifdef _WIN32
    int length = sizeof(peer);
#else
    socklen_t length = sizeof(peer);
#endif
    if (getpeername(client, reinterpret_cast<sockaddr*>(&peer), &length) != 0) return std::string();
    char host[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, host, INET_ADDRSTRLEN);
    return std::string(host) + ":" + std::to_string(ntohs(peer.sin_port));
}

// The "error" member of an {"ok":false,...} response, for the log's detail column.
std::string responseError(const std::string& response) {
    std::map<std::string, JsonField> fields;
    std::string parseError;
    if (!parseFlatJsonObject(response, fields, parseError)) return std::string();
    auto it = fields.find("error");
    return it == fields.end() ? std::string() : it->second.text;
}

void serveConnection(SocketHandle client, ResourceIndex& index, const std::string& token,
                     long long startUnix, bool verbose, int localPort, ConnectionLog& log) {
    ConnectionRecord record;
    record.peer = peerName(client);

    std::string line;
    const auto started = std::chrono::steady_clock::now();
    if (!readLine(client, line)) {
        record.detail = "no request within the timeout";
        record.durationMs = elapsedMs(started);
        log.add(std::move(record));
        closeSocket(client);
        return;
    }
    record.requestBytes = line.size() + 1;

    // Parsed here as well as inside handleRequestLine(): the log needs the
    // command and the caller's own "client" tag, and a request line is small.
    std::map<std::string, JsonField> fields;
    std::string parseError;
    if (parseFlatJsonObject(line, fields, parseError)) {
        record.command = fieldStr(fields, "cmd");
        record.client = fieldStr(fields, "client");
        record.detail = fieldStr(fields, "path");
    }

    std::string response = handleRequestLine(line, index, token, startUnix, &log);
    record.ok = response.rfind("{\"ok\":true", 0) == 0;
    response.push_back('\n');
    record.responseBytes = response.size();
    if (!record.ok) {
        record.detail = responseError(response);
        if (record.detail.empty()) record.detail = parseError;
    }
    if (verbose) {
        std::fprintf(stderr, "request from %s: %s (%s)\n", record.peer.c_str(), line.c_str(),
                     record.ok ? "ok" : "failed");
    }

    // The peer port is only in the TCP table while the socket is open, so the
    // owner lookup has to run before the close - and it must not be on the
    // response path: the client already has its answer.
    writeAll(client, response);
    queryConnectionOwner(record.peer, localPort, record.ownerPid, record.processName);
    record.durationMs = elapsedMs(started);
    log.add(std::move(record));
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

std::string secondsText(long long seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds < 60) return std::to_string(seconds) + "s";
    if (seconds < 3600) return std::to_string(seconds / 60) + "m" + std::to_string(seconds % 60) + "s";
    return std::to_string(seconds / 3600) + "h" + std::to_string((seconds % 3600) / 60) + "m";
}

std::string agoText(long long unixTime) {
    if (unixTime <= 0) return "never";
    return secondsText(std::max<long long>(0, static_cast<long long>(std::time(nullptr)) - unixTime)) +
           " ago";
}

// Everything the connections window shows. Built once per poll slice, and only
// while the window is on screen - copying the log is the expensive part.
WindowSnapshot makeWindowSnapshot(const ServerOptions& options, const ResourceIndex& index,
                                  const ConnectionLog& log, int port, bool editorRunning,
                                  long long startUnix) {
    WindowSnapshot snap;
    const long long now = static_cast<long long>(std::time(nullptr));

    std::vector<std::wstring>& lines = snap.headerLines;
    lines.push_back(wideFromUtf8("Project    " + options.projectDir.generic_u8string()));

    std::string listening = "Listening  127.0.0.1:" + std::to_string(port);
    listening += options.token.empty() ? "   (any local process can connect)"
                                       : "   (token protected)";
    lines.push_back(wideFromUtf8(listening));

    std::string roots;
    for (const std::string& root : index.foundRoots()) {
        if (!roots.empty()) roots += ", ";
        roots += root;
    }
    lines.push_back(wideFromUtf8(
        "Watching   " + std::to_string(index.fileCount()) + " files (scan " +
        std::to_string(static_cast<unsigned long long>(index.scanId())) + ")" +
        (roots.empty() ? "   no resource roots found" : "   roots: " + roots)));

    lines.push_back(wideFromUtf8(
        "Index      manifest " + index.manifestSha1().substr(0, 12) +
        "   last scan " + agoText(index.lastScanUnix())));

    std::string editorLine = "Editor     ";
    if (options.parentPid == 0) {
        editorLine += "started by hand, no editor pid given";
    } else {
        std::string name;
        queryProcessName(options.parentPid, name);
        editorLine += (name.empty() ? "pid " + std::to_string(options.parentPid)
                                    : name + " (pid " + std::to_string(options.parentPid) + ")") +
                      (editorRunning ? "  running" : "  gone");
    }
    lines.push_back(wideFromUtf8(editorLine));

    lines.push_back(wideFromUtf8(
        "Traffic    " + std::to_string(log.accepted()) + " requests, " +
        std::to_string(log.failures()) + " failed   uptime " + secondsText(now - startUnix) +
        "   last request " + agoText(log.lastRequestUnix())));

    snap.clients = log.clients();
    snap.records = log.records();
    // Any new request, or a clear, moves this and makes the window rebuild its rows.
    snap.generation = log.lastId() * (ConnectionLog::kCapacity + 1) + log.records().size();
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

    // Connections: the log is what both the window and the "connections"
    // command read, so it exists whether or not a window is open.
    ConnectionLog log;

    // A server the editor spawns stays in the tray (it is not the user's
    // window); one started by hand shows what it is doing. --window / --no-window
    // override either way.
    ServerWindow window;
    const bool wantWindow = !options.noWindow && (options.wantWindow || options.parentPid == 0);
    if (wantWindow) {
        if (!window.create("doriax-resource-server - connections") && options.verbose) {
            std::fprintf(stderr, "doriax-resource-server: no window (headless)\n");
        }
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
        // Only while it is on screen: copying the ring is the costly part, and a
        // hidden window is repainted from the same snapshot when it comes back.
        if (window.visible()) {
            window.setSnapshot(makeWindowSnapshot(options, index, log, actualPort, editorRunning,
                                                  startUnix));
        }

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
                serveConnection(client, index, options.token, startUnix, options.verbose,
                                actualPort, log);
        }

        // Shell messages are pumped right after the socket wait so a tray click
        // is seen within one slice (200 ms) without its own blocking wait.
        TrayChoice choice = tray.pump();
        window.pump();
        if (window.clearRequested()) {
            log.clear();
            window.acknowledgeClear();
        }
        if (choice == TrayChoice::ShowWindow) window.show();
        if (choice == TrayChoice::QuitServer || window.quitRequested()) break;
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
