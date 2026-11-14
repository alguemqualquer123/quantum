// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "object/Camera.h"
#include "object/Object.h"
#include "object/Shape.h"
#include "object/Light.h"
#include "object/environment/SkyBox.h"

namespace quantum::editor{

    class MaterialRender{
    private:
        Scene* scene;
        Camera* camera;

        Light* light;
        SkyBox* sky;

        Shape* sphere;
        bool receiveIBL = true;

        // Tiles per revolution of the sphere. A 1x1 map only wraps the ball once,
        // which reads as a stretched decal rather than a surface.
        float tilingU = 1.0f;
        float tilingV = 1.0f;

        void buildSphere();

    public:
        MaterialRender();
        virtual ~MaterialRender();

        MaterialRender(const MaterialRender&) = delete;
        MaterialRender& operator=(const MaterialRender&) = delete;
        MaterialRender(MaterialRender&&) noexcept = default;
        MaterialRender& operator=(MaterialRender&&) noexcept = default;

        void applyMaterial(const Material& material);
        const Material getMaterial();

        void setReceiveIBL(bool value);
        bool getReceiveIBL() const;

        // The controls a material browser needs and a 128px thumbnail never asked
        // for. Every one of them keeps the thumbnail's defaults when untouched.
        void setFrameSize(int width, int height);
        void setTiling(float tilesU, float tilesV);
        void setCameraPose(float yaw, float pitch, float distance);
        void setLightIntensity(float intensity);
        void setBackground(Vector4 color);

        Framebuffer* getFramebuffer();
        Texture getTexture();
        Scene* getScene();
        Object* getObject();
    };

}