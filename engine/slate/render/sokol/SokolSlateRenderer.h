// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef SokolSlateRenderer_h
#define SokolSlateRenderer_h

#include <memory>
#include <utility>

#include "buffer/InterleavedBuffer.h"
#include "buffer/IndexBuffer.h"
#include "math/Matrix4.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "render/CameraRender.h"
#include "render/ObjectRender.h"
#include "render/ShaderRender.h"
#include "slate/render/SlateRenderer.h"

namespace doriax {

    // Sokol backend of the Slate renderer. Draws the built SlateMesh with the
    // three UI shader variants (solid / texture / font atlas) shared with the
    // legacy scene UI, one draw per SlateDrawRange.
    class SokolSlateRenderer : public slate::ISlateRenderer {
    public:
        SokolSlateRenderer();
        virtual ~SokolSlateRenderer();

        void render(const slate::SlateMesh& mesh, Vector2 canvasSize, const Rect& viewRect, Framebuffer* target) override;

    private:
        bool ensureLoaded(const slate::SlateMesh& mesh);
        void destroySurface();
        void uploadMesh(const slate::SlateMesh& mesh);
        void applyScissor(const Rect& rect);
        void drawRange(const slate::SlateDrawRange& range, Matrix4 mvp, Vector4 white,
                       Vector2 canvasSize, const Rect& viewRect, PipelineType pipelines);

        bool loaded;

        //the exported pass abstraction (CameraRender) rather than SokolCamera:
        //the backend classes inside the dll are not exported, so a static-lib
        //consumer can only reach the render API through core/render.
        CameraRender renderPass;
        InterleavedBuffer vertexBuffer;
        IndexBuffer indexBuffer;

        ObjectRender solidRender;
        ObjectRender textureRender;
        ObjectRender fontRender;

        //keep the pool entries alive while the renders reference them
        std::shared_ptr<ShaderRender> solidShader;
        std::shared_ptr<ShaderRender> textureShader;
        std::shared_ptr<ShaderRender> fontShader;

        int solidSlotVS;
        int solidSlotFS;
        int textureSlotVS;
        int textureSlotFS;
        std::pair<int, int> textureSlotTex;
        int fontSlotVS;
        int fontSlotFS;
        std::pair<int, int> fontSlotTex;

        unsigned int vertexCapacity;
        unsigned int indexCapacity;

        bool shaderMissingLogged;
        bool hasScissor;
        Rect scissor;

        //height of the pass being drawn, in pixels: scissor rects arrive in
        //top-left window coordinates and sokol wants them bottom-origin
        float passHeight;
    };
}

#endif /* SokolSlateRenderer_h */
