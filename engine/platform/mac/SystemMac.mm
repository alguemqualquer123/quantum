// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "SystemMac.h"
#include "WindowMac.h"

#import <Foundation/Foundation.h>

#if defined(SOKOL_METAL)
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalLayer.h>
#elif defined(SOKOL_VULKAN)
#include "VulkanContext.h"
#elif defined(SOKOL_GLCORE)
#import <OpenGL/gl3.h>
#endif

#include "Log.h"

#include <cstring>
#include <vector>

using namespace quantum;

namespace {
#if defined(SOKOL_METAL)
    // MTKView::currentDrawable is only guaranteed during drawInMTKView. Keep a
    // strong reference to the drawable selected for this frame so the capture
    // that follows sg_commit() reads the same texture that was presented.
    id<CAMetalDrawable> gCaptureDrawable = nil;
#endif

    // Finder gives a bare executable the home folder as working directory, so
    // relative paths never resolve there. The main bundle points at the folder
    // holding the executable, or at the resources of a real .app.
    bool bundleResourcePath(NSString* name, std::string& out) {
        NSString* path = [[NSBundle mainBundle] pathForResource:name ofType:nil];
        if (!path) return false;

        out = [path UTF8String];
        return true;
    }
}

int SystemMac::getScreenWidth() {
    return WindowMac::getDrawableWidth();
}

int SystemMac::getScreenHeight() {
    return WindowMac::getDrawableHeight();
}

int SystemMac::getSampleCount() {
    return 1;
}

bool SystemMac::readBackFramebuffer(unsigned char* out, int width, int height) {
#if defined(SOKOL_GLCORE)
    if (!out || width <= 0 || height <= 0) return false;
    GLint previousAlignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousAlignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out);
    const GLenum error = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, previousAlignment);
    if (error != GL_NO_ERROR) {
        Log::error("readBackFramebuffer: OpenGL readback failed (%u)\n",
                   static_cast<unsigned int>(error));
        return false;
    }

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
#elif defined(SOKOL_METAL)
    if (!out || width <= 0 || height <= 0) return false;

    id<CAMetalDrawable> drawable = gCaptureDrawable;
    gCaptureDrawable = nil;
    if (!drawable) {
        Log::error("readBackFramebuffer: Metal has no drawable for this frame\n");
        return false;
    }

    id<MTLTexture> texture = drawable.texture;
    if (!texture || texture.width != (NSUInteger)width || texture.height != (NSUInteger)height) {
        Log::error("readBackFramebuffer: Metal drawable is %lux%lu, requested %dx%d\n",
                   (unsigned long)(texture ? texture.width : 0),
                   (unsigned long)(texture ? texture.height : 0), width, height);
        return false;
    }

    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)sg_mtl_command_queue();
    if (!queue) {
        Log::error("readBackFramebuffer: Metal command queue is unavailable\n");
        return false;
    }

    const NSUInteger sourceRowBytes = (NSUInteger)width * 4u;
    const NSUInteger destinationRowBytes = (sourceRowBytes + 255u) & ~255u;
    id<MTLBuffer> buffer = [queue.device newBufferWithLength:destinationRowBytes * (NSUInteger)height
                                                     options:MTLResourceStorageModeShared];
    if (!buffer) {
        Log::error("readBackFramebuffer: cannot allocate the Metal readback buffer\n");
        return false;
    }

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    [blit copyFromTexture:texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
                 toBuffer:buffer
        destinationOffset:0
   destinationBytesPerRow:destinationRowBytes
 destinationBytesPerImage:destinationRowBytes * (NSUInteger)height];
    [blit endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];

    if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        Log::error("readBackFramebuffer: Metal readback command failed (%ld)\n",
                   (long)commandBuffer.status);
        return false;
    }

    const unsigned char* source = static_cast<const unsigned char*>(buffer.contents);
    for (int y = 0; y < height; ++y) {
        const unsigned char* sourceRow = source + (size_t)y * destinationRowBytes;
        unsigned char* destinationRow = out + (size_t)y * sourceRowBytes;
        for (int x = 0; x < width; ++x) {
            const unsigned char* pixel = sourceRow + (size_t)x * 4u;
            unsigned char* output = destinationRow + (size_t)x * 4u;
            output[0] = pixel[2];
            output[1] = pixel[1];
            output[2] = pixel[0];
            output[3] = pixel[3];
        }
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

#if defined(SOKOL_METAL)
sg_environment SystemMac::getSokolEnvironment() {
    MTKView* view = (__bridge MTKView*)WindowMac::contentView();
    sg_environment env = {};

    env.defaults.sample_count = (int)view.sampleCount;
    env.defaults.color_format = SG_PIXELFORMAT_BGRA8;
    env.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    env.metal.device = (__bridge const void*)view.device;

    return env;
}

// Only valid inside the view's draw callback, where currentDrawable is set
sg_swapchain SystemMac::getSokolSwapchain() {
    MTKView* view = (__bridge MTKView*)WindowMac::contentView();
    sg_swapchain swapchain = {};

    id<CAMetalDrawable> drawable = view.currentDrawable;
    gCaptureDrawable = drawable;

    swapchain.width = (int)view.drawableSize.width;
    swapchain.height = (int)view.drawableSize.height;
    swapchain.sample_count = (int)view.sampleCount;
    swapchain.color_format = SG_PIXELFORMAT_BGRA8;
    swapchain.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    swapchain.metal.current_drawable = (__bridge const void*)drawable;
    swapchain.metal.depth_stencil_texture = (__bridge const void*)view.depthStencilTexture;
    swapchain.metal.msaa_color_texture = (__bridge const void*)view.multisampleColorTexture;

    return swapchain;
}
#elif defined(SOKOL_VULKAN)
sg_environment SystemMac::getSokolEnvironment() {
    return VulkanContext::environment();
}

sg_swapchain SystemMac::getSokolSwapchain() {
    return VulkanContext::swapchain();
}
#endif

bool SystemMac::isFullscreen() {
    return WindowMac::isFullscreen();
}

void SystemMac::requestFullscreen() {
    WindowMac::requestFullscreen();
}

void SystemMac::exitFullscreen() {
    WindowMac::exitFullscreen();
}

bool SystemMac::isWindowMaximized() {
    return WindowMac::isMaximized();
}

void SystemMac::maximizeWindow() {
    WindowMac::maximize();
}

void SystemMac::restoreWindow() {
    WindowMac::restore();
}

void SystemMac::setWindowSize(int width, int height) {
    WindowMac::setSize(width, height);
}

bool SystemMac::isWindowResizable() {
    return WindowMac::isResizable();
}

void SystemMac::setWindowResizable(bool resizable) {
    WindowMac::setResizable(resizable);
}

void SystemMac::setWindowTitle(const std::string& title) {
    WindowMac::setTitle(title);
}

void SystemMac::quit() {
    WindowMac::quit();
}

void SystemMac::setMouseCursor(CursorType type) {
    WindowMac::setMouseCursor(type);
}

void SystemMac::setMouseMode(MouseMode mode) {
    WindowMac::setMouseMode(mode);
}

void SystemMac::setMousePosition(float x, float y) {
    WindowMac::setMousePosition(x, y);
}

// The bundle lookup covers an exported game, which ships its assets next to the
// executable. A project run from outside the editor keeps them in the project
// directory, so the editor bakes that path in (Generator::getPlatformCMakeConfig).
std::string SystemMac::getAssetPath() {
    std::string bundlePath;
    if (bundleResourcePath(@"assets", bundlePath)) return bundlePath;

#ifdef QUANTUM_ASSET_PATH
    return QUANTUM_ASSET_PATH;
#else
    return "assets";
#endif
}

std::string SystemMac::getUserDataPath() {
    return ".";
}

std::string SystemMac::getLuaPath() {
    std::string bundlePath;
    if (bundleResourcePath(@"lua", bundlePath)) return bundlePath;

#ifdef QUANTUM_LUA_PATH
    return QUANTUM_LUA_PATH;
#else
    return "lua";
#endif
}

std::string SystemMac::getShaderPath() {
    std::string bundlePath;
    if (bundleResourcePath(@"shaders", bundlePath)) return bundlePath;

#ifdef QUANTUM_SHADER_PATH
    return QUANTUM_SHADER_PATH;
#else
    return System::getShaderPath();
#endif
}
