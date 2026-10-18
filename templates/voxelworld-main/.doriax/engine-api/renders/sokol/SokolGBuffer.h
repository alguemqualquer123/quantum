#pragma once

#include "SokolObject.h"
#include "SokolShader.h"
#include "FramebufferRender.h"
#include "CameraRender.h"
#include "../core/render/ShaderData.h"

namespace doriax {

// Wrapper for the G‑Buffer pass. It creates the MRT framebuffer (depth+normal+albedo) and
// provides a method to render the scene geometry into it.
class SokolGBuffer {
public:
    SokolGBuffer();
    ~SokolGBuffer();

    // Initialize the G‑Buffer with the given dimensions. Must be called before render().
    void init(unsigned int width, unsigned int height);

    // Returns the underlying framebuffer render target (used by later passes).
    const FramebufferRender& framebuffer() const { return gbufferFramebuffer; }

    // Execute the geometry pass. The RenderSystem will call this for each visible entity.
    void render(const CameraRender& cameraRender);

    // Cleanup resources.
    void destroy();

private:
    FramebufferRender gbufferFramebuffer; // MRT: color0 = albedo+params, color1 = normal+roughness+metallic, depth.
    SokolShader gbufferShader;            // Compiled shader that writes to the G‑Buffer.
    SokolObject gbufferObject;            // Helper for drawing geometry.
    bool initialized = false;
};

}
