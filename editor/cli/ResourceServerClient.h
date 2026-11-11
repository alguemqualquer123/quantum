// (c) SR VINIX
// SPDX-License-Identifier: MIT

// ResourceServerClient.h - editor-side client for quantum-resource-server.
// Self-contained free functions (no engine or editor includes): the main agent
// can drop ${EDITOR_DIR}/cli/ResourceServerClient.cpp into QUANTUM_EDITOR_SOURCES
// and call these from anywhere without touching anything else.
//
// Protocol (server: tools/resource-server/): newline-delimited JSON over one
// short-lived TCP connection per request on 127.0.0.1. The editor discovers
// the ephemeral port by reading the line
//   quantum-resource-server: listening on 127.0.0.1:<port>
// that the spawned process prints on stdout.

#pragma once

#include <filesystem>
#include <string>

namespace quantum::editor::cli {

namespace fs = std::filesystem;

// Locates the quantum-resource-server executable: beside the running editor
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

// Spawns quantum-resource-server --project <projectDir> --port <port> detached,
// waits for its "listening on 127.0.0.1:<port>" stdout line (a few seconds),
// and returns the discovered port and the child pid. The process keeps running
// after this call returns; use stopResourceServer() to end it. The overload
// with serverExe overrides the executable lookup (tests / custom layouts).
// parentPid is handed to the server as --parent so its tray menu can close the
// editor that owns it; 0 means nobody may. The loopback port is chosen before
// the child starts and passed with --port, so when recordFile is not empty the
// record is written as soon as the child exists - a crash while the server is
// still booting then leaves a recorded (adoptable) server, not an orphan.
bool spawnResourceServer(const fs::path& projectDir, int& portOut, unsigned long long& pidOut,
                         std::string& errorOut);
bool spawnResourceServer(const fs::path& projectDir, const fs::path& serverExe, int& portOut,
                         unsigned long long& pidOut, std::string& errorOut,
                         unsigned long long parentPid = 0,
                         const fs::path& recordFile = fs::path());

// Terminates a server spawned by spawnResourceServer() (best effort).
bool stopResourceServer(unsigned long long pid, std::string& errorOut);

// True when the pid still names a live process. Used to tell "the server is in
// the tray" from "the record is stale".
bool processIsRunning(unsigned long long pid);

// This process' id, for the --parent argument above.
unsigned long long currentProcessId();

// The record that lets a server outliving the editor be found again: three
// tab-separated fields (port, pid, project directory) in one text file. It is
// written as soon as the server process exists (see spawnResourceServer) and
// removed when the editor stops the server, because a terminated process gets
// no chance to clean up after itself.
bool writeServerRecord(const fs::path& recordFile, int port, unsigned long long pid,
                       const fs::path& projectDir, std::string& errorOut);
bool readServerRecord(const fs::path& recordFile, int& portOut, unsigned long long& pidOut,
                      fs::path& projectOut, std::string& errorOut);
void removeServerRecord(const fs::path& recordFile);

}
