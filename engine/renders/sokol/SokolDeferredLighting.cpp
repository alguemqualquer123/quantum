#include "SokolDeferredLighting.h"
#include "SokolCmdQueue.h"
#include "../core/component/LightComponent.h"

namespace doriax {

SokolDeferredLighting::SokolDeferredLighting() {}
SokolDeferredLighting::~SokolDeferredLighting() { destroy(); }

void SokolDeferredLighting::init(unsigned int width, unsigned int height) {
    if (initialized) return;
    // Simple single‑color framebuffer for the final lit image.
    sg_image_desc color = {};
    color.render_target = true;
    color.width = width;
    color.height = height;
    color.pixel_format = SG_PIXELFORMAT_RGBA8;
    color.sample_count = 1;
    sg_image img = sg_make_image(&color);
    lightingFramebuffer = FramebufferRender();
    lightingFramebuffer.setRenderTarget(0, img);

    // Load deferred lighting shader (reads G‑Buffer textures and applies lights).
    lightingShader.loadFromFile("engine/shaderlib/deferred_lighting.vert", "engine/shaderlib/deferred_lighting.frag");
    lightingObject.init(lightingShader.getPipeline());

    initialized = true;
}

void SokolDeferredLighting::render() {
    if (!initialized) return;
    // Bind the output framebuffer.
    SokolCmdQueue::add_command_bind_fbo(lightingFramebuffer.getHandle());
    // Clear to black.
    SokolCmdQueue::add_command_clear(sg_clearcolor{0,0,0,1}, 1.0f, 0);
    // Draw a fullscreen quad that samples the G‑Buffer.
    lightingObject.draw();
}

void SokolDeferredLighting::destroy() {
    if (!initialized) return;
    lightingObject.destroy();
    lightingShader.destroy();
    lightingFramebuffer.destroy();
    initialized = false;
}

}
