// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "util/ProcessLaunch.h"

#include "Out.h"

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
#endif

using namespace quantum;

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
std::string quoteArgument(const std::string& argument) {
    std::string quoted = "\"";
    for (const char character : argument) {
        if (character == '"') {
            quoted += "\\\"";
        } else {
            quoted += character;
        }
    }
    quoted += "\"";
    return quoted;
}
#endif

} // namespace

namespace {

bool launchDetached(const std::filesystem::path& executable,
                    const std::vector<std::string>& arguments,
                    const std::filesystem::path& workingDir,
                    bool visibleWindow) {
#ifdef _WIN32
    std::string command = quoteArgument(executable.string());
    for (const std::string& argument : arguments) {
        command += " ";
        command += quoteArgument(argument);
    }

    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    const std::string workingDirString = workingDir.string();
    const char* workingDirPointer = workingDirString.empty() ? nullptr : workingDirString.c_str();

    const DWORD creationFlags = CREATE_NEW_PROCESS_GROUP |
        (visibleWindow ? 0 : CREATE_NO_WINDOW);
    const BOOL created = CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                                        creationFlags, nullptr, workingDirPointer, &startupInfo,
                                        &processInfo);
    if (!created) {
        quantum::editor::Out::error("Failed to launch \"%s\" (error %lu).", executable.string().c_str(),
                   static_cast<unsigned long>(GetLastError()));
        return false;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
#else
    const pid_t pid = fork();
    if (pid < 0) {
        quantum::editor::Out::error("Failed to launch \"%s\": fork failed.", executable.string().c_str());
        return false;
    }

    if (pid == 0) {
        // Child: detach from this session so closing the launcher never takes
        // the started process with it.
        setsid();
        if (!workingDir.empty()) {
            std::error_code ec;
            fs::current_path(workingDir, ec);
            if (ec) {
                _exit(127);
            }
        }

        std::vector<std::string> storage;
        storage.reserve(arguments.size() + 1);
        storage.push_back(executable.string());
        for (const std::string& argument : arguments) {
            storage.push_back(argument);
        }

        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (std::string& argument : storage) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        // execvp searches PATH, so the platform tools below work by name too.
        execvp(storage[0].c_str(), argv.data());
        _exit(127);
    }

    return true;
#endif
}

} // namespace

bool editor::ProcessLaunch::detached(const std::filesystem::path& executable,
                                     const std::vector<std::string>& arguments,
                                     const std::filesystem::path& workingDir) {
    return launchDetached(executable, arguments, workingDir, false);
}

bool editor::ProcessLaunch::detachedVisible(const std::filesystem::path& executable,
                                            const std::vector<std::string>& arguments,
                                            const std::filesystem::path& workingDir) {
    return launchDetached(executable, arguments, workingDir, true);
}

bool editor::ProcessLaunch::revealFolder(const std::filesystem::path& folder) {
#ifdef _WIN32
    const std::wstring wide = folder.wstring();
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
#elif defined(__APPLE__)
    return detached("open", {folder.string()}, {});
#else
    return detached("xdg-open", {folder.string()}, {});
#endif
}
