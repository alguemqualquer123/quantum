// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "WinInputRouter.h"

#include "KeyCodesWin.h"
#include "WindowWin.h"

#include "Engine.h"

#include <imm.h>
#include <string>
#include <vector>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM

using namespace quantum;

namespace {

int mouseButtonFromMessage(UINT message, WPARAM wParam) {
    switch (message) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            return D_MOUSE_BUTTON_LEFT;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            return D_MOUSE_BUTTON_RIGHT;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            return D_MOUSE_BUTTON_MIDDLE;
        default:
            return GET_XBUTTON_WPARAM(wParam) == XBUTTON1
                ? D_MOUSE_BUTTON_4 : D_MOUSE_BUTTON_5;
    }
}

// While captured the pointer is clipped in place, so WM_MOUSEMOVE stops
// carrying information and only the WM_INPUT deltas are used.
bool isCaptured() {
    return Engine::getMouseMode() == MouseMode::CAPTURED;
}

std::string utf8FromWide(const wchar_t* text, int length) {
    if (!text || length <= 0) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string output(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length,
        output.data(), bytes, nullptr, nullptr);
    return output;
}

void dispatchImeComposition(HWND window, LPARAM flags) {
    HIMC context = ImmGetContext(window);
    if (!context) return;

    const bool hasResult = (flags & GCS_RESULTSTR) != 0;
    const bool hasComposition = (flags & GCS_COMPSTR) != 0;
    const DWORD kind = hasResult ? GCS_RESULTSTR : (hasComposition ? GCS_COMPSTR : 0);
    if (kind != 0) {
        const LONG byteCount = ImmGetCompositionStringW(context, kind, nullptr, 0);
        if (byteCount > 0) {
            std::vector<wchar_t> buffer(static_cast<size_t>(byteCount / sizeof(wchar_t)) + 1);
            ImmGetCompositionStringW(context, kind, buffer.data(), byteCount);
            const DWORD cursor = hasResult
                ? 0u : ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
            Engine::systemTextComposition(utf8FromWide(buffer.data(), static_cast<int>(byteCount / sizeof(wchar_t))),
                static_cast<int>(cursor), hasResult);
        } else if (hasComposition) {
            Engine::systemTextComposition({}, 0, false);
        }
    }
    ImmReleaseContext(window, context);
}

}

void WinInputRouter::setMousePosition(double x, double y) {
    mousePosX = x;
    mousePosY = y;
}

bool WinInputRouter::handleMessage(HWND window, UINT message, WPARAM wParam,
                                   LPARAM lParam, LRESULT& result) {
    switch (message) {
        case WM_INPUT: {
            if (!isCaptured()) break;
            LONG deltaX = 0;
            LONG deltaY = 0;
            if (!WindowWin::readRawMouseDelta(
                    reinterpret_cast<HRAWINPUT>(lParam), deltaX, deltaY))
                break;
            mousePosX += deltaX;
            mousePosY += deltaY;
            Engine::systemMouseMove(float(mousePosX), float(mousePosY),
                                    winKeyModifiers());
            break; // WM_INPUT still has to reach DefWindowProc
        }

        case WM_MOUSEMOVE:
            if (!isCaptured()) {
                mousePosX = GET_X_LPARAM(lParam);
                mousePosY = GET_Y_LPARAM(lParam);
                Engine::systemMouseMove(float(mousePosX), float(mousePosY),
                                        winKeyModifiers());
            }
            result = 0;
            return true;

        case WM_MOUSEWHEEL:
            Engine::systemMouseScroll(
                0.0f, float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA),
                winKeyModifiers());
            result = 0;
            return true;

        case WM_MOUSEHWHEEL:
            Engine::systemMouseScroll(
                float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA), 0.0f,
                winKeyModifiers());
            result = 0;
            return true;

        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            if (!isCaptured()) {
                mousePosX = GET_X_LPARAM(lParam);
                mousePosY = GET_Y_LPARAM(lParam);
            }
            // Keeps drags alive when the pointer leaves the client area
            SetCapture(window);
            Engine::systemMouseDown(mouseButtonFromMessage(message, wParam),
                                    float(mousePosX), float(mousePosY),
                                    winKeyModifiers());
            result = (message == WM_XBUTTONDOWN) ? TRUE : 0;
            return true;

        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        case WM_XBUTTONUP:
            if (!isCaptured()) {
                mousePosX = GET_X_LPARAM(lParam);
                mousePosY = GET_Y_LPARAM(lParam);
            }
            ReleaseCapture();
            Engine::systemMouseUp(mouseButtonFromMessage(message, wParam),
                                  float(mousePosX), float(mousePosY),
                                  winKeyModifiers());
            result = (message == WM_XBUTTONUP) ? TRUE : 0;
            return true;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const int key = winKeyFromMessage(wParam, lParam);
            if (key != D_KEY_UNKNOWN)
                Engine::systemKeyDown(key, (HIWORD(lParam) & KF_REPEAT) != 0,
                                      winKeyModifiers());
            break; // DefWindowProc turns this into WM_CHAR
        }

        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const int key = winKeyFromMessage(wParam, lParam);
            if (key != D_KEY_UNKNOWN)
                Engine::systemKeyUp(key, false, winKeyModifiers());
            break;
        }

        case WM_CHAR:
        case WM_SYSCHAR: {
            const wchar_t codepoint = static_cast<wchar_t>(wParam);
            // Other control characters would show up as boxes
            if ((codepoint >= 32 && codepoint != 0x7f) || codepoint == '\t' ||
                codepoint == '\b' || codepoint == '\r' || codepoint == 0x1b)
                Engine::systemCharInput(codepoint);
            result = 0;
            return true;
        }

        case WM_IME_COMPOSITION:
            dispatchImeComposition(window, lParam);
            result = 0;
            return true;

        case WM_IME_ENDCOMPOSITION:
            Engine::systemTextComposition({}, 0, false);
            result = 0;
            return true;

        default:
            break;
    }

    return false;
}
