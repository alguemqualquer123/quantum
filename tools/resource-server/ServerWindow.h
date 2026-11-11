// (c) SR VINIX
// SPDX-License-Identifier: MIT

// ServerWindow.h - the connections window of quantum-resource-server.
//
// Plain Win32 (user32 + gdi32 + comctl32), because this target deliberately
// links nothing but the standard library and the system socket/shell libs (see
// quantum_resource_server.cmake): no engine, no editor, no imgui. The window is
// an observer only - the serving loop owns every piece of state and hands a
// WindowSnapshot in once per poll slice, so painting can never block a request.
//
// Non-Windows builds compile the same class as a no-op, so the server keeps its
// headless behaviour there (the same arrangement as TrayIcon.h).

#pragma once

#include <string>
#include <vector>

#include "Connections.h"

namespace quantum::rs {

// Everything the window shows, already turned into display strings by the
// serving loop: the window never reaches into ResourceIndex.
struct WindowSnapshot {
    std::vector<std::wstring> headerLines;   // project, address, watching, uptime, totals
    std::vector<ClientGroup> clients;        // "who is connected"
    std::vector<ConnectionRecord> records;   // oldest first, as the log holds them
    unsigned long long generation = 0;       // changes whenever the record set does
};

class ServerWindow {
public:
    ~ServerWindow();

    // Registers the class and creates the window. Returns false when the
    // platform has no windowing or creation failed; the caller then simply
    // carries on without UI.
    bool create(const std::string& windowTitle);
    void destroy();

    void show();
    void hide();
    bool visible() const;

    // Refreshes the contents. Called every poll slice; rows are only rebuilt
    // when the generation moves, so an idle server repaints its header alone.
    void setSnapshot(const WindowSnapshot& snapshot);

    // Runs queued messages without waiting.
    void pump();

    // Set by the window's buttons, consumed by the serving loop.
    bool quitRequested() const { return quitRequested_; }
    bool clearRequested() const { return clearRequested_; }
    void acknowledgeClear() { clearRequested_ = false; }

    // --- used by the window procedure (Windows only) ---
    void attach(void* hwnd, void* listClients, void* listLog);
    void detach();
    const WindowSnapshot& snapshot() const { return snapshot_; }
    void rebuildLists();
    void refreshAges();
    void repaintHeader();
    void placeChildren(int header, int width, int clientsHeight, int logHeight, int buttonTop);
    // Recomputes the split from the current client rect and header height. WM_SIZE
    // and any snapshot that changes the number of header lines call it.
    void relayout();
    int headerHeight() const;
    void requestQuit() { quitRequested_ = true; }
    void requestClear() { clearRequested_ = true; }

private:
    // HWNDs, kept as void* so this header stays free of <windows.h>.
    void* hwnd_ = nullptr;
    void* listClients_ = nullptr;
    void* listLog_ = nullptr;
    WindowSnapshot snapshot_;
    unsigned long long shownGeneration_ = 0;
    bool quitRequested_ = false;
    bool clearRequested_ = false;
};

// UTF-8 to UTF-16 for the window's display strings. Empty on POSIX, where the
// class is inert anyway.
std::wstring wideFromUtf8(const std::string& utf8);

}
