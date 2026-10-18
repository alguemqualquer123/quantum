#include "SokolGBuffer.h"
#include "SokolCmdQueue.h"
#include "../core/render/ShaderData.h"
#include "../core/component/MeshComponent.h"
#include "../core/component/InstancedMeshComponent.h"
#include "../core/component/Transform.h"
#include "../core/component/CameraComponent.h"
#include "../core/scene/Scene.h"

namespace doriax {

SokolGBuffer::SokolGBuffer() {}
SokolGBuffer::~SokolGBuffer() { destroy(); }

void SokolGBuffer::init(unsigned int width, unsigned int height) {
    if (initialized) return;
    // Create MRT framebuffer: color0 = albedo+params, color1 = normal+roughness+metallic, depth
    sg_image_desc color0 = {};
    color0.render_target = true;
    color0.width = width;
    color0.height = height;
    color0.pixel_format = SG_PIXELFORMAT_RGBA8;
    color0.sample_count = 1;

    sg_image_desc color1 = {};
    color1.render_target = true;
    color1.width = width;
    color1.height = height;
    color1.pixel_format = SG_PIXELFORMAT_RGBA16F; // store normal (XYZ) + roughness/metallic in A
    color1.sample_count = 1;

    sg_image_desc depth = {};
    depth.render_target = true;
    depth.width = width;
    depth.height = height;
    depth.pixel_format = SG_PIXELFORMAT_DEPTH; // 24‑bit depth
    depth.sample_count = 1;

    sg_image img0 = sg_make_image(&color0);
    sg_image img1 = sg_make_image(&color1);
    sg_image imgDepth = sg_make_image(&depth);

    gbufferFramebuffer = FramebufferRender();
    gbufferFramebuffer.setRenderTarget(0, img0);
    gbufferFramebuffer.setRenderTarget(1, img1);
    gbufferFramebuffer.setDepthStencil(imgDepth);

    // Load G‑Buffer shader (vertex + fragment). The shader source files are placed under
    // engine/tools/shaderlib/gbuffer.vert and gbuffer.frag.
    gbufferShader.loadFromFile("engine/tools/shaderlib/gbuffer.vert", "engine/tools/shaderlib/gbuffer.frag");
    gbufferObject.init(gbufferShader.getPipeline());

    initialized = true;
}

void SokolGBuffer::render(const CameraRender& cameraRender) {
    if (!initialized) return;
    // Bind framebuffer
    SokolCmdQueue::add_command_bind_fbo(gbufferFramebuffer.getHandle());
    // Clear
    SokolCmdQueue::add_command_clear(sg_clearcolor{0,0,0,1}, 1.0f, 0);
    // Set viewport matching the framebuffer size
    SokolCmdQueue::add_command_viewport(0, 0, gbufferFramebuffer.width(), gbufferFramebuffer.height(), true);
    // No actual mesh draws here – RenderSystem will issue draw calls after this pass is set.
    (void)cameraRender; // placeholder for future view/projection uniform updates.
}

void SokolGBuffer::destroy() {
    if (!initialized) return;
    gbufferObject.destroy();
    gbufferShader.destroy();
    gbufferFramebuffer.destroy();
    initialized = false;
}

}
