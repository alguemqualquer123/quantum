// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "util/FrameCapture.h"

#include "Log.h"
#include "System.h"

#include "stb_image_write.h"

#include <vector>

using namespace quantum;

namespace {
    std::string g_pendingPath;
    int g_pendingDelay = 0;
}

void FrameCapture::request(const std::string& path, int delay){
    if (path.empty()){
        Log::error("FrameCapture: asked to capture to an empty path\n");
        return;
    }
    g_pendingPath = path;
    g_pendingDelay = delay > 0 ? delay : 0;
}

bool FrameCapture::isPending(){
    return !g_pendingPath.empty();
}

bool FrameCapture::captureTo(const std::string& path){
    const int width = System::instance().getScreenWidth();
    const int height = System::instance().getScreenHeight();
    if (width <= 0 || height <= 0){
        Log::error("FrameCapture: the window is %dx%d, nothing to read\n", width, height);
        return false;
    }

    std::vector<unsigned char> pixels((size_t)width * (size_t)height * 4u);
    if (!System::instance().readBackFramebuffer(pixels.data(), width, height))
        return false;

    if (!stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4)){
        Log::error("FrameCapture: cannot write %s\n", path.c_str());
        return false;
    }

    Log::print("FrameCapture: wrote %s (%dx%d)\n", path.c_str(), width, height);
    return true;
}

bool FrameCapture::runPending(){
    if (g_pendingPath.empty()) return false;

    if (g_pendingDelay > 0){
        g_pendingDelay--;
        return false;
    }

    const std::string path = g_pendingPath;
    g_pendingPath.clear();
    g_pendingDelay = 0;
    return captureTo(path);
}
