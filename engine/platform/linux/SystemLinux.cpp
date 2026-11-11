// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "SystemLinux.h"

#include "LinuxInputRouter.h"
#include "WindowLinux.h"
#include "Log.h"

#if defined(SOKOL_VULKAN)
#include "VulkanContext.h"
#elif defined(SOKOL_GLCORE)
#include <GL/glx.h>
#endif

#include <cstring>
#include <vector>

using namespace quantum;

#if defined(SOKOL_GLCORE)
namespace {

using ReadPixelsProc = void (*)(int, int, int, int, unsigned int, unsigned int, void*);
using PixelStoreiProc = void (*)(unsigned int, int);
using GetIntegervProc = void (*)(unsigned int, int*);

void* glEntry(const char* name) {
    return reinterpret_cast<void*>(glXGetProcAddressARB(
        reinterpret_cast<const GLubyte*>(name)));
}

}
#endif

SystemLinux::SystemLinux(LinuxInputRouter* router) : router(router) {
}

int SystemLinux::getScreenWidth() {
    return WindowLinux::getWidth();
}

int SystemLinux::getScreenHeight() {
    return WindowLinux::getHeight();
}

int SystemLinux::getSampleCount() {
    return 1;
}

bool SystemLinux::readBackFramebuffer(unsigned char* out, int width, int height) {
#if defined(SOKOL_GLCORE)
    if (!out || width <= 0 || height <= 0) return false;
    const auto readPixels = reinterpret_cast<ReadPixelsProc>(glEntry("glReadPixels"));
    const auto pixelStorei = reinterpret_cast<PixelStoreiProc>(glEntry("glPixelStorei"));
    const auto getIntegerv = reinterpret_cast<GetIntegervProc>(glEntry("glGetIntegerv"));
    if (!readPixels || !pixelStorei || !getIntegerv) {
        Log::error("readBackFramebuffer: OpenGL readback entry points are unavailable\n");
        return false;
    }
    int previousAlignment = 4;
    getIntegerv(0x0D05u, &previousAlignment); // GL_PACK_ALIGNMENT
    pixelStorei(0x0D05u, 1u);
    readPixels(0, 0, width, height, 0x1908u, 0x1401u, out); // GL_RGBA / GL_UNSIGNED_BYTE
    pixelStorei(0x0D05u, previousAlignment);

    const size_t rowBytes = static_cast<size_t>(width) * 4u;
    std::vector<unsigned char> row(rowBytes);
    for (int y = 0; y < height / 2; ++y) {
        unsigned char* top = out + static_cast<size_t>(y) * rowBytes;
        unsigned char* bottom = out + static_cast<size_t>(height - 1 - y) * rowBytes;
        std::memcpy(row.data(), top, rowBytes);
        std::memcpy(top, bottom, rowBytes);
        std::memcpy(bottom, row.data(), rowBytes);
    }
    return true;
#elif defined(SOKOL_VULKAN)
    if (!VulkanContext::readBackFramebuffer(out, width, height)) {
        Log::error("readBackFramebuffer: Vulkan swapchain readback failed\n");
        return false;
    }
    return true;
#else
    (void)out;
    (void)width;
    (void)height;
    Log::error("readBackFramebuffer: only the OpenGL backend can read the window back\n");
    return false;
#endif
}

#if defined(SOKOL_VULKAN)
sg_environment SystemLinux::getSokolEnvironment() {
    return VulkanContext::environment();
}

sg_swapchain SystemLinux::getSokolSwapchain() {
    return VulkanContext::swapchain();
}
#endif

bool SystemLinux::isFullscreen() {
    return WindowLinux::isFullscreen();
}

void SystemLinux::requestFullscreen() {
    WindowLinux::requestFullscreen();
}

void SystemLinux::exitFullscreen() {
    WindowLinux::exitFullscreen();
}

bool SystemLinux::isWindowMaximized() {
    return WindowLinux::isMaximized();
}

void SystemLinux::maximizeWindow() {
    WindowLinux::maximize();
}

void SystemLinux::restoreWindow() {
    WindowLinux::restore();
}

void SystemLinux::setWindowSize(int width, int height) {
    WindowLinux::setSize(width, height);
}

bool SystemLinux::isWindowResizable() {
    return WindowLinux::isResizable();
}

void SystemLinux::setWindowResizable(bool resizable) {
    WindowLinux::setResizable(resizable);
}

void SystemLinux::setWindowTitle(const std::string& title) {
    WindowLinux::setTitle(title);
}

void SystemLinux::quit() {
    WindowLinux::quit();
}

void SystemLinux::setMouseCursor(CursorType type) {
    WindowLinux::setMouseCursor(type);
}

void SystemLinux::setMouseMode(MouseMode mode) {
    WindowLinux::setMouseMode(mode);
}

void SystemLinux::setMousePosition(float x, float y) {
    // The window warps the pointer; the router owns the position the engine
    // sees, which reports no motion of its own while captured.
    if (router) router->setMousePosition(x, y);
    WindowLinux::setMousePosition(x, y);
}

// An exported game ships its assets next to the executable, so the relative
// default is right there. Running a project from outside the editor leaves the
// assets in the project directory instead, so the editor bakes absolute paths
// in through these macros (see Generator::getPlatformCMakeConfig).
std::string SystemLinux::getAssetPath() {
#ifdef QUANTUM_ASSET_PATH
    return QUANTUM_ASSET_PATH;
#else
    return "assets";
#endif
}

std::string SystemLinux::getUserDataPath() {
    return ".";
}

std::string SystemLinux::getLuaPath() {
#ifdef QUANTUM_LUA_PATH
    return QUANTUM_LUA_PATH;
#else
    return "lua";
#endif
}

std::string SystemLinux::getShaderPath() {
#ifdef QUANTUM_SHADER_PATH
    return QUANTUM_SHADER_PATH;
#else
    return System::getShaderPath();
#endif
}
