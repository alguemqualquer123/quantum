// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

// ResourceServer.h - the doriax-resource-server serving loop: localhost TCP,
// newline-delimited JSON, 1 s mtime watch tick.

#pragma once

#include <filesystem>
#include <string>

namespace doriax::rs {

namespace fs = std::filesystem;

struct ServerOptions {
    fs::path projectDir;
    int port = 0;            // 0 = ephemeral; the chosen port is printed
    bool verbose = false;
    bool once = false;       // single scan, print summary, exit
    std::string token;       // when set, every request must carry "token"
};

// Runs until the process is asked to terminate (Ctrl+C on Windows). Returns
// the process exit code. Handles WSAStartup/cleanup internally.
int runResourceServer(const ServerOptions& options);

// Scans once and prints a human-readable summary (the --once mode, and the
// unit-testable entry point). Returns 0 on success.
int runOnceSummary(const ServerOptions& options);

// Builds the JSON response line (without the '\n') for one request line.
// Exposed for tests. `startUnix` backs the uptime field.
std::string handleRequestLine(const std::string& requestLine, class ResourceIndex& index,
                              const std::string& token, long long startUnix);

}
