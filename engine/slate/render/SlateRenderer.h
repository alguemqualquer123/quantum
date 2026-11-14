// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateRenderer_h
#define SlateRenderer_h

#include <memory>

#include "Export.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "slate/core/Geometry.h"

namespace quantum {
    class Framebuffer;
}

namespace quantum::slate {

    // Backend interface. Implementations live in render/<backend>; no sokol or GL
    // type is allowed to leak through it, so the Slate module can be unit tested
    // and later ported to another backend.
    class ISlateRenderer {
    public:
        virtual ~ISlateRenderer() = default;

        //Submits one frame. canvasSize is the logical canvas, viewRect the
        //screen region it maps to (letterbox aware), target the offscreen
        //framebuffer or nullptr for the swapchain (world-space UI passes one).
        virtual void render(const SlateMesh& mesh, Vector2 canvasSize, const Rect& viewRect,
                            quantum::Framebuffer* target) = 0;
    };

    std::unique_ptr<ISlateRenderer> createSlateRenderer();
}

#endif /* SlateRenderer_h */
