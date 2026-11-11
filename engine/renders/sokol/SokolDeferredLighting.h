#pragma once

#include "SokolObject.h"
#include "SokolShader.h"
#include "FramebufferRender.h"
#include "../core/render/ShaderData.h"

namespace quantum {

// Wrapper for the deferred lighting pass. It reads the G‑Buffer textures and applies
// a list of lights supplied via a uniform buffer (fs_lighting_t). The result is written
// to the scene color framebuffer.
class SokolDeferredLighting {
public:
    SokolDeferredLighting();
    ~SokolDeferredLighting();

    void init(unsigned int width, unsigned int height);
    const FramebufferRender& framebuffer() const { return lightingFramebuffer; }

    // Execute the lighting pass. The RenderSystem must bind the G‑Buffer textures before calling.
    void render();

    void destroy();

private:
    FramebufferRender lightingFramebuffer; // Single color target for final lit scene.
    SokolShader lightingShader;
    SokolObject lightingObject;
    bool initialized = false;
};

}
