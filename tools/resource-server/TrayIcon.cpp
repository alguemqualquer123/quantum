// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

// TrayIcon.cpp - see TrayIcon.h.

#include "TrayIcon.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
    // WIN32_LEAN_AND_MEAN is defined for this target (see the .cmake); this
    // translation unit uses no sockets, so plain <windows.h> is safe here.
    #include <windows.h>
    #include <shellapi.h>

    // Older SDK and MinGW headers hide these behind _WIN32_IE version gates; the
    // values are fixed by the shell ABI, so spelling them out keeps both
    // toolchains happy.
    #ifndef NIN_SELECT
        #define NIN_SELECT (WM_USER + 0)
    #endif
    #ifndef NIN_BALLOONUSERCLICK
        #define NIN_BALLOONUSERCLICK (WM_USER + 5)
    #endif
#else
    #include <csignal>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace doriax::rs {

#ifdef _WIN32

namespace {

// One tray per process, so the window procedure can reach the object without
// subclassing tricks.
TrayIcon* g_tray = nullptr;

constexpr UINT kTrayId = 0x44525331;           // 'DRS1'
constexpr UINT kTrayCallback = WM_APP + 11;    // shell notifications
constexpr UINT kMenuQuitServer = 1;
constexpr UINT kMenuQuitServerAndEditor = 2;
constexpr const wchar_t kWindowClass[] = L"doriaxResourceServerTray";

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), &out[0], n);
    return out;
}

// The shell fields are fixed-size wchar_t arrays and the secure CRT templates
// are not portable to MinGW, so the copy is bounded by hand.
void copyInto(wchar_t* dst, size_t cap, const std::wstring& src) {
    size_t i = 0;
    for (; i + 1 < cap && i < src.size(); ++i) dst[i] = src[i];
    dst[i] = L'\0';
}

void copyField(wchar_t* dst, size_t dstBytes, const std::wstring& src) {
    copyInto(dst, dstBytes / sizeof(wchar_t), src);
}

NOTIFYICONDATAW trayData(HWND hwnd) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = kTrayId;
    return data;
}

// The icon comes from the executable's own resources: doriax_add_windows_resource()
// embeds editor/resources/icons/doriax-editor.ico under the resource name GLFW_ICON
// (that is the name GLFW looks up in the editor; here it is only a label).
HICON appIcon() {
    int cx = GetSystemMetrics(SM_CXSMICON);
    if (cx <= 0) cx = 16;
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), L"GLFW_ICON",
                                               IMAGE_ICON, cx, cx, LR_SHARED));
    if (!icon) {
        // MAKEINTRESOURCEW(32512) is IDI_APPLICATION. Spelled as a number because
        // this target is not built with UNICODE defined, where the IDI_* macros
        // expand through the ANSI MAKEINTRESOURCE.
        icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }
    return icon;
}

std::wstring tooltipText(const TraySnapshot& snap) {
    std::wstring tip = L"doriax resource server";
    if (!snap.projectName.empty()) tip += L" - " + toWide(snap.projectName);
    if (!snap.address.empty()) tip += L" (" + toWide(snap.address) + L")";
    return tip;
}

void applyTooltip(HWND hwnd, const TraySnapshot& snap) {
    NOTIFYICONDATAW data = trayData(hwnd);
    data.uFlags = NIF_TIP;
    copyField(data.szTip, sizeof(data.szTip), tooltipText(snap));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void showBalloon(HWND hwnd, const TraySnapshot& snap) {
    NOTIFYICONDATAW data = trayData(hwnd);
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    copyField(data.szInfoTitle, sizeof(data.szInfoTitle), L"doriax resource server");

    std::wstring text;
    if (!snap.projectPath.empty()) text += toWide(snap.projectPath) + L"\n";
    if (!snap.address.empty()) text += toWide(snap.address) + L"\n";
    if (!snap.watchedLine.empty()) text += toWide(snap.watchedLine);
    if (text.empty()) text = L"idle";
    copyField(data.szInfo, sizeof(data.szInfo), text);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

// Modal: while the menu is on screen the server is not accepting requests. A
// tray menu is dismissed long before the editor's request timeout, so serving
// simply resumes on the next poll slice.
void showMenu(HWND hwnd, const TraySnapshot& snap) {
    // Documented requirement: without a foreground owner the popup never goes
    // away when the user clicks elsewhere.
    SetForegroundWindow(hwnd);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    // The state lines are read-only, so they carry id 0 and stay grayed out.
    const auto addInfoLine = [menu](const std::wstring& text) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, text.c_str());
    };
    addInfoLine(L"doriax-resource-server");
    if (!snap.projectName.empty()) addInfoLine(L"Project: " + toWide(snap.projectName));
    if (!snap.address.empty()) addInfoLine(L"Address: " + toWide(snap.address));
    if (!snap.watchedLine.empty()) addInfoLine(L"Watching: " + toWide(snap.watchedLine));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuitServer, L"Close server");
    AppendMenuW(menu, snap.editorRunning ? MF_STRING : MF_STRING | MF_GRAYED,
                kMenuQuitServerAndEditor, L"Close server and editor");

    POINT where{};
    GetCursorPos(&where);
    // No TrackPopupMenuW in the SDK: TrackPopupMenu itself is the wide entry.
    const UINT choice = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                       int(where.x), int(where.y), 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (g_tray) {
        if (choice == kMenuQuitServer) g_tray->choose(TrayChoice::QuitServer);
        else if (choice == kMenuQuitServerAndEditor)
            g_tray->choose(TrayChoice::QuitServerAndEditor);
    }
}

LRESULT CALLBACK trayWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kTrayCallback) {
        const TraySnapshot snap = g_tray ? g_tray->snapshot() : TraySnapshot();
        switch (LOWORD(lParam)) {
            // An icon added without a version reports raw mouse messages; the
            // shell sends the NIN_* codes instead. Both arrive on this message.
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                showMenu(hwnd, snap);
                return 0;
            case WM_LBUTTONDBLCLK:
            case NIN_SELECT:
            case NIN_BALLOONUSERCLICK:
                showBalloon(hwnd, snap);
                return 0;
            default:
                break;
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

struct WindowScan {
    unsigned long long pid;
    HWND found;
};

BOOL CALLBACK collectTopLevelWindow(HWND hwnd, LPARAM param) {
    WindowScan* scan = reinterpret_cast<WindowScan*>(param);
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (static_cast<unsigned long long>(windowPid) != scan->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    // Only the unowned top-level window: an editor tool window closing would not
    // quit anything, and the main window is the one without an owner.
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    scan->found = hwnd;
    return FALSE;
}

bool registerTrayClass() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = trayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    // One tray and one registration per process, so no "already registered" case.
    return RegisterClassW(&wc) != 0;
}

}  // namespace

bool processIsRunning(unsigned long long pid) {
    if (pid == 0) return false;
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) return false;
    const bool running = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    CloseHandle(handle);
    return running;
}

bool requestProcessCloseWindow(unsigned long long pid) {
    if (pid == 0) return false;
    WindowScan scan{ pid, nullptr };
    EnumWindows(collectTopLevelWindow, reinterpret_cast<LPARAM>(&scan));
    if (!scan.found) return false;
    // WM_CLOSE, not TerminateProcess: the editor gets to flush the scene it is
    // showing and to ask about unsaved changes.
    return PostMessageW(scan.found, WM_CLOSE, 0, 0) != FALSE;
}

TrayIcon::~TrayIcon() {
    destroy();
}

bool TrayIcon::create(const TraySnapshot& snapshot) {
    snapshot_ = snapshot;
    if (hwnd_) return added_;

    if (!registerTrayClass()) return false;

    // A message-only window is how a process with no UI owns a tray icon: the
    // shell SendMessage()s the callback straight to this hwnd.
    HWND hwnd = CreateWindowExW(0, kWindowClass, L"doriax-resource-server", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return false;

    NOTIFYICONDATAW data = trayData(hwnd);
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallback;
    data.hIcon = appIcon();
    copyField(data.szTip, sizeof(data.szTip), tooltipText(snapshot));

    g_tray = this;
    if (Shell_NotifyIconW(NIM_ADD, &data) == FALSE) {
        g_tray = nullptr;
        DestroyWindow(hwnd);
        return false;
    }
    hwnd_ = hwnd;
    added_ = true;
    return true;
}

void TrayIcon::destroy() {
    if (!hwnd_) {
        added_ = false;
        return;
    }
    if (added_) {
        NOTIFYICONDATAW data = trayData(static_cast<HWND>(hwnd_));
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    DestroyWindow(static_cast<HWND>(hwnd_));
    hwnd_ = nullptr;
    added_ = false;
    if (g_tray == this) g_tray = nullptr;
}

void TrayIcon::setSnapshot(const TraySnapshot& snapshot) {
    snapshot_ = snapshot;
    if (hwnd_ && added_) applyTooltip(static_cast<HWND>(hwnd_), snapshot_);
}

TrayChoice TrayIcon::pump() {
    if (!hwnd_) return TrayChoice::None;
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    const TrayChoice choice = pending_;
    pending_ = TrayChoice::None;
    return choice;
}

#else
// ---------------------------------------------------------------------------
// POSIX: no notification area to live in, so the editor owns the server's
// lifetime exactly as before this feature.
// ---------------------------------------------------------------------------

bool processIsRunning(unsigned long long pid) {
    if (pid == 0) return false;
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    // EPERM means the process exists and simply is not ours to signal.
    return errno == EPERM;
}

bool requestProcessCloseWindow(unsigned long long) {
    return false;
}

TrayIcon::~TrayIcon() {}

bool TrayIcon::create(const TraySnapshot& snapshot) {
    snapshot_ = snapshot;
    return false;
}

void TrayIcon::destroy() {}

void TrayIcon::setSnapshot(const TraySnapshot& snapshot) {
    snapshot_ = snapshot;
}

TrayChoice TrayIcon::pump() {
    return TrayChoice::None;
}

#endif

}
