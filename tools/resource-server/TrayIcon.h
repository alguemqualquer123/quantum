// (c) SR VINIX
// SPDX-License-Identifier: MIT

// TrayIcon.h - Windows notification-area presence for quantum-resource-server.
//
// The server is meant to outlive the editor: it keeps indexing the project and
// sits in the tray (the area where Discord and the anti-cheat agents live) so
// the user can reach a service whose window they closed. A left click brings
// the connections window back; the menu is the way to stop the process:
// "Close server" ends this process, "Close server and editor" asks the editor
// that spawned it to quit first. POSIX builds compile a no-op object and the
// server behaves exactly as before.

#pragma once

#include <string>

namespace quantum::rs {

// Everything the menu and the tooltip show, refreshed every poll slice.
struct TraySnapshot {
    std::string projectName;   // UTF-8, used in the tooltip
    std::string projectPath;   // UTF-8
    std::string address;       // "127.0.0.1:51234"
    std::string watchedLine;   // "132 files (scan 7)"
    bool editorRunning = false;
};

enum class TrayChoice { None, ShowWindow, QuitServer, QuitServerAndEditor };

class TrayIcon {
public:
    ~TrayIcon();

    // Adds the icon to the notification area. Returns false when the platform
    // has no tray or the shell refused; either way the object is safe to keep
    // using (every call becomes a no-op) and the server must still serve.
    bool create(const TraySnapshot& snapshot);
    void destroy();

    // Refreshes the tooltip and the menu contents.
    void setSnapshot(const TraySnapshot& snapshot);

    // Current contents, for the window procedure that builds the menu.
    const TraySnapshot& snapshot() const { return snapshot_; }

    // Runs the queued shell messages without waiting. Call once per poll slice;
    // returns the menu choice the user made since the previous call.
    TrayChoice pump();

    // Internal: the window procedure records the user's choice here.
    void choose(TrayChoice choice) { pending_ = choice; }

private:
    // HWND, kept as void* so this header stays free of <windows.h>.
    void* hwnd_ = nullptr;
    bool added_ = false;
    TraySnapshot snapshot_;
    TrayChoice pending_ = TrayChoice::None;
};

// True while `pid` still names a running process.
bool processIsRunning(unsigned long long pid);

// Asks `pid` to close its main window (WM_CLOSE, so the editor can save).
// Returns false when the process has no top-level window to close.
bool requestProcessCloseWindow(unsigned long long pid);

}
