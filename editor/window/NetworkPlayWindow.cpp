// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "window/NetworkPlayWindow.h"

#include "App.h"
#include "Project.h"
#include "Backend.h"
#include "imgui.h"
#include "util/FileUtils.h"

#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#ifndef _WIN32
namespace {

std::vector<std::string> makeChildEnvironment(const char* name, const std::string& value) {
    std::vector<std::string> environment;
    const std::string prefix = std::string(name) + '=';
    for (char** entry = environ; entry && *entry; ++entry) {
        if (std::strncmp(*entry, prefix.c_str(), prefix.size()) != 0)
            environment.emplace_back(*entry);
    }
    environment.push_back(prefix + value);
    return environment;
}

}
#endif

namespace quantum::editor {

#ifdef _WIN32
namespace {

void appendArgument(std::string& command, const std::string& argument) {
    if (!command.empty()) command += ' ';
    command += '"';
    // The Windows CRT treats backslashes specially before a quote.
    std::size_t backslashes = 0;
    for (char character : argument) {
        if (character == '\\') {
            ++backslashes;
            continue;
        }
        command.append(backslashes * (character == '"' ? 2 : 1), '\\');
        backslashes = 0;
        if (character == '"') command += '\\';
        command += character;
    }
    command.append(backslashes * 2, '\\');
    command += '"';
}

struct CloseRequest {
    HANDLE process;
    DWORD pid;
    bool posted = false;
};

#ifdef _WIN32
std::string makeChildEnvironment(const char* name, const std::string& value) {
    std::string block;
    LPCH inherited = GetEnvironmentStringsA();
    if (inherited) {
        for (const char* entry = inherited; *entry; entry += std::strlen(entry) + 1) {
            const char* separator = std::strchr(entry, '=');
            if (separator && std::string(entry, static_cast<std::size_t>(separator - entry))
                                  .compare(name) == 0) {
                continue;
            }
            block.append(entry);
            block.push_back('\0');
        }
        FreeEnvironmentStringsA(inherited);
    }
    block.append(name);
    block.push_back('=');
    block.append(value);
    block.push_back('\0');
    block.push_back('\0');
    return block;
}
#else
std::vector<std::string> makeChildEnvironment(const char* name, const std::string& value) {
    std::vector<std::string> environment;
    const std::string prefix = std::string(name) + '=';
    for (char** entry = environ; entry && *entry; ++entry) {
        if (std::strncmp(*entry, prefix.c_str(), prefix.size()) != 0)
            environment.emplace_back(*entry);
    }
    environment.push_back(prefix + value);
    return environment;
}
#endif

BOOL CALLBACK closeProcessWindow(HWND window, LPARAM data) {
    auto& request = *reinterpret_cast<CloseRequest*>(data);
    if (!IsWindowVisible(window)) return TRUE;
    if (WaitForSingleObject(request.process, 0) != WAIT_TIMEOUT) return FALSE;

    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != request.pid) return TRUE;
    if (PostMessageW(window, WM_CLOSE, 0, 0)) request.posted = true;
    return TRUE;
}

}
#endif

NetworkPlayWindow::~NetworkPlayWindow() {
    stopProcess("Server", serverProcess);
    stopProcess("Client", clientProcess);
}

void NetworkPlayWindow::Process::poll() {
    if (!pid) return;
#ifdef _WIN32
    const DWORD result = WaitForSingleObject(handle, 0);
    statusKnown = result == WAIT_TIMEOUT;
    if (statusKnown) return;
    if (result != WAIT_OBJECT_0) {
        std::snprintf(status, sizeof(status), "Status unavailable (error %lu)", GetLastError());
        return;
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(handle, &exitCode)) {
        std::snprintf(status, sizeof(status), "Exited (code %lu)", exitCode);
    } else {
        std::snprintf(status, sizeof(status), "Exited (code unavailable)");
    }
    CloseHandle(handle);
    handle = nullptr;
#else
    int exitStatus = 0;
    pid_t result;
    do {
        result = waitpid(static_cast<pid_t>(pid), &exitStatus, WNOHANG);
    } while (result == -1 && errno == EINTR);
    statusKnown = result == 0;
    if (statusKnown) return;
    if (result == -1) {
        const int error = errno;
        std::snprintf(status, sizeof(status), "Status unavailable (error %d)", error);
        if (error != ECHILD) return;
    } else if (WIFEXITED(exitStatus)) {
        std::snprintf(status, sizeof(status), "Exited (code %d)", WEXITSTATUS(exitStatus));
    } else if (WIFSIGNALED(exitStatus)) {
        std::snprintf(status, sizeof(status), "Exited (signal %d)", WTERMSIG(exitStatus));
    } else {
        return;
    }
#endif
    pid = 0;
    stopRequested = false;
}

void NetworkPlayWindow::open(Project* targetProject) {
    const std::filesystem::path nextProjectPath = targetProject ? targetProject->getProjectPath()
                                                                : std::filesystem::path();
    const bool processActive = serverProcess.pid != 0 || clientProcess.pid != 0;
    if (processActive && !projectPath.empty() && nextProjectPath != projectPath) {
        setStatus("Stop the active network session before changing projects.");
        isOpen = true;
        return;
    }
    projectPath = nextProjectPath;
    isOpen = !projectPath.empty();
}

void NetworkPlayWindow::setStatus(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(status, sizeof(status), format, arguments);
    va_end(arguments);
}

std::filesystem::path NetworkPlayWindow::engineExecutable() const {
    std::filesystem::path directory = FileUtils::getExecutableDir();
#ifdef _WIN32
    const std::filesystem::path candidate = directory / "quantum-engine.exe";
#else
    const std::filesystem::path candidate = directory / "quantum-engine";
#endif
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error)) return candidate;

    const std::filesystem::path engineFolder = directory / "engine" / candidate.filename();
    if (std::filesystem::is_regular_file(engineFolder, error)) return engineFolder;
    return {};
}

bool NetworkPlayWindow::launch(bool server) {
    Process& process = server ? serverProcess : clientProcess;
    process.poll();
    if (process.pid) return false;

    if (projectPath.empty()) {
        setStatus("No project is open.");
        return false;
    }

    const std::filesystem::path executable = engineExecutable();
    if (executable.empty()) {
        setStatus("Could not find quantum-engine next to the editor executable.");
        return false;
    }

    std::error_code error;
    if (projectPath.empty() || !std::filesystem::is_directory(projectPath, error)) {
        setStatus("The project must be saved before launching network players.");
        return false;
    }

    const std::string role = server ? "Server" : "Client";
    std::vector<std::string> arguments = {
        "--project", projectPath.string(),
        "--scene", sceneEntry,
        "--window", std::to_string(windowWidth) + "x" + std::to_string(windowHeight),
        "--window-title", "Quantum Network " + role,
    };
    if (server) {
        arguments.push_back("--network-server");
        arguments.push_back(std::to_string(port));
    } else {
        arguments.push_back("--network-client");
        arguments.push_back(clientAddress);
        arguments.push_back(std::to_string(port));
    }
    const bool hasNetworkToken = networkToken[0] != '\0';
    if (hasNetworkToken) {
        arguments.push_back("--network-token-env");
        arguments.push_back("QUANTUM_NETWORK_TOKEN");
    }

#ifdef _WIN32
    std::string executableName = executable.string();
    const std::string workingDirectory = arguments[1];
    std::string command;
    appendArgument(command, executableName);
    for (const auto& argument : arguments) appendArgument(command, argument);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    std::string environment;
    if (hasNetworkToken) environment = makeChildEnvironment("QUANTUM_NETWORK_TOKEN", networkToken);
    if (!CreateProcessA(executableName.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP, hasNetworkToken ? environment.data() : nullptr,
                        workingDirectory.c_str(), &startup, &child)) {
        setStatus("Could not launch the %s process (error %lu).", role.c_str(), GetLastError());
        return false;
    }
    CloseHandle(child.hThread);
    process.handle = child.hProcess;
    process.pid = child.dwProcessId;
#else
    std::string executableName = executable.string();
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(executableName.data());
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::vector<std::string> childEnvironment;
    std::vector<char*> environmentPointers;
    if (hasNetworkToken) {
        childEnvironment = makeChildEnvironment("QUANTUM_NETWORK_TOKEN", networkToken);
        environmentPointers.reserve(childEnvironment.size() + 1);
        for (std::string& entry : childEnvironment) environmentPointers.push_back(entry.data());
        environmentPointers.push_back(nullptr);
    }
    const char* workingDirectory = arguments[1].c_str();
    char** childArguments = argv.data();
    char** childEnvironmentData = hasNetworkToken ? environmentPointers.data() : environ;

    const pid_t child = fork();
    if (child == -1) {
        setStatus("Could not launch the %s process (error %d).", role.c_str(), errno);
        return false;
    }
    if (child == 0) {
        // Only use async-signal-safe operations after fork in the threaded editor.
        if (setsid() == -1 || chdir(workingDirectory) == -1) _exit(126);
        execve(childArguments[0], childArguments, childEnvironmentData);
        _exit(127);
    }
    process.pid = static_cast<unsigned long>(child);
#endif

    process.statusKnown = true;
    process.stopRequested = false;
    status[0] = '\0';
    return true;
}

void NetworkPlayWindow::stopProcess(const char* role, Process& process) {
    process.poll();
    if (!process.pid) return;

#ifdef _WIN32
    CloseRequest request{process.handle, process.pid};
    EnumWindows(closeProcessWindow, reinterpret_cast<LPARAM>(&request));
    if (WaitForSingleObject(process.handle, 2000) == WAIT_TIMEOUT) {
        // This handle belongs to the child created by this panel, so the fallback
        // cannot affect an unrelated process.
        TerminateProcess(process.handle, 0xC000013A);
        WaitForSingleObject(process.handle, 1000);
    }
    process.poll();
    if (process.handle) {
        CloseHandle(process.handle);
        process.handle = nullptr;
    }
#else
    const pid_t child = static_cast<pid_t>(process.pid);
    // launch() creates a dedicated session, so signal the complete child group.
    kill(-child, SIGTERM);
    for (int attempt = 0; attempt < 20 && process.pid; ++attempt) {
        process.poll();
        if (!process.pid) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (process.pid) {
        kill(-child, SIGKILL);
        process.poll();
    }
#endif
    if (process.pid) {
        std::snprintf(process.status, sizeof(process.status), "%s stopped", role);
        process.pid = 0;
        process.statusKnown = true;
        process.stopRequested = false;
    }
}

void NetworkPlayWindow::requestStop(const char* role, Process& process) {
    process.poll();
    if (!process.pid || !process.statusKnown) return;

#ifdef _WIN32
    CloseRequest request{process.handle, process.pid};
    EnumWindows(closeProcessWindow, reinterpret_cast<LPARAM>(&request));
    if (!request.posted) {
        setStatus("Could not send a close request to the %s window. Try again once it appears.", role);
        return;
    }
#else
    // A live child or its unreaped exit status keeps this PID reserved.
    if (kill(static_cast<pid_t>(process.pid), SIGTERM) == -1) {
        setStatus("Could not send SIGTERM to the %s process (error %d).", role, errno);
        return;
    }
#endif
    process.stopRequested = true;
    status[0] = '\0';
}

void NetworkPlayWindow::drawProcess(const char* role, Process& process) {
    const char* state = process.status;
    if (process.pid && process.statusKnown) {
        state = process.stopRequested ? "Stop requested" : "Running";
    }
    if (process.pid) {
        ImGui::Text("%s (PID %lu): %s", role, process.pid, state);
    } else {
        ImGui::Text("%s: %s", role, state);
    }

    ImGui::PushID(role);
    ImGui::BeginDisabled(!process.pid || !process.statusKnown);
#ifdef _WIN32
    const char* stopLabel = "Request window close";
#else
    const char* stopLabel = "Send SIGTERM";
#endif
    if (ImGui::Button(stopLabel)) requestStop(role, process);
    ImGui::EndDisabled();
    ImGui::PopID();
}

void NetworkPlayWindow::show() {
    for (Process* process : {&serverProcess, &clientProcess}) {
        const auto previousPid = process->pid;
        const bool previousStatusKnown = process->statusKnown;
        process->poll();
        if (isOpen && (process->pid != previousPid || process->statusKnown != previousStatusKnown)) {
            Backend::getApp().requestRedraw();
        }
    }
    if (!isOpen) return;

    ImGui::SetNextWindowSize(ImVec2(500.0f, 440.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Network Play", &isOpen)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("Launch two independent runtime windows for a local multiplayer test. "
                      "The scene entry is the Lua entry used by quantum-engine.");
    ImGui::Separator();
    const bool sessionActive = serverProcess.pid != 0 || clientProcess.pid != 0;
    ImGui::BeginDisabled(sessionActive);
    ImGui::InputText("Scene entry", sceneEntry, sizeof(sceneEntry));
    ImGui::InputText("Client address", clientAddress, sizeof(clientAddress));
    ImGui::InputText("Session token", networkToken, sizeof(networkToken), ImGuiInputTextFlags_Password);
    ImGui::InputInt("Port", &port);
    if (port < 1) port = 1;
    if (port > 65535) port = 65535;
    ImGui::InputInt("Window width", &windowWidth);
    ImGui::InputInt("Window height", &windowHeight);
    if (windowWidth < 320) windowWidth = 320;
    if (windowHeight < 240) windowHeight = 240;
    ImGui::EndDisabled();

    ImGui::BeginDisabled(serverProcess.pid != 0);
    if (ImGui::Button("Launch server window")) launch(true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(clientProcess.pid != 0);
    if (ImGui::Button("Launch client window")) launch(false);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(serverProcess.pid != 0 || clientProcess.pid != 0);
    if (ImGui::Button("Launch both windows")) {
        if (launch(true) && !launch(false)) {
            stopProcess("Server", serverProcess);
            setStatus("Client launch failed; the server was stopped.");
        }
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    drawProcess("Server", serverProcess);
    drawProcess("Client", clientProcess);
    if (status[0] != '\0') ImGui::TextWrapped("%s", status);
    ImGui::End();
}

}
