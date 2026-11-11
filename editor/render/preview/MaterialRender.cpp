// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "MaterialRender.h"

#include "Configs.h"
#include "PreviewEnvironment.h"

#include <algorithm>

using namespace quantum;

editor::MaterialRender::MaterialRender(){
    scene = new Scene(EntityPool::System);
    camera = new Camera(scene);
    light = new Light(scene);
    sky = new SkyBox(scene);
    sphere = new Shape(scene);

    scene->setBackgroundColor(0.0, 0.0, 0.0, 0.0);
    scene->setCamera(camera);

    buildSphere();
    setReceiveIBL(receiveIBL);


    // interior environment used only as IBL light source; kept invisible so the
    // thumbnail keeps a transparent background (clean sphere icon on drag and drop)
    SkyComponent& skyComp = sky->getComponent<SkyComponent>();
    skyComp.visible = false;
    skyComp.texture.setCubeDatas("preview|environment",
        TextureData((unsigned char*)preview_env_pz, preview_env_pz_len),  // front (+Z)
        TextureData((unsigned char*)preview_env_nz, preview_env_nz_len),  // back  (-Z)
        TextureData((unsigned char*)preview_env_nx, preview_env_nx_len),  // left  (-X)
        TextureData((unsigned char*)preview_env_px, preview_env_px_len),  // right (+X)
        TextureData((unsigned char*)preview_env_py, preview_env_py_len),  // up    (+Y)
        TextureData((unsigned char*)preview_env_ny, preview_env_ny_len)); // down  (-Y)

    light->setDirection(-0.4, -0.5, -0.5);
    light->setIntensity(2.0);
    light->setType(LightType::DIRECTIONAL);

    camera->setPosition(0, 0, 3);
    camera->setTarget(0, 0, 0);
    camera->setType(CameraType::CAMERA_PERSPECTIVE);
    camera->setFramebufferSize(THUMBNAIL_SIZE, THUMBNAIL_SIZE);
    camera->setRenderToTexture(true);
}

editor::MaterialRender::~MaterialRender(){
    delete camera;
    delete light;
    delete sky;
    delete sphere;
    delete scene;
}

void editor::MaterialRender::applyMaterial(const Material& material){
    sphere->setMaterial(material);
}

const Material editor::MaterialRender::getMaterial(){
    return sphere->getMaterial();
}

void editor::MaterialRender::setReceiveIBL(bool value){
    MeshComponent& mesh = sphere->getComponent<MeshComponent>();
    if (mesh.receiveIBL != value){
        mesh.receiveIBL = value;
        mesh.needReload = true;
    }
    receiveIBL = value;
}

bool editor::MaterialRender::getReceiveIBL() const{
    return receiveIBL;
}

Framebuffer* editor::MaterialRender::getFramebuffer(){
    return camera->getFramebuffer();
}

Texture editor::MaterialRender::getTexture(){
    return Texture(getFramebuffer());
}

Scene* editor::MaterialRender::getScene(){
    return scene;
}

Object* editor::MaterialRender::getObject(){
    return sphere;
}

void editor::MaterialRender::buildSphere(){
    // The engine default of 36x18 is faceted enough to read as a polygon at the
    // size a browser draws it, and this geometry is never shipped to a game.
    sphere->createSphere(1.0f, 72, 36);

    if (tilingU == 1.0f && tilingV == 1.0f)
        return;

    MeshComponent& mesh = sphere->getComponent<MeshComponent>();
    Attribute* uv = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    if (!uv)
        return;

    for (unsigned int i = 0; i < uv->getCount(); i++){
        const Vector2 t = mesh.buffer.getVector2(uv, i);
        mesh.buffer.setVector2(i, uv, Vector2(t.x * tilingU, t.y * tilingV));
    }
    mesh.needReload = true;
}

void editor::MaterialRender::setFrameSize(int width, int height){
    if (width <= 0 || height <= 0)
        return;
    camera->setFramebufferSize(width, height);
}

void editor::MaterialRender::setTiling(float tilesU, float tilesV){
    const float u = std::max(0.1f, tilesU);
    const float v = std::max(0.1f, tilesV);
    if (u == tilingU && v == tilingV)
        return;

    tilingU = u;
    tilingV = v;
    buildSphere();
}

void editor::MaterialRender::setCameraPose(float yaw, float pitch, float distance){
    // The ball stays put and the eye moves around it: the key light is fixed, so
    // the studio highlight keeps landing in the same place across the whole grid.
    const float clamped = std::max(0.4f, distance);
    const float cp = std::cos(pitch);
    const Vector3 offset(std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp);

    camera->setPosition(offset.x * clamped, offset.y * clamped, offset.z * clamped);
    camera->setTarget(0, 0, 0);
}

void editor::MaterialRender::setLightIntensity(float intensity){
    light->setIntensity(intensity);
}

void editor::MaterialRender::setBackground(Vector4 color){
    scene->setBackgroundColor(color);
}
