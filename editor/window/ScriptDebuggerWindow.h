// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>

namespace quantum::editor {

class Project;
class RemoteLuaDebuggerClient;

class ScriptDebuggerWindow {
public:
    static constexpr const char* WINDOW_NAME = "Script Debugger";

    ScriptDebuggerWindow();
    ~ScriptDebuggerWindow();

    void setProject(Project* project) { targetProject = project; }
    void setOpen(bool open) { openState = open; }
    void open(Project* project);
    void show();
    bool isOpen() const { return openState; }

private:
    void drawLocalDebugger();
    void drawRemoteDebugger();
    void drawRemoteConnection();

    Project* targetProject = nullptr;
    bool openState = false;
    std::unique_ptr<RemoteLuaDebuggerClient> remoteClient;

    char source[512]{};
    int line = 1;
    char breakpointCondition[512]{};
    char watchExpression[256]{};
    char watchResult[512]{};
    char watchError[512]{};

    char remoteHost[64] = "127.0.0.1";
    int remotePort = 0;
    char remoteToken[256]{};
};

}


