// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>

namespace quantum::editor {

class Project;

class NetworkPlayWindow {
public:
    NetworkPlayWindow() = default;
    ~NetworkPlayWindow();
    NetworkPlayWindow(const NetworkPlayWindow&) = delete;
    NetworkPlayWindow& operator=(const NetworkPlayWindow&) = delete;

    void open(Project* project);
    void show();

private:
    struct Process {
        unsigned long pid = 0;
#ifdef _WIN32
        void* handle = nullptr;
#endif
        bool statusKnown = true;
        bool stopRequested = false;
        char status[128] = "Not started";

        void poll();
    };

    Process serverProcess;
    Process clientProcess;
    std::filesystem::path projectPath;
    bool isOpen = false;
    char sceneEntry[256] = "main";
    char clientAddress[128] = "127.0.0.1";
    char networkToken[256] = {};
    int port = 39179;
    int windowWidth = 1280;
    int windowHeight = 720;
    char status[512] = {};

    std::filesystem::path engineExecutable() const;
    bool launch(bool server);
    void stopProcess(const char* role, Process& process);
    void drawProcess(const char* role, Process& process);
    void requestStop(const char* role, Process& process);
    void setStatus(const char* format, ...);
};

}
