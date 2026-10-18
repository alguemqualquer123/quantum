// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

// ResourceServerClient.h - editor-side client for doriax-resource-server.
// Self-contained free functions (no engine or editor includes): the main agent
// can drop ${EDITOR_DIR}/cli/ResourceServerClient.cpp into DORIAX_EDITOR_SOURCES
// and call these from anywhere without touching anything else.
//
// Protocol (server: tools/resource-server/): newline-delimited JSON over one
// short-lived TCP connection per request on 127.0.0.1. The editor discovers
// the ephemeral port by reading the line
//   doriax-resource-server: listening on 127.0.0.1:<port>
// that the spawned process prints on stdout.

#pragma once

#include <filesystem>
#include <string>

namespace doriax::editor::cli {

namespace fs = std::filesystem;

// Locates the doriax-resource-server executable: beside the running editor
// executable first, then the usual build-output subdirectories. Returns false
// when none exists.
bool findResourceServerExecutable(fs::path& outPath);

// Sends one raw request line ("<json>\n") and returns the first response line
// (without the newline). Short timeout (connect + read) so a dead server never
// freezes the editor. Socket start-up/teardown is refcounted per process and
// therefore safe to call from any thread, any number of times.
bool requestServerLine(const std::string& host, int port, const std::string& requestLine,
                       std::string& responseOut, std::string& errorOut, int timeoutMs = 750);

// {"cmd":"status"} against host:port. statusJsonOut receives the raw JSON
// response line (check its "ok" field; this only reports transport failure).
bool probeServer(const std::string& host, int port, std::string& statusJsonOut,
                 std::string& errorOut);

// Convenience wrapper over probeServer() for the localhost default.
bool requestStatus(int port, std::string& statusJsonOut, std::string& errorOut);

// Spawns doriax-resource-server --project <projectDir> --port 0 detached,
// waits for its "listening on 127.0.0.1:<port>" stdout line (a few seconds),
// and returns the discovered port and the child pid. The process keeps running
// after this call returns; use stopResourceServer() to end it. The overload
// with serverExe overrides the executable lookup (tests / custom layouts).
bool spawnResourceServer(const fs::path& projectDir, int& portOut, unsigned long long& pidOut,
                         std::string& errorOut);
bool spawnResourceServer(const fs::path& projectDir, const fs::path& serverExe, int& portOut,
                         unsigned long long& pidOut, std::string& errorOut);

// Terminates a server spawned by spawnResourceServer() (best effort).
bool stopResourceServer(unsigned long long pid, std::string& errorOut);

}
