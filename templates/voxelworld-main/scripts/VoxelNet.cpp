// (c) SR VINIX
// SPDX-License-Identifier: MIT

// VoxelNet.cpp - see VoxelNet.h. Single-threaded, non-blocking raw Winsock2: the whole protocol
// runs inside tickVoxelNet() on the main thread, so it may touch VoxelWorld directly with no
// locking. Nothing here may block a frame.
//
// ---------------------------------------------------------------------------
// Wire framing
// ---------------------------------------------------------------------------
// Every message on the single TCP stream is
//
//     [uint32 little-endian length][uint8 type][payload...]
//
// where `length` counts the type byte plus the payload, so a bare BYE has length 1. Length is
// capped at MAX_MSG_BYTES and a peer that sends more is dropped before its payload is ever
// allocated: a peer-supplied size must never reach a resize().
//
// All integers are little-endian and written byte by byte rather than by memcpy-ing a struct, so
// the stream is identical under MSVC and MinGW and matches Chunk::serialize(), which already is.
// Floats travel as IEEE754 little-endian 4 bytes; every platform this game targets is
// little-endian (x86, x64, Apple silicon).
//
// ---------------------------------------------------------------------------
// Message table
// ---------------------------------------------------------------------------
//   1 HELLO   client -> host   u16 protoVersion, u16 nameLen, name bytes
//   2 WELCOME host   -> client u32 assignedId, u64 worldSeed
//   3 POS     both ways        u32 playerId, f32 x, f32 y, f32 z, f32 yaw,
//                             u16 nameLen, name bytes
//   4 EDIT    client -> host   i32 x, i32 y, i32 z, u16 id, u16 previous
//   5 SET     host   -> all    i32 x, i32 y, i32 z, u16 id
//   6 CHUNK   host   -> client i32 cx, i32 cz, then Chunk::serialize() bytes verbatim
//   7 BYE     both ways        (no payload)
//
// Flow: a client sends HELLO and gets WELCOME (its id plus the host's seed), then sends POS every
// tick and EDIT for every block it changed. The host validates each EDIT - y in range, a known
// block id, inside a loaded column, and the block still being exactly the `previous` the client
// claims it saw - and applies it through VoxelWorld::setBlock(), which records it in
// pendingEdits(). Draining that journal into SET broadcasts is what makes the host authoritative:
// no client message ever becomes world state directly, only the host's own journal does. A
// rejected or stale EDIT is answered with one SET carrying the authoritative value, so the
// client's local prediction self-corrects instead of diverging.
//
// SET is applied on the receiving side with setBlockQuiet(), which neither re-records the edit nor
// marks the column dirty for saving - that distinction is what stops the network echoing forever.
//
// WELCOME's seed: if it differs from the client's own seed, the client keeps generating its own
// terrain for every column the host has not pushed, and only columns the host does send
// (CHUNK) overwrite it, because the host is authoritative about terrain it already has. A mismatch
// is a misconfigured game rather than a protocol failure, so netStats() reports it and nothing
// else changes.
//
// CHUNK is rate limited (CHUNKS_PER_TICK per client per tick, nearest column to that client's
// reported position first). Each host-side connection remembers the columns it has already queued
// or sent, so a client is never handed the same column twice and a column the host has not
// generated yet is simply retried on a later tick.

// Winsock traps this repo has already hit twice (see editor/cli/ResourceServerClient.cpp and
// tools/resource-server/ResourceServer.cpp): WIN32_LEAN_AND_MEAN must be defined before
// <windows.h> or windows.h drags in the v1 <winsock.h>, and <winsock2.h> must come BEFORE
// <ws2tcpip.h>. Get either wrong and the sockaddr/fd_set redefinition cascade fires roughly a
// hundred include levels deep through the CRT headers. Defining the macro at the very top of the
// TU, ahead of every include, keeps that true no matter what the project headers grow later.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include "VoxelNet.h"

#include "Log.h"
#include "VoxelChunk.h"
#include "VoxelConfig.h"
#include "VoxelWorld.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace voxel {

// ---------------------------------------------------------------------------
// Public surface implementation
// ---------------------------------------------------------------------------
namespace {

// ---- tuning ----
const int MAX_CLIENTS = 8;                      // 1 host + 8 clients: a LAN party
const uint32_t MAX_MSG_BYTES = 1024u * 1024u;   // hard cap on [length]; oversized drops the peer
const size_t SEND_QUEUE_MAX = 2u * 1024u * 1024u;   // per-connection backlog before the peer is dropped
const size_t RECV_BUFFER_MAX = 8u * 1024u * 1024u;  // un-parsed receive backlog ceiling
const size_t RECV_BYTES_PER_TICK = 512u * 1024u;    // read ceiling per connection per tick
const double STALL_TIMEOUT_S = 5.0;             // no send progress for this long -> drop the peer
const double CLIENT_SILENT_S = 6.0;             // no message from a peer for this long -> drop it
const double CONNECT_TIMEOUT_S = 8.0;           // non-blocking connect() deadline
const double REMOTE_EXPIRE_S = 5.0;             // a player not heard of for this long is forgotten
const double STATS_WINDOW_S = 1.0;
const int CHUNKS_PER_TICK = 2;                  // per client; a column is ~96 KB serialized
const int CHUNK_WRITE_SLICE = 64 * 1024;        // per send() call, so a frame cannot send a whole chunk
const size_t PENDING_CHUNKS_MAX = 24;           // client-side chunks awaiting their column
const int KNOWN_KEYS_MAX = 4096;                // per-client sent-column memory bound
const size_t NAME_MAX = 32;
const uint16_t PROTO_VERSION = 1;

enum MsgType : uint8_t {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_POS = 3,
    MSG_EDIT = 4,
    MSG_SET = 5,
    MSG_CHUNK = 6,
    MSG_BYE = 7,
};

double nowSec() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

int iabs(int v) { return v < 0 ? -v : v; }

// Floor division: casting a float coordinate to int truncates toward zero, but chunk columns need
// arithmetic floor so world block -1 lands in column -1 and not column 0. <cmath> is not in the
// allowed includes for this module.
int floorDiv(int v, int d) {
    int q = v / d;
    if ((v % d) != 0 && ((v < 0) != (d < 0))) q--;
    return q;
}
int chunkXOf(float w) { return floorDiv((int)w, CHUNK_SX); }
int chunkZOf(float w) { return floorDiv((int)w, CHUNK_SZ); }

// ---------------------------------------------------------------------------
// Byte writers / readers: little-endian, bounds-checked, exception-free
// ---------------------------------------------------------------------------
struct Writer {
    std::vector<uint8_t> b;

    void u16(uint16_t v) {
        b.push_back((uint8_t)(v & 0xFFu));
        b.push_back((uint8_t)((v >> 8) & 0xFFu));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; i++) b.push_back((uint8_t)((v >> (8 * i)) & 0xFFu));
    }
    void i32(int32_t v) { u32((uint32_t)v); }
    void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v >> 32)); }
    void f32(float v) {
        uint8_t t[4];
        std::memcpy(t, &v, 4);
        for (int i = 0; i < 4; i++) b.push_back(t[i]);
    }
    void str(const std::string& s) {
        uint16_t n = (uint16_t)(s.size() > NAME_MAX ? NAME_MAX : s.size());
        u16(n);
        for (uint16_t i = 0; i < n; i++) b.push_back((uint8_t)s[i]);
    }
};

struct Reader {
    const uint8_t* p;
    size_t left;
    bool ok = true;

    uint16_t u16() {
        if (left < 2) { ok = false; return 0; }
        uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
        p += 2; left -= 2; return v;
    }
    uint32_t u32() {
        if (left < 4) { ok = false; return 0; }
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                     ((uint32_t)p[3] << 24);
        p += 4; left -= 4; return v;
    }
    int32_t i32() { return (int32_t)u32(); }
    uint64_t u64() {
        uint32_t lo = u32();
        uint32_t hi = u32();
        return ((uint64_t)hi << 32) | lo;
    }
    float f32() {
        if (left < 4) { ok = false; return 0.0f; }
        float v = 0.0f;
        std::memcpy(&v, p, 4);
        p += 4; left -= 4;
        return v;
    }
    std::string str() {
        uint16_t n = u16();
        if (!ok || (size_t)n > left || (size_t)n > NAME_MAX) { ok = false; return std::string(); }
        std::string s((const char*)p, (size_t)n);
        p += n; left -= n;
        return s;
    }
};

// [length][type][payload]
std::vector<uint8_t> frame(MsgType type, const std::vector<uint8_t>& payload) {
    uint32_t len = (uint32_t)(payload.size() + 1);
    Writer w;
    w.u32(len);
    std::vector<uint8_t> out = w.b;
    out.push_back((uint8_t)type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// ---------------------------------------------------------------------------
// Per-connection state
// ---------------------------------------------------------------------------
// `sock` is kept as a uintptr_t so this struct is well-formed on every platform even though only
// Winsock ever puts a real handle in it.
struct Conn {
    bool active = false;
    uintptr_t sock = ~(uintptr_t)0;
    bool linked = false;      // socket open; for a client, connect() also completed
    bool greeting = false;    // client: HELLO sent and WELCOME in; host: WELCOME sent
    int id = 0;               // player id. The host is 0, clients are 1..MAX_CLIENTS
    std::string name;

    std::vector<uint8_t> send;
    size_t sendPos = 0;
    std::vector<uint8_t> recv;
    size_t recvPos = 0;

    double opened = 0;
    double lastRead = 0;
    double lastSendProgress = 0;

    float x = 0, y = 0, z = 0, yaw = 0;   // last position reported by this peer
    bool positioned = false;              // any POS seen yet? Before that we cannot aim chunks

    std::vector<int64_t> chunkKeys;       // host: columns queued or sent to this peer (sorted)
};

// ---------------------------------------------------------------------------
// Module state: one session at a time, started and stopped around the game
// ---------------------------------------------------------------------------
bool g_wsaUp = false;             // WSAStartup/WSACleanup paired once per process session
NetMode g_mode = NetMode::OFFLINE;
bool g_running = false;
bool g_quitting = false;          // stopVoxelNet(): a failed best-effort BYE is not an error
std::string g_error;

uintptr_t g_listen = ~(uintptr_t)0;
Conn g_clients[MAX_CLIENTS];      // host side
Conn g_link;                      // client side: its one connection to the host

// Resolved peer address, kept platform-neutral so the struct graph above never names Winsock.
bool g_peerResolved = false;
int g_peerFamily = 0;
uint32_t g_peerIpv4 = 0;          // network byte order
uint16_t g_peerPortNet = 0;       // network byte order

int g_localId = 0;                // 0 for the host, assigned by WELCOME for a client
std::string g_localName = "player";  // how the other ends label us
uint64_t g_localSeed = 0;         // this process's world seed
uint64_t g_hostSeed = 0;          // seed advertised by the host (client) / our own (host)
bool g_seedMismatch = false;
int g_port = 0;
std::string g_hostLabel;

struct RemoteEntry {
    int id = 0;
    std::string name;
    float x = 0, y = 0, z = 0, yaw = 0;
    double lastSeen = 0;
};
std::vector<RemoteEntry> g_remoteStore;
std::vector<RemotePlayer> g_remote;

struct PendingChunk {
    int cx = 0, cz = 0;
    double when = 0;
    std::vector<uint8_t> bytes;
};
std::vector<PendingChunk> g_pendingChunks;

// ---- stats ----
double g_statsStart = 0;
uint64_t g_editsWindow = 0;
uint64_t g_bytesSentWindow = 0;
uint64_t g_bytesRecvWindow = 0;
uint32_t g_editsPerSec = 0;
uint32_t g_kbytesPerSec = 0;
uint64_t g_rejectedTotal = 0;

void setError(const std::string& msg) { if (!msg.empty()) g_error = msg; }

// VOXEL_NETLOG=1 makes the wire talk: one line per accepted peer, per edit applied from the other
// side and per refusal. Everything else this module reports is a rate (netStats), and a rate cannot
// answer the only question a host-authoritative layer has to answer - did *this* edit reach *that*
// peer - which is what the two-process round-trip harness reads out of a log. Off by default: a
// chunk push is tens of KB and an edit storm would print a line per block.
bool traceOn() {
    static const bool on = [] {
#ifdef _MSC_VER
        char* buffer = nullptr;
        size_t length = 0;
        if (_dupenv_s(&buffer, &length, "VOXEL_NETLOG") != 0 || buffer == nullptr) return false;
        const bool v = buffer[0] == '1';
        free(buffer);
        return v;
#else
        const char* v = std::getenv("VOXEL_NETLOG");
        return v && v[0] == '1';
#endif
    }();
    return on;
}

void trace(const char* fmt, ...) {
    if (!traceOn()) return;
    char line[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    quantum::Log::print("NET %s", line);
}

int liveClientCount() {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (g_clients[i].active && g_clients[i].linked) n++;
    return n;
}

// ---------------------------------------------------------------------------
// Forward declarations (the paths reference each other: enqueue can drop, dropping needs the
// connection table, the handlers need the senders)
// ---------------------------------------------------------------------------
void enqueue(Conn& c, const std::vector<uint8_t>& msg);
void flushSend(Conn& c);
bool readSome(Conn& c);
void parseFrames(Conn& c, VoxelWorld& world, bool asHost);
void dropConnection(Conn& c, const std::string& why);
void sendSetTo(Conn& c, int x, int y, int z, uint16_t id);
void sendHello();
void handleHello(Conn& c, Reader& r);
void handleWelcome(Reader& r);
void handlePos(Conn& c, Reader& r);
void handleEdit(Conn& c, Reader& r, VoxelWorld& world);
void handleSet(VoxelWorld& world, Reader& r);
void handleChunk(VoxelWorld& world, Reader& r);
void touchRemote(int id, const std::string& name, float x, float y, float z, float yaw);

// ---------------------------------------------------------------------------
// Platform layer. Only these functions name Winsock types; everything above is portable, which is
// what lets the file compile where networking is not supported at all.
// ---------------------------------------------------------------------------
#ifdef _WIN32

using Socklen = int;   // MSVC has no socklen_t; the Winsock prototypes take int
const uintptr_t kInvalidSocket = (uintptr_t)INVALID_SOCKET;

bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }

void closeSocket(uintptr_t s) {
    if (s == kInvalidSocket || s == ~(uintptr_t)0) return;
    ::closesocket((SOCKET)s);
}

bool setNonBlocking(uintptr_t s) {
    u_long one = 1;
    return ioctlsocket((SOCKET)s, FIONBIO, &one) == 0;
}

bool setNoDelay(uintptr_t s) {
    int on = 1;
    return ::setsockopt((SOCKET)s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on)) == 0;
}

std::string socketErrorText(const char* what) {
    return std::string(what) + " failed (winsock error " + std::to_string(WSAGetLastError()) + ")";
}

// Zero-timeout writability probe. It is used for the non-blocking connect() completion only:
// every other path just writes and reacts to WSAEWOULDBLOCK, so a frame never spends time blocked
// in the kernel.
bool socketWritableNow(uintptr_t s) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET((SOCKET)s, &set);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    return ::select(0, nullptr, &set, nullptr, &tv) > 0;
}

bool socketReadableNow(uintptr_t s) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET((SOCKET)s, &set);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    return ::select(0, &set, nullptr, nullptr, &tv) > 0;
}

bool socketFailed(uintptr_t s, int& errOut) {
    errOut = 0;
    Socklen len = (Socklen)sizeof(errOut);
    if (::getsockopt((SOCKET)s, SOL_SOCKET, SO_ERROR, (char*)&errOut, &len) != 0) return true;
    return errOut != 0;
}

// getaddrinfo, so "localhost" and a dotted IPv4 literal both work. Only IPv4 is asked for: this
// is a LAN game and a mixed family result would make the connect path ambiguous.
bool resolvePeer(const std::string& host, int port) {
    g_peerResolved = false;
    if (host.empty() || port <= 0 || port > 65535) { setError("invalid host or port"); return false; }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (rc != 0 || res == nullptr) {
        setError("cannot resolve \"" + host + "\" (" + std::to_string(rc) + ")");
        return false;
    }
    bool ok = false;
    // Take the first usable answer; a multi-homed name resolves to several entries and TCP
    // reconnect on a LAN rarely needs to try them all.
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_addrlen < sizeof(sockaddr_in) || ai->ai_addr == nullptr) continue;
        const sockaddr_in* a = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
        g_peerFamily = a->sin_family;
        g_peerIpv4 = a->sin_addr.s_addr;      // already network byte order
        g_peerPortNet = htons((unsigned short)port);
        ok = true;
        break;
    }
    ::freeaddrinfo(res);
    if (!ok) setError("no usable address for \"" + host + "\"");
    g_peerResolved = ok;
    return ok;
}

#else  // !_WIN32: inert socket layer, so the module still compiles and links elsewhere.

using Socklen = int;
const uintptr_t kInvalidSocket = ~(uintptr_t)0;

bool wouldBlock() { return false; }
void closeSocket(uintptr_t) {}
bool setNonBlocking(uintptr_t) { return false; }
bool setNoDelay(uintptr_t) { return false; }
std::string socketErrorText(const char* what) { return std::string(what) + " failed"; }
bool socketWritableNow(uintptr_t) { return false; }
bool socketReadableNow(uintptr_t) { return false; }
bool socketFailed(uintptr_t, int& errOut) { errOut = 0; return true; }
bool resolvePeer(const std::string&, int) {
    setError("networking not supported");
    return false;
}

// startVoxelNet() on this platform answers "networking not supported" and reaches none of the
// helpers above, which would leave them unreferenced and noisy under -Wall. Touch them once, here,
// so the file stays clean wherever there is no socket layer at all.
bool netUnsupported() {
    int err = 0;
    closeSocket(kInvalidSocket);
    return wouldBlock() || !setNonBlocking(kInvalidSocket) || !setNoDelay(kInvalidSocket) ||
           socketWritableNow(kInvalidSocket) || socketReadableNow(kInvalidSocket) ||
           socketFailed(kInvalidSocket, err) || resolvePeer("localhost", 0) ||
           socketErrorText("net").empty();
}

#endif

// ---------------------------------------------------------------------------
// Send queue. Partial writes stay queued and drain across frames, so a slow peer throttles itself
// - and is dropped after STALL_TIMEOUT_S of no progress - instead of throttling the server.
// ---------------------------------------------------------------------------
void compactSend(Conn& c) {
    if (c.sendPos == 0) return;
    if (c.sendPos >= c.send.size()) { c.send.clear(); c.sendPos = 0; return; }
    if (c.sendPos < 64u * 1024u) return;   // not worth the memmove yet
    c.send.erase(c.send.begin(), c.send.begin() + (ptrdiff_t)c.sendPos);
    c.sendPos = 0;
}

void enqueue(Conn& c, const std::vector<uint8_t>& msg) {
    if (!c.active) return;
    compactSend(c);
    if (c.send.size() - c.sendPos + msg.size() > SEND_QUEUE_MAX) {
        // The peer is not reading. Drop it rather than grow an unbounded backlog.
        c.send.clear();
        c.sendPos = 0;
        dropConnection(c, "send queue overflow (peer stopped reading)");
        return;
    }
    c.send.insert(c.send.end(), msg.begin(), msg.end());
}

void flushSend(Conn& c) {
    compactSend(c);
    if (c.sendPos >= c.send.size()) { c.lastSendProgress = nowSec(); return; }
#ifdef _WIN32
    while (c.sendPos < c.send.size()) {
        size_t want = c.send.size() - c.sendPos;
        if (want > (size_t)CHUNK_WRITE_SLICE) want = (size_t)CHUNK_WRITE_SLICE;
        int n = ::send((SOCKET)c.sock, (const char*)c.send.data() + c.sendPos, (int)want, 0);
        if (n > 0) {
            c.sendPos += (size_t)n;
            g_bytesSentWindow += (uint64_t)n;
            if ((size_t)n < want) break;   // kernel buffer filled: the rest waits for next tick
            continue;
        }
        if (n < 0 && wouldBlock()) break;  // would block: leave it queued, no frame time lost
        dropConnection(c, socketErrorText("send"));
        return;
    }
#endif
    if (c.sendPos >= c.send.size()) c.lastSendProgress = nowSec();
}

// ---------------------------------------------------------------------------
// Receive side: pull what is ready (bounded per tick), then parse only whole frames. An incomplete
// tail stays buffered for the next tick - TCP gives ordering, we only supply framing.
// ---------------------------------------------------------------------------
bool readSome(Conn& c) {
#ifdef _WIN32
    if (!c.active || c.sock == kInvalidSocket) return false;
    size_t budget = RECV_BYTES_PER_TICK;
    uint8_t buf[8192];
    while (budget > 0 && socketReadableNow(c.sock)) {
        int n = ::recv((SOCKET)c.sock, (char*)buf, (int)sizeof(buf), 0);
        if (n > 0) {
            budget -= (size_t)n;
            g_bytesRecvWindow += (uint64_t)n;
            if (c.recv.size() - c.recvPos + (size_t)n > RECV_BUFFER_MAX) {
                dropConnection(c, "receive buffer overflow");
                return false;
            }
            c.recv.insert(c.recv.end(), buf, buf + n);
            if (n < (int)sizeof(buf)) return true;   // drained for now
            continue;
        }
        if (n == 0) { dropConnection(c, "peer closed the connection"); return false; }
        if (wouldBlock()) return true;
        // A peer whose process exits without a BYE or an orderly close is a reset, not a fault of
        // ours. It gets the same words as an orderly drop, because dropConnection() keeps that phrase
        // out of netError() for a client slot - otherwise the host's overlay carries "recv failed
        // (winsock error 10054)" for the rest of the session after one ordinary disconnect.
        const int err = WSAGetLastError();
        if (err == WSAECONNRESET || err == WSAECONNABORTED) {
            dropConnection(c, "peer closed the connection");
            return false;
        }
        dropConnection(c, socketErrorText("recv"));
        return false;
    }
#else
    (void)c;
#endif
    return c.active;
}

void compactRecv(Conn& c) {
    if (c.recvPos == 0) return;
    if (c.recvPos >= c.recv.size()) { c.recv.clear(); c.recvPos = 0; return; }
    if (c.recvPos < 64u * 1024u) return;
    c.recv.erase(c.recv.begin(), c.recv.begin() + (ptrdiff_t)c.recvPos);
    c.recvPos = 0;
}

void parseFrames(Conn& c, VoxelWorld& world, bool asHost) {
    for (;;) {
        size_t avail = c.recv.size() - c.recvPos;
        if (avail < 4) break;
        const uint8_t* q = c.recv.data() + c.recvPos;
        uint32_t len = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) |
                       ((uint32_t)q[3] << 24);
        // Validate the peer's length before touching memory for it.
        if (len < 1 || len > MAX_MSG_BYTES) {
            dropConnection(c, "oversized message length (1 MB cap)");
            return;
        }
        if (avail < (size_t)len + 4) break;   // still in flight
        const uint8_t type = q[4];
        const uint8_t* payload = q + 5;
        c.recvPos += 4 + (size_t)len;
        c.lastRead = nowSec();

        Reader r{ payload, (size_t)len - 1 };
        switch (type) {
            case MSG_HELLO:   if (asHost) handleHello(c, r); break;
            case MSG_WELCOME: if (!asHost) handleWelcome(r); break;
            case MSG_POS:     handlePos(c, r); break;
            case MSG_EDIT:    if (asHost) handleEdit(c, r, world); break;
            case MSG_SET:     if (!asHost) handleSet(world, r); break;
            case MSG_CHUNK:   if (!asHost) handleChunk(world, r); break;
            case MSG_BYE:     dropConnection(c, std::string()); return;
            default:
                dropConnection(c, "unknown message type " + std::to_string((int)type));
                return;
        }
        if (!c.active) return;   // a handler closed us
        if (c.recv.size() - c.recvPos > RECV_BUFFER_MAX) {
            dropConnection(c, "receive buffer overflow");
            return;
        }
    }
    compactRecv(c);
}

// ---------------------------------------------------------------------------
// Remote players. POS entries expire, which is how a client that vanished without BYE leaves the
// HUD and the avatar list without needing an extra message type.
// ---------------------------------------------------------------------------
void touchRemote(int id, const std::string& name, float x, float y, float z, float yaw) {
    for (size_t i = 0; i < g_remoteStore.size(); i++) {
        if (g_remoteStore[i].id != id) continue;
        g_remoteStore[i].x = x; g_remoteStore[i].y = y;
        g_remoteStore[i].z = z; g_remoteStore[i].yaw = yaw;
        g_remoteStore[i].lastSeen = nowSec();
        if (!name.empty()) g_remoteStore[i].name = name;
        return;
    }
    RemoteEntry e;
    e.id = id; e.name = name; e.x = x; e.y = y; e.z = z; e.yaw = yaw;
    e.lastSeen = nowSec();
    g_remoteStore.push_back(e);
}

void forgetRemote(int id) {
    for (size_t i = 0; i < g_remoteStore.size(); i++) {
        if (g_remoteStore[i].id == id) {
            g_remoteStore.erase(g_remoteStore.begin() + (ptrdiff_t)i);
            return;
        }
    }
}

void rebuildRemoteList() {
    const double now = nowSec();
    g_remote.clear();
    for (size_t i = 0; i < g_remoteStore.size(); i++) {
        if (g_remoteStore[i].id == g_localId) continue;
        if (now - g_remoteStore[i].lastSeen > REMOTE_EXPIRE_S) continue;
        RemotePlayer p;
        p.id = g_remoteStore[i].id;
        p.name = g_remoteStore[i].name;
        p.x = g_remoteStore[i].x; p.y = g_remoteStore[i].y;
        p.z = g_remoteStore[i].z; p.yaw = g_remoteStore[i].yaw;
        g_remote.push_back(p);
    }
    std::sort(g_remote.begin(), g_remote.end(),
              [](const RemotePlayer& a, const RemotePlayer& b) { return a.id < b.id; });
    // Drop long-dead entries so the store cannot accumulate across a long session.
    for (size_t i = g_remoteStore.size(); i-- > 0;) {
        if (now - g_remoteStore[i].lastSeen > REMOTE_EXPIRE_S * 4.0)
            g_remoteStore.erase(g_remoteStore.begin() + (ptrdiff_t)i);
    }
}

// ---------------------------------------------------------------------------
// Message senders
// ---------------------------------------------------------------------------
void sendSetTo(Conn& c, int x, int y, int z, uint16_t id) {
    Writer w;
    w.i32(x); w.i32(y); w.i32(z); w.u16(id);
    enqueue(c, frame(MSG_SET, w.b));
}

void broadcastSet(int x, int y, int z, uint16_t id) {
    Writer w;
    w.i32(x); w.i32(y); w.i32(z); w.u16(id);
    std::vector<uint8_t> msg = frame(MSG_SET, w.b);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].active && g_clients[i].linked) enqueue(g_clients[i], msg);
}

// The host is a player too: it broadcasts its own POS as id 0 so clients can draw it.
void broadcastHostPos(float x, float y, float z, float yaw) {
    Writer w;
    w.u32(0);
    w.f32(x); w.f32(y); w.f32(z); w.f32(yaw);
    w.str("host");
    std::vector<uint8_t> msg = frame(MSG_POS, w.b);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].active && g_clients[i].linked) enqueue(g_clients[i], msg);
}

void sendLinkPos(float x, float y, float z, float yaw, const std::string& name) {
    Writer w;
    w.u32((uint32_t)(g_localId < 0 ? 0 : g_localId));
    w.f32(x); w.f32(y); w.f32(z); w.f32(yaw);
    w.str(name);
    enqueue(g_link, frame(MSG_POS, w.b));
}

// Our own handshake. The host answers it with WELCOME, which carries the id we show as and the
// seed the world was generated from.
void sendHello() {
    Writer w;
    w.u16(PROTO_VERSION);
    w.str(g_localName);
    enqueue(g_link, frame(MSG_HELLO, w.b));
    g_link.greeting = false;   // not until WELCOME says so
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
void handleHello(Conn& c, Reader& r) {
    uint16_t version = r.u16();
    std::string name = r.str();
    if (!r.ok) { dropConnection(c, "malformed HELLO"); return; }
    if (version != PROTO_VERSION) {
        const std::string why = "protocol version mismatch (host " +
                                std::to_string((int)PROTO_VERSION) + ", peer " +
                                std::to_string((int)version) + ")";
        setError(why);
        dropConnection(c, why);
        return;
    }
    if (!name.empty()) c.name = name;
    c.greeting = true;

    Writer w;
    w.u32((uint32_t)c.id);
    w.u64(g_hostSeed);
    enqueue(c, frame(MSG_WELCOME, w.b));
    trace("host: peer %d (%s) linked, seed %llu", c.id, c.name.c_str(),
          (unsigned long long)g_hostSeed);
}

void handleWelcome(Reader& r) {
    uint32_t id = r.u32();
    uint64_t seed = r.u64();
    if (!r.ok) { dropConnection(g_link, "malformed WELCOME"); return; }
    g_localId = (int)id;
    g_hostSeed = seed;
    g_link.greeting = true;
    // Now that the host has assigned an id, name ourselves with it: every client would otherwise
    // introduce itself as the same generic "player" in each other's HUD.
    g_localName = "player" + std::to_string(g_localId);
    // A different seed is survivable, not fatal: the client keeps its own generation for columns
    // the host never pushes, and only CHUNK messages override it. Say so in the overlay.
    g_seedMismatch = (seed != 0 && g_localSeed != 0 && seed != g_localSeed);
    trace("client: welcomed as %d by seed %llu%s", g_localId,
          (unsigned long long)g_hostSeed, g_seedMismatch ? " (SEED MISMATCH)" : "");
}

void handlePos(Conn& c, Reader& r) {
    uint32_t id = r.u32();
    float x = r.f32(), y = r.f32(), z = r.f32(), yaw = r.f32();
    std::string name = r.str();
    if (!r.ok) { dropConnection(c, "malformed POS"); return; }

    if (g_mode == NetMode::HOST) {
        // The host owns the id space: the connection's assigned id wins over any id the peer
        // claims, so a client cannot impersonate another player.
        c.x = x; c.y = y; c.z = z; c.yaw = yaw;
        c.positioned = true;
        if (!name.empty()) c.name = name;
        touchRemote(c.id, c.name, x, y, z, yaw);

        Writer w;
        w.u32((uint32_t)c.id);
        w.f32(x); w.f32(y); w.f32(z); w.f32(yaw);
        w.str(c.name);
        std::vector<uint8_t> msg = frame(MSG_POS, w.b);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (&g_clients[i] != &c && g_clients[i].active && g_clients[i].linked)
                enqueue(g_clients[i], msg);
        }
    } else {
        // Relayed by the host: id 0 is the host itself, anything else is another client.
        touchRemote((int)id, name, x, y, z, yaw);
    }
}

// The host's authority check for one client edit. Deliberately cheap - geometry and the optimistic
// `previous` match, no hardness or ownership rules (the game has no inventory yet) - but that
// match is what stops two clients breaking the same block twice.
void handleEdit(Conn& c, Reader& r, VoxelWorld& world) {
    int x = r.i32(), y = r.i32(), z = r.i32();
    uint16_t id = r.u16();
    uint16_t previous = r.u16();
    if (!r.ok) { dropConnection(c, "malformed EDIT"); return; }

    const int cx = floorDiv(x, CHUNK_SX), cz = floorDiv(z, CHUNK_SZ);
    const Chunk* col = world.chunk(cx, cz);
    const bool inRange = (y >= 0 && y < WORLD_HEIGHT);
    const bool known = (col != nullptr && col->generated && inRange);
    // A state type the block table does not describe can be sent by a stale or modded client.
    const bool badId = (stateType(id) >= BLOCK_COUNT);

    if (badId || !known) {
        // Refuse, and always answer with one SET so the client's local prediction does not
        // survive: the authoritative value wherever the host actually has the column, otherwise
        // the client's own `previous`, which simply puts back what it broke. Staying silent here
        // (a column the host never streamed, for instance) would let the two worlds diverge with
        // nobody the wiser.
        g_rejectedTotal++;
        trace("host: REFUSED edit %d %d %d id %d (%s), answered with %d", x, y, z, (int)id,
              badId ? "unknown block" : (!inRange ? "outside the world" : "column not streamed"),
              (int)(known ? world.get(x, y, z) : previous));
        sendSetTo(c, x, y, z, known ? world.get(x, y, z) : previous);
        return;
    }

    const uint16_t here = world.get(x, y, z);
    if (here != previous) {
        // Stale prediction: the client saw a different block than the host has now, so its edit is
        // out of date and the host's value wins.
        g_rejectedTotal++;
        trace("host: REFUSED stale edit %d %d %d id %d (expected %d there, holds %d)", x, y, z,
              (int)id, (int)previous, (int)here);
        sendSetTo(c, x, y, z, here);
        return;
    }
    // setBlock(), not setBlockQuiet(): this is the only way a remote intent becomes world state,
    // and the journal entry it leaves is what this tick's drain broadcasts to everyone. Counting
    // happens there, not here, so an edit is never counted twice.
    world.setBlock(x, y, z, id);
    trace("host: APPLIED edit from %d at %d %d %d -> id %d, now %d", c.id, x, y, z, (int)id,
          (int)world.get(x, y, z));
}

void handleSet(VoxelWorld& world, Reader& r) {
    int x = r.i32(), y = r.i32(), z = r.i32();
    uint16_t id = r.u16();
    if (!r.ok) return;
    if (y < 0 || y >= WORLD_HEIGHT || stateType(id) >= BLOCK_COUNT) return;
    // Quiet, always: a remote edit must not enter our own pendingEdits(), or every SET received
    // would be broadcast again and the network would echo forever.
    const uint16_t was = world.setBlockQuiet(x, y, z, id);
    if (was == id) return;   // already correct, nothing to remesh
    trace("%s: SET applied at %d %d %d: %d -> %d", g_mode == NetMode::HOST ? "host" : "client",
          x, y, z, (int)was, (int)world.get(x, y, z));
    world.requestRemesh(floorDiv(x, CHUNK_SX), floorDiv(z, CHUNK_SZ));
    g_editsWindow++;
}

// 1 = written into the column, 0 = column not loaded locally yet, -1 = payload rejected.
int applyChunkBytes(VoxelWorld& world, int cx, int cz, const uint8_t* data, size_t size) {
    Chunk* col = world.chunk(cx, cz);
    if (!col) return 0;
    // Chunk::deserialize validates its own magic, height and volume, so a corrupt or foreign
    // payload is refused rather than trusted.
    if (!col->deserialize(data, size)) return -1;
    // deserialize() sets generated + dirtyMesh; ask for this column and its seam neighbours so the
    // mesher rebuilds the faces that met the old local version.
    world.requestRemesh(cx, cz);
    world.requestRemesh(cx - 1, cz); world.requestRemesh(cx + 1, cz);
    world.requestRemesh(cx, cz - 1); world.requestRemesh(cx, cz + 1);
    return 1;
}

void handleChunk(VoxelWorld& world, Reader& r) {
    int cx = r.i32(), cz = r.i32();
    if (!r.ok || r.left == 0) return;
    const int applied = applyChunkBytes(world, cx, cz, r.p, r.left);
    if (applied != 0) return;   // written, or a payload we refuse to keep retrying

    // The column is not loaded locally yet - our own streaming reaches it a few frames later.
    // Queue our own copy of the bytes (never the peer's pointer) and retry; the payload was
    // already bounded by the frame length check.
    for (size_t i = 0; i < g_pendingChunks.size(); i++) {
        if (g_pendingChunks[i].cx == cx && g_pendingChunks[i].cz == cz) {
            g_pendingChunks[i].when = nowSec();
            g_pendingChunks[i].bytes.assign(r.p, r.p + r.left);
            return;
        }
    }
    if (g_pendingChunks.size() >= PENDING_CHUNKS_MAX)
        g_pendingChunks.erase(g_pendingChunks.begin());   // keep the freshest
    PendingChunk pc;
    pc.cx = cx; pc.cz = cz; pc.when = nowSec();
    pc.bytes.assign(r.p, r.p + r.left);
    g_pendingChunks.push_back(std::move(pc));
}

// Retry host chunks that arrived before their column existed locally.
void applyPendingChunks(VoxelWorld& world) {
    const double now = nowSec();
    for (size_t i = g_pendingChunks.size(); i-- > 0;) {
        PendingChunk& pc = g_pendingChunks[i];
        if (now - pc.when > 20.0) {
            g_pendingChunks.erase(g_pendingChunks.begin() + (ptrdiff_t)i);
            continue;
        }
        if (applyChunkBytes(world, pc.cx, pc.cz, pc.bytes.data(), pc.bytes.size()) != 0)
            g_pendingChunks.erase(g_pendingChunks.begin() + (ptrdiff_t)i);
    }
}

// ---------------------------------------------------------------------------
// Teardown of one peer
// ---------------------------------------------------------------------------
void dropConnection(Conn& c, const std::string& why) {
    if (!c.active) return;
    const bool isClientSlot = (&c >= &g_clients[0] && &c < &g_clients[0] + MAX_CLIENTS);
    closeSocket(c.sock);
    c.sock = ~(uintptr_t)0;
    const int lostId = c.id;
    c.active = false;
    c.linked = false;
    c.greeting = false;
    c.send.clear(); c.sendPos = 0;
    c.recv.clear(); c.recvPos = 0;
    c.chunkKeys.clear();
    c.name.clear();

    if (isClientSlot) {
        forgetRemote(lostId);
        // A peer leaving is normal traffic; only a real fault belongs in netError(), and nothing
        // is a fault while we are the one hanging up.
        if (!why.empty() && !g_quitting && why != "peer closed the connection") setError(why);
        return;
    }
    // The client's own link to the host went down: this is a hard error for the session, and the
    // integrator can restart the game in OFFLINE or try again.
    g_running = false;
    g_localId = -1;
    g_link.greeting = false;
    if (!why.empty() && !g_quitting) setError(why);
}

// ---------------------------------------------------------------------------
// Host: accept, and stream columns to the clients that entered the view radius
// ---------------------------------------------------------------------------
void acceptClients() {
#ifdef _WIN32
    for (int guard = 0; guard < MAX_CLIENTS; guard++) {
        sockaddr_in sa{};
        Socklen len = (Socklen)sizeof(sa);
        SOCKET s = ::accept((SOCKET)g_listen, (sockaddr*)&sa, &len);
        if (s == INVALID_SOCKET) {
            if (!wouldBlock()) setError(socketErrorText("accept"));
            return;
        }
        if (!setNonBlocking(s) || !setNoDelay(s)) {
            setError(socketErrorText("listen socket setup"));
            ::closesocket(s);
            continue;
        }
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].active) { slot = i; break; }
        }
        if (slot < 0) {
            // Full: refuse outright instead of half-connecting an extra peer.
            setError("server full (" + std::to_string(MAX_CLIENTS) + " clients)");
            ::closesocket(s);
            return;
        }
        Conn& c = g_clients[slot];
        const double now = nowSec();
        c.active = true;
        c.sock = (uintptr_t)s;
        c.linked = true;
        c.greeting = false;
        c.id = slot + 1;   // slot-based, so an id is never duplicated while another is live
        c.name = "player" + std::to_string(c.id);
        c.opened = now; c.lastRead = now; c.lastSendProgress = now;
        c.x = c.y = c.z = c.yaw = 0.0f;
        c.positioned = false;
        c.send.clear(); c.sendPos = 0;
        c.recv.clear(); c.recvPos = 0;
        c.chunkKeys.clear();
    }
#endif
}

bool keySeen(const Conn& c, int64_t k) {
    return std::binary_search(c.chunkKeys.begin(), c.chunkKeys.end(), k);
}

void markKeySeen(Conn& c, int64_t k) {
    std::vector<int64_t>::iterator it = std::lower_bound(c.chunkKeys.begin(), c.chunkKeys.end(), k);
    if (it != c.chunkKeys.end() && *it == k) return;
    c.chunkKeys.insert(it, k);
}

// Forget columns far behind a client so the remembered set cannot grow without bound while
// someone explores. If they walk back, the host offers the column again.
void pruneSeenKeys(Conn& c, int ccx, int ccz, int radius) {
    if ((int)c.chunkKeys.size() <= KNOWN_KEYS_MAX) return;
    std::vector<int64_t> keep;
    keep.reserve(c.chunkKeys.size());
    for (size_t i = 0; i < c.chunkKeys.size(); i++) {
        if (iabs(chunkKeyX(c.chunkKeys[i]) - ccx) <= radius &&
            iabs(chunkKeyZ(c.chunkKeys[i]) - ccz) <= radius)
            keep.push_back(c.chunkKeys[i]);
    }
    c.chunkKeys.swap(keep);
}

void pushChunks(VoxelWorld& world) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Conn& c = g_clients[i];
        if (!c.active || !c.linked) continue;
        if (!c.positioned) continue;   // no POS yet: aiming at (0,0) would ship the wrong columns
        const int ccx = chunkXOf(c.x), ccz = chunkZOf(c.z);
        const int radius = world.viewRadius;

        std::vector<std::pair<double, int64_t>> want;
        for (int dz = -radius; dz <= radius; dz++) {
            for (int dx = -radius; dx <= radius; dx++) {
                const int cx = ccx + dx, cz = ccz + dz;
                const int64_t key = chunkKey(cx, cz);
                if (keySeen(c, key)) continue;            // queued or sent already
                const Chunk* col = world.chunk(cx, cz);
                if (!col || !col->generated) continue;    // host lacks it: retry a later tick
                want.push_back(std::make_pair((double)(dx * dx + dz * dz), key));
            }
        }
        std::sort(want.begin(), want.end());   // nearest first

        int sent = 0;
        for (size_t k = 0; k < want.size() && sent < CHUNKS_PER_TICK; k++) {
            const int cx = chunkKeyX(want[k].second), cz = chunkKeyZ(want[k].second);
            const Chunk* col = world.chunk(cx, cz);
            markKeySeen(c, want[k].second);   // before anything else: never re-offer a column
            if (!col || !col->generated) continue;
            std::vector<uint8_t> bytes = col->serialize();
            if (bytes.size() + 9 > MAX_MSG_BYTES) continue;   // cannot frame it, leave it local

            Writer w;
            w.i32(cx); w.i32(cz);
            std::vector<uint8_t> payload = w.b;
            payload.insert(payload.end(), bytes.begin(), bytes.end());
            enqueue(c, frame(MSG_CHUNK, payload));
            if (!c.active) break;   // enqueue dropped an overflowing peer
            sent++;
        }
        pruneSeenKeys(c, ccx, ccz, radius + 2);
    }
}

// ---------------------------------------------------------------------------
// The shared edit journal
// ---------------------------------------------------------------------------
// VoxelWorld::pendingEdits() has two readers: this module and the save journal. Both follow the
// same contract - consume the vector, then call clearPendingEdits() exactly once per call - and
// because the netcode runs every frame the journal never holds more than a tick of edits here.
// The consequence of sharing is worth stating: anything that must not miss an edit (a redo log, an
// audit) has to copy the vector itself, because flushSaves() inside VoxelWorld::update() also
// clears it on its own cadence.
void drainJournalHost(VoxelWorld& world) {
    // Copy, not a reference: broadcasting while holding the journal reference would be safe today
    // (enqueue never re-enters the world), but the clear below would invalidate the reference we
    // are iterating if it ever moved.
    const std::vector<BlockEdit> edits = world.pendingEdits();
    if (edits.empty()) return;
    for (size_t i = 0; i < edits.size(); i++) {
        broadcastSet(edits[i].x, edits[i].y, edits[i].z, edits[i].id);
        g_editsWindow++;
    }
    world.clearPendingEdits();
}

void drainJournalClient(VoxelWorld& world) {
    if (!g_link.active || !g_link.linked || !g_link.greeting) return;
    const std::vector<BlockEdit> edits = world.pendingEdits();
    if (edits.empty()) return;
    for (size_t i = 0; i < edits.size(); i++) {
        const BlockEdit& e = edits[i];
        Writer w;
        // previous travels too: it is the host's optimistic-concurrency check.
        w.i32(e.x); w.i32(e.y); w.i32(e.z); w.u16(e.id); w.u16(e.previous);
        enqueue(g_link, frame(MSG_EDIT, w.b));
        // Not counted in edits/s: the host answers every accepted edit with a SET, and that is
        // what this process counts, so counting here would double every change.
    }
    world.clearPendingEdits();
}

void updateStats() {
    const double now = nowSec();
    const double el = now - g_statsStart;
    if (el < STATS_WINDOW_S) return;
    g_editsPerSec = (uint32_t)((double)g_editsWindow / el);
    g_kbytesPerSec = (uint32_t)(((double)(g_bytesSentWindow + g_bytesRecvWindow) / 1024.0) / el);
    g_editsWindow = 0;
    g_bytesSentWindow = 0;
    g_bytesRecvWindow = 0;
    g_statsStart = now;
}

// ---------------------------------------------------------------------------
// Client connect machine
// ---------------------------------------------------------------------------
void openClientSocket() {
#ifdef _WIN32
    if (!g_peerResolved) return;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { setError(socketErrorText("socket")); g_running = false; return; }
    if (!setNonBlocking(s) || !setNoDelay(s)) {
        setError(socketErrorText("socket setup"));
        ::closesocket(s);
        g_running = false;
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = (u_short)g_peerFamily;
    addr.sin_addr.s_addr = g_peerIpv4;
    addr.sin_port = g_peerPortNet;

    const int r = ::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (r != 0 && !wouldBlock()) {
        const std::string e = socketErrorText("connect");
        ::closesocket(s);
        setError(e);
        g_running = false;
        return;
    }
    const double now = nowSec();
    g_link.active = true;
    g_link.sock = (uintptr_t)s;
    g_link.linked = (r == 0);
    g_link.opened = now;
    g_link.lastRead = now;
    g_link.lastSendProgress = now;
    g_link.id = 0;
    g_link.name = "host";
    // A connection completing in the kernel sends HELLO the moment it is writable.
    if (g_link.linked) sendHello();
#endif
}

void tickClientConnect() {
#ifdef _WIN32
    if (g_link.active && !g_link.linked) {
        if (socketWritableNow(g_link.sock)) {
            int err = 0;
            if (socketFailed(g_link.sock, err)) {
                dropConnection(g_link, "connect refused by host");
                return;
            }
            g_link.linked = true;
            g_link.lastRead = nowSec();
            sendHello();
            return;
        }
        if (nowSec() - g_link.opened > CONNECT_TIMEOUT_S)
            dropConnection(g_link, "connect timed out");
    }
#else
    (void)0;
#endif
}

// ---------------------------------------------------------------------------
// Per-tick bodies
// ---------------------------------------------------------------------------
void tickHost(VoxelWorld& world, float px, float py, float pz, float yaw) {
#ifdef _WIN32
    acceptClients();
#endif

    // 1. Read and apply, so an edit accepted here is already in the journal the drain broadcasts
    //    below - one frame of latency end to end, not two.
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Conn& c = g_clients[i];
        if (!c.active) continue;
        if (!readSome(c)) continue;
        if (!c.active) continue;
        parseFrames(c, world, true);
    }

    // 2. Our own position, for the remote avatars on every client.
    if (liveClientCount() > 0) broadcastHostPos(px, py, pz, yaw);

    // 3. Broadcast the journal: the host's local edits and the remote edits just accepted.
    drainJournalHost(world);

    // 4. Stream columns into each client's view radius, rate limited.
    pushChunks(world);

    // 5. Drain the queues and enforce the timeouts.
    const double now = nowSec();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Conn& c = g_clients[i];
        if (!c.active) continue;
        flushSend(c);
        if (!c.active) continue;
        if (now - c.lastRead > CLIENT_SILENT_S) { dropConnection(c, "client timed out"); continue; }
        if (c.sendPos < c.send.size() && now - c.lastSendProgress > STALL_TIMEOUT_S)
            dropConnection(c, "client stalled (send backlog for 5 s)");
    }

    rebuildRemoteList();
    updateStats();
}

void tickClient(VoxelWorld& world, float px, float py, float pz, float yaw) {
    tickClientConnect();

    if (g_link.active) {
        if (readSome(g_link)) parseFrames(g_link, world, false);
    }

    if (g_link.active && g_link.linked && g_link.greeting) {
        sendLinkPos(px, py, pz, yaw, g_localName);
        drainJournalClient(world);
    }

    applyPendingChunks(world);

    if (g_link.active && g_link.linked) {
        // Never write to a socket whose connect() is still in flight: send() there reports a
        // would-block-ish error on some stacks and dropping on that would be wrong. HELLO simply
        // waits in the queue for the frame the connection completes.
        flushSend(g_link);
        const double now = nowSec();
        // Only a linked connection can be "silent": while connecting, CONNECT_TIMEOUT_S owns it.
        if (g_link.active && g_link.linked && now - g_link.lastRead > CLIENT_SILENT_S)
            dropConnection(g_link, "host timed out");
        if (g_link.active && g_link.sendPos < g_link.send.size() &&
            now - g_link.lastSendProgress > STALL_TIMEOUT_S)
            dropConnection(g_link, "host stalled (send backlog for 5 s)");
    }

    rebuildRemoteList();
    updateStats();
}

// Splits "host", "host:port" and "192.168.1.5:25565". An embedded port wins over the argument,
// so the integrator can pass either. A bare IPv6 literal is not supported (LAN, IPv4 only).
bool splitHostPort(const std::string& spec, std::string& hostOut, int& portOut) {
    const size_t colon = spec.find(':');
    if (colon == std::string::npos) {
        hostOut = spec;
        return portOut > 0 && portOut <= 65535;
    }
    if (colon == 0 || spec.find(':', colon + 1) != std::string::npos) return false;  // colon junk
    hostOut = spec.substr(0, colon);
    const std::string tail = spec.substr(colon + 1);
    if (tail.empty() || tail.size() > 5) return false;
    int v = 0;
    for (size_t i = 0; i < tail.size(); i++) {
        if (tail[i] < '0' || tail[i] > '9') return false;
        v = v * 10 + (tail[i] - '0');
    }
    if (v <= 0 || v > 65535) return false;
    portOut = v;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Frozen public surface
// ---------------------------------------------------------------------------

bool startVoxelNet(VoxelWorld& world, NetMode mode, int port, const std::string& hostAddress) {
    stopVoxelNet();

    g_mode = NetMode::OFFLINE;
    g_error.clear();
    g_localId = 0;
    g_localName = "player";
    g_seedMismatch = false;
    g_localSeed = world.seed();
    g_editsWindow = g_bytesSentWindow = g_bytesRecvWindow = 0;
    g_editsPerSec = g_kbytesPerSec = 0;
    g_rejectedTotal = 0;
    g_pendingChunks.clear();
    g_remoteStore.clear();
    g_remote.clear();

    if (mode == NetMode::OFFLINE) {
        g_running = false;
        return true;   // single player is not a failure
    }

#ifndef _WIN32
    // The engine ships no networking class and this module deliberately does not add a portable
    // socket layer: off Windows every entry point is inert and says so.
    (void)port; (void)hostAddress; (void)world;
    netUnsupported();
    g_error = "networking not supported";
    g_running = false;
    return false;
#else
    // One WSAStartup for the process session, released by stopVoxelNet().
    if (!g_wsaUp) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            g_error = "WSAStartup failed";
            g_running = false;
            return false;
        }
        g_wsaUp = true;
    }

    g_statsStart = nowSec();
    g_port = port;
    g_hostSeed = world.seed();

    if (mode == NetMode::HOST) {
        if (port <= 0 || port > 65535) { setError("invalid port"); stopVoxelNet(); return false; }
        SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls == INVALID_SOCKET) { setError(socketErrorText("socket")); stopVoxelNet(); return false; }
        if (!setNonBlocking(ls)) {
            setError(socketErrorText("listen socket"));
            ::closesocket(ls);
            stopVoxelNet();
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);   // LAN: reach us on any interface
        addr.sin_port = htons((unsigned short)port);
        if (::bind((SOCKET)ls, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            setError("bind on port " + std::to_string(port) + ": " + socketErrorText("bind"));
            ::closesocket(ls);
            stopVoxelNet();
            return false;
        }
        if (::listen((SOCKET)ls, MAX_CLIENTS + 1) != 0) {
            setError(socketErrorText("listen"));
            ::closesocket(ls);
            stopVoxelNet();
            return false;
        }
        g_listen = (uintptr_t)ls;
        g_mode = NetMode::HOST;
        g_localId = 0;
        g_localName = "host";   // clients draw the host as an avatar named "host"
        g_running = true;
        // The harness waits for this line before it starts a client, so a port that is still free
        // when the second process launches is a real failure rather than a race.
        trace("host: listening on %d, seed %llu", (int)g_port,
              (unsigned long long)g_localSeed);
        return true;
    }

    // CLIENT
    std::string host = hostAddress;
    int usePort = port;
    if (!splitHostPort(hostAddress, host, usePort)) {
        setError("invalid host address \"" + hostAddress + "\"");
        stopVoxelNet();
        return false;
    }
    g_port = usePort;
    g_hostLabel = host;
    if (!resolvePeer(host, usePort)) { stopVoxelNet(); return false; }
    g_mode = NetMode::CLIENT;
    g_localId = -1;            // unknown until WELCOME; -1 keeps the host's id 0 out of our list
    g_link.name = "host";
    g_running = true;
    openClientSocket();
    if (!g_link.active) {      // the socket could not even be created
        if (g_error.empty()) setError("could not open the client socket");
        g_running = false;
        g_mode = NetMode::OFFLINE;
        return false;
    }
    return true;
#endif
}

void stopVoxelNet() {
    // A peer that is already gone cannot take a BYE, and that is not an error worth showing.
    g_quitting = true;
#ifdef _WIN32
    if (g_link.active) {
        // BYE first, then one best-effort flush: the queue is already framed, and a peer that is
        // not listening simply loses it. No blocking wait - stop() runs on the main thread too.
        enqueue(g_link, frame(MSG_BYE, std::vector<uint8_t>()));
        flushSend(g_link);
        dropConnection(g_link, std::string());
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].active) continue;
        enqueue(g_clients[i], frame(MSG_BYE, std::vector<uint8_t>()));
        flushSend(g_clients[i]);
        dropConnection(g_clients[i], std::string());
    }
    closeSocket(g_listen);
    g_listen = ~(uintptr_t)0;
    if (g_wsaUp) {
        WSACleanup();
        g_wsaUp = false;
    }
#endif
    g_pendingChunks.clear();
    g_remoteStore.clear();
    g_remote.clear();
    g_mode = NetMode::OFFLINE;
    g_running = false;
    g_localId = 0;
    g_localName = "player";
    g_seedMismatch = false;
    g_peerResolved = false;
    g_hostLabel.clear();
    g_quitting = false;
}

void tickVoxelNet(VoxelWorld& world, float dt, float playerX, float playerY, float playerZ,
                  float yaw) {
    if (g_mode == NetMode::OFFLINE || !g_running) return;   // single player: no cost at all
    (void)dt;   // timing is wall-clock here; dt only matters if edits ever get interpolated
    if (g_mode == NetMode::HOST) {
        tickHost(world, playerX, playerY, playerZ, yaw);
        return;
    }
    tickClient(world, playerX, playerY, playerZ, yaw);
}

const std::vector<RemotePlayer>& remotePlayers() { return g_remote; }

bool netReady() {
    if (g_mode == NetMode::OFFLINE || !g_running) return false;
    // A host is ready the moment it is listening: an edit made before anyone connects is still a
    // local edit, and the journal will carry it to the first peer that arrives.
    if (g_mode == NetMode::HOST) return g_listen != ~(uintptr_t)0;
    // A client is only ready once the WELCOME has been read: drainJournalClient() refuses to send
    // before the greeting, so an edit made earlier never reaches the wire and a test would measure
    // a dropped packet instead of the protocol.
    return g_link.active && g_link.linked && g_link.greeting;
}

std::string netStats() {
    if (g_mode == NetMode::OFFLINE) {
        return g_error.empty() ? std::string("Net: offline") : ("Net: " + g_error);
    }
    std::string s;
    if (g_mode == NetMode::HOST) {
        s = "Net: host " + std::to_string(g_port) + ", " + std::to_string(liveClientCount()) +
            " clients, " + std::to_string(g_editsPerSec) + " edits/s, " +
            std::to_string(g_kbytesPerSec) + " KB/s";
    } else {
        s = "Net: client of " + g_hostLabel + ":" + std::to_string(g_port) + ", " +
            std::to_string(g_remote.size()) + " players, " + std::to_string(g_editsPerSec) +
            " edits/s, " + std::to_string(g_kbytesPerSec) + " KB/s";
        if (!g_link.linked) s += ", connecting";
        else if (!g_link.greeting) s += ", unverified";
        if (g_seedMismatch) s += ", seed mismatch";
    }
    if (g_rejectedTotal > 0) s += ", " + std::to_string((unsigned long long)g_rejectedTotal) + " rejected";
    if (!g_error.empty() && g_running) s += ", last error: " + g_error;
    else if (!g_error.empty() && !g_running) s = "Net: " + g_error;
    return s;
}

const char* netError() { return g_error.c_str(); }

}  // namespace voxel
