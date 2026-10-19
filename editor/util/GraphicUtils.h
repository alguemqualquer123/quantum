// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "object/Camera.h"
#include <filesystem>

namespace fs = std::filesystem;

namespace doriax::editor {
    class GraphicUtils {
    public:
        static bool saveFramebufferImage(Framebuffer* framebuffer, fs::path path, bool flipY = false, std::function<void()> onComplete = nullptr);

        // Read the composed backbuffer (engine view + UI) and write it as a PNG.
        // Must run on the render thread between the frame submit and the present.
        static bool captureEditorFrame(const fs::path& path, int width, int height);

        static Vector2 getUILayoutCenter(Scene* scene, Entity entity, const UILayoutComponent& layout);
    };
}
