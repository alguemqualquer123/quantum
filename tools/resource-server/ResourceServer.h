// (c) SR VINIX
// SPDX-License-Identifier: MIT

// ResourceServer.h - the quantum-resource-server serving loop: localhost TCP,
// newline-delimited JSON, 1 s mtime watch tick.

#pragma once

#include <filesystem>
#include <string>

#include "Connections.h"

namespace quantum::rs {

namespace fs = std::filesystem;

struct ServerOptions {
    fs::path projectDir;
    int port = 0;            // 0 = ephemeral; the chosen port is printed
    bool verbose = false;
    bool once = false;       // single scan, print summary, exit
    std::string token;       // when set, every request must carry "token"
    // The editor process that spawned this server (0 when unknown). The tray
    // menu uses it for "Close server and editor", and it is what tells an
    // editor-spawned server (silent, tray only) from one a human started.
    unsigned long long parentPid = 0;
    // --window / --no-window. Unset means automatic: a server started by hand
    // opens its connections window, one the editor spawns stays in the tray.
    bool wantWindow = false;
    bool noWindow = false;
};

// Runs until the process is asked to terminate (Ctrl+C, or the tray menu's
// "Close server"). Returns the process exit code. Handles WSAStartup/cleanup
// internally. Outlives the editor on purpose: see TrayIcon.h.
int runResourceServer(const ServerOptions& options);

// Scans once and prints a human-readable summary (the --once mode, and the
// unit-testable entry point). Returns 0 on success.
int runOnceSummary(const ServerOptions& options);

// Builds the JSON response line (without the '\n') for one request line.
// Exposed for tests. `startUnix` backs the uptime field, and `log` (optional)
// is what the "connections" command reads - every other command ignores it.
std::string handleRequestLine(const std::string& requestLine, class ResourceIndex& index,
                              const std::string& token, long long startUnix,
                              const ConnectionLog* log = nullptr);

}
