// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "SystemWin.h"

#include "Log.h"
#include "WinInputRouter.h"
#include "WindowWin.h"

#include <cstring>
#include <vector>

#if defined(SOKOL_VULKAN)
#include "VulkanContext.h"
#elif defined(SOKOL_D3D11)
#include "D3D11Context.h"
#endif

using namespace quantum;

namespace {

// Nothing in the engine includes a GL header - sokol_gfx.h loads its own entry points and blocks
// the legacy ones - so the two calls the readback needs are resolved by name against the context
// that is already current. wglGetProcAddress answers 1..3 or -1 for a core 1.x function rather
// than failing, and those are the ones that live in opengl32.dll itself.
void* glEntry(const char* name) {
    void* address = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (address == nullptr || address == reinterpret_cast<void*>(1) ||
        address == reinterpret_cast<void*>(2) || address == reinterpret_cast<void*>(3) ||
        address == reinterpret_cast<void*>(-1))
        address = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("opengl32.dll"), name));
    return address;
}

}

SystemWin::SystemWin(WinInputRouter* router) : router(router) {
}

int SystemWin::getScreenWidth() {
    return WindowWin::getClientWidth();
}

int SystemWin::getScreenHeight() {
    return WindowWin::getClientHeight();
}

int SystemWin::getSampleCount() {
    return 1;
}

// The GL backend renders into the window's default framebuffer (System::getSokolSwapchain assumes
// framebuffer 0), so a read of it is the frame the platform is about to present. Sample count is
// 1, which is what makes glReadPixels legal here directly - a multisampled backbuffer would have
// to be blitted first.
bool SystemWin::readBackFramebuffer(unsigned char* out, int width, int height) {
#if defined(SOKOL_GLCORE)
    typedef void (*ReadPixelsProc)(int x, int y, int w, int h, unsigned int format,
                                   unsigned int type, void* pixels);
    typedef void (*PixelStoreiProc)(unsigned int pname, int param);

    const unsigned int GL_RGBA = 0x1908;
    const unsigned int GL_UNSIGNED_BYTE = 0x1401;
    const unsigned int GL_PACK_ALIGNMENT = 0x0D05;

    static ReadPixelsProc readPixels = reinterpret_cast<ReadPixelsProc>(glEntry("glReadPixels"));
    static PixelStoreiProc pixelStorei = reinterpret_cast<PixelStoreiProc>(glEntry("glPixelStorei"));
    if (!readPixels || !pixelStorei) {
        Log::error("readBackFramebuffer: glReadPixels is not reachable in this context\n");
        return false;
    }

    std::vector<unsigned char> rows((size_t)width * (size_t)height * 4u);
    pixelStorei(GL_PACK_ALIGNMENT, 4);
    readPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());

    // GL counts rows from the bottom-left, a PNG from the top-left.
    const size_t rowBytes = (size_t)width * 4u;
    for (int y = 0; y < height; y++)
        std::memcpy(out + (size_t)y * rowBytes,
                    rows.data() + (size_t)(height - 1 - y) * rowBytes, rowBytes);
    return true;
#elif defined(SOKOL_D3D11)
    if (!D3D11Context::readBackFramebuffer(out, width, height)) {
        Log::error("readBackFramebuffer: D3D11 backbuffer readback failed\n");
        return false;
    }
    return true;
#elif defined(SOKOL_VULKAN)
    if (!VulkanContext::readBackFramebuffer(out, width, height)) {
        Log::error("readBackFramebuffer: Vulkan swapchain readback failed\n");
        return false;
    }
    return true;
#else
    Log::error("readBackFramebuffer: only the OpenGL backend can read the window back\n");
    return false;
#endif
}

#if defined(SOKOL_VULKAN)
sg_environment SystemWin::getSokolEnvironment() {
    return VulkanContext::environment();
}

sg_swapchain SystemWin::getSokolSwapchain() {
    return VulkanContext::swapchain();
}
#elif defined(SOKOL_D3D11)
sg_environment SystemWin::getSokolEnvironment() {
    return D3D11Context::environment();
}

sg_swapchain SystemWin::getSokolSwapchain() {
    return D3D11Context::swapchain();
}
#endif

bool SystemWin::isFullscreen() {
    return WindowWin::isFullscreen();
}

void SystemWin::requestFullscreen() {
    WindowWin::requestFullscreen();
}

void SystemWin::exitFullscreen() {
    WindowWin::exitFullscreen();
}

bool SystemWin::isWindowMaximized() {
    return WindowWin::isMaximized();
}

void SystemWin::maximizeWindow() {
    WindowWin::maximize();
}

void SystemWin::restoreWindow() {
    WindowWin::restore();
}

void SystemWin::setWindowSize(int width, int height) {
    WindowWin::setSize(width, height);
}

bool SystemWin::isWindowResizable() {
    return WindowWin::isResizable();
}

void SystemWin::setWindowResizable(bool resizable) {
    WindowWin::setResizable(resizable);
}

void SystemWin::setWindowTitle(const std::string& title) {
    WindowWin::setTitle(title);
}

bool SystemWin::hasFocus() {
    return WindowWin::hasFocus();
}

void SystemWin::quit() {
    WindowWin::quit();
}

void SystemWin::setMouseCursor(CursorType type) {
    WindowWin::setMouseCursor(type);
}

void SystemWin::setMouseMode(MouseMode mode) {
    WindowWin::setMouseMode(mode);
}

void SystemWin::setMousePosition(float x, float y) {
    // The window warps the pointer; the router owns the position the engine
    // sees, which reports no motion of its own while captured.
    if (router) router->setMousePosition(x, y);
    WindowWin::setMousePosition(x, y);
}

// An exported game ships its assets next to the executable, so the relative
// default is right there. Running a project from outside the editor leaves the
// assets in the project directory instead, so the editor bakes absolute paths
// in through these macros (see Generator::getPlatformCMakeConfig).
std::string SystemWin::getAssetPath() {
#ifdef QUANTUM_ASSET_PATH
    return QUANTUM_ASSET_PATH;
#else
    return "assets";
#endif
}

std::string SystemWin::getUserDataPath() {
    return ".";
}

std::string SystemWin::getLuaPath() {
#ifdef QUANTUM_LUA_PATH
    return QUANTUM_LUA_PATH;
#else
    return "lua";
#endif
}

std::string SystemWin::getShaderPath() {
#ifdef QUANTUM_SHADER_PATH
    return QUANTUM_SHADER_PATH;
#else
    return System::getShaderPath();
#endif
}
