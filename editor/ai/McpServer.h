// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AiTypes.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace httplib {
class Server;
struct Request;
struct Response;
}

namespace quantum::editor {
class Project;
class ResourcesWindow;
}

namespace quantum::editor::ai {

// MCP server on 127.0.0.1 that gives outside agents (Claude Code, Cursor, ...)
// the editor actions of the AI chat. Tool calls run on the main thread.
class McpServer {
public:
    McpServer(Project* project, ResourcesWindow* resourcesWindow);
    ~McpServer();

    // Starts, restarts or stops the listener. Main thread only.
    void applySettings(const McpSettings& settings);
    void shutdown();

    bool isRunning() const;
    std::string getError() const;

    static std::string endpointUrl(int port);
    // Bearer token kept in the SecretStore, empty until Editor Settings creates one
    static std::string storedToken();
    static std::string newToken();

private:
    struct PendingCall;

    Project* project;
    ResourcesWindow* resourcesWindow;

    std::unique_ptr<httplib::Server> server;
    std::thread listener;
    int port = 0;
    std::string error;

    std::atomic<bool> stopping{false};
    std::atomic<bool> allowChanges{true};

    std::mutex callsMutex;
    std::vector<std::shared_ptr<PendingCall>> pendingCalls;

    // Main thread only
    int budgetFrame = -1;
    std::chrono::steady_clock::time_point budgetStart;

    void start(int port);
    void handlePost(const httplib::Request& req, httplib::Response& res);
    Json dispatch(const std::string& method, const Json& id, const Json& params,
                  bool modern, const httplib::Request& req);
    Json callTool(const Json& id, const Json& params, const httplib::Request& req);
    ActionResult runOnMainThread(const std::string& name, const Json& arguments,
                                 const std::string& description, const Json& id,
                                 const httplib::Request& req);
    void runCall(const std::shared_ptr<PendingCall>& call);
    void cancelCall(const Json& requestId);
};

} // namespace quantum::editor::ai
