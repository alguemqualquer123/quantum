// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef UIRenderer_h
#define UIRenderer_h

#include <memory>

#include "Export.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "ui/render/VertexBuilder.h"

namespace quantum {
    class Framebuffer;
}

namespace quantum::ui {

    // Backend interface for the retained-mode UI renderer. Implementations live
    // in renders/<backend> (SokolUIRenderer); no backend types leak through it.
    class QUANTUM_API UIRenderer {
    public:
        virtual ~UIRenderer() = default;

        // Submits one frame of UI. `canvasSize` is the logical canvas and
        // `viewRect` the screen region it maps to (letterbox aware). `target`
        // is the active offscreen framebuffer, nullptr for the swapchain.
        virtual void render(const UIMesh& mesh, Vector2 canvasSize, const Rect& viewRect,
                            quantum::Framebuffer* target) = 0;
    };

    QUANTUM_API std::unique_ptr<UIRenderer> createUIRenderer();
}

#endif /* UIRenderer_h */
