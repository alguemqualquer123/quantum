// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "util/ShellSession.h"

#include "util/ShellEnv.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef _WIN32
    #include <cerrno>
    #include <csignal>
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>

    extern char** environ;
#endif

namespace quantum::editor {

namespace {

    constexpr size_t READ_CHUNK = 4096;

#ifdef _WIN32
    // Windows: the shell path plus the arguments that make it read statements from
    // stdin. Without -Command - powershell.exe waits for a terminal it does not have.
    std::string windowsShellLaunch() {
        const char* systemRoot = std::getenv("SystemRoot");
        fs::path ps = systemRoot
            ? fs::path(systemRoot) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
            : fs::path("powershell.exe");
        if (!fs::exists(ps)) {
            const std::string found = ShellEnv::findExecutable("powershell.exe");
            if (!found.empty())
                ps = fs::path(found);
        }
        if (fs::exists(ps))
            return "\"" + ps.string() + "\" -NoProfile -NonInteractive -Command -";
        return "cmd.exe /Q /K";
    }
#endif

}

std::string ShellSession::kindOf(const std::string& shellCommand) {
    std::string lowered;
    lowered.reserve(shellCommand.size());
    for (char c : shellCommand)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    // Substring, not basename: the launch line carries arguments and quotes.
    if (lowered.find("pwsh") != std::string::npos)
        return "pwsh";
    if (lowered.find("powershell") != std::string::npos)
        return "powershell";
    if (lowered.find("cmd.exe") != std::string::npos || lowered.find("cmd /") != std::string::npos)
        return "cmd";
    if (lowered.find("bash") != std::string::npos || lowered.find("zsh") != std::string::npos)
        return "bash";
    if (lowered.find("sh") != std::string::npos)
        return "sh";
    return "";
}

std::string ShellSession::cwdProbe(const std::string& kind) {
    if (kind == "powershell" || kind == "pwsh")
        return "Write-Output ('" + std::string(PWD_MARKER) + "' + (Get-Location).Path)";
    if (kind == "cmd")
        return "echo " + std::string(PWD_MARKER) + "%CD%";
    if (kind == "bash" || kind == "sh")
        return "printf '" + std::string(PWD_MARKER) + "%s\\n' \"$PWD\"";
    return "";
}

std::string ShellSession::defaultShellCommand() {
#ifdef _WIN32
    return windowsShellLaunch();
#else
    const char* shell = std::getenv("SHELL");
    if (shell && *shell && fs::exists(shell))
        return shell;
    for (const char* candidate : {"/bin/bash", "/bin/zsh", "/bin/sh"}) {
        if (fs::exists(candidate))
            return candidate;
    }
    return "/bin/sh";
#endif
}

#ifdef _WIN32
bool ShellSession::readAvailable(HANDLE handle, bool fromStderr) {
    char buffer[READ_CHUNK];
    DWORD read = 0;
    // Blocking read on a worker thread: it ends when the child exits and the pipe
    // breaks, which is also how the session learns the shell is gone.
    while (ReadFile(handle, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr)) {
        if (read == 0)
            break;
        if (onOutput)
            onOutput(buffer, read, fromStderr);
    }
    return true;
}
#else
bool ShellSession::readAvailable(int fd, bool fromStderr) {
    char buffer[READ_CHUNK];
    for (;;) {
        const ssize_t read = ::read(fd, buffer, sizeof(buffer));
        if (read > 0) {
            if (onOutput)
                onOutput(buffer, static_cast<size_t>(read), fromStderr);
            continue;
        }
        if (read < 0 && errno == EINTR)
            continue;
        break; // 0 = EOF, anything else is the child going away
    }
    return true;
}
#endif

bool ShellSession::start(const fs::path& workingDir, const std::string& shellCommand, OutputFn onOutputFn) {
    stop();
    if (shellCommand.empty()) {
        error = "Terminal: no shell to start.";
        return false;
    }

    error.clear();
    onOutput = std::move(onOutputFn);
    std::string line = shellCommand;
    const std::string workDir = workingDir.empty() ? "" : workingDir.string();

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE stdinRead = nullptr, stdinWriteLocal = nullptr;
    HANDLE stdoutWrite = nullptr, stderrWrite = nullptr;
    if (!CreatePipe(&stdinRead, &stdinWriteLocal, &sa, 0)) {
        error = "Terminal: CreatePipe for stdin failed (" + std::to_string(GetLastError()) + ").";
        return false;
    }
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0) || !CreatePipe(&stderrRead, &stderrWrite, &sa, 0)) {
        error = "Terminal: CreatePipe for the output failed (" + std::to_string(GetLastError()) + ").";
        CloseHandle(stdinRead);
        CloseHandle(stdinWriteLocal);
        stdinRead = nullptr;
        stdinWrite = nullptr;
        return false;
    }
    // Only the child ends may be inherited, or the editor would also hold the write
    // side open and never see the pipe break.
    SetHandleInformation(stdinWriteLocal, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdinRead;
    si.hStdOutput = stdoutWrite;
    si.hStdError = stderrWrite;

    PROCESS_INFORMATION pi{};
    // The whole child tree belongs to a job so stop() kills cmake and the compilers
    // the shell spawned, not only the shell (same reason as in CommandRunner).
    job = CreateJobObjectA(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    std::vector<char> mutableLine(line.begin(), line.end());
    mutableLine.push_back('\0');
    const BOOL created = CreateProcessA(nullptr, mutableLine.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                       workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    if (!created) {
        error = "Terminal: could not start \"" + line + "\" (" + std::to_string(GetLastError()) + ").";
        CloseHandle(stdinRead);
        CloseHandle(stdinWriteLocal);
        stdinWrite = nullptr;
        CloseHandle(stdoutRead);
        stdoutRead = nullptr;
        CloseHandle(stderrRead);
        stderrRead = nullptr;
        if (job) {
            CloseHandle(job);
            job = nullptr;
        }
        return false;
    }
    if (job)
        AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);

    process = pi.hProcess;
    primaryThread = pi.hThread;
    stdinWrite = stdinWriteLocal;
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);

    readers.emplace_back([this] { readAvailable(stdoutRead, false); readerFinished(); });
    readers.emplace_back([this] { readAvailable(stderrRead, true); readerFinished(); });
#else
    int inPipe[2] = { -1, -1 }, outPipe[2] = { -1, -1 }, errPipe[2] = { -1, -1 };
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        error = std::string("Terminal: pipe() failed: ") + strerror(errno);
        return false;
    }

    // fork() is off the table in a threaded GUI process, so the child is spawned
    // from a pipe-and-dup2 recipe like the rest of the editor's shell-outs.
    std::vector<std::string> words;
    {
        std::string current;
        bool quoted = false;
        for (char c : line) {
            if (c == '"') {
                quoted = !quoted;
            } else if (c == ' ' && !quoted) {
                if (!current.empty())
                    words.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty())
            words.push_back(current);
    }
    if (words.empty()) {
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        return false;
    }

    std::vector<char*> argv;
    for (std::string& word : words)
        argv.push_back(const_cast<char*>(word.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
    // posix_spawn does not close the rest of the parent's descriptors, so the raw
    // pipe ends are dropped by hand: a leftover write end would keep the read side
    // open past the shell's exit and the readers would never see EOF.
    posix_spawn_file_actions_addclose(&actions, inPipe[0]);
    posix_spawn_file_actions_addclose(&actions, outPipe[1]);
    posix_spawn_file_actions_addclose(&actions, errPipe[1]);
    posix_spawn_file_actions_addclose(&actions, inPipe[1]);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    posix_spawn_file_actions_addclose(&actions, errPipe[0]);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    const int spawned = posix_spawn(&pid, argv[0], &actions, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);

    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);

    if (spawned != 0) {
        error = std::string("Terminal: posix_spawn(\"") + argv[0] + "\") failed: " + strerror(spawned);
        close(inPipe[1]);
        close(outPipe[0]);
        close(errPipe[0]);
        pid = -1;
        return false;
    }

    stdinWrite = inPipe[1];
    stdoutRead = outPipe[0];
    stderrRead = errPipe[0];

    if (!workDir.empty())
        // No chdir file action is portable enough to rely on, so the directory
        // change is simply the first thing the shell is told.
        write("cd \"" + workDir + "\"\n");

    readers.emplace_back([this] { readAvailable(stdoutRead, false); readerFinished(); });
    readers.emplace_back([this] { readAvailable(stderrRead, true); readerFinished(); });
#endif

    running.store(true, std::memory_order_relaxed);
    return true;
}

void ShellSession::joinReaders() {
    for (std::thread& reader : readers) {
        if (reader.joinable())
            reader.join();
    }
    readers.clear();
    exitedReaders.store(0, std::memory_order_relaxed);
}

// A reader returns when its pipe breaks. The shell closing its own side (an `exit`,
// a crash) breaks both at once, so the second one to notice is the session ending -
// without it the panel would still offer a prompt for a process that is gone.
void ShellSession::readerFinished() {
    if (exitedReaders.fetch_add(1, std::memory_order_relaxed) == 1)
        running.store(false, std::memory_order_relaxed);
}

void ShellSession::stop() {
    if (readers.empty() && !running.load(std::memory_order_relaxed))
        return;
    running.store(false, std::memory_order_relaxed);

#ifdef _WIN32
    // Closing our stdin end first lets a shell that is merely waiting for the next
    // command exit on its own; the job kill is what handles one that is busy.
    if (stdinWrite) {
        CloseHandle(stdinWrite);
        stdinWrite = nullptr;
    }
    if (job)
        TerminateJobObject(job, 1);
    else if (process)
        TerminateProcess(process, 1);

    joinReaders();

    if (process) {
        WaitForSingleObject(process, 1000);
        CloseHandle(process);
        process = nullptr;
    }
    if (primaryThread) {
        CloseHandle(primaryThread);
        primaryThread = nullptr;
    }
    if (stdoutRead) {
        CloseHandle(stdoutRead);
        stdoutRead = nullptr;
    }
    if (stderrRead) {
        CloseHandle(stderrRead);
        stderrRead = nullptr;
    }
    if (job) {
        CloseHandle(job);
        job = nullptr;
    }
#else
    if (stdinWrite >= 0) {
        close(stdinWrite);
        stdinWrite = -1;
    }
    if (pid > 0) {
        // The shell was spawned into its own group, so the signal reaches cmake and
        // the compilers it started as well.
        if (kill(-pid, SIGTERM) != 0 && errno != ESRCH)
            error = std::string("Terminal: SIGTERM to the group failed: ") + strerror(errno);
        int status = 0;
        for (int attempt = 0; attempt < 20; attempt++) {
            const pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid || waited < 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (kill(-pid, 0) == 0)
            kill(-pid, SIGKILL);
        waitpid(pid, &status, 0);
    }

    joinReaders();

    if (stdoutRead >= 0) {
        close(stdoutRead);
        stdoutRead = -1;
    }
    if (stderrRead >= 0) {
        close(stderrRead);
        stderrRead = -1;
    }
    pid = -1;
#endif
}

bool ShellSession::write(const std::string& text) {
    if (!running.load(std::memory_order_relaxed) || text.empty())
        return false;

#ifdef _WIN32
    if (!stdinWrite)
        return false;
    size_t written = 0;
    while (written < text.size()) {
        DWORD chunk = 0;
        const DWORD remaining = static_cast<DWORD>(std::min<size_t>(text.size() - written, READ_CHUNK));
        if (!WriteFile(stdinWrite, text.data() + written, remaining, &chunk, nullptr) || chunk == 0)
            return false;
        written += chunk;
    }
    return true;
#else
    if (stdinWrite < 0)
        return false;
    size_t written = 0;
    while (written < text.size()) {
        const ssize_t sent = ::write(stdinWrite, text.data() + written, text.size() - written);
        if (sent > 0) {
            written += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
#endif
}

ShellSession::~ShellSession() {
    stop();
}

}
