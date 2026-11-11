// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "SokolUIRenderer.h"

#include <algorithm>
#include <cmath>

#include "Engine.h"
#include "Log.h"
#include "pool/ShaderPool.h"
#include "sokol_gfx.h"
#include "texture/Framebuffer.h"
#include "render/FramebufferRender.h"

using namespace quantum;
using namespace quantum::ui;

namespace {

    //same property bits the legacy scene UI requests from ShaderType::UI
    constexpr uint8_t kSolidProperties = 1u << 3;                  //Vc4
    constexpr uint8_t kTextureProperties = (1u << 0) | (1u << 3);  //Tex + Vc4
    //legacy text draws without Vc4 (RenderSystem.cpp:4311-4313): the glyph
    //buffer carries no color, so the FS uiParams.color tints the atlas
    constexpr uint8_t kFontProperties = 1u << 1;                   //Ftx

    //seed sizes so the buffers exist when endLoad() binds them
    constexpr unsigned int kSeedVertexBytes = 4096;
    constexpr unsigned int kSeedIndexBytes = 4096;

    bool rectsEqual(const Rect& a, const Rect& b) {
        return a.getX() == b.getX() && a.getY() == b.getY() &&
               a.getWidth() == b.getWidth() && a.getHeight() == b.getHeight();
    }
}

SokolUIRenderer::SokolUIRenderer() :
    loaded(false),
    solidSlotVS(-1),
    solidSlotFS(-1),
    textureSlotVS(-1),
    textureSlotFS(-1),
    textureSlotTex(-1, -1),
    fontSlotVS(-1),
    fontSlotFS(-1),
    fontSlotTex(-1, -1),
    vertexCapacity(0),
    indexCapacity(0),
    shaderMissingLogged(false),
    hasScissor(false),
    scissor(0.0f, 0.0f, 0.0f, 0.0f) {

    //the vertex layout is fixed for the renderer lifetime (reshape a
    //UIVertex: POSITION(3) + TEXCOORD1(2) + COLOR(4), 36 bytes)
    vertexBuffer.addAttribute(AttributeType::POSITION, 3);
    vertexBuffer.addAttribute(AttributeType::TEXCOORD1, 2);
    vertexBuffer.addAttribute(AttributeType::COLOR, 4);
    vertexBuffer.setUsage(BufferUsage::STREAM);

    //uint32 indices: the default IndexBuffer attribute is uint16, too small
    indexBuffer.addAttribute(AttributeType::INDEX, AttributeDataType::UNSIGNED_INT, 1, 0);
    indexBuffer.setStride(sizeof(uint32_t));
    indexBuffer.setUsage(BufferUsage::STREAM);
}

SokolUIRenderer::~SokolUIRenderer() {
}

namespace quantum {
    std::unique_ptr<ui::UIRenderer> ui::createUIRenderer() {
        return std::unique_ptr<ui::UIRenderer>(new SokolUIRenderer());
    }
}

bool SokolUIRenderer::ensureLoaded(const ui::UIMesh& mesh) {
    const unsigned int vertexBytes = (unsigned int)(mesh.vertices.size() * vertexBuffer.getStride());
    const unsigned int indexBytes = (unsigned int)(mesh.indices.size() * indexBuffer.getStride());

    //pipelines and both buffers are already sized for this frame
    if (loaded && vertexBytes <= vertexCapacity && indexBytes <= indexCapacity)
        return true;

    if (!Engine::isViewLoaded())
        return false;

    //the load below binds the buffers into the SokolObject bindings, so when a
    //bigger mesh arrives the previous surface (pipelines + buffers) must go
    //first instead of leaving stale sg_buffer handles behind
    destroySurface();

    vertexCapacity = std::max(kSeedVertexBytes, vertexBytes);
    indexCapacity = std::max(kSeedIndexBytes, indexBytes);

    if (!vertexBuffer.getRender()->createBuffer(vertexCapacity, vertexBuffer.getData(), vertexBuffer.getType(), vertexBuffer.getUsage()) ||
        !indexBuffer.getRender()->createBuffer(indexCapacity, indexBuffer.getData(), indexBuffer.getType(), indexBuffer.getUsage())) {
        Log::error("Quantum UI: failed to create the UI vertex/index buffers");
        return false;
    }

    const uint8_t properties[3] = { kSolidProperties, kTextureProperties, kFontProperties };
    ObjectRender* renders[3] = { &solidRender, &textureRender, &fontRender };
    std::shared_ptr<ShaderRender>* shaders[3] = { &solidShader, &textureShader, &fontShader };
    int* slotVS[3] = { &solidSlotVS, &textureSlotVS, &fontSlotVS };
    int* slotFS[3] = { &solidSlotFS, &textureSlotFS, &fontSlotFS };

    for (int i = 0; i < 3; i++) {
        std::shared_ptr<ShaderRender> shader = ShaderPool::get(ShaderType::UI, properties[i], 0);
        if (!shader || !shader->isCreated()) {
            if (!shaderMissingLogged) {
                Log::error("Quantum UI: the UI shader variants are not compiled yet; run 'quantum-resource-compiler.exe shaders --project <dir> --backend opengl'");
                shaderMissingLogged = true;
            }
            return false;
        }

        *shaders[i] = shader;

        ShaderData& shaderData = shader->shaderData;
        *slotVS[i] = shaderData.getUniformBlockIndex(UniformBlockType::UI_VS_PARAMS);
        *slotFS[i] = shaderData.getUniformBlockIndex(UniformBlockType::UI_FS_PARAMS);

        ObjectRender& render = *renders[i];
        render.beginLoad(PrimitiveType::TRIANGLES);
        render.setShader(shader.get());

        for (auto const& attr : vertexBuffer.getAttributes()) {
            render.addAttribute(shaderData.getAttrIndex(attr.first), vertexBuffer.getRender(),
                                attr.second.getElements(), attr.second.getDataType(),
                                vertexBuffer.getStride(), attr.second.getOffset(),
                                attr.second.getNormalized(), attr.second.getPerInstance());
        }

        render.setIndex(indexBuffer.getRender(), AttributeDataType::UNSIGNED_INT, 0);

        if (i == 1)
            textureSlotTex = shaderData.getTextureIndex(TextureShaderType::UI);
        else if (i == 2)
            fontSlotTex = shaderData.getTextureIndex(TextureShaderType::UI);

        if (!render.endLoad(PIP_DEFAULT | PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW)) {
            Log::error("Quantum UI: failed to create the UI pipeline (shader variant %u)", (unsigned int)properties[i]);
            return false;
        }
    }

    loaded = true;
    return true;
}

void SokolUIRenderer::destroySurface() {
    solidRender.destroy();
    textureRender.destroy();
    fontRender.destroy();

    vertexBuffer.getRender()->destroyBuffer();
    indexBuffer.getRender()->destroyBuffer();

    vertexCapacity = 0;
    indexCapacity = 0;
    loaded = false;
}

void SokolUIRenderer::uploadMesh(const ui::UIMesh& mesh) {
    vertexBuffer.clear();
    Attribute* positionAttribute = vertexBuffer.getAttribute(AttributeType::POSITION);
    Attribute* texcoordAttribute = vertexBuffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* colorAttribute = vertexBuffer.getAttribute(AttributeType::COLOR);

    for (const ui::UIVertex& vertex : mesh.vertices) {
        vertexBuffer.addVector3(positionAttribute, vertex.position);
        vertexBuffer.addVector2(texcoordAttribute, vertex.uv);
        vertexBuffer.addVector4(colorAttribute, vertex.color);
    }

    indexBuffer.clear();
    Attribute* indexAttribute = indexBuffer.getAttribute(AttributeType::INDEX);
    for (uint32_t value : mesh.indices) {
        indexBuffer.addUInt32(indexAttribute, value);
    }

    //the capacities were sized for exactly this mesh by ensureLoaded()
    const unsigned int vertexBytes = (unsigned int)vertexBuffer.getSize();
    const unsigned int indexBytes = (unsigned int)indexBuffer.getSize();

    if (vertexBytes > 0)
        vertexBuffer.getRender()->updateBuffer(vertexBytes, vertexBuffer.getData());

    if (indexBytes > 0)
        indexBuffer.getRender()->updateBuffer(indexBytes, indexBuffer.getData());
}

void SokolUIRenderer::applyScissor(const Rect& rect) {
    const float x = std::floor(rect.getX());
    const float y = std::floor(rect.getY());
    const float right = std::ceil(rect.getX() + rect.getWidth());
    const float bottom = std::ceil(rect.getY() + rect.getHeight());

    //top-left origin rects; sokol converts per backend
    sg_apply_scissor_rect((int)x, (int)y, (int)(right - x), (int)(bottom - y), true);

    scissor = rect;
    hasScissor = true;
}

void SokolUIRenderer::drawRange(const ui::UIDrawRange& range, Matrix4 mvp, Vector4 white,
                                Vector2 canvasSize, const Rect& viewRect, PipelineType pipelines) {
    //resolve the clip to view pixels (top-left origin)
    if (range.hasClip) {
        const float scaleX = viewRect.getWidth() / canvasSize.x;
        const float scaleY = viewRect.getHeight() / canvasSize.y;

        const float x0 = viewRect.getX() + std::max(0.0f, range.clip.getX()) * scaleX;
        const float y0 = viewRect.getY() + std::max(0.0f, range.clip.getY()) * scaleY;
        const float x1 = viewRect.getX() + std::min(canvasSize.x, range.clip.getX() + range.clip.getWidth()) * scaleX;
        const float y1 = viewRect.getY() + std::min(canvasSize.y, range.clip.getY() + range.clip.getHeight()) * scaleY;

        if (x1 <= x0 || y1 <= y0)
            return; //fully clipped away

        const float left = std::max(x0, viewRect.getX());
        const float top = std::max(y0, viewRect.getY());
        const float right = std::min(x1, viewRect.getX() + viewRect.getWidth());
        const float bottom = std::min(y1, viewRect.getY() + viewRect.getHeight());

        const Rect target(left, top, right - left, bottom - top);
        if (target.getWidth() <= 0.0f || target.getHeight() <= 0.0f)
            return;

        if (!hasScissor || !rectsEqual(scissor, target))
            applyScissor(target);
    }else{
        if (!hasScissor || !rectsEqual(scissor, viewRect))
            applyScissor(viewRect);
    }

    ObjectRender* render = nullptr;
    int slotVS = -1;
    int slotFS = -1;

    if (range.fontAtlas) {
        //the mesh (and its Texture copies) is owned by the runtime and rebuilt
        //every frame; getRender() is const-safe through the cast (it only
        //returns or lazily builds the backend render)
        TextureRender* atlas = const_cast<Texture&>(range.texture).getRender(nullptr);
        if (atlas == nullptr || !atlas->isCreated() || fontSlotTex.first < 0)
            return;

        fontRender.addTexture(fontSlotTex, ShaderStageType::FRAGMENT, atlas);
        render = &fontRender;
        slotVS = fontSlotVS;
        slotFS = fontSlotFS;
    }else if (!range.texture.empty()) {
        TextureRender* image = const_cast<Texture&>(range.texture).getRender(nullptr);
        if (image == nullptr || !image->isCreated() || textureSlotTex.first < 0)
            return;

        textureRender.addTexture(textureSlotTex, ShaderStageType::FRAGMENT, image);
        render = &textureRender;
        slotVS = textureSlotVS;
        slotFS = textureSlotFS;
    }else{
        render = &solidRender;
        slotVS = solidSlotVS;
        slotFS = solidSlotFS;
    }

    if (!render->beginDraw(pipelines)) {
        loaded = false;
        return;
    }

    //text has no vertex color (Ftx only), so its tint comes from the FS color;
    //solid/image ranges carry the color per vertex and keep the uniform white
    Vector4 fsColor = range.fontAtlas ? range.color : white;

    render->applyUniformBlock(slotVS, sizeof(float) * 16, &mvp);
    render->applyUniformBlock(slotFS, sizeof(float) * 4, &fsColor);
    render->draw(range.indexOffset, range.indexCount, 1);
}

void SokolUIRenderer::render(const ui::UIMesh& mesh, Vector2 canvasSize, const Rect& viewRect, Framebuffer* target) {
    if (!ensureLoaded(mesh))
        return;

    if (mesh.vertices.empty() || mesh.indices.empty())
        return;
    if (canvasSize.x <= 0.0f || canvasSize.y <= 0.0f || viewRect.getWidth() <= 0.0f || viewRect.getHeight() <= 0.0f)
        return;

    uploadMesh(mesh);

    if (target != nullptr) {
        if (!target->isCreated())
            return;
        renderPass.startRenderPass(&target->getRender(), 0);
    }else{
        renderPass.startRenderPass();
    }

    renderPass.applyViewport(viewRect);

    const Matrix4 mvp = Matrix4::orthoMatrix(0.0f, canvasSize.x, canvasSize.y, 0.0f, -1.0f, 1.0f);
    const Vector4 white(1.0f, 1.0f, 1.0f, 1.0f);
    const PipelineType pipelines = (target != nullptr) ? PIP_RTT : PIP_DEFAULT;

    //the first range (re)applies the scissor, later ones narrow or reset it
    hasScissor = false;
    applyScissor(viewRect);

    for (const ui::UIDrawRange& range : mesh.ranges) {
        drawRange(range, mvp, white, canvasSize, viewRect, pipelines);
    }

    renderPass.endRenderPass();
}
