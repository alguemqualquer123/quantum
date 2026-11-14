// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "object/Camera.h"
#include "object/Model.h"
#include "object/Light.h"

namespace quantum::editor{

    class ModelRender{
    private:
        Scene* scene;
        Camera* camera;
        Light* light;
        Model* model;

    public:
        ModelRender();
        virtual ~ModelRender();

        ModelRender(const ModelRender&) = delete;
        ModelRender& operator=(const ModelRender&) = delete;
        ModelRender(ModelRender&&) noexcept = default;
        ModelRender& operator=(ModelRender&&) noexcept = default;

        void clearScene();
        void clearAndInitScene();
        bool loadModel(const std::string& filename);
        void positionCameraForModel();
        void fixDarkMaterials();

        // The combined bounds of the model branch, so a viewer can frame and orbit it
        bool getModelBounds(AABB& outBounds);
        void setFrameSize(int width, int height);
        void setLightIntensity(float intensity);
        // The orbit look: the eye placed around a focus point at a distance and two angles
        void setCameraPose(const Vector3& focus, float yaw, float pitch, float distance);

        Framebuffer* getFramebuffer();
        Texture getTexture();
        Scene* getScene();
        Object* getObject();
        Camera* getCamera();
        Model* getModel();
    };

}
