// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace quantum::editor {

    namespace fs = std::filesystem;

    // A shell kept alive behind pipes so the editor can type into it and read what it
    // prints. One process holds the session, so a `cd` or a variable survives from one
    // command to the next.
    //
    // There is no pseudo-terminal, and that is the whole trade: without a TTY the shell
    // never paints a prompt and full-screen programs (vim, the opencode interface) refuse
    // to draw, but in exchange the output is a plain byte stream the panel can show as
    // lines, and a command that never returns is still killable as a process tree.
    class ShellSession {
    public:
        // Called from the reader threads with raw bytes. Must not touch ImGui: the
        // panel queues these and drains them on the UI thread.
        using OutputFn = std::function<void(const char* data, size_t size, bool fromStderr)>;

        ShellSession() = default;
        ~ShellSession();

        ShellSession(const ShellSession&) = delete;
        ShellSession& operator=(const ShellSession&) = delete;

        // shellCommand is the whole line to start, e.g. the path to powershell.exe plus
        // its arguments. Fails (and says why through the log) when it cannot start.
        bool start(const fs::path& workingDir, const std::string& shellCommand, OutputFn onOutput);

        // Kills the shell and everything it spawned, then joins the readers. Idempotent.
        void stop();

        bool isRunning() const { return running.load(std::memory_order_relaxed); }

        // Bytes into the shell's stdin; the caller keeps the line discipline (newlines).
        bool write(const std::string& text);

        // Why the last start() failed, empty otherwise. The panel logs it: a process
        // wrapper that logged by itself could not be used outside the editor.
        const std::string& lastError() const { return error; }

        // "powershell" | "pwsh" | "cmd" | "bash" | "sh", from the shell path.
        static std::string kindOf(const std::string& shellCommand);

        // A line the shell prints back reporting its own directory, prefixed with
        // PWD_MARKER. The shell has no prompt over a pipe, so the panel has to ask for
        // the directory itself. "" for a shell kind we do not know, whose cwd then
        // stays at the value it started with.
        static std::string cwdProbe(const std::string& kind);
        static constexpr const char* PWD_MARKER = "@QUANTUMPWD@";

        // powershell.exe on Windows, $SHELL or /bin/sh elsewhere.
        static std::string defaultShellCommand();

    private:
        void joinReaders();
        void readerFinished();

        OutputFn onOutput;
        std::string error;
        std::atomic<bool> running{false};
        std::atomic<int> exitedReaders{0};   // both pipes breaking means the shell is gone
        std::vector<std::thread> readers;

#ifdef _WIN32
        HANDLE stdinWrite = nullptr;   // our end of the shell's stdin
        HANDLE stdoutRead = nullptr;
        HANDLE stderrRead = nullptr;
        HANDLE process = nullptr;
        HANDLE primaryThread = nullptr;
        HANDLE job = nullptr;          // holds the tree, so stop() kills cmake too
        bool readAvailable(HANDLE handle, bool fromStderr);
#else
        pid_t pid = -1;
        int stdinWrite = -1;
        int stdoutRead = -1;
        int stderrRead = -1;
        bool readAvailable(int fd, bool fromStderr);
#endif
    };

}
