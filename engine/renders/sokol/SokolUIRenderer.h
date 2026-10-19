// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef SokolUIRenderer_h
#define SokolUIRenderer_h

#include <memory>
#include <utility>

#include "buffer/InterleavedBuffer.h"
#include "buffer/IndexBuffer.h"
#include "math/Matrix4.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "render/ObjectRender.h"
#include "render/ShaderRender.h"
#include "SokolCamera.h"
#include "ui/render/UIRenderer.h"

namespace doriax {

    // Sokol backend of the retained-mode UI renderer. Draws the built UIMesh
    // with the three UI shader variants (solid / texture / font atlas) that
    // the legacy scene UI also uses, one draw per UIDrawRange.
    class SokolUIRenderer : public ui::UIRenderer {
    public:
        SokolUIRenderer();
        virtual ~SokolUIRenderer();

        void render(const ui::UIMesh& mesh, Vector2 canvasSize, const Rect& viewRect, Framebuffer* target) override;

    private:
        bool ensureLoaded(const ui::UIMesh& mesh);
        void destroySurface();
        void uploadMesh(const ui::UIMesh& mesh);
        void applyScissor(const Rect& rect);
        void drawRange(const ui::UIDrawRange& range, Matrix4 mvp, Vector4 white,
                       Vector2 canvasSize, const Rect& viewRect, PipelineType pipelines);

        bool loaded;

        SokolCamera renderPass;
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
    };
}

#endif /* SokolUIRenderer_h */
