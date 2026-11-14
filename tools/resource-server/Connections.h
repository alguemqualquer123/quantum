// (c) SR VINIX
// SPDX-License-Identifier: MIT

// Connections.h - who is talking to quantum-resource-server.
//
// One ConnectionRecord is kept per accepted TCP connection (the protocol is
// request/response, so a "connection" here is one client round trip). The log
// feeds both the connections window (ServerWindow.h) and the {"cmd":"connections"}
// protocol command, and groups the records per client so the window can answer
// "who is connected" without the client declaring anything: the peer port is
// resolved to its owning process through the loopback TCP table.

#pragma once

#include <string>
#include <vector>

namespace quantum::rs {

struct ConnectionRecord {
    unsigned long long id = 0;
    std::string peer;           // "127.0.0.1:53112"
    std::string command;        // ping | status | index | entry | changes | connections
    std::string detail;         // entry path, error text, or empty
    std::string client;         // "client" member of the request, when it sends one
    std::string processName;    // owning process of the peer port, when found
    unsigned long long ownerPid = 0;
    long long unixTime = 0;     // when the request was answered
    double durationMs = 0.0;
    unsigned long long requestBytes = 0;
    unsigned long long responseBytes = 0;
    bool ok = false;
};

// Everything one peer (process + address) did, folded up for the client list.
struct ClientGroup {
    std::string processName;
    std::string pidText;
    std::string peerHost;       // always 127.0.0.1 today, kept for display
    std::string client;         // self-reported identity, when present
    unsigned long long requests = 0;
    unsigned long long failures = 0;
    long long firstSeenUnix = 0;
    long long lastSeenUnix = 0;
    std::string lastCommand;
};

class ConnectionLog {
public:
    // Ring buffer size: the window shows the tail, and the protocol command is
    // bounded so a client cannot pull an unbounded string out of the server.
    static constexpr size_t kCapacity = 256;

    // Takes ownership of `record` and fills id/unixTime. Returns the id.
    unsigned long long add(ConnectionRecord record);

    // Oldest first, at most kCapacity entries.
    const std::vector<ConnectionRecord>& records() const { return records_; }
    unsigned long long accepted() const { return accepted_; }
    unsigned long long failures() const { return failures_; }
    long long lastRequestUnix() const { return lastRequestUnix_; }
    unsigned long long lastId() const { return lastId_; }

    std::vector<ClientGroup> clients() const;

    // Empties the log and the totals (the window's Clear button). Ids keep
    // rising so a stale row cannot look like a new one.
    void clear();

private:
    std::vector<ConnectionRecord> records_;
    unsigned long long nextId_ = 1;
    unsigned long long lastId_ = 0;
    unsigned long long accepted_ = 0;
    unsigned long long failures_ = 0;
    long long lastRequestUnix_ = 0;
};

// Resolves a "127.0.0.1:53112" peer to the process holding that socket. Must
// run while the connection is open: the TCP table only lists live sockets.
// Both outputs are best effort - a non-loopback peer, a toolchain without
// iphlpapi, or a socket that already closed leaves them empty and returns false.
bool queryConnectionOwner(const std::string& peer, int localPort,
                          unsigned long long& pidOut, std::string& nameOut);

// File name only ("quantum-editor.exe"), empty when the process is gone or is
// another user's (query limits are enough for the name).
bool queryProcessName(unsigned long long pid, std::string& nameOut);

}
