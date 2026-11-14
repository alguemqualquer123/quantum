// (c) SR VINIX
// SPDX-License-Identifier: MIT

// ServerWindow.cpp - see ServerWindow.h.

#include "ServerWindow.h"

#include <algorithm>
#include <cwchar>
#include <ctime>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <windows.h>
    #include <commctrl.h>
#endif

namespace quantum::rs {

#ifdef _WIN32

namespace {

// One window per process, so the static window procedure can reach the object
// without a GWLP_USERDATA write before the child controls exist (the same trick
// as tools/resource-server/TrayIcon.cpp).
ServerWindow* g_window = nullptr;

constexpr const wchar_t kWindowClass[] = L"quantumResourceServerWindow";
constexpr int kIdClients = 100;
constexpr int kIdLog = 101;
constexpr int kBtnClear = 200;
constexpr int kBtnHide = 201;
constexpr int kBtnQuit = 202;
constexpr UINT kRefreshTimer = 1;
constexpr UINT kRefreshMs = 1000;
constexpr int kClientColumns = 8;
constexpr int kLogColumns = 10;

// The manifest this target embeds is DPI aware, so the fixed 96-pixel layout
// numbers are scaled by the device DPI instead of being shown tiny.
int g_scale = 100;

int px(int value) { return value * g_scale / 100; }

int cy(LONG value) { return static_cast<int>(value); }

// The (COLOR_WINDOW + 1) idiom is an integer pseudo-handle, so it takes a
// reinterpret cast rather than the static_cast windows.h does not define.
HBRUSH windowBrush() {
    return reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
}

std::wstring wide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), &out[0], n);
    return out;
}

std::wstring number(unsigned long long value) {
    wchar_t buffer[32];
    std::swprintf(buffer, 32, L"%llu", value);
    return buffer;
}

long long nowUnix() { return static_cast<long long>(std::time(nullptr)); }

std::wstring clockOf(long long unixTime) {
    if (unixTime <= 0) return L"-";
    const time_t seconds = static_cast<time_t>(unixTime);
    std::tm local{};
    localtime_s(&local, &seconds);
    wchar_t buffer[16];
    std::swprintf(buffer, 16, L"%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    return buffer;
}

std::wstring agoOf(long long unixTime) {
    if (unixTime <= 0) return L"-";
    const long long delta = nowUnix() - unixTime;
    if (delta < 2) return L"now";
    if (delta < 60) return std::to_wstring(delta) + L"s ago";
    if (delta < 3600) return std::to_wstring(delta / 60) + L"m ago";
    if (delta < 86400) return std::to_wstring(delta / 3600) + L"h ago";
    return std::to_wstring(delta / 86400) + L"d ago";
}

std::wstring durationOf(double ms) {
    if (ms < 1.0) return L"<1 ms";
    wchar_t buffer[24];
    std::swprintf(buffer, 24, L"%.1f ms", ms);
    return buffer;
}

// Row `row` of the log list (newest first) to its index in the oldest-first
// vector. SIZE_MAX when the row no longer exists.
size_t recordIndexForRow(const std::vector<ConnectionRecord>& records, int row) {
    if (row < 0 || static_cast<size_t>(row) >= records.size()) return SIZE_MAX;
    return records.size() - 1 - static_cast<size_t>(row);
}

void insertColumn(HWND list, int index, const wchar_t* title, int width, int format = LVCFMT_LEFT) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
    column.iSubItem = index;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = px(width);
    column.fmt = format;
    SendMessageW(list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index),
                 reinterpret_cast<LPARAM>(&column));
}

// The header paints an unnamed filler cell wherever the columns stop short of the
// control, which reads as a broken table; the last column takes the leftover so
// the two lists always end flush with their frame.
void stretchLastColumn(HWND list, int columnCount, int width) {
    if (!list || columnCount < 2 || width <= 0) return;
    int used = 0;
    for (int i = 0; i + 1 < columnCount; ++i) {
        used += static_cast<int>(SendMessageW(list, LVM_GETCOLUMNWIDTH, static_cast<WPARAM>(i), 0));
    }
    const int last = std::max(px(60), width - GetSystemMetrics(SM_CXVSCROLL) - used);
    SendMessageW(list, LVM_SETCOLUMNWIDTH, static_cast<WPARAM>(columnCount - 1),
                 static_cast<LPARAM>(last));
}

void setCell(HWND list, int row, int column, const std::wstring& text) {
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row),
                 reinterpret_cast<LPARAM>(&item));
}

void fillList(HWND list, const std::vector<std::vector<std::wstring>>& rows) {
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < rows.size(); ++i) {
        const std::vector<std::wstring>& cells = rows[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(cells.empty() ? L"" : cells[0].c_str());
        const int row = static_cast<int>(
            SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
        if (row < 0) continue;
        for (size_t c = 1; c < cells.size(); ++c) setCell(list, row, static_cast<int>(c), cells[c]);
    }
}

HWND createList(HWND parent, UINT_PTR id, const RECT& area) {
    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                    LVS_SHOWSELALWAYS,
                                area.left, area.top, area.right - area.left,
                                area.bottom - area.top, parent,
                                reinterpret_cast<HMENU>(id), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 static_cast<LPARAM>(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES));
    return list;
}

void createButton(HWND parent, UINT_PTR id, const wchar_t* text) {
    CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    0, 0, px(120), px(26), parent, reinterpret_cast<HMENU>(id),
                    GetModuleHandleW(nullptr), nullptr);
}

// The header block occupies the strip above the lists; the lists own the rest.
RECT headerRect(const RECT& client, int headerHeight) {
    return RECT{px(10), px(6), client.right - px(10), headerHeight};
}

void drawHeader(HDC dc, RECT rect, const WindowSnapshot& snapshot) {
    FillRect(dc, &rect, windowBrush());
    SetBkMode(dc, TRANSPARENT);
    RECT line = rect;
    for (const std::wstring& text : snapshot.headerLines) {
        line.bottom = line.top + px(18);
        DrawTextW(dc, text.c_str(), -1, &line, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        line.top = line.bottom + px(1);
    }
}

std::vector<std::vector<std::wstring>> clientRows(const std::vector<ClientGroup>& clients) {
    std::vector<std::vector<std::wstring>> rows;
    rows.reserve(clients.size());
    for (const ClientGroup& g : clients) {
        std::wstring who = g.processName.empty() ? L"unknown" : wide(g.processName);
        if (!g.client.empty()) who += L" [" + wide(g.client) + L"]";
        rows.push_back({who,
                        g.pidText.empty() ? L"-" : wide(g.pidText),
                        wide(g.peerHost.empty() ? "127.0.0.1" : g.peerHost),
                        number(g.requests),
                        number(g.failures),
                        wide(g.lastCommand),
                        agoOf(g.lastSeenUnix),
                        clockOf(g.firstSeenUnix)});
    }
    return rows;
}

std::vector<std::vector<std::wstring>> logRows(const std::vector<ConnectionRecord>& records) {
    std::vector<std::vector<std::wstring>> rows;
    rows.reserve(records.size());
    // Newest on top: the question the window answers is "what just happened".
    for (size_t i = records.size(); i-- > 0;) {
        const ConnectionRecord& r = records[i];
        std::wstring result = r.ok ? L"ok" : L"failed";
        if (!r.ok && !r.detail.empty()) result += L": " + wide(r.detail);
        rows.push_back({clockOf(r.unixTime),
                        agoOf(r.unixTime),
                        wide(r.command.empty() ? "?" : r.command),
                        wide(r.client.empty() ? "-" : r.client),
                        wide(r.processName.empty() ? "-" : r.processName),
                        r.ownerPid ? number(r.ownerPid) : L"-",
                        wide(r.peer),
                        result,
                        number(r.requestBytes) + L" / " + number(r.responseBytes),
                        durationOf(r.durationMs)});
    }
    return rows;
}

void showDetails(HWND parent, const ConnectionRecord& r) {
    std::wstring text;
    const auto line = [&text](const wchar_t* key, const std::wstring& value) {
        text += key;
        text += L": ";
        text += value;
        text += L"\r\n";
    };
    line(L"Request", number(r.id));
    line(L"Time", clockOf(r.unixTime) + L" (" + agoOf(r.unixTime) + L")");
    line(L"Command", wide(r.command.empty() ? "(not a json cmd)" : r.command));
    line(L"Client tag", wide(r.client.empty() ? "(the request sent none)" : r.client));
    line(L"Process", wide(r.processName.empty() ? "(not resolved)" : r.processName) +
                         (r.ownerPid ? L" (pid " + number(r.ownerPid) + L")" : std::wstring()));
    line(L"Peer", wide(r.peer));
    line(L"Result", r.ok ? L"ok" : L"failed");
    if (!r.detail.empty()) line(L"Detail", wide(r.detail));
    line(L"Bytes", number(r.requestBytes) + L" in / " + number(r.responseBytes) + L" out");
    line(L"Duration", durationOf(r.durationMs));
    MessageBoxW(parent, text.c_str(), L"Connection details", MB_OK | MB_ICONINFORMATION);
}

void showClientDetails(HWND parent, const ClientGroup& g) {
    std::wstring text;
    text += L"Process: " + (g.processName.empty() ? L"(not resolved)" : wide(g.processName)) +
            L"\r\n";
    text += L"Pid: " + (g.pidText.empty() ? L"-" : wide(g.pidText)) + L"\r\n";
    text += L"Client tag: " + (g.client.empty() ? L"(none)" : wide(g.client)) + L"\r\n";
    text += L"Peer host: " + wide(g.peerHost) + L"\r\n";
    text += L"Requests: " + number(g.requests) + L" (" + number(g.failures) + L" failed)\r\n";
    text += L"First seen: " + clockOf(g.firstSeenUnix) + L"\r\n";
    text += L"Last seen: " + clockOf(g.lastSeenUnix) + L" (" + agoOf(g.lastSeenUnix) + L")\r\n";
    text += L"Last command: " + (g.lastCommand.empty() ? L"-" : wide(g.lastCommand)) + L"\r\n";
    MessageBoxW(parent, text.c_str(), L"Client details", MB_OK | MB_ICONINFORMATION);
}

void handleDoubleClick(HWND hwnd, HWND list, UINT id) {
    if (!g_window) return;
    const int row = static_cast<int>(SendMessageW(list, LVM_GETNEXTITEM, -1, LVNI_SELECTED));
    if (row < 0) return;
    const WindowSnapshot& snapshot = g_window->snapshot();
    if (id == kIdLog) {
        const size_t index = recordIndexForRow(snapshot.records, row);
        if (index != SIZE_MAX) showDetails(hwnd, snapshot.records[index]);
    } else if (static_cast<size_t>(row) < snapshot.clients.size()) {
        showClientDetails(hwnd, snapshot.clients[static_cast<size_t>(row)]);
    }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ServerWindow* self = g_window;
    switch (message) {
        case WM_CREATE: {
            // Created at the default split; WM_SIZE/relayout() fixes the geometry
            // as soon as the real header height is known.
            const int header = px(126);
            const int clientsHeight = px(150);
            const int logHeight = px(240);
            const int width = px(1020);

            const RECT clientsArea{px(10), header, px(10) + width, header + clientsHeight};
            HWND clients = createList(hwnd, kIdClients, clientsArea);
            insertColumn(clients, 0, L"Client", 220);
            insertColumn(clients, 1, L"Pid", 70, LVCFMT_RIGHT);
            insertColumn(clients, 2, L"Peer", 110);
            insertColumn(clients, 3, L"Requests", 80, LVCFMT_RIGHT);
            insertColumn(clients, 4, L"Failed", 70, LVCFMT_RIGHT);
            insertColumn(clients, 5, L"Last command", 120);
            insertColumn(clients, 6, L"Last seen", 90);
            insertColumn(clients, 7, L"First seen", 90);

            const RECT logArea{px(10), header + clientsHeight + px(6), px(10) + width,
                               header + clientsHeight + px(6) + logHeight};
            HWND log = createList(hwnd, kIdLog, logArea);
            insertColumn(log, 0, L"Time", 70);
            insertColumn(log, 1, L"Age", 68);
            insertColumn(log, 2, L"Command", 92);
            insertColumn(log, 3, L"Tag", 76);
            insertColumn(log, 4, L"Process", 132);
            insertColumn(log, 5, L"Pid", 62, LVCFMT_RIGHT);
            insertColumn(log, 6, L"Peer", 112);
            insertColumn(log, 7, L"Result", 186);
            insertColumn(log, 8, L"Bytes in / out", 104, LVCFMT_RIGHT);
            insertColumn(log, 9, L"Duration", 78, LVCFMT_RIGHT);

            createButton(hwnd, kBtnClear, L"Clear log");
            createButton(hwnd, kBtnHide, L"Hide to tray");
            createButton(hwnd, kBtnQuit, L"Quit server");

            // attach() has to happen before the first WM_SIZE/WM_PAINT of the
            // children, and WM_CREATE is the only place that is guaranteed.
            if (self) {
                self->attach(hwnd, clients, log);
                self->relayout();
            }
            SetTimer(hwnd, kRefreshTimer, kRefreshMs, nullptr);
            return 0;
        }
        case WM_SIZE: {
            if (!self) return 0;
            RECT client{};
            GetClientRect(hwnd, &client);
            const int header = self->headerHeight();
            const int total = std::max(0, cy(client.bottom) - header - px(44));
            const int clientsHeight = std::max(px(70), total * 38 / 100);
            self->placeChildren(header, cy(client.right) - px(20), clientsHeight,
                                std::max(px(70), total - clientsHeight - px(6)),
                                client.bottom - px(36));
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            if (self) {
                RECT client{};
                GetClientRect(hwnd, &client);
                drawHeader(dc, headerRect(client, self->headerHeight()), self->snapshot());
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND:
            if (!self) return 0;
            switch (LOWORD(wParam)) {
                case kBtnClear:
                    self->requestClear();
                    return 0;
                case kBtnHide:
                    ShowWindow(hwnd, SW_HIDE);
                    return 0;
                case kBtnQuit:
                    self->requestQuit();
                    return 0;
                default:
                    return 0;
            }
        case WM_NOTIFY: {
            const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
            if (!header) return 0;
            if (header->code == NM_DBLCLK) {
                handleDoubleClick(hwnd, header->hwndFrom, static_cast<UINT>(header->idFrom));
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == kRefreshTimer && self) {
                // Ages move even while no request arrives.
                self->refreshAges();
                self->repaintHeader();
            }
            return 0;
        case WM_CLOSE:
            // The service must keep serving, so the close box hides the window;
            // "Quit server" or the tray menu is how the process stops.
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kRefreshTimer);
            if (self) self->detach();
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

bool registerWindowClass() {
    // Registered once per process, so a second create() reuses the class.
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = windowBrush();
    wc.lpszClassName = kWindowClass;
    wc.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), L"GLFW_ICON", IMAGE_ICON,
                                             px(32), px(32), LR_SHARED));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // IDI_APPLICATION
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));            // IDC_ARROW
    registered = RegisterClassExW(&wc) != 0;
    return registered;
}

int deviceScale() {
    HDC dc = GetDC(nullptr);
    if (!dc) return 100;
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    if (dpi <= 0) return 100;
    return std::max(100, dpi * 100 / 96);
}

}  // namespace

std::wstring wideFromUtf8(const std::string& utf8) { return wide(utf8); }

ServerWindow::~ServerWindow() {
    destroy();
}

bool ServerWindow::create(const std::string& windowTitle) {
    if (hwnd_) return true;
    g_scale = deviceScale();

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    if (!registerWindowClass()) return false;

    g_window = this;
    HWND hwnd = CreateWindowExW(0, kWindowClass, wide(windowTitle).c_str(), WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, px(1040), px(660), nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        g_window = nullptr;
        return false;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    return true;
}

void ServerWindow::attach(void* hwnd, void* listClients, void* listLog) {
    hwnd_ = hwnd;
    listClients_ = listClients;
    listLog_ = listLog;
}

void ServerWindow::detach() {
    hwnd_ = nullptr;
    listClients_ = nullptr;
    listLog_ = nullptr;
}

void ServerWindow::destroy() {
    if (!hwnd_) return;
    DestroyWindow(static_cast<HWND>(hwnd_));
    if (g_window == this) g_window = nullptr;
}

void ServerWindow::show() {
    if (!hwnd_) return;
    ShowWindow(static_cast<HWND>(hwnd_), SW_SHOW);
    SetForegroundWindow(static_cast<HWND>(hwnd_));
}

void ServerWindow::hide() {
    if (hwnd_) ShowWindow(static_cast<HWND>(hwnd_), SW_HIDE);
}

bool ServerWindow::visible() const {
    return hwnd_ && IsWindowVisible(static_cast<HWND>(hwnd_)) != FALSE;
}

void ServerWindow::setSnapshot(const WindowSnapshot& snapshot) {
    const size_t previousLines = snapshot_.headerLines.size();
    snapshot_ = snapshot;
    if (!hwnd_) return;
    if (shownGeneration_ != snapshot.generation) rebuildLists();
    // The header is drawn, not a control, so its height only feeds the split
    // between the two lists when the line count moves.
    if (snapshot.headerLines.size() != previousLines) relayout();
    // The header line count can change (roots appear, requests arrive), so the
    // whole client area is repainted rather than only the strip.
    InvalidateRect(static_cast<HWND>(hwnd_), nullptr, TRUE);
}

void ServerWindow::rebuildLists() {
    if (listClients_) {
        HWND clients = static_cast<HWND>(listClients_);
        fillList(clients, clientRows(snapshot_.clients));
        RECT inner{};
        GetClientRect(clients, &inner);
        stretchLastColumn(clients, kClientColumns, cy(inner.right));
    }
    if (listLog_) {
        HWND log = static_cast<HWND>(listLog_);
        fillList(log, logRows(snapshot_.records));
        RECT inner{};
        GetClientRect(log, &inner);
        stretchLastColumn(log, kLogColumns, cy(inner.right));
    }
    shownGeneration_ = snapshot_.generation;
}

void ServerWindow::refreshAges() {
    if (!listLog_ || !listClients_) return;
    HWND log = static_cast<HWND>(listLog_);
    const int rows = static_cast<int>(SendMessageW(log, LVM_GETITEMCOUNT, 0, 0));
    const auto& records = snapshot_.records;
    for (int i = 0; i < rows; ++i) {
        const size_t index = recordIndexForRow(records, i);
        if (index == SIZE_MAX) break;
        setCell(log, i, 1, agoOf(records[index].unixTime));
    }
    HWND clients = static_cast<HWND>(listClients_);
    const int cRows = static_cast<int>(SendMessageW(clients, LVM_GETITEMCOUNT, 0, 0));
    for (int i = 0; i < cRows && static_cast<size_t>(i) < snapshot_.clients.size(); ++i) {
        setCell(clients, i, 6, agoOf(snapshot_.clients[static_cast<size_t>(i)].lastSeenUnix));
    }
}

void ServerWindow::repaintHeader() {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(static_cast<HWND>(hwnd_), &client);
    InvalidateRect(static_cast<HWND>(hwnd_), &headerRect(client, headerHeight()), TRUE);
}

void ServerWindow::relayout() {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(static_cast<HWND>(hwnd_), &client);
    const int header = headerHeight();
    const int total = std::max(0, cy(client.bottom) - header - px(44));
    const int clientsHeight = std::max(px(70), total * 38 / 100);
    placeChildren(header, cy(client.right) - px(20), clientsHeight,
                  std::max(px(70), total - clientsHeight - px(6)), cy(client.bottom) - px(36));
}

void ServerWindow::placeChildren(int header, int width, int clientsHeight, int logHeight,
                                 int buttonTop) {
    HWND parent = static_cast<HWND>(hwnd_);
    if (!parent) return;
    if (listClients_) {
        MoveWindow(static_cast<HWND>(listClients_), px(10), header, width, clientsHeight, TRUE);
    }
    if (listLog_) {
        MoveWindow(static_cast<HWND>(listLog_), px(10), header + clientsHeight + px(6), width,
                   logHeight, TRUE);
    }
    if (HWND clear = GetDlgItem(parent, kBtnClear)) {
        MoveWindow(clear, px(10), buttonTop, px(110), px(26), TRUE);
    }
    if (HWND hide = GetDlgItem(parent, kBtnHide)) {
        MoveWindow(hide, px(128), buttonTop, px(130), px(26), TRUE);
    }
    if (HWND quit = GetDlgItem(parent, kBtnQuit)) {
        MoveWindow(quit, px(266), buttonTop, px(110), px(26), TRUE);
    }
}

int ServerWindow::headerHeight() const {
    const size_t lines = snapshot_.headerLines.empty() ? 5 : snapshot_.headerLines.size();
    return px(6) + px(19) * static_cast<int>(lines) + px(6);
}

void ServerWindow::pump() {
    if (!hwnd_) return;
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

#else
// ---------------------------------------------------------------------------
// POSIX: there is no window to open, so every call is inert and the server runs
// headless exactly as before.
// ---------------------------------------------------------------------------

ServerWindow::~ServerWindow() {}
std::wstring wideFromUtf8(const std::string&) { return std::wstring(); }
bool ServerWindow::create(const std::string&) { return false; }
void ServerWindow::attach(void*, void*, void*) {}
void ServerWindow::detach() {}
void ServerWindow::destroy() {}
void ServerWindow::show() {}
void ServerWindow::hide() {}
bool ServerWindow::visible() const { return false; }
void ServerWindow::setSnapshot(const WindowSnapshot&) {}
void ServerWindow::rebuildLists() {}
void ServerWindow::refreshAges() {}
void ServerWindow::repaintHeader() {}
void ServerWindow::placeChildren(int, int, int, int, int) {}
void ServerWindow::relayout() {}
int ServerWindow::headerHeight() const { return 0; }
void ServerWindow::pump() {}

#endif

}
