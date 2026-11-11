// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "GitMonitor.h"

#include "Backend.h"
#include "FileUtils.h"
#include "ShellEnv.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <cerrno>
    #include <fcntl.h>
    #include <poll.h>
    #include <signal.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>

    extern char **environ;
#endif

using namespace quantum;
namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto TICK = std::chrono::seconds(1);
// Status also catches files edited outside the editor
constexpr auto STATUS_INTERVAL = std::chrono::seconds(3);
constexpr auto DISCOVERY_INTERVAL = std::chrono::seconds(10);
constexpr auto TIMEOUT = std::chrono::seconds(10);
constexpr size_t MAX_OUTPUT = 16 * 1024 * 1024;

struct ProcessOutput {
    bool ok = false;
    std::string text;
};

#ifdef _WIN32

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

// Quoted the way CommandLineToArgvW reads it back
void appendArgument(std::wstring& commandLine, const std::wstring& arg) {
    if (!commandLine.empty()) commandLine += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        commandLine += arg;
        return;
    }

    commandLine += L'"';
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            backslashes++;
            continue;
        }
        commandLine.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        commandLine += c;
        backslashes = 0;
    }
    commandLine.append(backslashes * 2, L'\\');
    commandLine += L'"';
}

// UTF-8 arguments, no console window, stderr dropped
ProcessOutput runProcess(const fs::path& program, const std::vector<std::string>& args, const std::atomic<bool>& cancel) {
    ProcessOutput output;

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return output;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul;
    startup.hStdOutput = writePipe;
    startup.hStdError = nul;

    std::wstring commandLine;
    appendArgument(commandLine, program.wstring());
    for (const std::string& arg : args) {
        appendArgument(commandLine, widen(arg));
    }

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(program.wstring().c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!created) {
        CloseHandle(readPipe);
        return output;
    }

    const Clock::time_point deadline = Clock::now() + TIMEOUT;
    bool killed = false;
    char buffer[65536];
    while (true) {
        if (cancel || Clock::now() > deadline || output.text.size() > MAX_OUTPUT) {
            TerminateProcess(process.hProcess, 1);
            killed = true;
            break;
        }

        DWORD available = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available > 0) {
            DWORD bytesRead = 0;
            if (!ReadFile(readPipe, buffer, std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer))), &bytesRead, nullptr) || bytesRead == 0) break;
            output.text.append(buffer, bytesRead);
            continue;
        }

        // Another process can hold the pipe open, so the exit is what ends it
        if (WaitForSingleObject(process.hProcess, 5) == WAIT_OBJECT_0 &&
            (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) || available == 0)) {
            break;
        }
    }

    if (!killed && WaitForSingleObject(process.hProcess, 2000) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        killed = true;
    }

    DWORD exitCode = 1;
    if (!killed) GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);

    output.ok = exitCode == 0;
    return output;
}

#else

// Close-on-exec, or a build started meanwhile would inherit the pipe and keep it open
bool openPipe(int fds[2]) {
#ifdef __linux__
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

// No shell in between, stderr dropped
ProcessOutput runProcess(const fs::path& program, const std::vector<std::string>& args, const std::atomic<bool>& cancel) {
    ProcessOutput output;

    int fds[2];
    if (!openPipe(fds)) return output;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);

    const std::string programPath = program.string();
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(programPath.c_str()));
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, programPath.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (spawned != 0) {
        close(fds[0]);
        return output;
    }

    const Clock::time_point deadline = Clock::now() + TIMEOUT;
    bool killed = false;
    bool reaped = false;
    int status = 0;
    char buffer[65536];
    while (true) {
        if (cancel || Clock::now() > deadline || output.text.size() > MAX_OUTPUT) {
            kill(pid, SIGKILL);
            killed = true;
            break;
        }

        struct pollfd pfd = {fds[0], POLLIN, 0};
        const int ready = poll(&pfd, 1, 20);
        if (ready > 0) {
            const ssize_t bytes = read(fds[0], buffer, sizeof(buffer));
            if (bytes > 0) {
                output.text.append(buffer, static_cast<size_t>(bytes));
                continue;
            }
            if (bytes == 0 || (errno != EINTR && errno != EAGAIN)) break;
        } else if (ready < 0) {
            if (errno != EINTR) break;
        } else if (waitpid(pid, &status, WNOHANG) == pid) {
            // Another process can hold the pipe open, so the exit is what ends it
            reaped = true;
            break;
        }
    }
    close(fds[0]);

    if (!reaped) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }

    output.ok = !killed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return output;
}

#endif

fs::path findGit([[maybe_unused]] const std::atomic<bool>& cancel) {
    fs::path git = editor::ShellEnv::findExecutable("git");
#ifdef __APPLE__
    // Without the developer tools, /usr/bin/git only asks to install them
    if (git == "/usr/bin/git" && !runProcess("/usr/bin/xcode-select", {"-p"}, cancel).ok) {
        return {};
    }
#endif
    return git;
}

std::vector<std::string> outputLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string fileStamp(const fs::path& path) {
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec) return "-";
    const auto size = fs::file_size(path, ec);
    // libc++ file_time_type ticks are __int128, which has no to_string overload
    return std::to_string(static_cast<long long>(time.time_since_epoch().count())) + ":" + std::to_string(ec ? 0 : size);
}

// git status --porcelain=v2 --branch, commit is the one HEAD is at
editor::GitStatus parseStatus(const std::string& text, std::string& commit) {
    editor::GitStatus status;
    status.repository = true;

    for (const std::string& line : outputLines(text)) {
        if (line.empty()) continue;

        if (line.rfind("# branch.oid ", 0) == 0) {
            commit = line.substr(13);
        } else if (line.rfind("# branch.head ", 0) == 0) {
            status.head = line.substr(14);
        } else if (line.rfind("# branch.upstream ", 0) == 0) {
            status.upstream = line.substr(18);
        } else if (line.rfind("# branch.ab ", 0) == 0) {
            std::sscanf(line.c_str() + 12, "+%d -%d", &status.ahead, &status.behind);
        } else if ((line[0] == '1' || line[0] == '2') && line.size() > 3) {
            // XY, the index and working tree states, '.' when unchanged
            if (line[2] != '.') status.staged++;
            if (line[3] != '.') status.unstaged++;
        } else if (line[0] == 'u') {
            // A conflict is left by a merge, rebase, stash pop or cherry-pick
            status.unstaged++;
            status.operationInProgress = true;
        } else if (line[0] == '?') {
            status.unstaged++;
        }
    }

    return status;
}

// git ls-files --stage -z, "<mode> <object> <stage>\t<file>"
void parseStagedFiles(const std::string& text, std::unordered_map<std::string, std::string>& blobs) {
    std::istringstream stream(text);
    std::string entry;
    while (std::getline(stream, entry, '\0')) {
        const size_t tab = entry.find('\t');
        const size_t objectStart = entry.find(' ');
        const size_t objectEnd = entry.find(' ', objectStart + 1);
        if (tab == std::string::npos || objectEnd == std::string::npos || objectEnd >= tab) continue;

        // Only a regular file at stage 0 has a base. The blob of a symlink is the link path,
        // and a conflict has three stages.
        const std::string file = entry.substr(tab + 1);
        if (entry[tab - 1] != '0' || entry.compare(0, 3, "100") != 0) {
            blobs[file] = "";
        } else if (blobs.find(file) == blobs.end()) {
            blobs[file] = entry.substr(objectStart + 1, objectEnd - objectStart - 1);
        }
    }
}

// nullopt when the repository is gone
std::optional<editor::GitStatus> readStatus(const fs::path& git, const std::string& project, const fs::path& gitDir,
                                            const std::atomic<bool>& cancel) {
    // No -u, a repository set to skip untracked files stays fast
    ProcessOutput out = runProcess(git, {"--no-optional-locks", "-C", project, "status", "--porcelain=v2", "--branch"}, cancel);
    if (!out.ok) return std::nullopt;

    std::string commit;
    editor::GitStatus status = parseStatus(out.text, commit);

    // A detached HEAD shows a tag on its commit, or the commit itself
    if (status.head == "(detached)") {
        ProcessOutput tags = runProcess(git, {"-C", project, "tag", "--points-at", "HEAD"}, cancel);
        const std::vector<std::string> lines = outputLines(tags.text);
        const bool tagged = tags.ok && !lines.empty() && !lines[0].empty();
        status.headType = tagged ? editor::GitHeadType::Tag : editor::GitHeadType::Commit;
        status.head = tagged ? lines[0] : commit.substr(0, 8);
    }

    std::error_code ec;
    status.operationInProgress = status.operationInProgress || fs::exists(gitDir / "MERGE_HEAD", ec) ||
                                 fs::exists(gitDir / "rebase-merge", ec) || fs::exists(gitDir / "rebase-apply", ec);
    return status;
}

}

bool editor::GitStatus::operator==(const GitStatus& other) const {
    return repository == other.repository && headType == other.headType && head == other.head &&
           upstream == other.upstream && ahead == other.ahead && behind == other.behind &&
           staged == other.staged && unstaged == other.unstaged && operationInProgress == other.operationInProgress;
}

editor::GitMonitor::~GitMonitor() {
    shutdown();
}

void editor::GitMonitor::update(const fs::path& path, bool enable) {
    if (cancel || (path == projectPath && enable == enabled)) return;

    projectPath = path;
    enabled = enable;
    generation++;
    status = GitStatus();
    fileBases.clear();

    {
        std::lock_guard<std::mutex> lock(mutex);
        request.generation = generation;
        request.projectPath = path;
        request.enabled = enable;
        request.refresh = true;
    }

    // Started on first use, the App holding it is a static
    if (enable && !path.empty() && !worker.joinable()) {
        worker = std::thread(&GitMonitor::run, this);
    }
    wake.notify_one();
}

void editor::GitMonitor::setFiles(const std::vector<std::string>& openFiles) {
    std::vector<std::string> sorted = openFiles;
    std::sort(sorted.begin(), sorted.end());
    if (sorted == files) return;

    files = std::move(sorted);
    for (auto it = fileBases.begin(); it != fileBases.end();) {
        it = std::binary_search(files.begin(), files.end(), it->first) ? std::next(it) : fileBases.erase(it);
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        request.files = files;
        request.filesChanged = true;
    }
    wake.notify_one();
}

void editor::GitMonitor::requestRefresh() {
    if (!enabled) return;

    {
        std::lock_guard<std::mutex> lock(mutex);
        request.refresh = true;
    }
    wake.notify_one();
}

void editor::GitMonitor::setFocused(bool focus) {
    if (focus == focused) return;
    focused = focus;

    {
        std::lock_guard<std::mutex> lock(mutex);
        request.focused = focus;
        request.refresh = request.refresh || focus;
    }
    wake.notify_one();
}

const editor::GitFileBase* editor::GitMonitor::getFileBase(const std::string& file) const {
    auto it = fileBases.find(file);
    return it != fileBases.end() ? &it->second : nullptr;
}

void editor::GitMonitor::shutdown() {
    cancel = true;
    {
        std::lock_guard<std::mutex> lock(mutex);
        request.stop = true;
    }
    wake.notify_one();

    if (worker.joinable()) {
        worker.join();
    }
}

void editor::GitMonitor::apply(Update update) {
    // Polled for another project or setting
    if (update.generation != generation) return;

    if (update.status) {
        status = std::move(*update.status);
    }

    for (FileUpdate& file : update.files) {
        // Closed since then
        if (!std::binary_search(files.begin(), files.end(), file.file)) continue;

        GitFileBase& base = fileBases[file.file];
        const bool tracked = file.content.has_value();
        if (base.version != 0 && base.tracked == tracked && (!tracked || base.content == *file.content)) continue;

        base.tracked = tracked;
        base.content = tracked ? std::move(*file.content) : std::string();
        base.version = nextVersion++;
    }
}

void editor::GitMonitor::run() {
    fs::path git;
    bool gitSearched = false;
    uint64_t current = 0;
    Repository repo;

    std::unique_lock<std::mutex> lock(mutex);
    while (true) {
        auto pending = [&] {
            return request.stop || request.generation != current || request.refresh || request.filesChanged;
        };
        if (request.enabled && !request.projectPath.empty()) {
            wake.wait_for(lock, TICK, pending);
        } else {
            wake.wait(lock, pending);
        }
        if (request.stop) return;

        if (request.generation != current) {
            current = request.generation;
            repo = Repository();
        }

        const fs::path projectDir = request.projectPath;
        const std::vector<std::string> openFiles = request.files;
        const bool active = request.enabled && !projectDir.empty();
        const bool isFocused = request.focused;
        const bool refresh = request.refresh;
        const bool filesChanged = request.filesChanged;
        request.refresh = false;
        request.filesChanged = false;
        if (!active) continue;

        lock.unlock();

        if (!gitSearched) {
            git = findGit(cancel);
            gitSearched = true;
        }

        Update update;
        update.generation = current;
        const std::string project = FileUtils::pathToUtf8(projectDir);
        const Clock::time_point now = Clock::now();

        if (!git.empty() && !repo.found && (refresh || (isFocused && now >= repo.nextDiscovery))) {
            ProcessOutput out = runProcess(git, {"-C", project, "rev-parse", "--absolute-git-dir", "--show-prefix"}, cancel);
            const std::vector<std::string> lines = outputLines(out.text);
            repo.found = out.ok && !lines.empty() && !lines[0].empty();
            if (repo.found) {
                repo.gitDir = FileUtils::pathFromUtf8(lines[0]);
                repo.prefix = lines.size() > 1 ? lines[1] : "";
            } else {
                repo.nextDiscovery = now + DISCOVERY_INTERVAL;
            }
        }

        if (repo.found) {
            // Stages, commits and checkouts all write one of them
            const std::string stamp = fileStamp(repo.gitDir / "index") + "|" + fileStamp(repo.gitDir / "HEAD");
            const bool stampChanged = stamp != repo.stamp;
            repo.stamp = stamp;

            if (refresh || stampChanged || (isFocused && now >= repo.nextStatus)) {
                const std::optional<GitStatus> latest = readStatus(git, project, repo.gitDir, cancel);
                if (latest) {
                    if (repo.status != latest) {
                        repo.status = latest;
                        update.status = latest;
                    }
                    // A slow repository is polled less often
                    repo.nextStatus = Clock::now() + std::max<Clock::duration>(STATUS_INTERVAL, (Clock::now() - now) * 20);
                } else if (!cancel) {
                    // The repository is gone
                    if (repo.status && repo.status->repository) {
                        update.status = GitStatus();
                    }
                    for (const std::string& file : openFiles) {
                        update.files.push_back({file, std::nullopt});
                    }
                    repo = Repository();
                    repo.nextDiscovery = now + DISCOVERY_INTERVAL;
                }
            }
            if (repo.found && (refresh || stampChanged || filesChanged)) {
                readFiles(git, projectDir, repo.prefix, openFiles, update);
            }
        }

        if (!cancel && (update.status || !update.files.empty())) {
            Backend::getApp().enqueueMainThreadTask([this, update = std::move(update)]() mutable {
                apply(std::move(update));
            });
        }

        lock.lock();
    }
}

void editor::GitMonitor::readFiles(const fs::path& git, const fs::path& projectDir, const std::string& prefix,
                                   const std::vector<std::string>& openFiles, Update& update) {
    const std::string project = FileUtils::pathToUtf8(projectDir);
    std::vector<std::string> args = {"--literal-pathspecs", "-C", project, "ls-files", "--stage", "-z", "--"};
    const size_t fixedArgs = args.size();

    // Paths for git, empty for a file without base
    std::vector<std::string> paths;
    for (const std::string& file : openFiles) {
        const fs::path relative = fs::path(file).lexically_normal();
        std::string path = FileUtils::pathToGenericUtf8(relative);

        // Out of the project it may be out of the repository, and through a symlink the
        // editor shows another file than the staged one
        std::error_code ec;
        if (relative.empty() || relative.is_absolute() || path == ".." || path.rfind("../", 0) == 0 ||
            fs::is_symlink(projectDir / relative, ec)) {
            path.clear();
        } else {
            args.push_back(path);
        }
        paths.push_back(path);
    }

    std::unordered_map<std::string, std::string> staged;
    if (args.size() > fixedArgs) {
        ProcessOutput out = runProcess(git, args, cancel);
        if (out.ok) {
            parseStagedFiles(out.text, staged);
        } else {
            // One bad path (in a submodule, say) fails them all
            for (size_t i = fixedArgs; i < args.size() && !cancel; i++) {
                std::vector<std::string> single(args.begin(), args.begin() + fixedArgs);
                single.push_back(args[i]);
                ProcessOutput one = runProcess(git, single, cancel);
                if (one.ok) parseStagedFiles(one.text, staged);
            }
        }
    }

    // Read every time, the checked out form also depends on attributes and config.
    // --path is relative to the repository root.
    for (size_t i = 0; i < openFiles.size(); i++) {
        auto it = paths[i].empty() ? staged.end() : staged.find(paths[i]);
        std::optional<std::string> content;
        if (it != staged.end() && !it->second.empty()) {
            ProcessOutput out = runProcess(git, {"-C", project, "cat-file", "--filters", "--path=" + prefix + paths[i], it->second},
                                           cancel);
            if (out.ok) content = std::move(out.text);
        }
        update.files.push_back({openFiles[i], std::move(content)});
    }
}
