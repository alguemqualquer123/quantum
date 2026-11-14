// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "RenderSystem.h"

#include "texture/TextureData.h"
#include "object/Camera.h"
#include "math/Quaternion.h"
#include "Scene.h"
#include "Engine.h"
#include "System.h"
#include "render/Render.h"
#include "render/ObjectRender.h"
#include "render/SystemRender.h"
#include "pool/ShaderPool.h"
#include "pool/TexturePool.h"
#include "math/Vector3.h"
#include "texture/IBLEnvironment.h"
#include "util/Angle.h"
#include "util/Color.h"
#include "util/Wind.h"
#include "buffer/ExternalBuffer.h"
#include "math/AABB.h"
#include "pool/TextureDataPool.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/WaterSystem.h"
#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <cstdint>
#include <cstring>

using namespace quantum;

namespace {
    constexpr int SPOT_MASK_TILE_SIZE = 256;

    int shadowVirtualPageIndex(int logicalPage, int tile){
        return logicalPage * SHADOW_VIRTUAL_PAGES_PER_MAP + tile;
    }

    Matrix4 shadowTileCrop(int tile){
        Matrix4 crop;
        crop.identity();
        const int tileX = tile % SHADOW_VIRTUAL_PAGE_GRID;
        const int tileY = tile / SHADOW_VIRTUAL_PAGE_GRID;
        crop.set(0, 0, (float)SHADOW_VIRTUAL_PAGE_GRID);
        crop.set(1, 1, (float)SHADOW_VIRTUAL_PAGE_GRID);
        crop.set(3, 0, 1.0f - 2.0f * (float)tileX);
        crop.set(3, 1, 1.0f - 2.0f * (float)tileY);
        return crop;
    }

    // every light, not just the selected ones, so the variant does not change with the camera
    bool allLightsDirectional(Scene* scene){
        auto lights = scene->getComponentArray<LightComponent>();
        size_t count = lights->size();
        for (size_t i = 0; i < count; i++){
            if (lights->getComponentFromIndex(i).type != LightType::DIRECTIONAL){
                return false;
            }
        }
        return count > 0;
    }

    bool hasMaterialExtensions(const Material& material){
        return material.clearcoatFactor > 0.0001f ||
               material.sheenColorFactor.x > 0.0001f ||
               material.sheenColorFactor.y > 0.0001f ||
               material.sheenColorFactor.z > 0.0001f ||
               material.transmissionFactor > 0.0001f ||
               material.thicknessFactor > 0.0001f ||
               material.anisotropyStrength > 0.0001f ||
               material.attenuationDistance > 0.0001f;
    }

    static_assert(sizeof(Matrix4) == sizeof(float) * 16);

    bool bindSkinningResource(ObjectRender& render, ShaderData& shader, MeshComponent& mesh){
        const size_t boneCount = mesh.bonesMatrix.size();
        const unsigned int size = static_cast<unsigned int>(boneCount * sizeof(Matrix4));
        const int storageSlot = shader.getStorageBufferIndex(StorageBufferType::VS_SKINNING);
        if (storageSlot != -1){
            if (!mesh.bonesBuffer.isCreated() &&
                    !mesh.bonesBuffer.createBuffer(size, nullptr, BufferType::STORAGE_BUFFER, BufferUsage::STREAM))
                return false;
            render.addStorageBuffer(storageSlot, ShaderStageType::VERTEX, &mesh.bonesBuffer);
        }else{
            const auto textureSlot = shader.getTextureIndex(TextureShaderType::BONES);
            if (textureSlot.first == -1 || (!mesh.bonesTexture.isCreated() &&
                    !mesh.bonesTexture.createDynamicTexture("mesh-bones", 4,
                        static_cast<int>(boneCount))))
                return false;
            render.addTexture(textureSlot, ShaderStageType::VERTEX, &mesh.bonesTexture);
        }
        mesh.needUpdateBones = true;
        return true;
    }

    void applySkinningUniform(ObjectRender& render, int slot, const MeshComponent& mesh,
                              const Submesh& submesh){
        Vector4 normAdjust(
            submesh.hasSkinningNormalization ? submesh.normAdjustJoint : mesh.normAdjustJoint,
            submesh.hasSkinningNormalization ? submesh.normAdjustWeight : mesh.normAdjustWeight,
            0.0f, 0.0f);
        render.applyUniformBlock(slot, sizeof(normAdjust), &normAdjust);
    }

    // fade distance past which instances can be dropped: only the built-in triangle
    // shaders shrink them to nothing, and the SSR G-buffer has no fade yet
    float instanceFadeCullEnd(const InstancedMeshComponent& instmesh, const MeshComponent& mesh, const Scene& scene){
        if (scene.isSSREnabled() || !scene.getDefaultMeshShader().empty() ||
            !instmesh.distanceFade || !mesh.customShader.empty() || !mesh.customDepthShader.empty() ||
            !(instmesh.fadeEnd > instmesh.fadeStart) || !std::isfinite(instmesh.fadeStart) ||
            !std::isfinite(instmesh.fadeEnd) || instmesh.fadeEnd < 0.0f){
            return -1.0f;
        }
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            const auto primitive = mesh.submeshes[s].primitiveType;
            if (primitive != PrimitiveType::TRIANGLES && primitive != PrimitiveType::TRIANGLE_STRIP){
                return -1.0f;
            }
        }
        return instmesh.fadeEnd;
    }

    // distance inside which instances are dropped, a CPU cut that works with any shader
    float instanceCullNear(const InstancedMeshComponent& instmesh){
        if (!(instmesh.cullNearDistance > 0.0f) || !std::isfinite(instmesh.cullNearDistance)){
            return 0.0f;
        }
        return instmesh.cullNearDistance;
    }

    // shadow-map texels of error a caster may lose, well inside the PCF blur
    const float SHADOW_LOD_TEXELS = 2.0f;

    // an instanced mesh with culling off keeps its whole batch, bounds or not
    bool cullsBatch(const MeshComponent& mesh, const InstancedMeshComponent* instmesh){
        return mesh.worldAABB != AABB::ZERO && (!instmesh || instmesh->cullInstances);
    }

    // a box without finite bounds counts as inside
    bool isInsidePlanes(const Plane* planes, int count, const AABB& box){
        if (!box.isFinite())
            return true;

        Vector3 center = box.getCenter();
        Vector3 halfSize = box.getHalfSize();
        for (int i = 0; i < count; i++){
            if (planes[i].getSide(center, halfSize) == Plane::Side::NEGATIVE_SIDE)
                return false;
        }
        return true;
    }

    // getCenter() asserts on null/infinite boxes. New foliage (and any empty
    // instanced mesh) leaves worldAABB null; AABB::ZERO is the unloaded default
    // and is not a useful sample point.
    Vector3 meshProbeSamplePosition(const MeshComponent& mesh, const Transform& transform){
        if (mesh.worldAABB.isFinite() && mesh.worldAABB != AABB::ZERO){
            return mesh.worldAABB.getCenter();
        }
        return transform.worldPosition;
    }

    void applyInstanceFadeUniform(ObjectRender& render, int slot, const InstancedMeshComponent& instmesh){
        float fade[8] = {instmesh.fadeStart, instmesh.fadeEnd, 0.0f, 0.0f,
            instmesh.fadeEyeLocal.x, instmesh.fadeEyeLocal.y, instmesh.fadeEyeLocal.z, 0.0f};
        render.applyUniformBlock(slot, sizeof(fade), &fade);
    }

    // custom block of a forked shader, with the engine-written members refreshed
    void applyCustomUniforms(ObjectRender& render, CustomUniformBlock& block, float frameTime, const Vector2& resolution){
        if (block.slot == -1)
            return;

        block.writeTime(frameTime);
        block.writeResolution(resolution.x, resolution.y);
        render.applyUniformBlock(block.slot, (unsigned int)block.data.size(), block.data.data());
    }

    // both stages of a single-block component, rewritten first when its values changed
    template<typename T>
    void applyCustomUniforms(ObjectRender& render, T& component, const Scene* scene, ShaderType type,
                             float frameTime, const Vector2& resolution){
        if (component.needUpdateShaderUniforms){
            const ShaderUniformValues* inherited = scene->getInheritedShaderUniforms(type, component.customShader);
            component.customVSParams.writeValues(component.shaderUniforms, inherited);
            component.customFSParams.writeValues(component.shaderUniforms, inherited);
            component.needUpdateShaderUniforms = false;
        }
        applyCustomUniforms(render, component.customVSParams, frameTime, resolution);
        applyCustomUniforms(render, component.customFSParams, frameTime, resolution);
    }

    // the mesh blocks of both forks, from whichever pass draws first (depth runs before color)
    void writeCustomUniforms(MeshComponent& mesh, const Scene* scene){
        if (!mesh.needUpdateShaderUniforms)
            return;

        const ShaderUniformValues* inherited = scene->getInheritedShaderUniforms(ShaderType::MESH, mesh.customShader);
        for (int i = 0; i < mesh.numSubmeshes; i++){
            mesh.submeshes[i].customVSParams.writeValues(mesh.shaderUniforms, inherited);
            mesh.submeshes[i].customFSParams.writeValues(mesh.shaderUniforms, inherited);
            mesh.submeshes[i].customVSDepthParams.writeValues(mesh.shaderUniforms, inherited);
            mesh.submeshes[i].customFSDepthParams.writeValues(mesh.shaderUniforms, inherited);
        }
        mesh.needUpdateShaderUniforms = false;
    }

    // the water blocks of both forks, from whichever pass draws first (underwater runs before the surface)
    void writeCustomUniforms(WaterComponent& water, const Scene* scene){
        if (!water.needUpdateShaderUniforms)
            return;

        const ShaderUniformValues* inherited = scene->getInheritedShaderUniforms(ShaderType::WATER, water.customShader);
        water.customVSParams.writeValues(water.shaderUniforms, inherited);
        water.customFSParams.writeValues(water.shaderUniforms, inherited);
        water.customUnderwaterParams.writeValues(water.shaderUniforms, inherited);
        water.needUpdateShaderUniforms = false;
    }

    // flags the components that inherit their default shader
    template<typename ComponentArrayPtr>
    void flagInheritedUniforms(const ComponentArrayPtr& components){
        for (size_t i = 0; i < components->size(); i++){
            auto& component = components->getComponentFromIndex(i);
            if (component.customShader.empty())
                component.needUpdateShaderUniforms = true;
        }
    }

    bool usesAlphaBlend(const Material& material){
        return material.alphaMode == MaterialAlphaMode::BLEND ||
            (material.alphaMode == MaterialAlphaMode::AUTO &&
             (material.baseColorTexture.isTransparent() || material.baseColorFactor.w != 1.0f));
    }

    // the destination selects swapchain or RTT, a reflection pass reverses winding
    // and a canvas camera drops the depth buffer entirely
    PipelineType colorPipeline(const CameraComponent& camera, bool offscreenTarget){
        if (!offscreenTarget)
            return camera.depthTest ? PIP_DEFAULT : PIP_DEFAULT_NODEPTH;
        if (camera.invertCulling)
            return PIP_RTT_INVERT;
        return camera.depthTest ? PIP_RTT : PIP_RTT_NODEPTH;
    }

    // a canvas camera has no depth buffer to fix blending order, so it composites
    // strictly in submission order instead
    bool sortsByDistance(const CameraComponent& camera){
        return camera.depthTest && camera.transparentSort;
    }

    float readSpotMaskTexel(TextureData& source, int x, int y, bool useAlpha){
        x = std::clamp(x, 0, source.getWidth() - 1);
        y = std::clamp(y, 0, source.getHeight() - 1);

        const int channels = source.getChannels();
        const int bytesPerChannel = TextureData::getBytesPerChannel(source.getColorFormat());
        const size_t pixelOffset =
            (static_cast<size_t>(y) * static_cast<size_t>(source.getWidth()) + static_cast<size_t>(x)) *
            static_cast<size_t>(channels) * static_cast<size_t>(bytesPerChannel);
        const unsigned char* pixels = static_cast<const unsigned char*>(source.getData());

        if (source.getColorFormat() == ColorFormat::RED16){
            uint16_t value = 0;
            std::memcpy(&value, pixels + pixelOffset, sizeof(value));
            return static_cast<float>(value) / 65535.0f;
        }

        if (useAlpha && channels >= 4){
            return static_cast<float>(pixels[pixelOffset + 3]) / 255.0f;
        }

        if (channels >= 3){
            Vector3 color = Color::sRGBToLinear(
                static_cast<float>(pixels[pixelOffset]) / 255.0f,
                static_cast<float>(pixels[pixelOffset + 1]) / 255.0f,
                static_cast<float>(pixels[pixelOffset + 2]) / 255.0f);
            return 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
        }

        return static_cast<float>(pixels[pixelOffset]) / 255.0f;
    }

    void rasterizeSpotMaskTile(TextureData& source, int tileIndex, int atlasWidth, std::vector<unsigned char>& atlas){
        const bool useAlpha = source.getColorFormat() == ColorFormat::RGBA && source.hasAlpha();
        const float sourceWidth = static_cast<float>(source.getWidth());
        const float sourceHeight = static_cast<float>(source.getHeight());

        for (int y = 0; y < SPOT_MASK_TILE_SIZE; y++){
            const float sourceY = ((static_cast<float>(y) + 0.5f) / static_cast<float>(SPOT_MASK_TILE_SIZE)) * sourceHeight - 0.5f;
            const int y0 = static_cast<int>(std::floor(sourceY));
            const int y1 = y0 + 1;
            const float fy = sourceY - static_cast<float>(y0);

            for (int x = 0; x < SPOT_MASK_TILE_SIZE; x++){
                const float sourceX = ((static_cast<float>(x) + 0.5f) / static_cast<float>(SPOT_MASK_TILE_SIZE)) * sourceWidth - 0.5f;
                const int x0 = static_cast<int>(std::floor(sourceX));
                const int x1 = x0 + 1;
                const float fx = sourceX - static_cast<float>(x0);

                const float top = readSpotMaskTexel(source, x0, y0, useAlpha) * (1.0f - fx) +
                                  readSpotMaskTexel(source, x1, y0, useAlpha) * fx;
                const float bottom = readSpotMaskTexel(source, x0, y1, useAlpha) * (1.0f - fx) +
                                     readSpotMaskTexel(source, x1, y1, useAlpha) * fx;
                const float value = top * (1.0f - fy) + bottom * fy;

                const int atlasX = tileIndex * SPOT_MASK_TILE_SIZE + x;
                atlas[static_cast<size_t>(y) * static_cast<size_t>(atlasWidth) + static_cast<size_t>(atlasX)] =
                    static_cast<unsigned char>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f));
            }
        }
    }
}

uint32_t RenderSystem::pixelsWhite[64];
uint32_t RenderSystem::pixelsBlack[64];
uint32_t RenderSystem::pixelsNormal[64];

TextureRender RenderSystem::emptyWhite;
TextureRender RenderSystem::emptyArrayWhite;
TextureRender RenderSystem::emptyBlack;
TextureRender RenderSystem::emptyCubeBlack;
TextureRender RenderSystem::emptyCubeWhite;
TextureRender RenderSystem::emptyNormal;
TextureRender RenderSystem::emptyShadowDepth;

bool RenderSystem::emptyTexturesCreated = false;

RenderSystem::RenderSystem(Scene* scene): SubSystem(scene){
    instanceViewsDirty = true;
    mainInstanceViewDirty = true;
    shadowInstanceViewsDirty = true;
    signature.set(scene->getComponentId<Transform>());

    this->scene = scene;

    ssaoLoaded = false;
    ssaoWidth = 0;
    ssaoHeight = 0;
    ssaoSlotParams = -1;
    ssaoBlurSlotParams = -1;
    currentSSAOTexture = NULL;
    contactShadowsLoaded = false;
    contactShadowsHDR = false;
    contactShadowsWidth = 0;
    contactShadowsHeight = 0;
    contactShadowsSlotParams = -1;
    ssgiLoaded = false;
    ssgiHDR = false;
    ssgiWidth = 0;
    ssgiHeight = 0;
    ssgiSlotParams = -1;
    decalsLoaded = false;
    decalsHDR = false;
    decalSlotParams = -1;
    currentSceneDepthTexture = NULL;
    depthPrePassRendered = false;
    currentSceneCopy = NULL;
    currentSceneCopyRect = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    resumePassRender.setLoadActionLoad();
    resumePassRender.setDepthLoadActionLoad();
    currentUnderwater = NULL;
    hasWaterDepth = false;
    onlyDirectionalLights = false;
    frameTime = 0.0f;
    passResolution = Vector2(0.0f, 0.0f);

    ssrLoaded = false;
    ssrWidth = 0;
    ssrHeight = 0;
    ssrCompositeHDR = false;
    ssrSlotParams = -1;
    ssrBlurSlotParams = -1;
    compositeSlotParams = -1;
    swapchainRedirect = false;

    blitLoaded = false;
    fixedResWidth = 0;
    fixedResHeight = 0;
    blitSlotParams = -1;

    postProcessNeedsDepth = false;
    postProcessNeedsGBuffer = false;

    bloomLoaded = false;
    bloomWidth = 0;
    bloomHeight = 0;
    activeBloomMipLevels = 0;
    bloomIntensity = 0.0f;
    bloomThresholdSlotParams = -1;
    bloomDownsampleSlotParams = -1;
    bloomUpsampleSlotParams = -1;
    bloomCompositeSlotParams = -1;

    tonemapLoaded = false;
    tonemapSlotParams = -1;

    exposureLoaded = false;
    exposureSlotParams = -1;
    exposureWriteIndex = 0;
    hdrFeatureMask = 0;

    hdrLoaded = false;
    hdrWidth = 0;
    hdrHeight = 0;
    lut3DLoaded = false;
    lut3DSize = 0;

    fxaaLoaded = false;
    fxaaWidth = 0;
    fxaaHeight = 0;
    fxaaSlotParams = -1;

    dofLoaded = false;
    dofWidth = 0;
    dofHeight = 0;
    dofHalfWidth = 0;
    dofHalfHeight = 0;
    dofCocSlotParams = -1;
    dofBlurSlotParams = -1;
    dofCompositeSlotParams = -1;

    atmosphereLoaded = false;
    atmosphereWidth = 0;
    atmosphereHeight = 0;
    atmosphereSlotParams = -1;

    velocityLoaded = false;
    taaLoaded = false;
    motionBlurLoaded = false;
    temporalWidth = 0;
    temporalHeight = 0;
    temporalInputWidth = 0;
    temporalInputHeight = 0;
    velocitySlotParams = -1;
    taaSlotParams = -1;
    motionBlurSlotParams = -1;
    taaHistoryIndex = 0;
    taaHistoryValid = false;
    previousViewProjectionValid = false;

    shadowAtlasSlotResolution = 0;
    shadowAtlasCols = 0;
    shadowAtlasRows = 0;
    shadowAtlasUsedSlots = 0;
    needUpdateShadowAtlas = true;
    shadowAtlasContentDirty = true;
    shadowPageTableCreated = false;
    shadowPageTablePixels.fill(0);
    shadowVirtualToPhysical.fill(-1);
    shadowPhysicalPageKeys.fill(0);
    shadowPointAtlasSlotResolution = 0;
    needUpdateShadowPointAtlas = true;
    shadowPointAtlasContentDirty = true;
    needUpdateShadowBindings = false;
    hasShadowAtlas = false;
    hasShadowPointAtlas = false;
    spotMaskAtlasCreated = false;
    waterExclusionMapLayers = 0;
    needUpdateWaterExclusionMaps = true;
    initShadowAtlasRects();
    initShadowPointAtlasRects();

    shadow2DAtlasWidth = 0;
    hasShadow2DAtlas = false;
    occluder2DLoaded = false;
    shadow2DSlotParams = -1;
    occluder2DBufferCapacity = 0;
    occluder2DVertexCount = 0;
    occluder2DNeedUpdateBuffer = false;
    hasReflectionProbes = false;
    capturingReflectionProbe = false;
    clusteredLightGridWidth = 0;
    clusteredLightGridHeight = 0;
    clusteredLightGridDepthSlices = 0;
    clusteredLightGridReady = false;
    clusteredLightGridKey = 0;
    clusteredLightGridInfo = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    clusteredLightGridDepth = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
}

RenderSystem::~RenderSystem(){
}

void RenderSystem::load(){
    hasLights = false;
    hasShadows = false;
    onlyDirectionalLights = false;
    hasFog = false;
    hasHeightFog = false;
    hasIBL = false;
    hasReflectionProbes = false;
    hasWaterDepth = false;
    hasMultipleCameras = false;
    lastMultiCameraDraw = false;
    capturingReflectionProbe = false;
    lastBatchSort = true;
    lastMeshLodEnabled = false;
    loadedPipelines = 0;
    hasLights2D = false;
    hasShadows2D = false;
    numLights2D = 0;

    createEmptyTextures();

    update(0); // first update
}

void RenderSystem::destroy(){
    // View teardown keeps the scene alive; retain CPU assets for its next load.
    const bool reloadView = !Engine::isViewLoaded();

    // Before the transform traversal below, which the camera entities are part of.
    while (!mirrorCameras.empty()){
        destroyMirrorCamera(mirrorCameras.begin()->first);
    }

    // cannot destroy static textures because it affects other scenes
    //emptyWhite.destroyTexture();
    //emptyBlack.destroyTexture();
    //emptyCubeBlack.destroyTexture();
    //emptyNormal.destroyTexture();

    emptyTexturesCreated = false;

    destroySSAO();
    destroyContactShadows();
    destroySSGI();
    destroyDecals();
    destroySSR();
    destroyBlit();
    destroyPostProcess();
    destroyHDR();
    destroyTemporalEffects();
    destroyFXAA();
    destroyDOF();
    destroyAtmosphere();
    clusteredLightGridBuffer.destroyBuffer();
    clusteredLightGridData.clear();
    clusteredLightGridWidth = 0;
    clusteredLightGridHeight = 0;
    clusteredLightGridDepthSlices = 0;
    clusteredLightGridReady = false;
    clusteredLightGridKey = 0;
    clusteredLightGridInfo = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    clusteredLightGridDepth = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    for (InstanceView& view : instanceViews){
        view.buffer.getRender()->destroyBuffer();
        view.data.clear();
        view.capacity = 0;
    }
    instanceViewsDirty = true;
    shadowAtlasFramebuffer.destroyFramebuffer();
    shadowPageTable.destroyTexture();
    shadowPageTableCreated = false;
    shadowAtlasSlotResolution = 0;
    shadowAtlasCols = 0;
    shadowAtlasRows = 0;
    shadowAtlasUsedSlots = 0;
    hasShadowAtlas = false;
    shadowPointAtlasFramebuffer.destroyFramebuffer();
    shadowPointAtlasSlotResolution = 0;
    hasShadowPointAtlas = false;
    shadow2DAtlasFramebuffer.destroyFramebuffer();
    shadow2DAtlasWidth = 0;
    hasShadow2DAtlas = false;
    destroyOccluder2DPass();
    if (spotMaskAtlasCreated){
        spotMaskAtlas.destroyTexture();
        spotMaskAtlasCreated = false;
    }
    spotMaskAtlasPixels.clear();
    spotMaskAtlasEntries.fill({});
    if (waterExclusionMapLayers > 0){
        waterExclusionMaps.destroyTexture();
        waterExclusionMapLayers = 0;
    }
    needUpdateWaterExclusionMaps = true;

    auto skys = scene->getComponentArray<SkyComponent>();
    if (skys->size() > 0){
        SkyComponent& sky = skys->getComponentFromIndex(0);
        Entity entity = skys->getEntity(0);
        if (sky.loaded){
            if (reloadView){
                sky.needReload = true;
                releaseSkyEnvironment(sky);
                sky.needUpdateEnvironment = true;
            }
            destroySky(entity, sky);
        }
    }

    auto waters = scene->getComponentArray<WaterComponent>();
    for (int i = 0; i < waters->size(); i++){
        WaterComponent& water = waters->getComponentFromIndex(i);
        if (water.loaded){
            if (reloadView) water.needReload = true;
            destroyWater(waters->getEntity(i), water);
        }
    }

    auto probes = scene->getComponentArray<ReflectionProbeComponent>();
    for (int i = 0; i < probes->size(); i++){
        ReflectionProbeComponent& probe = probes->getComponentFromIndex(i);
        destroyReflectionProbe(probes->getEntity(i), probe);
    }
    // defensive sweep: a runtime can outlive its component (e.g. the probe lost
    // its Transform and updateReflectionProbes never revisited it)
    for (auto& entry : reflectionProbeRuntimes){
        entry.second->captureFramebuffer.destroyFramebuffer();
        releaseReflectionProbeMaps(*entry.second);
    }
    reflectionProbeRuntimes.clear();

    auto transforms = scene->getComponentArray<Transform>();
    for (int i = 0; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);
        Entity entity = transforms->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            if (mesh.loaded){
                if (reloadView) mesh.needReload = true;
                destroyMesh(entity, mesh);
            }
        }else if (signature.test(scene->getComponentId<UIComponent>())){
            UIComponent& ui = scene->getComponent<UIComponent>(entity);
            if (ui.loaded){
                if (reloadView) ui.needReload = true;
                destroyUI(entity, ui);
            }
        }else if (signature.test(scene->getComponentId<PointsComponent>())){
            PointsComponent& points = scene->getComponent<PointsComponent>(entity);
            if (points.loaded){
                if (reloadView) points.needReload = true;
                destroyPoints(entity, points);
            }
        }else if (signature.test(scene->getComponentId<LinesComponent>())){
            LinesComponent& lines = scene->getComponent<LinesComponent>(entity);
            if (lines.loaded){
                if (reloadView) lines.needReload = true;
                destroyLines(entity, lines);
            }
        }else if (signature.test(scene->getComponentId<LightComponent>())){
            LightComponent& light = scene->getComponent<LightComponent>(entity);
            destroyLight(light);
        }else if (signature.test(scene->getComponentId<CameraComponent>())){
            CameraComponent& camera = scene->getComponent<CameraComponent>(entity);
            destroyCamera(camera, false);
        }
    }

    // Cached GPU arrays cannot survive renderer shutdown, even when CPU assets are retained.
    while (!terrainDetailArrays.empty()){
        destroyTerrainDetailArray(terrainDetailArrays.begin()->first);
    }
}

void RenderSystem::updateFramebuffer(CameraComponent& camera){
    if (camera.framebuffer->isCreated()){
        camera.framebuffer->destroy();
        camera.framebuffer->create();
    }
}

void RenderSystem::createEmptyTextures(){
    if (!emptyTexturesCreated){
        void* data_array[6];
        size_t size_array[6];

        for (int i = 0; i < 64; i++) {
            pixelsNormal[i] = 0xFF808080;
        }

        for (int i = 0; i < 6; i++){
            data_array[i]  = (void*)pixelsNormal;
            size_array[i] = 8 * 8 * 4;
        }

        emptyNormal.createTexture(
                "empty|normal", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data_array, size_array, 
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

        for (int i = 0; i < 64; i++) {
            pixelsWhite[i] = 0xFFFFFFFF;
        }

        for (int i = 0; i < 6; i++){
            data_array[i]  = (void*)pixelsWhite;
            size_array[i] = 8 * 8 * 4;
        }

        emptyWhite.createTexture(
                "empty|white", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data_array, size_array,
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

        emptyArrayWhite.createTexture(
                "empty|array|white", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_ARRAY, 1, data_array, size_array,
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

        // white cube fallback for the skybox cube sampler: a color-only sky (no
        // texture) multiplies skyParams.color by this, so white keeps the color intact
        emptyCubeWhite.createTexture(
                "empty|cube|white", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_CUBE, 6, data_array, size_array,
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

        for (int i = 0; i < 64; i++) {
            pixelsBlack[i] = 0xFF000000;
        }

        for (int i = 0; i < 6; i++){
            data_array[i]  = (void*)pixelsBlack;
            size_array[i] = 8 * 8 * 4;
        }

        emptyBlack.createTexture(
                "empty|black", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data_array, size_array, 
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);
        emptyCubeBlack.createTexture(
                "empty|cube|black", 8, 8, ColorFormat::RGBA, TextureType::TEXTURE_CUBE, 6, data_array, size_array, 
                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

        emptyTexturesCreated = true;
    }

    // Point-only scenes still declare the projective sampler, so keep a
    // type-compatible binding available even if it is never sampled.
    if (!emptyShadowDepth.isCreated()){
        emptyShadowDepth.createFramebufferTexture(
            TextureType::TEXTURE_2D, true, true, 1, 1,
            TextureFilter::NEAREST, TextureFilter::NEAREST,
            TextureWrap::CLAMP_TO_BORDER, TextureWrap::CLAMP_TO_BORDER);
    }
}

// Past MAX_FORWARD_LIGHTS lit lights, directional ones go first, then the nearest to the eye.
// One in use counts as 10% nearer, so close ones do not swap every frame.
std::vector<size_t> RenderSystem::chooseLights(const Vector3& eye, const std::vector<size_t>& inUse){
    auto lights = scene->getComponentArray<LightComponent>();

    std::vector<std::pair<float, size_t>> candidates;
    for (size_t i = 0; i < lights->size(); i++){
        const LightComponent& light = lights->getComponentFromIndex(i);
        if (light.intensity <= 0.0f)
            continue;
        float score = -1.0f;
        if (light.type != LightType::DIRECTIONAL){
            Transform* transform = scene->findComponent<Transform>(lights->getEntity(i));
            score = ((transform ? transform->worldPosition : Vector3::ZERO) - eye).squaredLength();
            if (std::binary_search(inUse.begin(), inUse.end(), i))
                score *= 0.81f;
        }
        candidates.push_back({score, i});
    }
    if (candidates.size() > (size_t)MAX_FORWARD_LIGHTS){
        std::nth_element(candidates.begin(), candidates.begin() + MAX_FORWARD_LIGHTS, candidates.end());
        candidates.resize(MAX_FORWARD_LIGHTS);
    }

    // component order, so a light keeps its slot while the set stays the same
    std::vector<size_t> chosen;
    for (const auto& candidate : candidates){
        chosen.push_back(candidate.second);
    }
    std::sort(chosen.begin(), chosen.end());
    return chosen;
}

void RenderSystem::selectLights(const Vector3& eye){
    std::vector<size_t> chosen = chooseLights(eye, activeLights);
    // shadow slots have their own culled instances
    if (chosen != activeLights){
        instanceViewsDirty = true;
    }
    activeLights = std::move(chosen);
}

// A view other than the main camera's picks its own lights. The main ones it keeps stay in
// their slots, with shadows and masks; the other slots go to nearer lights, without them.
void RenderSystem::applyViewLights(const Vector3& eye){
    if (activeLights.size() < (size_t)MAX_LIGHTS)
        return;

    std::vector<size_t> chosen = chooseLights(eye, activeLights);
    std::vector<size_t> nearer;
    std::set_difference(chosen.begin(), chosen.end(), activeLights.begin(), activeLights.end(), std::back_inserter(nearer));
    size_t next = 0;
    for (int slot = 0; slot < MAX_LIGHTS && next < nearer.size(); slot++){
        if (!std::binary_search(chosen.begin(), chosen.end(), activeLights[slot])){
            setLightSlot(slot, nearer[next++]);
        }
    }
}

void RenderSystem::setLightSlot(int slot, size_t index){
    auto lights = scene->getComponentArray<LightComponent>();
    const LightComponent& light = lights->getComponentFromIndex(index);
    Transform* transform = scene->findComponent<Transform>(lights->getEntity(index));
    const Vector3 worldPosition = transform ? transform->worldPosition : Vector3::ZERO;

    int type = 0;
    if (light.type == LightType::POINT)
        type = 1;
    if (light.type == LightType::SPOT)
        type = 2;

    fs_lighting.direction_range[slot] = Vector4(light.worldDirection.x, light.worldDirection.y, light.worldDirection.z, light.range);
    fs_lighting.color_intensity[slot] = Vector4(light.color.x, light.color.y, light.color.z, light.intensity);
    fs_lighting.position_type[slot] = Vector4(worldPosition.x, worldPosition.y, worldPosition.z, (float)type);
    fs_lighting.inCon_ouCon_shadows_cascades[slot] = Vector4(light.innerConeCos, light.outerConeCos, light.shadowMapIndex, light.numShadowCascades);
    fs_lighting.spotUp_maskAspect[slot] = Vector4(
        light.worldUp.x,
        light.worldUp.y,
        light.worldUp.z,
        (slot < MAX_LIGHTS && light.type == LightType::SPOT && light.spotMaskReady) ? light.spotMaskAspect : 0.0f);
}

void RenderSystem::checkLightsAndShadow(){
    hasLights = false;
    hasShadows = false;

    auto lights = scene->getComponentArray<LightComponent>();

    if (scene->getLightState() == LightState::AUTO){
        if (lights->size() > 0)
            hasLights = true;
    }else if (scene->getLightState() == LightState::ON){
        hasLights = true;
    }

    for (size_t i = 0; i < lights->size(); i++){
        LightComponent& light = lights->getComponentFromIndex(i);
        if (light.shadows){
            hasShadows = true;
        }
    }

    // 2D lights are gated only by their own component count, NOT by
    // scene->getLightState(): the editor forces LightState::OFF in 2D scenes
    // to keep 3D lights out of them, but 2D lights must still work there.
    hasLights2D = false;
    hasShadows2D = false;

    auto lights2d = scene->getComponentArray<Light2DComponent>();
    numLights2D = lights2d->size();
    if (numLights2D > MAX_LIGHTS_2D)
        numLights2D = MAX_LIGHTS_2D;

    if (numLights2D > 0)
        hasLights2D = true;
}

bool RenderSystem::loadLights(int numLights){
    int freeProjectiveSlot = 0;
    int freePointSlot = 0;
    unsigned int maxProjectiveSlotResolution = 0;
    unsigned int maxPointSlotResolution = 0;

    auto lights = scene->getComponentArray<LightComponent>();

    // every light, so the shadow passes skip the unselected ones
    for (size_t i = 0; i < lights->size(); i++){
        lights->getComponentFromIndex(i).shadowMapIndex = -1;
    }

    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(activeLights[i]);
        
        if (light.shadows){
            if (light.type == LightType::DIRECTIONAL &&
                (light.numShadowCascades < 1 || light.numShadowCascades > MAX_SHADOWCASCADES)){
                unsigned int requestedCascades = light.numShadowCascades;
                light.numShadowCascades = std::clamp(
                    light.numShadowCascades, 1u, (unsigned int)MAX_SHADOWCASCADES);
                Log::warn("Shadow cascades clamped from %u to %u", requestedCascades, light.numShadowCascades);
            }

            if (light.needUpdateShadowMap){
                needUpdateShadowAtlas = true;
                needUpdateShadowPointAtlas = true;
                shadowAtlasContentDirty = true;
                shadowPointAtlasContentDirty = true;
                needUpdateShadowBindings = true;
                light.needUpdateShadowMap = false;
            }
        }
    }

    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(activeLights[i]);
        if (!light.shadows) continue;

        if (light.type == LightType::SPOT){
            if (freeProjectiveSlot < MAX_SHADOW_ATLAS_SLOTS){
                light.shadowMapIndex = freeProjectiveSlot;
                freeProjectiveSlot++;
                maxProjectiveSlotResolution = std::max(maxProjectiveSlotResolution, light.mapResolution);
            }
        }else if (light.type == LightType::DIRECTIONAL){
            if ((freeProjectiveSlot + light.numShadowCascades) <= MAX_SHADOW_ATLAS_SLOTS){
                light.shadowMapIndex = freeProjectiveSlot;
                freeProjectiveSlot += light.numShadowCascades;
                maxProjectiveSlotResolution = std::max(maxProjectiveSlotResolution, light.mapResolution);
            }
        }
    }

    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(activeLights[i]);
        if (!light.shadows || light.type != LightType::POINT) continue;

        if ((freePointSlot + SHADOW_CUBE_FACES) <= MAX_POINT_SHADOW_ATLAS_SLOTS){
            light.shadowMapIndex = freePointSlot;
            freePointSlot += SHADOW_CUBE_FACES;
            maxPointSlotResolution = std::max(maxPointSlotResolution, light.mapResolution);
        }
    }

    if (freeProjectiveSlot > 0){
        const int physicalPageBudget = std::min(
            MAX_SHADOW_ATLAS_SLOTS,
            freeProjectiveSlot * SHADOW_VIRTUAL_PAGES_PER_MAP);
        if (!ensureShadowAtlas(maxProjectiveSlotResolution, physicalPageBudget)){
            Log::warn("Failed to create projective shadow atlas");
            for (int i = 0; i < numLights; i++){
                LightComponent& light = lights->getComponentFromIndex(activeLights[i]);
                if (light.shadows && light.type != LightType::POINT){
                    light.shadowMapIndex = -1;
                }
            }
        }
    }else{
        if (hasShadowAtlas || shadowAtlasFramebuffer.isCreated()){
            needUpdateShadowBindings = true;
        }
        shadowAtlasFramebuffer.destroyFramebuffer();
        shadowAtlasSlotResolution = 0;
        shadowAtlasCols = 0;
        shadowAtlasRows = 0;
        shadowAtlasUsedSlots = 0;
        shadowAtlasContentDirty = true;
        initShadowAtlasRects();
        hasShadowAtlas = false;
    }

    rebuildShadowPageMapping(hasShadowAtlas ? shadowAtlasUsedSlots : 0);

    if (freePointSlot > 0){
        if (!ensureShadowPointAtlas(maxPointSlotResolution)){
            Log::warn("Failed to create point shadow atlas");
            for (int i = 0; i < numLights; i++){
                LightComponent& light = lights->getComponentFromIndex(activeLights[i]);
                if (light.shadows && light.type == LightType::POINT){
                    light.shadowMapIndex = -1;
                }
            }
        }
    }else{
        if (hasShadowPointAtlas || shadowPointAtlasFramebuffer.isCreated()){
            needUpdateShadowBindings = true;
        }
        shadowPointAtlasFramebuffer.destroyFramebuffer();
        shadowPointAtlasSlotResolution = 0;
        shadowPointAtlasContentDirty = true;
        hasShadowPointAtlas = false;
    }

    return true;
}

void RenderSystem::updateSpotMaskAtlas(int numLights){
    auto lights = scene->getComponentArray<LightComponent>();
    const int maskLights = std::min(numLights, MAX_LIGHTS);
    std::array<SpotMaskAtlasEntry, MAX_LIGHTS> newEntries;
    std::array<TextureData*, MAX_LIGHTS> sources{};
    std::array<std::shared_ptr<std::array<TextureData, 6>>, MAX_LIGHTS> sourceOwners;
    std::array<bool, MAX_LIGHTS> previousReady{};
    std::array<float, MAX_LIGHTS> previousAspects{};
    std::array<int, MAX_LIGHTS> committedSlots;
    committedSlots.fill(-1);

    for (int i = 0; i < MAX_LIGHTS; i++){
        if (i >= maskLights){
            continue;
        }

        LightComponent& light = lights->getComponentFromIndex(activeLights[i]);
        previousReady[i] = light.spotMaskReady;
        previousAspects[i] = light.spotMaskAspect;

        if (light.type == LightType::SPOT && !light.spotMask.empty() && !light.spotMask.isFramebuffer()){
            const std::string sourceId = light.spotMask.getId();

            auto acceptSource = [&](const std::shared_ptr<std::array<TextureData, 6>>& owner){
                if (!owner){
                    return false;
                }

                TextureData& source = owner->at(0);
                if (!source.getData() || source.getWidth() <= 0 || source.getHeight() <= 0 || source.getChannels() <= 0){
                    return false;
                }

                sourceOwners[i] = owner;
                sources[i] = &sourceOwners[i]->at(0);
                newEntries[i].sourceId = sourceId;
                newEntries[i].sourceOwner = owner.get();
                newEntries[i].sourcePixels = source.getData();
                newEntries[i].width = source.getWidth();
                newEntries[i].height = source.getHeight();
                newEntries[i].aspect = std::clamp(
                    static_cast<float>(source.getWidth()) / static_cast<float>(source.getHeight()),
                    0.1f, 10.0f);
                return true;
            };

            std::shared_ptr<std::array<TextureData, 6>> pooledSource = TextureDataPool::get(sourceId);
            bool hasSource = acceptSource(pooledSource);

            if (!hasSource){
                // from any slot, as the light selection can move a light to another one
                for (int j = 0; j < MAX_LIGHTS; j++){
                    const SpotMaskAtlasEntry& committedEntry = spotMaskAtlasEntries[j];
                    if (spotMaskAtlasCreated &&
                        pooledSource &&
                        pooledSource.get() == committedEntry.sourceOwner &&
                        sourceId == committedEntry.sourceId &&
                        !committedEntry.empty()){
                        committedSlots[i] = j;
                        break;
                    }
                }

                if (committedSlots[i] >= 0){
                    newEntries[i] = spotMaskAtlasEntries[committedSlots[i]];
                }else{
                    TextureLoadResult result = light.spotMask.load();
                    if (result.state == ResourceLoadState::Finished){
                        acceptSource(result.data);
                    }
                }
            }
        }
    }

    auto applyCommittedReadiness = [&](){
        for (int i = 0; i < maskLights; i++){
            LightComponent& light = lights->getComponentFromIndex(activeLights[i]);
            const bool ready =
                spotMaskAtlasCreated &&
                !newEntries[i].empty() &&
                newEntries[i] == spotMaskAtlasEntries[i];

            light.spotMaskReady = ready;
            light.spotMaskAspect = ready ? spotMaskAtlasEntries[i].aspect : 1.0f;

            if (previousReady[i] != light.spotMaskReady ||
                std::abs(previousAspects[i] - light.spotMaskAspect) > 0.0001f){
                light.needUpdateShadowCamera = true;
            }
        }

        for (size_t i = 0; i < lights->size(); i++){
            if (std::binary_search(activeLights.begin(), activeLights.end(), i))
                continue;
            LightComponent& light = lights->getComponentFromIndex(i);
            if (light.spotMaskReady || std::abs(light.spotMaskAspect - 1.0f) > 0.0001f){
                light.spotMaskReady = false;
                light.spotMaskAspect = 1.0f;
                light.needUpdateShadowCamera = true;
            }
        }
    };

    if (spotMaskAtlasCreated && newEntries == spotMaskAtlasEntries){
        applyCommittedReadiness();
        return;
    }
    if (!Engine::isViewLoaded()){
        applyCommittedReadiness();
        return;
    }

    const bool hasSpotMasks = std::any_of(
        newEntries.begin(),
        newEntries.end(),
        [](const SpotMaskAtlasEntry& entry){ return !entry.empty(); });
    if (!hasSpotMasks){
        if (spotMaskAtlasCreated){
            spotMaskAtlas.destroyTexture();
            spotMaskAtlasCreated = false;
        }
        spotMaskAtlasEntries.fill({});
        spotMaskAtlasPixels.clear();
        applyCommittedReadiness();
        return;
    }

    const int atlasWidth = SPOT_MASK_TILE_SIZE * MAX_LIGHTS;
    const size_t atlasSize =
        static_cast<size_t>(atlasWidth) * static_cast<size_t>(SPOT_MASK_TILE_SIZE);
    std::vector<unsigned char> newPixels = spotMaskAtlasPixels;
    const bool rebuildAllTiles = newPixels.size() != atlasSize;
    if (rebuildAllTiles){
        newPixels.assign(atlasSize, 255);
    }

    for (int i = 0; i < MAX_LIGHTS; i++){
        if (sources[i] &&
            (rebuildAllTiles || !spotMaskAtlasCreated || !(newEntries[i] == spotMaskAtlasEntries[i]))){
            rasterizeSpotMaskTile(*sources[i], i, atlasWidth, newPixels);
        }else if (committedSlots[i] >= 0 && committedSlots[i] != i){
            for (int y = 0; y < SPOT_MASK_TILE_SIZE; y++){
                const size_t row = static_cast<size_t>(y) * static_cast<size_t>(atlasWidth);
                std::copy_n(
                    spotMaskAtlasPixels.begin() + row + static_cast<size_t>(committedSlots[i] * SPOT_MASK_TILE_SIZE),
                    SPOT_MASK_TILE_SIZE,
                    newPixels.begin() + row + static_cast<size_t>(i * SPOT_MASK_TILE_SIZE));
            }
        }else if (newEntries[i].empty() &&
                  (rebuildAllTiles || !spotMaskAtlasEntries[i].empty())){
            const int tileStart = i * SPOT_MASK_TILE_SIZE;
            for (int y = 0; y < SPOT_MASK_TILE_SIZE; y++){
                auto row = newPixels.begin() +
                    static_cast<size_t>(y) * static_cast<size_t>(atlasWidth) +
                    static_cast<size_t>(tileStart);
                std::fill(row, row + SPOT_MASK_TILE_SIZE, 255);
            }
        }
    }

    void* data[6] = {};
    size_t size[6] = {};
    data[0] = newPixels.data();
    size[0] = newPixels.size();

    TextureRender newAtlas;
    const bool newAtlasCreated = newAtlas.createTexture(
        "spot-mask-atlas",
        atlasWidth,
        SPOT_MASK_TILE_SIZE,
        ColorFormat::RED,
        TextureType::TEXTURE_2D,
        1,
        data,
        size,
        TextureFilter::LINEAR,
        TextureFilter::LINEAR,
        TextureWrap::CLAMP_TO_EDGE,
        TextureWrap::CLAMP_TO_EDGE);

    if (!newAtlasCreated){
        newAtlas.destroyTexture();
        applyCommittedReadiness();
        return;
    }

    if (spotMaskAtlasCreated){
        spotMaskAtlas.destroyTexture();
    }
    spotMaskAtlas = newAtlas;
    spotMaskAtlasCreated = true;
    spotMaskAtlasPixels = std::move(newPixels);
    spotMaskAtlasEntries = newEntries;
    applyCommittedReadiness();

    // The atlas owns its resampled copy, so honor the source Texture's normal
    // release-after-upload policy instead of retaining full-resolution pixels.
    for (int i = 0; i < maskLights; i++){
        LightComponent& light = lights->getComponentFromIndex(activeLights[i]);
        if (sourceOwners[i] && light.spotMask.isReleaseDataAfterLoad()){
            sourceOwners[i]->at(0).releaseImageData();
            spotMaskAtlasEntries[i].sourcePixels = nullptr;
        }
    }
}

void RenderSystem::loadSpotMaskTexture(ShaderData& shaderData, ObjectRender& render){
    render.addTexture(
        shaderData.getTextureIndex(TextureShaderType::SPOTMASKATLAS),
        ShaderStageType::FRAGMENT,
        spotMaskAtlasCreated ? &spotMaskAtlas : &emptyWhite);
}

void RenderSystem::processLights(int numLights, CameraComponent& camera, Transform& cameraTransform){
    auto lights = scene->getComponentArray<LightComponent>();

    for (int i = 0; i < numLights; i++){
        LightComponent& light = lights->getComponentFromIndex(activeLights[i]);

        if (light.shadows){
            if (light.shadowMapIndex >= 0){
                size_t numMaps = 1;
                if (light.type == LightType::DIRECTIONAL){
                    numMaps = light.numShadowCascades;
                }else if (light.type == LightType::POINT){
                    numMaps = SHADOW_CUBE_FACES;
                }

                for (int c = 0; c < (int)numMaps; c++){
                    if (light.type == LightType::POINT){
                        fs_point_shadows.bias_texSize_nearFar[light.shadowMapIndex+c] = Vector4(
                            light.shadowBias,
                            (float)shadowPointAtlasSlotResolution,
                            light.cameras[c].nearFar.x,
                            light.cameras[c].nearFar.y);
                    }else{
                        const int logicalPage = light.shadowMapIndex + c;
                        if (logicalPage < 0 || logicalPage >= MAX_SHADOW_ATLAS_SLOTS)
                            continue;
                        shadowProjection.logicalLightViewProjectionMatrix[logicalPage] =
                            light.cameras[c].lightViewProjectionMatrix;

                        for (int tile = 0; tile < SHADOW_VIRTUAL_PAGES_PER_MAP; tile++){
                            const int virtualPage = shadowVirtualPageIndex(logicalPage, tile);
                            const int physicalPage = (virtualPage >= 0 && virtualPage < MAX_SHADOW_VIRTUAL_PAGES) ?
                                shadowVirtualToPhysical[virtualPage] : -1;
                            if (physicalPage < 0 || physicalPage >= MAX_SHADOW_ATLAS_SLOTS)
                                continue;

                            const LightCamera tileCamera = shadowTileCamera(light, c, tile);
                            shadowProjection.lightViewProjectionMatrix[physicalPage] =
                                tileCamera.lightViewProjectionMatrix;

                            float projScale = std::max(std::abs(tileCamera.lightProjectionMatrix[0][0]),
                                                       std::abs(tileCamera.lightProjectionMatrix[1][1]));
                            float texelSizeWorld = (projScale > 0.0f) ?
                                2.0f / (projScale * (float)shadowAtlasSlotResolution) : 0.0f;
                            shadowProjection.shadowParams[physicalPage] =
                                Vector4(light.shadowNormalBias * texelSizeWorld, 0.0, 0.0, 0.0);

                            fs_shadows.bias_texSize_nearFar[physicalPage] = Vector4(
                                light.shadowBias,
                                (float)shadowAtlasSlotResolution,
                                light.cameras[c].nearFar.x,
                                light.cameras[c].nearFar.y);
                        }
                    }
                }
            }
            // shadowMapIndex < 0 means the atlas had no free slot for this light this
            // frame: it is passed to the shader (inCon_ouCon_shadows_cascades.z) which
            // treats a negative index as "no shadow". The light keeps its shadow intent
            // and reclaims a slot automatically once one frees up.
        }

        setLightSlot(i, activeLights[i]);
    }

    // PCF tap radius of the 3D shadow filter, packed into cameraDir.w
    // (kernel = (2*radius+1)^2 taps; uniform-driven, no shader variant)
    float pcfRadius = 1.0f;
    switch (scene->getShadowQuality()){
        case ShadowQuality::NONE:   pcfRadius = 0.0f; break;
        case ShadowQuality::LOW:    pcfRadius = 1.0f; break;
        case ShadowQuality::MEDIUM: pcfRadius = 2.0f; break;
        case ShadowQuality::HIGH:   pcfRadius = 3.0f; break;
    }

    fs_lighting.eyePos = Vector4(cameraTransform.worldPosition.x, cameraTransform.worldPosition.y, cameraTransform.worldPosition.z, 0.0);
    fs_lighting.cameraDir = Vector4(camera.worldDirection.x, camera.worldDirection.y, camera.worldDirection.z, pcfRadius);
    fs_lighting.globalIllum = Vector4(scene->getGlobalIlluminationColorLinear(), scene->getGlobalIlluminationIntensity());

    fs_lighting.envColor = Vector4(1.0, 1.0, 1.0, 0.0);
    if (hasIBL){
        auto skys = scene->getComponentArray<SkyComponent>();
        if (skys->size() > 0){
            SkyComponent& sky = skys->getComponentFromIndex(0);
            // The same intensity that scales the constant ambient has to scale the sky
            // cubemap too: ibl.glsl multiplies both the Lambertian and the GGX lookup by
            // envColor.rgb, so leaving it out made "Global Illumination Intensity" a no-op
            // for every scene with a sky, and a bright sky could not be dimmed at all.
            float giIntensity = scene->getGlobalIlluminationIntensity();
            Vector3 linColor = Color::sRGBToLinear(Vector3(sky.color.x, sky.color.y, sky.color.z));
            fs_lighting.envColor = Vector4(linColor.x * giIntensity, linColor.y * giIntensity,
                                           linColor.z * giIntensity, Angle::defaultToRad(sky.rotation));
        }
    }

    // inverse viewport size, used by USE_SSAO to convert gl_FragCoord to a screen UV.
    // .z = SSAO debug flag (output raw AO); .w = rendering-flipped flag (the AO buffer
    // is in logical orientation, so flip Y on sample when the color pass is flipped).
    float vpW = Engine::getViewRect().getWidth();
    float vpH = Engine::getViewRect().getHeight();
    if (isFixedResolutionActive() && !camera.renderToTexture){
        // the main color pass renders into the fixed-resolution target
        vpW = (float)scene->getFixedResolutionWidth();
        vpH = (float)scene->getFixedResolutionHeight();
    }
    fs_lighting.viewportInfo = Vector4(
        (vpW > 0.0f) ? 1.0f / vpW : 0.0f,
        (vpH > 0.0f) ? 1.0f / vpH : 0.0f,
        scene->isSSAODebug() ? 1.0f : 0.0f,
        isRenderingFlipped(camera) ? 1.0f : 0.0f);
    fs_lighting.clusterInfo = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    fs_lighting.clusterDepth = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    fs_lighting.clusterDebug = Vector4(0.0f, 0.0f, 0.0f, 0.0f);

    // Setting intensity of other lights to zero
    for (int i = numLights; i < MAX_FORWARD_LIGHTS; i++){
        fs_lighting.color_intensity[i].w = 0.0;
        fs_lighting.spotUp_maskAspect[i].w = 0.0;
    }
}

void RenderSystem::updateClusteredLightGrid(CameraComponent& camera){
    clusteredLightGridReady = false;
    fs_lighting.clusterInfo = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    fs_lighting.clusterDepth = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    fs_lighting.clusterDebug = Vector4(0.0f, 0.0f, 0.0f, 0.0f);

    // OpenGL 4.1 and GLES3 have no fragment-stage storage buffers. Their shader
    // variants keep the original six-light UBO path, while the other backends
    // use this grid.
#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    return;
#endif
    if (!Engine::isViewLoaded() || activeLights.empty())
        return;

    float viewportWidth = Engine::getViewRect().getWidth();
    float viewportHeight = Engine::getViewRect().getHeight();
    if (isFixedResolutionActive() && !camera.renderToTexture){
        viewportWidth = (float)scene->getFixedResolutionWidth();
        viewportHeight = (float)scene->getFixedResolutionHeight();
    }
    if (viewportWidth <= 0.0f || viewportHeight <= 0.0f)
        return;

    // The grid depends only on the camera projection/pose, viewport and the
    // light volumes. Avoid touching the storage buffer on a completely static
    // frame; this also keeps sokol's one-update-per-buffer-per-frame contract
    // quiet when the editor redraws the same scene repeatedly.
    uint64_t gridKey = 1469598103934665603ULL;
    auto hashValue = [&gridKey](uint64_t value){
        gridKey ^= value;
        gridKey *= 1099511628211ULL;
    };
    auto hashFloat = [&hashValue](float value){
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        hashValue(bits);
    };
    hashFloat(viewportWidth);
    hashFloat(viewportHeight);
    hashValue(camera.renderToTexture ? 1ULL : 0ULL);
    hashFloat(camera.nearClip);
    hashFloat(camera.farClip);
    for (int row = 0; row < 4; row++){
        for (int column = 0; column < 4; column++)
            hashFloat(camera.viewProjectionMatrix[row][column]);
    }

    auto lights = scene->getComponentArray<LightComponent>();
    hashValue(static_cast<uint64_t>(activeLights.size()));
    for (size_t i = 0; i < activeLights.size(); i++){
        const size_t lightIndex = activeLights[i];
        const LightComponent& light = lights->getComponentFromIndex(lightIndex);
        hashValue(static_cast<uint64_t>(lightIndex));
        hashValue(static_cast<uint64_t>(light.type));
        hashFloat(light.range);
        hashFloat(light.intensity);
        hashFloat(light.worldDirection.x);
        hashFloat(light.worldDirection.y);
        hashFloat(light.worldDirection.z);
        Transform* transform = scene->findComponent<Transform>(lights->getEntity(lightIndex));
        if (transform){
            hashFloat(transform->worldPosition.x);
            hashFloat(transform->worldPosition.y);
            hashFloat(transform->worldPosition.z);
        }else{
            hashValue(0ULL);
            hashValue(0ULL);
            hashValue(0ULL);
        }
    }

    if (clusteredLightGridReady && clusteredLightGridKey == gridKey && clusteredLightGridBuffer.isCreated()){
        clusteredLightGridReady = true;
        fs_lighting.clusterInfo = clusteredLightGridInfo;
        fs_lighting.clusterDepth = clusteredLightGridDepth;
        fs_lighting.clusterDebug = Vector4(showClusteredLightGrid ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // Keep the worst-case grid bounded at very high resolutions. The actual tile
    // size is passed in the UBO, so 4K does not allocate hundreds of megabytes.
    const unsigned int tileSize = std::max((unsigned int)CLUSTER_TILE_SIZE,
        (unsigned int)std::ceil(std::max(viewportWidth, viewportHeight) / 96.0f));
    const unsigned int gridWidth = std::max(1u, (unsigned int)std::ceil(viewportWidth / (float)tileSize));
    const unsigned int gridHeight = std::max(1u, (unsigned int)std::ceil(viewportHeight / (float)tileSize));
    const unsigned int depthSlices = (unsigned int)CLUSTER_Z_SLICES;
    const unsigned int stride = 1u + (unsigned int)MAX_CLUSTER_LIGHTS_PER_TILE;
    const unsigned int clusterCount = gridWidth * gridHeight * depthSlices;
    const size_t wordCount = static_cast<size_t>(clusterCount) * stride;
    clusteredLightGridData.assign(wordCount, 0u);

    auto appendLight = [&](unsigned int tile, unsigned int lightIndex){
        uint32_t* entry = clusteredLightGridData.data() + static_cast<size_t>(tile) * stride;
        const unsigned int count = std::min(entry[0], (uint32_t)MAX_CLUSTER_LIGHTS_PER_TILE);
        for (unsigned int i = 0; i < count; i++){
            if (entry[1u + i] == lightIndex)
                return;
        }
        if (count < (unsigned int)MAX_CLUSTER_LIGHTS_PER_TILE){
            entry[1u + count] = lightIndex;
            entry[0] = count + 1u;
        }
    };

    auto addAllTiles = [&](unsigned int lightIndex){
        for (unsigned int cluster = 0; cluster < clusterCount; cluster++)
            appendLight(cluster, lightIndex);
    };

    const float nearDepth = std::max(std::abs(camera.nearClip), 0.001f);
    const float farDepth = std::max(std::abs(camera.farClip), nearDepth + 0.001f);
    const float logDepthRange = std::max(std::log2(farDepth / nearDepth), 0.001f);
    auto depthSlice = [&](float depth){
        const float normalized = std::log2(std::max(depth, nearDepth) / nearDepth) / logDepthRange;
        return std::clamp((int)std::floor(normalized * (float)depthSlices), 0, (int)depthSlices - 1);
    };

    const bool renderingFlipped = isRenderingFlipped(camera);
    auto project = [&](const Vector3& position, float& pixelX, float& pixelY, float& clipW){
        const Vector4 clip = camera.viewProjectionMatrix * Vector4(position.x, position.y, position.z, 1.0f);
        clipW = clip.w;
        if (clip.w <= 0.0001f)
            return false;
        float ndcX = clip.x / clip.w;
        float ndcY = clip.y / clip.w;
        if (renderingFlipped)
            ndcY = -ndcY;
        pixelX = (ndcX * 0.5f + 0.5f) * viewportWidth;
        pixelY = (ndcY * 0.5f + 0.5f) * viewportHeight;
        return true;
    };

    for (unsigned int slot = 0; slot < activeLights.size(); slot++){
        const LightComponent& light = lights->getComponentFromIndex(activeLights[slot]);
        if (light.type == LightType::DIRECTIONAL){
            addAllTiles(slot);
            continue;
        }

        // In glTF, a non-positive range means an unlimited punctual light. It
        // cannot be conservatively represented by a finite screen-space sphere.
        if (light.range <= 0.0f){
            addAllTiles(slot);
            continue;
        }

        Transform* transform = scene->findComponent<Transform>(lights->getEntity(activeLights[slot]));
        const Vector3 position = transform ? transform->worldPosition : Vector3::ZERO;
        float centerX = 0.0f;
        float centerY = 0.0f;
        float clipW = 0.0f;
        if (!project(position, centerX, centerY, clipW)){
            // A sphere crossing the near plane is conservative when included everywhere;
            // a fully-behind light is harmless because its range attenuation is zero.
            addAllTiles(slot);
            continue;
        }

        const Vector4 viewPosition = camera.viewMatrix * Vector4(position.x, position.y, position.z, 1.0f);
        const float viewDepth = std::max(std::abs(viewPosition.z), 0.001f);
        const float projectionScale = std::max(std::abs(camera.projectionMatrix[0][0]),
                                               std::abs(camera.projectionMatrix[1][1]));
        const float radiusNdc = light.range * projectionScale / viewDepth;
        const float radiusPixels = 0.5f * radiusNdc * std::max(viewportWidth, viewportHeight) + (float)tileSize;

        int minX = (int)std::floor((centerX - radiusPixels) / (float)tileSize);
        int maxX = (int)std::floor((centerX + radiusPixels) / (float)tileSize);
        int minY = (int)std::floor((centerY - radiusPixels) / (float)tileSize);
        int maxY = (int)std::floor((centerY + radiusPixels) / (float)tileSize);
        minX = std::max(0, minX);
        minY = std::max(0, minY);
        maxX = std::min((int)gridWidth - 1, maxX);
        maxY = std::min((int)gridHeight - 1, maxY);
        const float minDepth = std::max(nearDepth, viewDepth - light.range);
        const float maxDepth = std::min(farDepth, viewDepth + light.range);
        if (maxDepth < nearDepth || minDepth > farDepth)
            continue;
        const int minZ = depthSlice(minDepth);
        const int maxZ = depthSlice(maxDepth);
        for (int z = minZ; z <= maxZ; z++){
            for (int y = minY; y <= maxY; y++){
                for (int x = minX; x <= maxX; x++){
                    const unsigned int cluster = ((unsigned int)z * gridHeight + (unsigned int)y) * gridWidth + (unsigned int)x;
                    appendLight(cluster, slot);
                }
            }
        }
    }

    const unsigned int byteSize = static_cast<unsigned int>(clusteredLightGridData.size() * sizeof(uint32_t));
    if (clusteredLightGridBuffer.isCreated()){
        if (clusteredLightGridWidth != gridWidth || clusteredLightGridHeight != gridHeight){
            clusteredLightGridBuffer.destroyBuffer();
        }
    }
    bool uploaded = true;
    if (!clusteredLightGridBuffer.isCreated()){
        uploaded = clusteredLightGridBuffer.createBuffer(byteSize, clusteredLightGridData.data(),
                                                         BufferType::STORAGE_BUFFER, BufferUsage::STREAM);
    }else{
        clusteredLightGridBuffer.updateBuffer(byteSize, clusteredLightGridData.data());
    }
    if (!uploaded)
        return;

    clusteredLightGridWidth = gridWidth;
    clusteredLightGridHeight = gridHeight;
    clusteredLightGridDepthSlices = depthSlices;
    clusteredLightGridReady = true;
    clusteredLightGridKey = gridKey;
    clusteredLightGridInfo = Vector4((float)tileSize, (float)gridWidth,
                                     (float)gridHeight, (float)depthSlices);
    clusteredLightGridDepth = Vector4(nearDepth, farDepth, logDepthRange, 1.0f);
    fs_lighting.clusterInfo = Vector4((float)tileSize, (float)gridWidth,
                                      (float)gridHeight, (float)depthSlices);
    fs_lighting.clusterDepth = Vector4(nearDepth, farDepth, logDepthRange, 1.0f);
    fs_lighting.clusterDebug = Vector4(showClusteredLightGrid ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
}

bool RenderSystem::loadLights2D(){
    bool prevHasShadows2D = hasShadows2D;
    hasShadows2D = false;

    auto lights2d = scene->getComponentArray<Light2DComponent>();
    auto occluders = scene->getComponentArray<Occluder2DComponent>();

    bool anyOccluder = false;
    for (int i = 0; i < occluders->size(); i++){
        if (occluders->getComponentFromIndex(i).enabled){
            anyOccluder = true;
            break;
        }
    }

    int freeRow = 0;
    unsigned int maxResolution = 0;
    for (int i = 0; i < numLights2D; i++){
        Light2DComponent& light = lights2d->getComponentFromIndex(i);

        light.shadowMapIndex = -1;

        if (light.shadows && anyOccluder){
            light.shadowMapIndex = freeRow++;
            maxResolution = std::max(maxResolution, light.mapResolution);
        }
    }

    hasShadows2D = (freeRow > 0);

    if (hasShadows2D){
        if (!ensureShadow2DAtlas(maxResolution)){
            Log::warn("Failed to create 2D shadow atlas");
            for (int i = 0; i < numLights2D; i++){
                lights2d->getComponentFromIndex(i).shadowMapIndex = -1;
            }
            hasShadows2D = false;
        }
    }

    if (!hasShadows2D){
        if (hasShadow2DAtlas){
            needUpdateShadowBindings = true;
        }
        hasShadow2DAtlas = false;
    }

    // USE_SHADOWS_2D is baked into mesh shader variants, so a flip (first shadow
    // light + occluder appearing, or the last one going away) rebuilds meshes
    if (hasShadows2D != prevHasShadows2D){
        needReloadMeshes();
    }

    return true;
}

bool RenderSystem::ensureShadow2DAtlas(unsigned int width){
    int maxTextureSize = sg_query_limits().max_image_size_2d;
    if (maxTextureSize < 1){
        maxTextureSize = 4096;
    }
    if (width < 1){
        width = 1;
    }
    if ((int)width > maxTextureSize){
        Log::warn("2D shadow atlas width clamped from %u to %d (GPU max texture size)", width, maxTextureSize);
        width = (unsigned int)maxTextureSize;
    }

    if (shadow2DAtlasFramebuffer.isCreated() && shadow2DAtlasWidth == width){
        if (!hasShadow2DAtlas){
            needUpdateShadowBindings = true;
        }
        hasShadow2DAtlas = true;
        return true;
    }

    shadow2DAtlasFramebuffer.destroyFramebuffer();
    if (!shadow2DAtlasFramebuffer.createFramebuffer(
            TextureType::TEXTURE_2D, (int)width, MAX_LIGHTS_2D,
            TextureFilter::NEAREST, TextureFilter::NEAREST,
            TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, true)){
        hasShadow2DAtlas = false;
        return false;
    }

    shadow2DAtlasWidth = width;
    hasShadow2DAtlas = true;
    needUpdateShadowBindings = true;
    return true;
}

void RenderSystem::processLights2D(){
    auto lights2d = scene->getComponentArray<Light2DComponent>();

    for (int i = 0; i < numLights2D; i++){
        Light2DComponent& light = lights2d->getComponentFromIndex(i);
        Entity entity = lights2d->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);

        Vector3 worldPosition;
        if (transform){
            worldPosition = Vector3(transform->worldPosition);
        }

        fs_lighting2d.position_range[i] = Vector4(worldPosition.x, worldPosition.y, light.height, std::max(light.range, 0.001f));
        fs_lighting2d.color_intensity[i] = Vector4(light.color.x, light.color.y, light.color.z, light.intensity);
        fs_lighting2d.falloff_shadow[i] = Vector4(light.falloff, (float)light.shadowMapIndex, light.shadowSoftness, light.shadowBias);
    }

    Vector3 ambient = scene->getAmbientLight2DColorLinear() * scene->getAmbientLight2DIntensity();
    fs_lighting2d.ambient = Vector4(ambient.x, ambient.y, ambient.z, (float)numLights2D);

    // PCF tap radius from the scene's 2D shadow quality (taps = 2*radius + 1)
    float pcfRadius = 2.0f;
    switch (scene->getShadow2DQuality()){
        case ShadowQuality::NONE:   pcfRadius = 0.0f; break;
        case ShadowQuality::LOW:    pcfRadius = 2.0f; break;
        case ShadowQuality::MEDIUM: pcfRadius = 4.0f; break;
        case ShadowQuality::HIGH:   pcfRadius = 6.0f; break;
    }

    // rows without shadows keep shadowMapIndex -1 and never sample the atlas
    float atlasW = (hasShadow2DAtlas && shadow2DAtlasWidth > 0) ? (float)shadow2DAtlasWidth : 1.0f;
    fs_lighting2d.atlasInfo = Vector4(1.0f / atlasW, 1.0f / (float)MAX_LIGHTS_2D, atlasW, pcfRadius);
}

bool RenderSystem::loadAndProcessFog(){

    FogComponent* fog = disableFog ? nullptr : scene->findComponentFromIndex<FogComponent>(0);
    hasFog = false;

    // The analytic pass integrates the same density along the whole view ray and reaches the
    // skybox as well, which mesh.frag never does, so leaving the per-material term on would fog
    // every surface twice. Handing aerial perspective to the pass is a variant change.
    const bool analyticFog = fog && fog->heightFog;
    if (analyticFog != hasHeightFog){
        hasHeightFog = analyticFog;
        needReloadMeshes();
    }

    if (fog && !analyticFog){
        hasFog = true;

        int fogTypeI;
        if (fog->type == FogType::LINEAR){
            fogTypeI = 0;
        }else if (fog->type == FogType::EXPONENTIAL){
            fogTypeI = 1;
        }else if (fog->type == FogType::EXPONENTIALSQUARED){
            fogTypeI = 2;
        }

        fs_fog.color_type = Vector4(fog->color.x, fog->color.y, fog->color.z, fogTypeI);
        fs_fog.density_start_end = Vector4(fog->density, 0.0, fog->linearStart, fog->linearEnd);
    }

    return hasFog;
}

// pooled maps stay alive in TexturePool for reuse; only unpooled ones are destroyed
void RenderSystem::releaseSkyEnvironment(SkyComponent& sky){
    if (sky.irradianceMap && sky.irradianceMap.use_count() == 1){
        sky.irradianceMap->destroyTexture();
    }
    if (sky.prefilteredMap && sky.prefilteredMap.use_count() == 1){
        sky.prefilteredMap->destroyTexture();
    }
    sky.irradianceMap.reset();
    sky.prefilteredMap.reset();
    sky.envMapsLoaded = false;
}

// generates (or regenerates) the IBL environment maps from the sky texture,
// must run while the texture pixels are still on CPU (before the sky GPU upload)
void RenderSystem::updateSkyEnvironment(SkyComponent& sky){
    if (!sky.needUpdateEnvironment)
        return;

    if (sky.texture.empty() || sky.texture.getNumFaces() != 6){
        releaseSkyEnvironment(sky);
        sky.needUpdateEnvironment = false;
        return;
    }

    // generation is expensive (cosine + GGX convolution), reuse cached maps
    const std::string texId = sky.texture.getId();
    const std::string irradianceId = texId + "|ibl|irradiance";
    const std::string prefilteredId = texId + "|ibl|prefiltered";

    if (!texId.empty()){
        std::shared_ptr<TextureRender> irradiance = TexturePool::get(irradianceId);
        std::shared_ptr<TextureRender> prefiltered = TexturePool::get(prefilteredId);
        if (irradiance && prefiltered){
            // A hidden sky never clears needUpdateTexture, so this is requested every frame
            if (sky.irradianceMap != irradiance || sky.prefilteredMap != prefiltered){
                releaseSkyEnvironment(sky);
                sky.irradianceMap = irradiance;
                sky.prefilteredMap = prefiltered;
                sky.envMapsLoaded = true;
                needReloadMeshes();
            }
            sky.needUpdateEnvironment = false;
            return;
        }
    }

    TextureLoadResult texResult = sky.texture.load();

    if (texResult.state == ResourceLoadState::Loading){
        return; // try again next frame
    }

    if (texResult.state == ResourceLoadState::Finished && texResult.data){
        releaseSkyEnvironment(sky);

        std::shared_ptr<TextureRender> irradiance = std::make_shared<TextureRender>();
        std::shared_ptr<TextureRender> prefiltered = std::make_shared<TextureRender>();

        if (IBLEnvironment::generate(texId, *(texResult.data), *irradiance, *prefiltered)){
            sky.irradianceMap = irradiance;
            sky.prefilteredMap = prefiltered;
            sky.envMapsLoaded = true;
            TexturePool::add(irradianceId, irradiance);
            TexturePool::add(prefilteredId, prefiltered);
            needReloadMeshes();
        }
    }

    sky.needUpdateEnvironment = false;
}

void RenderSystem::releaseReflectionProbeMaps(ReflectionProbeRuntime& runtime){
    if (runtime.irradianceMap && runtime.irradianceMap.use_count() == 1)
        runtime.irradianceMap->destroyTexture();
    if (runtime.prefilteredMap && runtime.prefilteredMap.use_count() == 1)
        runtime.prefilteredMap->destroyTexture();
    runtime.irradianceMap.reset();
    runtime.prefilteredMap.reset();
}

void RenderSystem::destroyReflectionProbe(Entity entity, ReflectionProbeComponent& probe){
    auto it = reflectionProbeRuntimes.find(entity);
    if (it != reflectionProbeRuntimes.end()){
        it->second->captureFramebuffer.destroyFramebuffer();
        releaseReflectionProbeMaps(*it->second);
        reflectionProbeRuntimes.erase(it);
    }
    probe.texture.destroy();
    if (activeReflectionProbe == entity)
        activeReflectionProbe = NULL_ENTITY;
}

void RenderSystem::updateReflectionProbes(double dt){
    auto probes = scene->getComponentArray<ReflectionProbeComponent>();
    bool newHasReflectionProbes = probes->size() > 0;
    if (newHasReflectionProbes != hasReflectionProbes){
        hasReflectionProbes = newHasReflectionProbes;
        needReloadMeshes();
    }

    activeReflectionProbe = NULL_ENTITY;

    // Finish an already-started cube before scheduling another probe. This keeps
    // the one-face-per-frame budget without interleaving two partial cubemaps.
    for (int i = 0; i < probes->size(); i++){
        Entity entity = probes->getEntity(i);
        auto runtimeIt = reflectionProbeRuntimes.find(entity);
        if (runtimeIt != reflectionProbeRuntimes.end() && runtimeIt->second->captureInProgress &&
            probes->getComponentFromIndex(i).needUpdate && scene->findComponent<Transform>(entity)){
            activeReflectionProbe = entity;
            break;
        }
    }

    for (int i = 0; i < probes->size(); i++){
        Entity entity = probes->getEntity(i);
        ReflectionProbeComponent& probe = probes->getComponentFromIndex(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform)
            continue;

        auto& runtimePtr = reflectionProbeRuntimes[entity];
        if (!runtimePtr)
            runtimePtr = std::make_unique<ReflectionProbeRuntime>();
        ReflectionProbeRuntime& runtime = *runtimePtr;

        if (!runtime.modeInitialized){
            runtime.lastMode = probe.mode;
            runtime.modeInitialized = true;
        }else if (runtime.lastMode != probe.mode){
            if (activeReflectionProbe == entity)
                activeReflectionProbe = NULL_ENTITY;
            runtime.lastMode = probe.mode;
            runtime.captureInProgress = false;
            runtime.nextFace = 0;
            runtime.ready = false;
            runtime.elapsed = 0.0f;
            runtime.retryDelay = 0.0f;
            probe.needUpdate = true;
        }

        // Capture-affecting editor changes can arrive while a six-face job is
        // already running. Restart that job so no face uses the old settings.
        if (runtime.observedCaptureRevision != probe.captureRevision){
            if (activeReflectionProbe == entity)
                activeReflectionProbe = NULL_ENTITY;
            runtime.observedCaptureRevision = probe.captureRevision;
            runtime.captureInProgress = false;
            runtime.nextFace = 0;
        }

        auto scheduleCapture = [&](){
            if (activeReflectionProbe != NULL_ENTITY && activeReflectionProbe != entity)
                return;
            if (!runtime.captureInProgress){
                runtime.captureInProgress = true;
                // A finished cube keeps being sampled while its faces are
                // rewritten. Only the first capture leaves the probe unselectable.
                if (!runtime.captureFramebuffer.isCreated())
                    runtime.ready = false;
                runtime.nextFace = 0;
                // Freeze the entity origin for all six faces. boxOffset controls
                // only the influence volume and is intentionally not a capture offset.
                runtime.capturePosition = transform->worldPosition;
            }
            activeReflectionProbe = entity;
        };

        if (probe.mode == ReflectionProbeMode::STATIC){
            bool hasAuthoredCubemap = !probe.texture.empty() && probe.texture.getNumFaces() == 6;
            if (hasAuthoredCubemap && runtime.ready)
                runtime.capturedPosition = transform->worldPosition;

            if (!probe.needUpdate)
                continue;

            if (!Engine::isViewLoaded())
                continue;

            // An authored cubemap is the preferred static path because it can be
            // GGX-prefiltered once and cached. With no cubemap, capture the scene
            // once on load (or when Refresh is pressed) and then keep that result.
            if (!hasAuthoredCubemap){
                runtime.retryDelay = 0.0f;
                releaseReflectionProbeMaps(runtime);
                scheduleCapture();
                continue;
            }

            if (activeReflectionProbe == entity)
                activeReflectionProbe = NULL_ENTITY;
            runtime.captureInProgress = false;
            runtime.captureFramebuffer.destroyFramebuffer();
            runtime.resolution = 0;
            runtime.nextFace = 0;
            runtime.ready = false;
            releaseReflectionProbeMaps(runtime);

            const std::string textureId = probe.texture.getId();
            const std::string irradianceId = textureId + "|probe|irradiance";
            const std::string prefilteredId = textureId + "|probe|prefiltered";
            if (!textureId.empty()){
                runtime.irradianceMap = TexturePool::get(irradianceId);
                runtime.prefilteredMap = TexturePool::get(prefilteredId);
                if (runtime.irradianceMap && runtime.prefilteredMap){
                    runtime.ready = true;
                    runtime.capturedPosition = transform->worldPosition;
                    runtime.retryDelay = 0.0f;
                    probe.needUpdate = false;
                    continue;
                }
            }

            runtime.retryDelay = std::max(0.0f, runtime.retryDelay - (float)dt);
            if (runtime.retryDelay > 0.0f)
                continue;

            TextureLoadResult loadResult = probe.texture.load();
            if (loadResult.state == ResourceLoadState::Loading)
                continue;

            if (loadResult.state == ResourceLoadState::Finished && loadResult.data){
                auto irradiance = std::make_shared<TextureRender>();
                auto prefiltered = std::make_shared<TextureRender>();
                if (IBLEnvironment::generate(textureId, *loadResult.data, *irradiance, *prefiltered)){
                    runtime.irradianceMap = irradiance;
                    runtime.prefilteredMap = prefiltered;
                    runtime.ready = true;
                    runtime.capturedPosition = transform->worldPosition;
                    runtime.retryDelay = 0.0f;
                    probe.needUpdate = false;
                    if (!textureId.empty()){
                        TexturePool::add(irradianceId, irradiance);
                        TexturePool::add(prefilteredId, prefiltered);
                    }
                    continue;
                }
            }

            // Texture::load disables its own load latch after a failure. Re-arm
            // it and back off so a transient authored-bake failure is recoverable
            // without logging/retrying on every frame.
            if (loadResult.state == ResourceLoadState::Failed)
                probe.texture.retryLoad();
            runtime.retryDelay = 1.0f;
            continue;
        }

        // A component switched from an authored static probe to dynamic must not
        // keep selecting the stale prefiltered map while its live capture updates.
        releaseReflectionProbeMaps(runtime);
        runtime.retryDelay = 0.0f;
        runtime.elapsed += (float)dt;
        Vector3 capturePosition = transform->worldPosition;
        if (probe.updateMode == ReflectionProbeUpdateMode::ON_MOVE && runtime.ready){
            if ((capturePosition - runtime.capturedPosition).length() > 0.0001f)
                probe.needUpdate = true;
        }else if (probe.updateMode == ReflectionProbeUpdateMode::INTERVAL && runtime.ready){
            if (runtime.elapsed >= std::max(probe.updateInterval, 0.01f))
                probe.needUpdate = true;
        }

        if (probe.needUpdate)
            scheduleCapture();
    }
}

bool RenderSystem::selectReflectionProbe(const Vector3& worldPosition, fs_reflection_probe_t& params, TextureRender*& texture){
    params.position_weight = Vector4(0.0, 0.0, 0.0, 0.0);
    params.boxMin_intensity = Vector4(-1.0, -1.0, -1.0, 1.0);
    params.boxMax_lod = Vector4(1.0, 1.0, 1.0, 0.0);
    texture = &emptyCubeBlack;

    if (capturingReflectionProbe)
        return false;

    auto probes = scene->getComponentArray<ReflectionProbeComponent>();
    ReflectionProbeComponent* selected = nullptr;
    ReflectionProbeRuntime* selectedRuntime = nullptr;
    Vector3 selectedMin;
    Vector3 selectedMax;
    float selectedDistance = 0.0f;

    for (int i = 0; i < probes->size(); i++){
        Entity entity = probes->getEntity(i);
        ReflectionProbeComponent& probe = probes->getComponentFromIndex(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        auto runtimeIt = reflectionProbeRuntimes.find(entity);
        if (!transform || runtimeIt == reflectionProbeRuntimes.end() || !runtimeIt->second->ready)
            continue;

        Vector3 center = transform->modelMatrix * probe.boxOffset;
        Vector3 half(
            std::abs(probe.boxSize.x * transform->worldScale.x) * 0.5f,
            std::abs(probe.boxSize.y * transform->worldScale.y) * 0.5f,
            std::abs(probe.boxSize.z * transform->worldScale.z) * 0.5f);
        Vector3 boxMin = center - half;
        Vector3 boxMax = center + half;
        if (worldPosition.x < boxMin.x || worldPosition.y < boxMin.y || worldPosition.z < boxMin.z ||
            worldPosition.x > boxMax.x || worldPosition.y > boxMax.y || worldPosition.z > boxMax.z)
            continue;

        float distance = (worldPosition - center).length();
        if (!selected || probe.priority > selected->priority ||
            (probe.priority == selected->priority && distance < selectedDistance)){
            selected = &probe;
            selectedRuntime = runtimeIt->second.get();
            selectedMin = boxMin;
            selectedMax = boxMax;
            selectedDistance = distance;
        }
    }

    if (!selected || !selectedRuntime)
        return false;

    bool hasPrefilteredMap = selectedRuntime->prefilteredMap && selectedRuntime->prefilteredMap->isCreated();
    if (hasPrefilteredMap){
        texture = selectedRuntime->prefilteredMap.get();
    }else{
        if (!selectedRuntime->captureFramebuffer.isCreated())
            return false;
        texture = &selectedRuntime->captureFramebuffer.getColorTexture();
    }

    float edgeDistance = std::min(
        std::min(worldPosition.x - selectedMin.x, selectedMax.x - worldPosition.x),
        std::min(std::min(worldPosition.y - selectedMin.y, selectedMax.y - worldPosition.y),
                 std::min(worldPosition.z - selectedMin.z, selectedMax.z - worldPosition.z)));
    Vector3 selectedHalf = (selectedMax - selectedMin) * 0.5f;
    float maxBlendDistance = std::max(0.0f, std::min(selectedHalf.x, std::min(selectedHalf.y, selectedHalf.z)));
    float blendDistance = std::min(std::max(selected->blendDistance, 0.0f), maxBlendDistance);
    float weight = blendDistance > 0.0001f
        ? std::max(0.0f, std::min(1.0f, edgeDistance / blendDistance))
        : 1.0f;

    params.position_weight = Vector4(selectedRuntime->capturedPosition.x, selectedRuntime->capturedPosition.y,
                                     selectedRuntime->capturedPosition.z, weight);
    params.boxMin_intensity = Vector4(selectedMin.x, selectedMin.y, selectedMin.z, std::max(selected->intensity, 0.0f));
    params.boxMax_lod = Vector4(selectedMax.x, selectedMax.y, selectedMax.z,
        hasPrefilteredMap ? (float)(IBLEnvironment::SPECULAR_MIPS - 1) : 0.0f);
    return true;
}

void RenderSystem::renderReflectionProbeCapture(){
    if (activeReflectionProbe == NULL_ENTITY || !scene->isEntityCreated(activeReflectionProbe))
        return;

    ReflectionProbeComponent* probe = scene->findComponent<ReflectionProbeComponent>(activeReflectionProbe);
    Transform* probeTransform = scene->findComponent<Transform>(activeReflectionProbe);
    auto runtimeIt = reflectionProbeRuntimes.find(activeReflectionProbe);
    if (!probe || !probeTransform || runtimeIt == reflectionProbeRuntimes.end())
        return;

    ReflectionProbeRuntime& runtime = *runtimeIt->second;
    if (!runtime.captureInProgress || !probe->needUpdate)
        return;
    unsigned int resolution = std::max(16u, std::min(probe->resolution, 1024u));
    if (!runtime.captureFramebuffer.isCreated() || runtime.resolution != resolution){
        runtime.captureFramebuffer.destroyFramebuffer();
        if (!runtime.captureFramebuffer.createFramebuffer(TextureType::TEXTURE_CUBE, (int)resolution, (int)resolution,
                TextureFilter::LINEAR, TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, false)){
            return;
        }
        runtime.resolution = resolution;
        runtime.nextFace = 0;
        runtime.ready = false;
    }

    static const Vector3 directions[6] = {
        Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0),
        Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1)
    };
    static const Vector3 ups[6] = {
        Vector3(0, -1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1),
        Vector3(0, 0, -1), Vector3(0, -1, 0), Vector3(0, -1, 0)
    };

    int face = runtime.nextFace;
    Vector3 capturePosition = runtime.capturePosition;

    Transform captureTransform;
    captureTransform.position = capturePosition;
    captureTransform.worldPosition = capturePosition;
    captureTransform.worldScale = Vector3(1.0f, 1.0f, 1.0f);
    captureTransform.parent = NULL_ENTITY;

    CameraComponent captureCamera;
    captureCamera.type = CameraType::CAMERA_PERSPECTIVE;
    captureCamera.renderToTexture = true;
    // cube faces flip on the opposite backends from 2D RTT (see
    // isRenderingFlipped), so the matching winding is PIP_RTT_INVERT
    captureCamera.invertCulling = true;
    captureCamera.autoResize = false;
    captureCamera.useTarget = true;
    captureCamera.target = capturePosition + directions[face];
    captureCamera.up = ups[face];
    captureCamera.yfov = 1.57079632679f;
    captureCamera.aspect = 1.0f;
    captureCamera.nearClip = std::max(probe->nearClip, 0.001f);
    captureCamera.farClip = std::max(probe->farClip, captureCamera.nearClip + 0.001f);
    updateCamera(captureCamera, captureTransform);

    const fs_lighting_t mainLighting = fs_lighting;
    fs_lighting.eyePos = Vector4(capturePosition.x, capturePosition.y, capturePosition.z, 0.0f);
    fs_lighting.cameraDir = Vector4(captureCamera.worldDirection.x, captureCamera.worldDirection.y,
                                    captureCamera.worldDirection.z, mainLighting.cameraDir.w);
    applyViewLights(capturePosition);

    capturingReflectionProbe = true;
    passResolution = Vector2((float)resolution, (float)resolution);
    runtime.capturePass.setClearColor(probe->includeSky ? scene->getBackgroundColor() : Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    runtime.capturePass.startRenderPass(&runtime.captureFramebuffer, (size_t)face);

    auto skys = scene->getComponentArray<SkyComponent>();
    if (probe->includeSky && skys->size() > 0){
        SkyComponent& sky = skys->getComponentFromIndex(0);
        if (sky.visible){
            updateSkyViewProjection(sky, captureCamera);
            drawSky(sky, PIP_RTT_INVERT);
        }
    }

    auto transforms = scene->getComponentArray<Transform>();
    // Point/spot shadows are light-space and can be reused here. Directional
    // cascades are fitted to the main camera, so their coverage is a known
    // limitation until probe-specific directional shadow rendering is added.
    for (int i = 0; i < transforms->size(); i++){
        Entity entity = transforms->getEntity(i);
        if (entity == activeReflectionProbe)
            continue;
        Transform& transform = transforms->getComponentFromIndex(i);
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || !transform.visible || mesh->transparent || !mesh->renderInReflectionProbes)
            continue;

        updateMVP(i, transform, captureCamera, captureTransform);
        InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(entity);
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);
        drawMesh(entity, *mesh, transform, captureCamera, captureTransform, PIP_RTT_INVERT, instanced, terrain, tilemap, 0);
    }

    runtime.capturePass.endRenderPass();
    capturingReflectionProbe = false;
    fs_lighting = mainLighting;

    delete captureCamera.framebuffer;
    captureCamera.framebuffer = nullptr;

    runtime.nextFace++;
    if (runtime.nextFace >= 6){
        runtime.nextFace = 0;
        runtime.ready = true;
        runtime.captureInProgress = false;
        runtime.elapsed = 0.0f;
        runtime.capturedPosition = capturePosition;
        probe->needUpdate = false;
        activeReflectionProbe = NULL_ENTITY;
    }
}

void RenderSystem::initShadowAtlasRects(){
    for (int s = 0; s < MAX_SHADOW_ATLAS_SLOTS; s++){
        if (s >= shadowAtlasUsedSlots || shadowAtlasCols < 1 || shadowAtlasRows < 1){
            fs_shadows.atlasRect[s] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
            continue;
        }

        int col = s % shadowAtlasCols;
        int row = s / shadowAtlasCols;
        fs_shadows.atlasRect[s] = Vector4(
            (float)col / (float)shadowAtlasCols,
            (float)row / (float)shadowAtlasRows,
            1.0f / (float)shadowAtlasCols,
            1.0f / (float)shadowAtlasRows);
    }
}

void RenderSystem::initShadowPointAtlasRects(){
    for (int s = 0; s < MAX_POINT_SHADOW_ATLAS_SLOTS; s++){
        int col = s % SHADOW_POINT_ATLAS_COLS;
        int row = s / SHADOW_POINT_ATLAS_COLS;
        fs_point_shadows.atlasRect[s] = Vector4(
            (float)col / (float)SHADOW_POINT_ATLAS_COLS,
            (float)row / (float)SHADOW_POINT_ATLAS_ROWS,
            1.0f / (float)SHADOW_POINT_ATLAS_COLS,
            1.0f / (float)SHADOW_POINT_ATLAS_ROWS);
    }
}

// Clamps a per-slot shadow resolution so the whole atlas (slotResolution * grid) stays
// within the GPU's max 2D texture size (sg_query_limits().max_image_size_2d). The atlas
// is square-tiled, so the limiting dimension is the larger grid axis. Returns the usable
// per-slot resolution (possibly reduced, logged once when clamped), or 0 when even a
// 1px slot cannot fit. This keeps shadow allocation succeeding at lower quality instead
// of failing outright on GPUs with small texture caps (e.g. 4096/8192).
unsigned int RenderSystem::clampShadowAtlasSlotResolution(unsigned int requestedResolution, int atlasCols, int atlasRows) const{
    if (requestedResolution < 1 || atlasCols < 1 || atlasRows < 1){
        return 0;
    }

    int maxTextureSize = sg_query_limits().max_image_size_2d;
    if (maxTextureSize < 1){
        maxTextureSize = 4096;
    }

    int gridMax = std::max(atlasCols, atlasRows);
    unsigned int maxSlotResolution = (unsigned int)(maxTextureSize / gridMax);
    if (maxSlotResolution < 1){
        Log::warn("Shadow atlas grid %dx%d does not fit in GPU max texture size %d", atlasCols, atlasRows, maxTextureSize);
        return 0;
    }

    if (requestedResolution > maxSlotResolution){
        Log::warn("Shadow atlas slot resolution clamped from %u to %u (GPU max texture %d, %dx%d grid)",
                  requestedResolution, maxSlotResolution, maxTextureSize, atlasCols, atlasRows);
        return maxSlotResolution;
    }

    return requestedResolution;
}

bool RenderSystem::ensureShadowAtlas(unsigned int slotResolution, int usedSlots){
    usedSlots = std::clamp(usedSlots, 1, MAX_SHADOW_ATLAS_SLOTS);

    // Smallest grid within the 3x3 capacity; equal-area layouts prefer more
    // columns so three cascades pack into a 3x1 row.
    int atlasCols = 1;
    int atlasRows = usedSlots;
    int bestArea = MAX_SHADOW_ATLAS_SLOTS + 1;
    for (int cols = 1; cols <= SHADOW_ATLAS_COLS; cols++){
        int rows = (usedSlots + cols - 1) / cols;
        if (rows > SHADOW_ATLAS_ROWS){
            continue;
        }

        int area = cols * rows;
        if (area < bestArea || (area == bestArea && cols > atlasCols)){
            atlasCols = cols;
            atlasRows = rows;
            bestArea = area;
        }
    }

    slotResolution = clampShadowAtlasSlotResolution(slotResolution, atlasCols, atlasRows);
    if (slotResolution < 1){
        return false;
    }

    int atlasWidth = (int)slotResolution * atlasCols;
    int atlasHeight = (int)slotResolution * atlasRows;

    if (shadowAtlasFramebuffer.isCreated() &&
        shadowAtlasSlotResolution == slotResolution &&
        shadowAtlasCols == atlasCols &&
        shadowAtlasRows == atlasRows &&
        shadowAtlasUsedSlots == usedSlots &&
        !needUpdateShadowAtlas){
        if (!hasShadowAtlas){
            needUpdateShadowBindings = true;
        }
        hasShadowAtlas = true;
        return true;
    }

    shadowAtlasFramebuffer.destroyFramebuffer();
    // LINEAR on a comparison sampler is hardware PCF: each tap is bilinearly filtered
    // over 2x2 comparison results, so every quality level gets softer edges for free.
    if (!shadowAtlasFramebuffer.createDepthOnlyFramebuffer(
            atlasWidth, atlasHeight,
            TextureFilter::LINEAR, TextureFilter::LINEAR,
            TextureWrap::CLAMP_TO_BORDER, TextureWrap::CLAMP_TO_BORDER, true)){
        shadowAtlasSlotResolution = 0;
        shadowAtlasCols = 0;
        shadowAtlasRows = 0;
        shadowAtlasUsedSlots = 0;
        initShadowAtlasRects();
        hasShadowAtlas = false;
        needUpdateShadowBindings = true;
        return false;
    }

    shadowAtlasSlotResolution = slotResolution;
    shadowAtlasCols = atlasCols;
    shadowAtlasRows = atlasRows;
    shadowAtlasUsedSlots = usedSlots;
    initShadowAtlasRects();
    needUpdateShadowAtlas = false;
    shadowAtlasContentDirty = true;
    hasShadowAtlas = true;
    needUpdateShadowBindings = true;
    return true;
}

bool RenderSystem::ensureShadowPointAtlas(unsigned int slotResolution){
    slotResolution = clampShadowAtlasSlotResolution(slotResolution, SHADOW_POINT_ATLAS_COLS, SHADOW_POINT_ATLAS_ROWS);
    if (slotResolution < 1){
        return false;
    }

    int atlasWidth = (int)slotResolution * SHADOW_POINT_ATLAS_COLS;
    int atlasHeight = (int)slotResolution * SHADOW_POINT_ATLAS_ROWS;

    if (shadowPointAtlasFramebuffer.isCreated() && shadowPointAtlasSlotResolution == slotResolution && !needUpdateShadowPointAtlas){
        if (!hasShadowPointAtlas){
            needUpdateShadowBindings = true;
        }
        hasShadowPointAtlas = true;
        return true;
    }

    shadowPointAtlasFramebuffer.destroyFramebuffer();
    if (!shadowPointAtlasFramebuffer.createFramebuffer(
            TextureType::TEXTURE_2D, atlasWidth, atlasHeight,
            TextureFilter::NEAREST, TextureFilter::NEAREST,
            TextureWrap::CLAMP_TO_BORDER, TextureWrap::CLAMP_TO_BORDER, true)){
        hasShadowPointAtlas = false;
        return false;
    }

    shadowPointAtlasSlotResolution = slotResolution;
    needUpdateShadowPointAtlas = false;
    shadowPointAtlasContentDirty = true;
    hasShadowPointAtlas = true;
    needUpdateShadowBindings = true;
    return true;
}

Rect RenderSystem::getShadowAtlasSlotRect(int slotIndex) const{
    if (shadowAtlasCols < 1 || slotIndex < 0 || slotIndex >= shadowAtlasUsedSlots){
        return Rect();
    }

    int col = slotIndex % shadowAtlasCols;
    int row = slotIndex / shadowAtlasCols;
    float x = (float)(col * (int)shadowAtlasSlotResolution);
    float y = (float)(row * (int)shadowAtlasSlotResolution);
    return Rect(x, y, (float)shadowAtlasSlotResolution, (float)shadowAtlasSlotResolution);
}

Rect RenderSystem::getShadowPointAtlasSlotRect(int slotIndex) const{
    int col = slotIndex % SHADOW_POINT_ATLAS_COLS;
    int row = slotIndex / SHADOW_POINT_ATLAS_COLS;
    float x = (float)(col * (int)shadowPointAtlasSlotResolution);
    float y = (float)(row * (int)shadowPointAtlasSlotResolution);
    return Rect(x, y, (float)shadowPointAtlasSlotResolution, (float)shadowPointAtlasSlotResolution);
}

bool RenderSystem::checkPBRFrabebufferUpdate(Material& material){
    return (
        material.baseColorTexture.isFramebufferOutdated() ||
        material.metallicRoughnessTexture.isFramebufferOutdated() ||
        material.normalTexture.isFramebufferOutdated() ||
        material.occlusionTexture.isFramebufferOutdated() ||
        material.emissiveTexture.isFramebufferOutdated() ||
        material.heightTexture.isFramebufferOutdated() );
}

bool RenderSystem::checkPBRTextures(Material& material, bool receiveLights){
    bool hasBaseColor = !material.baseColorTexture.empty();

    bool hasOtherPBRTextures = false;
    if (hasLights && receiveLights) {
        hasOtherPBRTextures = !material.metallicRoughnessTexture.empty() ||
                              !material.normalTexture.empty() ||
                              !material.occlusionTexture.empty() ||
                              !material.emissiveTexture.empty() ||
                              // the height map needs the UV set compiled in, exactly
                              // like the maps it displaces
                              !material.heightTexture.empty();
    } else if (hasLights2D && receiveLights) {
        // the 2D light path samples only the normal map from the PBR set
        hasOtherPBRTextures = !material.normalTexture.empty();
    }

    return hasBaseColor || hasOtherPBRTextures;
}

bool RenderSystem::loadPBRTextures(Material& material, ShaderData& shaderData, ObjectRender& render, bool receiveLights){
    TextureRender* textureRender = NULL;
    std::pair<int, int> slotTex(-1, -1);

    textureRender = material.baseColorTexture.getRender(&emptyWhite);
    slotTex = shaderData.getTextureIndex(TextureShaderType::BASECOLOR);
    if (textureRender){
        if (!textureRender->isCreated()){
            return false;
        }
        render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
    }else{
        render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyWhite);
    }

    if (hasLights && receiveLights){
        textureRender = material.metallicRoughnessTexture.getRender(&emptyWhite);
        slotTex = shaderData.getTextureIndex(TextureShaderType::METALLICROUGHNESS);
        if (textureRender){
            if (!textureRender->isCreated()){
                return false;
            }
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
        }else{
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyWhite);
        }

        textureRender = material.occlusionTexture.getRender(&emptyWhite);
        slotTex = shaderData.getTextureIndex(TextureShaderType::OCCULSION);
        if (textureRender){
            if (!textureRender->isCreated()){
                return false;
            }
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
        }else{
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyWhite);
        }

        // A missing emissive texture must default to white (glTF: emissive =
        // emissiveFactor * emissiveTexture, absent texture = 1,1,1,1). The shader
        // unconditionally multiplies emissiveFactor by this sample for UV meshes, so a
        // black fallback would silently zero out emissiveFactor when no texture is set.
        textureRender = material.emissiveTexture.getRender(&emptyWhite);
        slotTex = shaderData.getTextureIndex(TextureShaderType::EMISSIVE);
        if (textureRender){
            if (!textureRender->isCreated()){
                return false;
            }
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
        }else{
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyWhite);
        }
    }

    // normal map is sampled by both the 3D lit path and the 2D light path (which
    // otherwise keeps the unlit shader); addTexture no-ops when the compiled
    // variant has no u_normalTexture slot
    if ((hasLights || hasLights2D) && receiveLights){
        textureRender = material.normalTexture.getRender(&emptyNormal);
        slotTex = shaderData.getTextureIndex(TextureShaderType::NORMAL);
        if (textureRender){
            if (!textureRender->isCreated()){
                return false;
            }
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
        }else{
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyNormal);
        }
    }

    if (hasLights && receiveLights){
        loadSpotMaskTexture(shaderData, render);
    }

    // HAS_HEIGHT (property bit 29) only exists on the lit path: the march needs the
    // eye position from u_fs_lighting, which an unlit variant does not declare. The
    // slot lookup returns (-1, -1) for a variant compiled without the map, so binding
    // here is a no-op for every plain material. A white fallback is the flat top of a
    // height field, which yields a zero offset while the real map is still loading.
    if (hasLights && receiveLights){
        textureRender = material.heightTexture.getRender(&emptyWhite);
        slotTex = shaderData.getTextureIndex(TextureShaderType::MATERIALHEIGHT);
        if (textureRender){
            if (!textureRender->isCreated()){
                return false;
            }
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
        }else{
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyWhite);
        }
    }

    return true;
}

void RenderSystem::updateShadowBindings(){
    if (!needUpdateShadowBindings){
        return;
    }

    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        if (!mesh.loaded || mesh.needReload){
            continue;
        }

        for (int s = 0; s < mesh.numSubmeshes; s++){
            bool has3DShadows = mesh.submeshes[s].shaderProperties & (1u << 4);  // 'Shw'
            bool has2DShadows = mesh.submeshes[s].shaderProperties & (1u << 23); // 'S2d'
            if (!has3DShadows && !has2DShadows){
                continue;
            }
            if (!mesh.submeshes[s].shader){
                continue;
            }

            ShaderData& shaderData = mesh.submeshes[s].shader.get()->shaderData;
            if (has3DShadows){
                loadShadowTextures(shaderData, mesh.submeshes[s].render, mesh.receiveLights, true);
            }
            if (has2DShadows){
                loadShadow2DTexture(shaderData, mesh.submeshes[s].render, true);
            }
        }
    }

    needUpdateShadowBindings = false;
}

void RenderSystem::loadShadowTextures(ShaderData& shaderData, ObjectRender& render, bool receiveLights, bool receiveShadows){
    std::pair<int, int> slotTex(-1, -1);

    if (hasLights && receiveLights && hasShadows && receiveShadows){
        // Bind a depth/comparison fallback when the projective atlas is absent.
        slotTex = shaderData.getTextureIndex(TextureShaderType::SHADOWATLAS);
        render.addTexture(slotTex, ShaderStageType::FRAGMENT,
                          hasShadowAtlas ? &shadowAtlasFramebuffer.getDepthTexture() : &emptyShadowDepth);

        slotTex = shaderData.getTextureIndex(TextureShaderType::SHADOWPAGETABLE);
        render.addTexture(slotTex, ShaderStageType::FRAGMENT,
                          shadowPageTableCreated ? &shadowPageTable : &emptyWhite);

        slotTex = shaderData.getTextureIndex(TextureShaderType::SHADOWPOINTATLAS);
        render.addTexture(slotTex, ShaderStageType::FRAGMENT,
                          hasShadowPointAtlas ? &shadowPointAtlasFramebuffer.getColorTexture() : &emptyWhite);
    }
}

void RenderSystem::loadShadow2DTexture(ShaderData& shaderData, ObjectRender& render, bool receiveShadows2D){
    if (receiveShadows2D){
        // same white fallback as the 3D atlases: decoded depth ~1.0 = unoccluded
        std::pair<int, int> slotTex = shaderData.getTextureIndex(TextureShaderType::SHADOW2DATLAS);
        render.addTexture(slotTex, ShaderStageType::FRAGMENT,
                          hasShadow2DAtlas ? &shadow2DAtlasFramebuffer.getColorTexture() : &emptyWhite);
    }
}

void RenderSystem::updateShadowPageTable(){
    std::array<unsigned char, MAX_SHADOW_VIRTUAL_PAGES * 4> pixels{};

    // The logical shadow index is deliberately kept stable for the shader arrays. The
    // page table is the indirection point that lets the allocator remap it to a
    // resident physical atlas slot without changing material bindings.
    auto lights = scene->getComponentArray<LightComponent>();
    for (size_t i = 0; i < activeLights.size(); i++){
        LightComponent& light = lights->getComponentFromIndex(activeLights[i]);
        if (!light.shadows || light.shadowMapIndex < 0 || light.type == LightType::POINT){
            continue;
        }

        int numPages = (light.type == LightType::DIRECTIONAL) ? light.numShadowCascades : 1;
        for (int c = 0; c < numPages; c++){
            const int logicalPage = light.shadowMapIndex + c;
            if (logicalPage < 0 || logicalPage >= MAX_SHADOW_ATLAS_SLOTS)
                continue;
            for (int tile = 0; tile < SHADOW_VIRTUAL_PAGES_PER_MAP; tile++){
                const int virtualPage = shadowVirtualPageIndex(logicalPage, tile);
                const int physicalPage = shadowVirtualToPhysical[virtualPage];
                if (physicalPage < 0 || physicalPage >= shadowAtlasUsedSlots)
                    continue;

                // R stores physicalSlot + 1 normalized to [0, 1]. G is residency.
                const size_t offset = static_cast<size_t>(virtualPage) * 4;
                pixels[offset] = static_cast<unsigned char>(std::round(
                    (static_cast<float>(physicalPage + 1) / static_cast<float>(MAX_SHADOW_ATLAS_SLOTS)) * 255.0f));
                pixels[offset + 1] = 255;
                pixels[offset + 3] = 255;
            }
        }
    }

    if (!shadowPageTableCreated){
        if (!shadowPageTable.createDynamicTexture(
                "shadow|virtual|page-table", MAX_SHADOW_VIRTUAL_PAGES, 1,
                TextureFilter::NEAREST, TextureFilter::NEAREST)){
            return;
        }
        shadowPageTableCreated = true;
        needUpdateShadowBindings = true;
    }

    if (pixels != shadowPageTablePixels){
        shadowPageTable.updateTexture(pixels.data(), pixels.size());
        shadowPageTablePixels = pixels;
    }
}

void RenderSystem::rebuildShadowPageMapping(int usedSlots){
    usedSlots = std::clamp(usedSlots, 0, MAX_SHADOW_ATLAS_SLOTS);

    std::array<int, MAX_SHADOW_VIRTUAL_PAGES> newVirtualToPhysical;
    std::array<uint64_t, MAX_SHADOW_ATLAS_SLOTS> newPhysicalPageKeys{};
    std::array<uint64_t, MAX_SHADOW_VIRTUAL_PAGES> virtualPageKeys{};
    std::array<bool, MAX_SHADOW_ATLAS_SLOTS> physicalClaimed{};
    newVirtualToPhysical.fill(-1);

    auto lights = scene->getComponentArray<LightComponent>();
    for (size_t i = 0; i < activeLights.size(); i++){
        LightComponent& light = lights->getComponentFromIndex(activeLights[i]);
        if (!light.shadows || light.type == LightType::POINT || light.shadowMapIndex < 0)
            continue;

        const int numPages = (light.type == LightType::DIRECTIONAL) ? (int)light.numShadowCascades : 1;
        const uint64_t entityKey = static_cast<uint64_t>(lights->getEntity(activeLights[i])) + 1ULL;
        for (int c = 0; c < numPages; c++){
            const int logicalPage = light.shadowMapIndex + c;
            if (logicalPage < 0 || logicalPage >= MAX_SHADOW_ATLAS_SLOTS)
                continue;
            for (int tile = 0; tile < SHADOW_VIRTUAL_PAGES_PER_MAP; tile++){
                const int virtualPage = shadowVirtualPageIndex(logicalPage, tile);
                // Cascade and tile are part of the key, so a light changing its
                // cascade count cannot inherit another page's depth data.
                virtualPageKeys[virtualPage] = (entityKey << 8) |
                    (static_cast<uint64_t>(c + 1) << 4) | static_cast<uint64_t>(tile + 1);
            }
        }
    }

    // Preserve resident pages first. Reordering lights therefore changes only the
    // page table, while new pages receive deterministic free physical slots.
    for (int virtualPage = 0; virtualPage < MAX_SHADOW_VIRTUAL_PAGES; virtualPage++){
        const uint64_t key = virtualPageKeys[virtualPage];
        if (key == 0)
            continue;
        for (int physicalPage = 0; physicalPage < usedSlots; physicalPage++){
            if (!physicalClaimed[physicalPage] && shadowPhysicalPageKeys[physicalPage] == key){
                newVirtualToPhysical[virtualPage] = physicalPage;
                newPhysicalPageKeys[physicalPage] = key;
                physicalClaimed[physicalPage] = true;
                break;
            }
        }
    }

    for (int virtualPage = 0; virtualPage < MAX_SHADOW_VIRTUAL_PAGES; virtualPage++){
        if (virtualPageKeys[virtualPage] == 0 || newVirtualToPhysical[virtualPage] >= 0)
            continue;
        for (int physicalPage = 0; physicalPage < usedSlots; physicalPage++){
            if (!physicalClaimed[physicalPage]){
                newVirtualToPhysical[virtualPage] = physicalPage;
                newPhysicalPageKeys[physicalPage] = virtualPageKeys[virtualPage];
                physicalClaimed[physicalPage] = true;
                break;
            }
        }
    }

    if (shadowVirtualToPhysical != newVirtualToPhysical || shadowPhysicalPageKeys != newPhysicalPageKeys){
        // A remap changes which camera owns a tile. The next cache write must redraw
        // every affected tile; updateShadowPageTable uploads the indirection itself.
        shadowAtlasContentDirty = true;
    }
    shadowVirtualToPhysical = newVirtualToPhysical;
    shadowPhysicalPageKeys = newPhysicalPageKeys;
}

// Merges every enabled Occluder2D into occluder2DSegments as a world-space line
// list: each vertex carries its own endpoint (POSITION) plus the segment's other
// endpoint (TEXCOORD1), which the shadow2d vertex shader needs to unwrap segments
// crossing the polar seam. Returns the vertex count.
unsigned int RenderSystem::buildOccluder2DSegments(){
    occluder2DSegments.clear();

    auto occluders = scene->getComponentArray<Occluder2DComponent>();

    std::vector<Vector2> worldPoints;
    for (int i = 0; i < occluders->size(); i++){
        Occluder2DComponent& occluder = occluders->getComponentFromIndex(i);
        if (!occluder.enabled){
            continue;
        }
        Entity entity = occluders->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !transform->visible){
            continue;
        }

        worldPoints.clear();
        bool closed = true;

        if (occluder.shape == Occluder2DShape::AUTO_QUAD){
            MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!mesh || mesh->aabb == AABB::ZERO){
                continue;
            }
            Vector3 mn = mesh->aabb.getMinimum();
            Vector3 mx = mesh->aabb.getMaximum();
            worldPoints.push_back(Vector2(mn.x, mn.y));
            worldPoints.push_back(Vector2(mx.x, mn.y));
            worldPoints.push_back(Vector2(mx.x, mx.y));
            worldPoints.push_back(Vector2(mn.x, mx.y));
        }else{
            worldPoints.assign(occluder.points.begin(), occluder.points.end());
            closed = occluder.closed;
        }

        size_t numPoints = worldPoints.size();
        if (numPoints < 2){
            continue;
        }

        for (size_t p = 0; p < numPoints; p++){
            Vector3 world = transform->modelMatrix * Vector3(worldPoints[p].x, worldPoints[p].y, 0.0f);
            worldPoints[p] = Vector2(world.x, world.y);
        }

        size_t numSegments = closed ? numPoints : (numPoints - 1);
        for (size_t s = 0; s < numSegments; s++){
            const Vector2& a = worldPoints[s];
            const Vector2& b = worldPoints[(s + 1) % numPoints];

            occluder2DSegments.push_back(a.x);
            occluder2DSegments.push_back(a.y);
            occluder2DSegments.push_back(b.x);
            occluder2DSegments.push_back(b.y);

            occluder2DSegments.push_back(b.x);
            occluder2DSegments.push_back(b.y);
            occluder2DSegments.push_back(a.x);
            occluder2DSegments.push_back(a.y);
        }
    }

    return (unsigned int)(occluder2DSegments.size() / 4);
}

bool RenderSystem::loadOccluder2DPass(unsigned int vertexCapacity){
    if (!Engine::isViewLoaded())
        return false;

    destroyOccluder2DPass(); // also called when growing the buffer capacity

    ObjectRender& render = occluder2DRender;

    render.beginLoad(PrimitiveType::LINES);

    shadow2DShader = ShaderPool::get(ShaderType::SHADOW2D, 0);
    if (!shadow2DShader->isCreated())
        return false;
    render.setShader(shadow2DShader.get());
    ShaderData& shaderData = shadow2DShader.get()->shaderData;

    shadow2DSlotParams = shaderData.getUniformBlockIndex(UniformBlockType::SHADOW2D_VS_PARAMS);

    occluder2DBuffer.clear();
    occluder2DBuffer.addAttribute(AttributeType::POSITION, 2, 0);
    occluder2DBuffer.addAttribute(AttributeType::TEXCOORD1, 2, 2 * sizeof(float));
    occluder2DBuffer.setStride(4 * sizeof(float));
    occluder2DBuffer.setRenderAttributes(true);
    occluder2DBuffer.setUsage(BufferUsage::STREAM);

    size_t bufferSize = vertexCapacity * occluder2DBuffer.getStride();
    if (bufferSize == 0)
        return false;

    occluder2DBuffer.getRender()->createBuffer(bufferSize, occluder2DBuffer.getData(), occluder2DBuffer.getType(), occluder2DBuffer.getUsage());
    for (auto const &attr : occluder2DBuffer.getAttributes()) {
        render.addAttribute(shaderData.getAttrIndex(attr.first), occluder2DBuffer.getRender(), attr.second.getElements(), attr.second.getDataType(), occluder2DBuffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
    }

    if (!render.endLoad(PIP_DEPTH, false, true, CullingMode::BACK, WindingOrder::CCW)){
        return false;
    }

    occluder2DBufferCapacity = vertexCapacity;
    occluder2DLoaded = true;
    return true;
}

void RenderSystem::destroyOccluder2DPass(){
    if (!occluder2DLoaded)
        return;

    if (shadow2DShader){
        shadow2DShader.reset();
        ShaderPool::remove(ShaderType::SHADOW2D, 0);
    }

    occluder2DRender.destroy();

    occluder2DBuffer.clearAll();
    occluder2DBuffer.getRender()->destroyBuffer();

    shadow2DSlotParams = -1;
    occluder2DBufferCapacity = 0;
    occluder2DLoaded = false;
}

bool RenderSystem::loadDepthTexture(Material& material, ShaderData& shaderData, ObjectRender& render){
    TextureRender* textureDepthRender = material.baseColorTexture.getRender(&emptyWhite);
    std::pair<int, int> slotTex = shaderData.getTextureIndex(TextureShaderType::DEPTHTEXTURE);
    if (textureDepthRender){
        if (!textureDepthRender->isCreated()){
            return false;
        }
        render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureDepthRender);
    }else{
        render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyWhite);
    }

    return true;
}

bool RenderSystem::loadGBufferTextures(Material& material, ShaderData& shaderData, ObjectRender& render){
    // base color (albedo + alpha cutout); bound only when the gbuffer variant declares it
    std::pair<int, int> slotBase = shaderData.getTextureIndex(TextureShaderType::BASECOLOR);
    if (slotBase.first != -1){
        TextureRender* t = material.baseColorTexture.getRender(&emptyWhite);
        if (!t || !t->isCreated()){
            return false;
        }
        render.addTexture(slotBase, ShaderStageType::FRAGMENT, t);
    }

    // metallic-roughness (per-pixel roughness/metallic)
    std::pair<int, int> slotMR = shaderData.getTextureIndex(TextureShaderType::METALLICROUGHNESS);
    if (slotMR.first != -1){
        TextureRender* t = material.metallicRoughnessTexture.getRender(&emptyWhite);
        if (!t || !t->isCreated()){
            return false;
        }
        render.addTexture(slotMR, ShaderStageType::FRAGMENT, t);
    }

    return true;
}

namespace {
    // The loader parses the scale back out of the suffix, so the cache key carries it
    std::string terrainSourceKey(const Texture& texture){
        const std::string path = texture.getPath(0);
        if (path.empty()){
            return std::string();
        }
        return TextureData::hasSvgExtension(path.c_str()) ? TextureData::buildSvgScalePath(path, texture.getSvgScale()) : path;
    }

    bool terrainLayerHasSurfaceMaps(const TerrainSurfaceLayer& layer){
        return !layer.occlusionTexture.empty() || !layer.roughnessTexture.empty() ||
               !layer.metallicTexture.empty() || !layer.heightTexture.empty();
    }

    // A map is read from its conventional channel when it has color, and from its only
    // channel when it is grayscale, so packed and separate sources both work
    unsigned char terrainSourceChannel(TextureData* source, size_t texel, int channel, unsigned char fallback){
        if (!source || !source->getData()){
            return fallback;
        }
        const int channels = source->getChannels();
        if (channel == 3 && channels < 4){
            return fallback;
        }
        const unsigned char* pixels = static_cast<const unsigned char*>(source->getData());
        return pixels[texel * channels + ((channels < 3) ? 0 : channel)];
    }
}

// Read here rather than through Texture, which would upload a 2D image per layer on top
// of the array. Rebuilt only when the set of source files changes.
RenderSystem::TerrainDetailArray* RenderSystem::getTerrainDetailArray(Entity entity, TerrainComponent& terrain){
    // The shader reaches all three layers of any bound blend map, so colors come in threes
    const size_t layerCount = std::min(terrain.surfaceLayers.size(), (size_t)MAX_TERRAIN_LAYERS);
    const size_t colorSlices = ((layerCount + 2) / 3) * 3;

    // One slice key per array slice: the color slices first, then the normal and packed
    // surface slices of the layers that have them
    std::vector<std::string> sliceKeys(colorSlices);
    std::array<int, MAX_TERRAIN_LAYERS> normalSlice;
    std::array<int, MAX_TERRAIN_LAYERS> surfaceSlice;
    normalSlice.fill(-1);
    surfaceSlice.fill(-1);

    bool anySource = false;
    for (size_t i = 0; i < layerCount; i++){
        const TerrainSurfaceLayer& layer = terrain.surfaceLayers[i];
        sliceKeys[i] = terrainSourceKey(layer.colorTexture);
        anySource = anySource || !sliceKeys[i].empty();

        if (!layer.pbr){
            continue;
        }
        if (!layer.normalTexture.empty()){
            normalSlice[i] = (int)sliceKeys.size();
            sliceKeys.push_back("normal|" + terrainSourceKey(layer.normalTexture));
            anySource = true;
        }
        if (terrainLayerHasSurfaceMaps(layer)){
            surfaceSlice[i] = (int)sliceKeys.size();
            sliceKeys.push_back("surface|" + terrainSourceKey(layer.occlusionTexture) + "|" + terrainSourceKey(layer.roughnessTexture) +
                                "|" + terrainSourceKey(layer.metallicTexture) + "|" + terrainSourceKey(layer.heightTexture));
            anySource = true;
        }
    }

    if (!anySource){
        // The layers are set but carry no path, so there is nothing to bake. A script that
        // assigned a texture of the wrong kind lands here with a silent flat terrain.
        Log::warn("Terrain has %d surface layers but none of them names a file", (int)layerCount);
        destroyTerrainDetailArray(entity);
        return NULL;
    }

    // One sampler covers every slice, so the first assigned layer sets it for all of them
    const Texture* sampled = &terrain.surfaceLayers[0].colorTexture;
    for (size_t i = 0; i < layerCount; i++){
        if (!terrain.surfaceLayers[i].colorTexture.empty()){
            sampled = &terrain.surfaceLayers[i].colorTexture;
            break;
        }
    }

    // Tiled surface maps alias badly at distance without mipmaps. A color-only terrain
    // keeps whatever filter its layer textures ask for, so its look is unchanged.
    TextureFilter minFilter = sampled->getMinFilter();
    if (minFilter == TextureFilter::LINEAR && hasTerrainSurfaceLayers(terrain)){
        minFilter = TextureFilter::LINEAR_MIPMAP_LINEAR;
    }

    auto it = terrainDetailArrays.find(entity);
    if (it != terrainDetailArrays.end()){
        const TerrainDetailArray& cached = it->second;
        if (cached.sliceKeys == sliceKeys && cached.minFilter == minFilter && cached.magFilter == sampled->getMagFilter() &&
            cached.wrapU == sampled->getWrapU() && cached.wrapV == sampled->getWrapV()){
            // A set that could not be built is remembered, not retried every frame
            return cached.failed ? NULL : &it->second;
        }
        destroyTerrainDetailArray(entity);
    }

    auto remember = [&](bool failed) -> TerrainDetailArray& {
        TerrainDetailArray& entry = terrainDetailArrays[entity];
        entry.sliceKeys = sliceKeys;
        entry.colorSlices = (int)colorSlices;
        entry.normalSlice = normalSlice;
        entry.surfaceSlice = surfaceSlice;
        entry.minFilter = minFilter;
        entry.magFilter = sampled->getMagFilter();
        entry.wrapU = sampled->getWrapU();
        entry.wrapV = sampled->getWrapV();
        entry.failed = failed;
        return entry;
    };

    // A map shared by several layers is read once, and the array takes the largest of them
    std::map<std::string, TextureData> sources;
    std::vector<std::string> missing;
    int width = 0;
    int height = 0;
    auto source = [&](const Texture& texture) -> TextureData* {
        const std::string key = terrainSourceKey(texture);
        if (key.empty()){
            return NULL;
        }
        auto found = sources.find(key);
        if (found == sources.end()){
            found = sources.emplace(key, TextureData()).first;
            if (!found->second.loadTextureFromFile(key.c_str())){
                missing.push_back(key);
                return NULL;
            }
            // stb hands over the pixels, and nothing else will free them
            found->second.setDataOwned(true);
            width = std::max(width, found->second.getWidth());
            height = std::max(height, found->second.getHeight());
        }
        return found->second.getData() ? &found->second : NULL;
    };

    struct SliceSources{
        enum Kind{ SLICE_COLOR, SLICE_NORMAL, SLICE_SURFACE };
        Kind kind = SLICE_COLOR;
        TextureData* color = NULL;
        TextureData* normal = NULL;
        TextureData* occlusion = NULL;
        TextureData* roughness = NULL;
        TextureData* metallic = NULL;
        TextureData* height = NULL;
    };
    std::vector<SliceSources> sliceSources(sliceKeys.size());
    for (size_t i = 0; i < layerCount; i++){
        const TerrainSurfaceLayer& layer = terrain.surfaceLayers[i];
        sliceSources[i].color = source(layer.colorTexture);
        if (normalSlice[i] >= 0){
            sliceSources[normalSlice[i]].kind = SliceSources::SLICE_NORMAL;
            sliceSources[normalSlice[i]].normal = source(layer.normalTexture);
        }
        if (surfaceSlice[i] >= 0){
            SliceSources& packed = sliceSources[surfaceSlice[i]];
            packed.kind = SliceSources::SLICE_SURFACE;
            packed.occlusion = source(layer.occlusionTexture);
            packed.roughness = source(layer.roughnessTexture);
            packed.metallic = source(layer.metallicTexture);
            packed.height = source(layer.heightTexture);
        }
    }

    if (width == 0){
        // A terrain whose layers cannot be read is drawn as if it were unpainted, so this has to
        // say so: without it the only symptom is a flat-coloured island and no trace of why.
        Log::warn("Terrain detail array has no readable layer map (%d of %d slices), first one tried: %s",
                  (int)sliceKeys.size(), (int)layerCount, missing.empty() ? "(no path)" : missing.front().c_str());
        remember(true);
        return NULL;
    }
    if (width > MAX_TERRAIN_DETAIL_SIZE || height > MAX_TERRAIN_DETAIL_SIZE){
        Log::warn("Terrain detail maps are capped at %d px, larger sources are downscaled", MAX_TERRAIN_DETAIL_SIZE);
        width = std::min(width, MAX_TERRAIN_DETAIL_SIZE);
        height = std::min(height, MAX_TERRAIN_DETAIL_SIZE);
    }

    for (auto& entry : sources){
        // A map that failed to load is kept so it is not retried, but it has nothing to resize
        if (!entry.second.getData()){
            continue;
        }
        if (entry.second.getWidth() != width || entry.second.getHeight() != height){
            entry.second.resize(width, height);
        }
    }

    // Slices share one format, and the packed ones need four channels anyway
    const size_t texels = (size_t)width * height;
    std::vector<std::vector<unsigned char>> slices(sliceKeys.size());
    std::vector<void*> data(sliceKeys.size());
    std::vector<size_t> size(sliceKeys.size());
    for (size_t s = 0; s < sliceKeys.size(); s++){
        const SliceSources& src = sliceSources[s];
        // An unassigned map falls back to the neutral value of its channel: white for
        // color and surface, and a flat normal that leaves the geometry alone
        slices[s].resize(texels * 4, 0xFF);
        for (size_t t = 0; t < texels; t++){
            unsigned char* texel = &slices[s][t * 4];
            if (src.kind == SliceSources::SLICE_NORMAL){
                texel[0] = terrainSourceChannel(src.normal, t, 0, 0x80);
                texel[1] = terrainSourceChannel(src.normal, t, 1, 0x80);
                texel[2] = terrainSourceChannel(src.normal, t, 2, 0xFF);
            }else if (src.kind == SliceSources::SLICE_SURFACE){
                texel[0] = terrainSourceChannel(src.occlusion, t, 0, 0xFF);
                texel[1] = terrainSourceChannel(src.roughness, t, 1, 0xFF);
                texel[2] = terrainSourceChannel(src.metallic, t, 2, 0xFF);
                texel[3] = terrainSourceChannel(src.height, t, 0, 0xFF);
            }else{
                texel[0] = terrainSourceChannel(src.color, t, 0, 0xFF);
                texel[1] = terrainSourceChannel(src.color, t, 1, 0xFF);
                texel[2] = terrainSourceChannel(src.color, t, 2, 0xFF);
                texel[3] = terrainSourceChannel(src.color, t, 3, 0xFF);
            }
        }
        data[s] = slices[s].data();
        size[s] = slices[s].size();
    }

    TerrainDetailArray& detail = remember(false);
    if (!detail.render.createTexture("terrain|detail|" + std::to_string(entity), width, height,
            ColorFormat::RGBA, TextureType::TEXTURE_ARRAY, (int)sliceKeys.size(), data.data(), size.data(),
            minFilter, sampled->getMagFilter(), sampled->getWrapU(), sampled->getWrapV())){
        detail.failed = true;
        Log::warn("Terrain detail array of %d slices at %dx%d could not be created; the painted layers are ignored",
                  (int)sliceKeys.size(), width, height);
        return NULL;
    }

    Log::verbose("Terrain detail array: %d slices at %dx%d, %d painted blend maps available",
                 (int)sliceKeys.size(), width, height, detail.colorSlices / 3);
    for (size_t m = 0; m < terrain.blendMaps.size(); m++){
        Log::verbose("  blend map %d: %s", (int)m, terrainSourceKey(terrain.blendMaps[m]).c_str());
    }
    return &detail;
}

void RenderSystem::destroyTerrainDetailArray(Entity entity){
    auto it = terrainDetailArrays.find(entity);
    if (it != terrainDetailArrays.end()){
        it->second.render.destroyTexture();
        terrainDetailArrays.erase(it);
    }
}

// Factors change without rebuilding the array, so the block is filled at draw time
void RenderSystem::applyTerrainLayersUniform(ObjectRender& render, int slot, Entity entity, TerrainComponent& terrain){
    if (slot == -1){
        return;
    }

    auto it = terrainDetailArrays.find(entity);
    const TerrainDetailArray* detail = (it != terrainDetailArrays.end() && !it->second.failed) ? &it->second : NULL;

    static const TerrainSurfaceLayer emptyLayer;

    fs_terrain_layers_t layers;
    for (int i = 0; i < MAX_TERRAIN_LAYERS; i++){
        const bool present = i < (int)terrain.surfaceLayers.size();
        const TerrainSurfaceLayer& layer = present ? terrain.surfaceLayers[i] : emptyLayer;
        const bool pbr = present && layer.pbr;

        layers.colorFactor[i] = Vector4(layer.colorFactor.x, layer.colorFactor.y, layer.colorFactor.z, pbr ? 1.0f : 0.0f);
        layers.uvTransform[i] = Vector4(layer.uvScale.x, layer.uvScale.y, layer.uvOffset.x, layer.uvOffset.y);
        layers.surface[i] = Vector4(layer.roughnessFactor, layer.metallicFactor, layer.normalStrength, layer.occlusionStrength);
        layers.slices[i] = Vector4((float)i,
                                   detail ? (float)detail->normalSlice[i] : -1.0f,
                                   detail ? (float)detail->surfaceSlice[i] : -1.0f, 0.0f);
    }

    render.applyUniformBlock(slot, sizeof(fs_terrain_layers_t), &layers);
}

bool RenderSystem::loadTerrainTextures(Entity entity, TerrainComponent& terrain, ObjectRender& render, ShaderData& shaderData){
    TextureRender* textureRender = NULL;
    std::pair<int, int> slotTex(-1, -1);

    // The heightmap and the blend map span the whole 0..1 range, so their borders must read
    // the border texels, not the REPEAT blend with the opposite edge (detail maps are tiled).
    terrain.heightMap.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    terrain.heightMap.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    for (Texture& blendMap : terrain.blendMaps){
        blendMap.setWrapU(TextureWrap::CLAMP_TO_EDGE);
        blendMap.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    }

    textureRender = terrain.heightMap.getRender(&emptyWhite);
    slotTex = shaderData.getTextureIndex(TextureShaderType::HEIGHTMAP);
    if (textureRender){
        if (!textureRender->isCreated()){
            return false;
        }
        render.addTexture(slotTex, ShaderStageType::VERTEX, textureRender);
    }else{
        render.addTexture(slotTex, ShaderStageType::VERTEX, &emptyWhite);
    }

    // The array comes first: how deep it came out is what decides which blend maps may
    // weight anything, and the white fallback is a single slice.
    TerrainDetailArray* detail = getTerrainDetailArray(entity, terrain);
    const bool arrayCreated = detail && detail->render.isCreated();
    slotTex = shaderData.getTextureIndex(TextureShaderType::TERRAINDETAIL);
    render.addTexture(slotTex, ShaderStageType::FRAGMENT, arrayCreated ? &detail->render : &emptyArrayWhite);

    // The rest bind black, which weights none of their layers
    const int coveredMaps = arrayCreated ? detail->colorSlices / 3 : 0;
    int dbgIndex[2 * MAX_TERRAIN_BLENDMAPS];
    bool dbgBlack[MAX_TERRAIN_BLENDMAPS];
    for (int m = 0; m < MAX_TERRAIN_BLENDMAPS; m++){
        const bool covered = m < coveredMaps && m < static_cast<int>(terrain.blendMaps.size());
        textureRender = covered ? terrain.blendMaps[m].getRender(&emptyBlack) : NULL;
        slotTex = shaderData.getTextureIndex(static_cast<TextureShaderType>(static_cast<int>(TextureShaderType::BLENDMAP) + m));
        dbgIndex[m * 2] = slotTex.first;
        dbgIndex[m * 2 + 1] = slotTex.second;
        if (textureRender){
            if (!textureRender->isCreated()){
                return false;
            }
            dbgBlack[m] = (textureRender == &emptyBlack);
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
        }else{
            dbgBlack[m] = true;
            render.addTexture(slotTex, ShaderStageType::FRAGMENT, &emptyBlack);
        }
    }

    Log::verbose("Terrain bind: array=%d covered=%d maps=%d | %d:%d black=%d, %d:%d black=%d, %d:%d black=%d",
                 (int)arrayCreated, coveredMaps, (int)terrain.blendMaps.size(),
                 dbgIndex[0], dbgIndex[1], (int)dbgBlack[0],
                 dbgIndex[2], dbgIndex[3], (int)dbgBlack[1],
                 dbgIndex[4], dbgIndex[5], (int)dbgBlack[2]);

    return true;
}

bool RenderSystem::loadTerrainHeightTexture(TerrainComponent& terrain, ObjectRender& render, ShaderData& shaderData){
    std::pair<int, int> slotTex = shaderData.getTextureIndex(TextureShaderType::HEIGHTMAP);
    if (slotTex.first == -1){
        return true;
    }

    TextureRender* textureRender = terrain.heightMap.getRender(&emptyWhite);
    if (!textureRender || !textureRender->isCreated()){
        return false;
    }

    render.addTexture(slotTex, ShaderStageType::VERTEX, textureRender);
    return true;
}

bool RenderSystem::updateTerrainRenderTextures(Entity entity, TerrainComponent& terrain, MeshComponent& mesh){
    if (!terrain.needUpdateTexture){
        return true;
    }

    bool texLoaded = true;
    for (int s = 0; s < 2; s++){
        if (s >= mesh.numSubmeshes){
            break;
        }
        if (!mesh.submeshes[s].shader){
            texLoaded = false;
            continue;
        }
        ShaderData& shaderData = mesh.submeshes[s].shader.get()->shaderData;
        if (!loadTerrainTextures(entity, terrain, mesh.submeshes[s].render, shaderData)){
            texLoaded = false;
        }
        if (mesh.submeshes[s].depthShader){
            ShaderData& depthShaderData = mesh.submeshes[s].depthShader.get()->shaderData;
            if (!loadTerrainHeightTexture(terrain, mesh.submeshes[s].depthRender, depthShaderData)){
                texLoaded = false;
            }
        }
        if (mesh.submeshes[s].gbufferShader){
            ShaderData& gbufferShaderData = mesh.submeshes[s].gbufferShader.get()->shaderData;
            const bool painted = mesh.submeshes[s].slotFSGBufferTerrainLayers != -1;
            if (painted ? !loadTerrainTextures(entity, terrain, mesh.submeshes[s].gbufferRender, gbufferShaderData)
                        : !loadTerrainHeightTexture(terrain, mesh.submeshes[s].gbufferRender, gbufferShaderData)){
                texLoaded = false;
            }
        }
    }

    if (texLoaded){
        terrain.needUpdateTexture = false;
    }

    return texLoaded;
}

void RenderSystem::updateAllTerrainRenderTextures(){
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        if (!mesh.loaded || mesh.needReload){
            continue;
        }

        Entity entity = meshes->getEntity(i);
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        if (terrain){
            updateTerrainRenderTextures(entity, *terrain, mesh);
        }
    }
}

void RenderSystem::setDisableFaceCulling(bool disableFaceCulling){
    if (this->disableFaceCulling == disableFaceCulling){
        return;
    }
    this->disableFaceCulling = disableFaceCulling;

    // Cull mode is baked into the pipeline at creation time, so every mesh must be
    // reloaded for the override to take effect (editor-only debug view).
    needReloadMeshes();
}

void RenderSystem::setDisableFog(bool disableFog){
    if (this->disableFog == disableFog){
        return;
    }
    this->disableFog = disableFog;

    // fog is a shader variant
    needReloadMeshes();
}

void RenderSystem::setDisableUnderwater(bool disableUnderwater){
    this->disableUnderwater = disableUnderwater;
}

void RenderSystem::setShowClusteredLightGrid(bool showClusteredLightGrid){
    this->showClusteredLightGrid = showClusteredLightGrid;
}

void RenderSystem::setDisableFixedResolution(bool disableFixedResolution){
    // No reload needed: the editor always renders through Engine::getFramebuffer(),
    // so the PIP_RTT pipeline is already baked whether fixed resolution is on or off.
    this->disableFixedResolution = disableFixedResolution;
}

void RenderSystem::setWaterPaused(bool waterPaused){
    this->waterPaused = waterPaused;
}

// A mesh carrying no vertex data, no index data and no external buffer has nothing the GPU could
// be given: loadMesh() can only fail on it. Such a component is a container waiting for content
// (a tilemap with no tiles yet, a mesh a script is about to fill), not a load in progress, so
// both the retry loop and the load counter have to step aside - otherwise it burns an attempt
// every 30 frames, inflates the mesh-load stats, and holds isAllLoaded() false for the session.
static bool meshHasNothingToLoad(const MeshComponent& mesh){
    return mesh.buffer.getSize() == 0 && mesh.indices.getSize() == 0 && mesh.numExternalBuffers == 0;
}

bool RenderSystem::loadMesh(Entity entity, MeshComponent& mesh, uint16_t pipelines, InstancedMeshComponent* instmesh, TerrainComponent* terrain){

    if (!Engine::isViewLoaded()) 
        return false;

    if (terrain && !terrain->heightMapLoaded)
        return false;

    if (terrain && instmesh){
        Log::warn("Terrain cannot be an InstancedMesh");
    }

    if (mesh.autoTransparency){
        // Recompute from every current submesh below. This also clears a stale
        // transparent flag when a model is reloaded with OPAQUE/MASK materials.
        mesh.transparent = false;
    }

    Engine::addMeshLoadStats();

    std::map<std::string, Buffer*> buffers;
    std::map<std::string, BufferRender*> bufferNameToRender;
    bool allBuffersEmpty = true;

    if (mesh.buffer.getSize() > 0){
        buffers["vertices"] = &mesh.buffer;
        allBuffersEmpty = false;
        if (mesh.buffer.getUsage() != BufferUsage::IMMUTABLE){
            mesh.needUpdateBuffer = true;
        }
    }
    if (mesh.indices.getSize() > 0){
        buffers["indices"] = &mesh.indices;
        allBuffersEmpty = false;
        if (mesh.indices.getUsage() != BufferUsage::IMMUTABLE){
            mesh.needUpdateBuffer = true;
        }
    }
    for (int i = 0; i < mesh.numExternalBuffers; i++){
        buffers[mesh.eBuffers[i].getName()] = &mesh.eBuffers[i];
        allBuffersEmpty = false;
        if (mesh.eBuffers[i].getUsage() != BufferUsage::IMMUTABLE){
            mesh.needUpdateBuffer = true;
        }
    }

    if (mesh.vertexCount == 0){
        mesh.vertexCount = mesh.buffer.getCount();
    }

    if (allBuffersEmpty) {
        // Multi-node glTF models use their root entity only as a hierarchy
        // container; renderable geometry lives in child MeshComponents. Treat
        // that empty root as successfully loaded so one-time preview scenes can
        // complete instead of waiting forever in RenderSystem::isAllLoaded().
        ModelComponent* model = scene->findComponent<ModelComponent>(entity);
        if (model && !model->meshNodesMapping.empty() && mesh.numSubmeshes == 0) {
            mesh.needReload = false;
            mesh.needUpdateAABB = true;
            mesh.loadCalled = true;
            SystemRender::addQueueCommand(&changeLoaded, new check_load_t{scene, entity});
            return true;
        }
        return false;
    }

    std::map<std::string, unsigned int> bufferStride;

    std::vector<BufferRender*> buffersCreatedThisLoad;
    for (auto const& buf : buffers){
        BufferRender* bufferRender = buf.second->getRender();

        // A GPU buffer keeps for life the usage it was created with, so a generator
        // that turns its buffer dynamic after the mesh was first loaded from
        // serialized data (a tilemap rebuilding its indices) needs the buffer
        // recreated — updating an immutable one is rejected by the backend.
        if (bufferRender->isCreated() && bufferRender->getCreatedUsage() != buf.second->getUsage()) {
            bufferRender->destroyBuffer();
        }

        if (!bufferRender->isCreated()) {
            if (!bufferRender->createBuffer(buf.second->getSize(), buf.second->getData(), buf.second->getType(), buf.second->getUsage())) {
                Log::error("Cannot create GPU buffer '%s' (%zu bytes) for mesh entity %u; resource pool may be exhausted",
                           buf.first.c_str(), buf.second->getSize(), entity);
                for (BufferRender* created : buffersCreatedThisLoad) {
                    created->destroyBuffer();
                }
                return false;
            }
            buffersCreatedThisLoad.push_back(bufferRender);
        }
        bufferNameToRender[buf.first] = bufferRender;
        bufferStride[buf.first] = buf.second->getStride();
    }

    if (instmesh){
        // Now buffer size is zero than it needed to be calculated
        size_t bufferSize = instmesh->maxInstances * instmesh->buffer.getStride();
        instmesh->buffer.getRender()->createBuffer(bufferSize, instmesh->buffer.getData(), instmesh->buffer.getType(), instmesh->buffer.getUsage());

        instmesh->needUpdateBuffer = true;
    }

    if (terrain){
        for (int v = 0; v < MAX_TERRAIN_VIEWS; v++){
            for (int s = 0; s < 2; s++){
                size_t bufferSize = terrain->nodes.size() * terrain->views[v].nodesbuffer[s].getStride();
                terrain->views[v].nodesbuffer[s].getRender()->createBuffer(bufferSize, terrain->views[v].nodesbuffer[s].getData(), terrain->views[v].nodesbuffer[s].getType(), terrain->views[v].nodesbuffer[s].getUsage());
            }
        }

        // using same material in both terrain submeshes
        mesh.submeshes[1].material = mesh.submeshes[0].material;
    }

    // sprites, tilemaps and polygons draw images with transparent borders
    Signature signature = scene->getSignature(entity);
    if (signature.test(scene->getComponentId<SpriteComponent>()) || signature.test(scene->getComponentId<TilemapComponent>()) || signature.test(scene->getComponentId<MeshPolygonComponent>())){
        for (int i = 0; i < mesh.numSubmeshes; i++){
            mesh.submeshes[i].material.baseColorTexture.setAlphaBorderAuto(true);
        }
    }

    for (int i = 0; i < mesh.numSubmeshes; i++){

        ObjectRender& render = mesh.submeshes[i].render;

        // These flags are derived from the current buffers/attributes below. Reset all
        // of them before rescanning: model reloads can replace and reorder submeshes,
        // so retaining a flag from the previous slot may select a shader attribute
        // that the rebuilt submesh does not provide.
        mesh.submeshes[i].hasTexCoord1 = false;
        mesh.submeshes[i].hasTexCoord2 = false;
        mesh.submeshes[i].hasNormal = false;
        mesh.submeshes[i].hasNormalMap = false;
        mesh.submeshes[i].hasTangent = false;
        mesh.submeshes[i].hasVertexColor3 = false;
        mesh.submeshes[i].hasVertexColor4 = false;
        mesh.submeshes[i].hasSkinning = false;
        mesh.submeshes[i].hasMorphTarget = false;
        mesh.submeshes[i].hasMorphNormal = false;
        mesh.submeshes[i].hasMorphTangent = false;

        render.beginLoad(mesh.submeshes[i].primitiveType);

        bool hasPBRTextures = checkPBRTextures(mesh.submeshes[i].material, mesh.receiveLights);

        for (auto const& buf : buffers){
            if (buf.second->isRenderAttributes()) {
                for (auto const &attr : buf.second->getAttributes()) {
                    if (attr.first == AttributeType::TEXCOORD1){
                        mesh.submeshes[i].hasTexCoord1 = true;
                    }
                    if (attr.first == AttributeType::TEXCOORD2){
                        mesh.submeshes[i].hasTexCoord2 = true;
                    }
                    if (attr.first == AttributeType::NORMAL){
                        mesh.submeshes[i].hasNormal = true;
                    }
                    if (attr.first == AttributeType::TANGENT){
                        mesh.submeshes[i].hasTangent = true;
                    }
                    if (attr.first == AttributeType::COLOR){
                        if (attr.second.getElements() == 3){
                            mesh.submeshes[i].hasVertexColor3 = true;
                        }else{
                            mesh.submeshes[i].hasVertexColor4 = true;
                        }
                    }
                    if (attr.first == AttributeType::BONEIDS || attr.first == AttributeType::BONEWEIGHTS){
                        mesh.submeshes[i].hasSkinning = true;
                    }
                    if (attr.first == AttributeType::MORPHTARGET0){
                        mesh.submeshes[i].hasMorphTarget = true;
                    }
                    if (attr.first == AttributeType::MORPHNORMAL0){
                        mesh.submeshes[i].hasMorphNormal = true;
                    }
                    if (attr.first == AttributeType::MORPHTANGENT0){
                        mesh.submeshes[i].hasMorphTangent = true;
                    }
                }
            }
        }
        for (auto const& attr : mesh.submeshes[i].attributes){
            if (attr.first == AttributeType::TEXCOORD1){
                mesh.submeshes[i].hasTexCoord1 = true;
            }
            if (attr.first == AttributeType::TEXCOORD2){
                mesh.submeshes[i].hasTexCoord2 = true;
            }
            if (attr.first == AttributeType::NORMAL){
                mesh.submeshes[i].hasNormal = true;
            }
            if (attr.first == AttributeType::TANGENT){
                mesh.submeshes[i].hasTangent = true;
            }
            if (attr.first == AttributeType::COLOR){
                if (attr.second.getElements() == 3){
                    mesh.submeshes[i].hasVertexColor3 = true;
                }else{
                    mesh.submeshes[i].hasVertexColor4 = true;
                }
            }
            if (attr.first == AttributeType::BONEIDS || attr.first == AttributeType::BONEWEIGHTS){
                mesh.submeshes[i].hasSkinning = true;
            }
            if (attr.first == AttributeType::MORPHTARGET0){
                mesh.submeshes[i].hasMorphTarget = true;
            }
            if (attr.first == AttributeType::MORPHNORMAL0){
                mesh.submeshes[i].hasMorphNormal = true;
            }
            if (attr.first == AttributeType::MORPHTANGENT0){
                mesh.submeshes[i].hasMorphTangent = true;
            }
        }

        if (!mesh.submeshes[i].material.normalTexture.empty()){
            mesh.submeshes[i].hasNormalMap = true;
        }

        bool p_unlit = false;
        bool p_punctual = false;
        bool p_ibl = false;
        bool p_mirror = (scene->findComponent<MirrorComponent>(entity) != NULL);
        bool p_hasTexture1 = false;
        bool p_hasTexture2 = false;
        bool p_hasNormalMap = false;
        bool p_hasNormal = false;
        bool p_hasTangent = false;
        bool p_receiveShadows = false;

        if (mesh.submeshes[i].hasTexCoord1 && hasPBRTextures){
            p_hasTexture1 = true;
        }
        // second UV set: only when both UV attributes and PBR textures are present
        // (never for terrain, which generates its UVs in the shader)
        if (mesh.submeshes[i].hasTexCoord1 && mesh.submeshes[i].hasTexCoord2 && hasPBRTextures){
            p_hasTexture2 = true;
        }
        if (terrain && (!terrain->blendMaps.empty() || hasPBRTextures)){
            p_hasTexture1 = true;
        }
        // Painted layers that bring their own surface need the wider terrain path; a
        // terrain of plain color layers keeps the cheaper one it always had.
        const bool p_terrainSurface = terrain && hasTerrainSurfaceLayers(*terrain);
        bool useIBL = (hasIBL || hasReflectionProbes) && mesh.receiveIBL;
        if ((hasLights || useIBL) && mesh.receiveLights){
            p_punctual = hasLights;
            p_ibl = useIBL;

            // a mesh without normals gets flat ones from the shader
            p_hasNormal = mesh.submeshes[i].hasNormal;
            if (p_hasNormal && mesh.submeshes[i].hasTangent){
                p_hasTangent = true;
            }
            if (mesh.submeshes[i].hasNormalMap){
                p_hasNormalMap = true;
            }
            if (hasShadows && mesh.receiveShadows){
                p_receiveShadows = true;
            }
        }else{
            p_unlit = true;
        }

        // dedicated 2D light path: composes with MATERIAL_UNLIT (sprites keep the
        // cheap unlit shader; accumulated 2D light multiplies the base color) and
        // with the lit path in mixed scenes (contributions add)
        bool useLight2D = hasLights2D && mesh.receiveLights;
        bool p_light2d = useLight2D;
        bool p_shadows2d = useLight2D && hasShadows2D && mesh.receiveShadows;
        if (useLight2D){
            p_hasNormal = mesh.submeshes[i].hasNormal;
            if (p_hasNormal && mesh.submeshes[i].hasTangent){
                p_hasTangent = true;
            }
            if (mesh.submeshes[i].hasNormalMap){
                p_hasNormalMap = true;
            }
        }

        // remember whether this submesh's lit shader applies IBL specular, so the SSR
        // G-buffer can flag those pixels for the energy-conserving composite
        mesh.submeshes[i].hasIBL = p_ibl;

        // screen-space AO modulates ambient/indirect light on lit submeshes.
        bool p_ssao = scene->isSSAOEnabled() && !p_unlit && (p_punctual || p_ibl);
        bool p_alphaMask = mesh.submeshes[i].material.alphaMode == MaterialAlphaMode::MASK;
        bool p_alphaOpaque = mesh.submeshes[i].material.alphaMode == MaterialAlphaMode::ALPHA_OPAQUE;
        // AUTO + textureShadow preserves the legacy opt-in for editor-created
        // materials; explicit glTF modes derive the depth behavior from alphaMode.
        bool p_depthAlphaMask = usesAlphaMask(mesh.submeshes[i].material, mesh.submeshes[i].textureShadow);
        bool p_depthTexture = p_depthAlphaMask && mesh.submeshes[i].hasTexCoord1 &&
            !mesh.submeshes[i].material.baseColorTexture.empty();

        // Opted in per field, not per band, so emptying the band (editor preview) neither
        // rebuilds the shader nor drops the variant from an export.
        const bool p_instanceFade = instmesh && instmesh->distanceFade;
        // Parallax displaces the sampled UVs, so it needs a lit variant that has a UV
        // set to displace. Terrain is excluded: it already displaces its own surface in
        // the vertex shader from its height map, and its fragment UVs are generated.
        const bool p_height = !p_unlit && !terrain && p_hasTexture1 &&
                              !mesh.submeshes[i].material.heightTexture.empty();
        const bool p_materialExtensions = !p_unlit &&
            hasMaterialExtensions(mesh.submeshes[i].material);
        mesh.submeshes[i].shaderProperties = ShaderPool::getMeshProperties(
                        p_unlit, p_hasTexture1, p_hasTexture2, p_punctual,
                        p_receiveShadows, p_hasNormal, p_hasNormalMap,
                        p_hasTangent, mesh.submeshes[i].hasVertexColor3, mesh.submeshes[i].hasVertexColor4, mesh.submeshes[i].hasTextureRect,
                        hasFog, mesh.submeshes[i].hasSkinning, mesh.submeshes[i].hasMorphTarget, mesh.submeshes[i].hasMorphNormal, mesh.submeshes[i].hasMorphTangent,
                        (terrain)?true:false, (instmesh)?true:false, p_ibl, p_mirror, p_ssao, p_light2d, p_shadows2d,
                        p_alphaMask, p_alphaOpaque, p_instanceFade, p_terrainSurface, p_height,
                        p_materialExtensions);
        // a user-forked main shader overrides the built-in Mesh shader; the variant
        // (#define) system, depth/gbuffer passes and bind-slots are unchanged.
        // Priority: component customShader > scene default shader > built-in (empty)
        const std::string& meshShaderSrc = mesh.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::MESH) : mesh.customShader;
        const ShaderUniformValues* inheritedUniforms = scene->getInheritedShaderUniforms(ShaderType::MESH, mesh.customShader);
        mesh.submeshes[i].customShaderId = ShaderPool::registerCustomShader(meshShaderSrc);
        // 'Dlo': built-in shader without the point/spot paths
        if (p_punctual && meshShaderSrc.empty() && onlyDirectionalLights){
            mesh.submeshes[i].shaderProperties |= 1u << 28;
        }
        mesh.submeshes[i].shader = ShaderPool::get(ShaderType::MESH, mesh.submeshes[i].shaderProperties, mesh.submeshes[i].customShaderId);
        // the depth shader feeds shadow maps (casters), the SSAO depth pre-pass, DOF's
        // CoC, the atmosphere pass and SSR's G-buffer consumers (SSR itself uses its own
        // G-buffer shader, built below). A depth fork is built even
        // without a pass, so its uniforms and build errors show up in the editor.
        // Every pass listed here draws with PIP_DEPTH: leaving DOF out would hand the
        // pre-pass an unset depth pipeline, and a failed beginDraw marks the mesh
        // needReload, so the scene would rebuild all its geometry every frame and draw none.
        const bool needPrepass = scene->isDepthPrepassEnabled();
        bool needDepthPass = (hasShadows && mesh.castShadows) || scene->isSSAOEnabled() ||
                needPrepass || hasWaterDepth || scene->isDOFEnabled() || atmosphereRequested();
        if (needDepthPass || !mesh.customDepthShader.empty()){
            mesh.submeshes[i].depthShaderProperties = ShaderPool::getDepthMeshProperties(
                p_depthTexture, mesh.submeshes[i].hasSkinning, mesh.submeshes[i].hasMorphTarget,
                mesh.submeshes[i].hasMorphNormal, mesh.submeshes[i].hasMorphTangent, (terrain)?true:false, (instmesh)?true:false,
                p_depthAlphaMask, p_instanceFade);
            mesh.submeshes[i].customDepthShaderId = ShaderPool::registerCustomShader(mesh.customDepthShader);
            mesh.submeshes[i].depthShader = ShaderPool::get(ShaderType::DEPTH, mesh.submeshes[i].depthShaderProperties, mesh.submeshes[i].customDepthShaderId);
            if (!mesh.submeshes[i].depthShader->isCreated()){
                // same fallback as the main shader below
                if (mesh.submeshes[i].customDepthShaderId != 0 &&
                    ShaderPool::isShaderBuildFailed(ShaderType::DEPTH, mesh.submeshes[i].depthShaderProperties, mesh.submeshes[i].customDepthShaderId)){
                    Log::error("Custom depth shader '%s' failed to compile; rendering '%s' with the built-in shader",
                               mesh.customDepthShader.c_str(), scene->getEntityName(entity).c_str());
                    mesh.submeshes[i].customDepthShaderId = 0;
                    mesh.submeshes[i].depthShader = ShaderPool::get(ShaderType::DEPTH, mesh.submeshes[i].depthShaderProperties, 0);
                }
                if (!mesh.submeshes[i].depthShader->isCreated())
                    return false;
            }
            // custom blocks of a forked depth shader, sharing the mesh values
            ShaderData& depthShaderData = mesh.submeshes[i].depthShader.get()->shaderData;
            mesh.submeshes[i].customVSDepthParams.resolve(depthShaderData, "u_vs_customParams");
            mesh.submeshes[i].customFSDepthParams.resolve(depthShaderData, "u_fs_customParams");
            mesh.submeshes[i].customVSDepthParams.writeValues(mesh.shaderUniforms, inheritedUniforms);
            mesh.submeshes[i].customFSDepthParams.writeValues(mesh.shaderUniforms, inheritedUniforms);
        }else if (mesh.submeshes[i].depthShader){
            // a reload keeps the handle; release it with the key it was taken with
            mesh.submeshes[i].depthShader.reset();
            ShaderPool::remove(ShaderType::DEPTH, mesh.submeshes[i].depthShaderProperties, mesh.submeshes[i].customDepthShaderId);
            mesh.submeshes[i].customDepthShaderId = 0;
        }
        // the G-buffer shader feeds the SSR geometry pass (MRT: packed depth + view-space
        // normal/roughness/metallic). Built only when SSR is enabled (it needs normals).
        if (scene->isSSREnabled()){
            bool gbufferHasBaseColorTex = !mesh.submeshes[i].material.baseColorTexture.empty();
            bool gbufferHasMRTex = !mesh.submeshes[i].material.metallicRoughnessTexture.empty();
            mesh.submeshes[i].gbufferShaderProperties = ShaderPool::getGBufferMeshProperties(
                gbufferHasBaseColorTex, mesh.submeshes[i].hasNormal, mesh.submeshes[i].hasSkinning,
                mesh.submeshes[i].hasMorphTarget, mesh.submeshes[i].hasMorphNormal, mesh.submeshes[i].hasMorphTangent,
                (terrain)?true:false, (instmesh)?true:false, gbufferHasMRTex, p_terrainSurface && mesh.submeshes[i].hasNormal);
            mesh.submeshes[i].gbufferShader = ShaderPool::get(ShaderType::GBUFFER, mesh.submeshes[i].gbufferShaderProperties);
            if (!mesh.submeshes[i].gbufferShader->isCreated())
                return false;
        }
        if (!mesh.submeshes[i].shader->isCreated()){
            // A custom shader that failed to compile falls back to the built-in shader so
            // the object stays visible and the mesh finishes loading (otherwise needReload
            // stays set and we retry forever). The fork is retried after its source changes
            // (invalidateCustomShaders clears the failure and re-flags needReload).
            if (mesh.submeshes[i].customShaderId != 0 &&
                ShaderPool::isShaderBuildFailed(ShaderType::MESH, mesh.submeshes[i].shaderProperties, mesh.submeshes[i].customShaderId)){
                Log::error("Custom shader '%s' failed to compile; rendering '%s' with the built-in shader",
                           meshShaderSrc.c_str(), scene->getEntityName(entity).c_str());
                mesh.submeshes[i].customShaderId = 0;
                mesh.submeshes[i].shader = ShaderPool::get(ShaderType::MESH, mesh.submeshes[i].shaderProperties, 0);
            }
            if (!mesh.submeshes[i].shader->isCreated())
                return false;
        }
        render.setShader(mesh.submeshes[i].shader.get());
        ShaderData& shaderData = mesh.submeshes[i].shader.get()->shaderData;

        mesh.submeshes[i].slotVSParams = shaderData.getUniformBlockIndex(UniformBlockType::PBR_VS_PARAMS);
        mesh.submeshes[i].slotFSParams = shaderData.getUniformBlockIndex(UniformBlockType::PBR_FS_PARAMS);
        if (p_materialExtensions){
            mesh.submeshes[i].slotFSMaterialExtensions = shaderData.getUniformBlockIndex(
                UniformBlockType::PBR_FS_MATERIAL_EXTENSIONS);
        }
        if (p_hasTexture2){
            mesh.submeshes[i].slotFSTexCoordSets = shaderData.getUniformBlockIndex(UniformBlockType::PBR_FS_TEXCOORDSETS);
        }
        if (p_height){
            mesh.submeshes[i].slotFSHeight = shaderData.getUniformBlockIndex(UniformBlockType::PBR_FS_HEIGHT);
        }
        if (p_instanceFade){
            mesh.submeshes[i].slotVSFade = shaderData.getUniformBlockIndex(UniformBlockType::PBR_VS_FADE);
        }
        if (hasFog){
            mesh.submeshes[i].slotFSFog = shaderData.getUniformBlockIndex(UniformBlockType::FS_FOG);
        }
        if (p_mirror){
            mesh.submeshes[i].slotFSMirror = shaderData.getUniformBlockIndex(UniformBlockType::FS_MIRROR);
        }
        if ((hasLights || useIBL) && mesh.receiveLights){
            mesh.submeshes[i].slotFSLighting = shaderData.getUniformBlockIndex(UniformBlockType::FS_LIGHTING);
            if (p_ibl){
                mesh.submeshes[i].slotFSReflectionProbe = shaderData.getUniformBlockIndex(UniformBlockType::FS_REFLECTION_PROBE);
            }
            if (p_receiveShadows){
                mesh.submeshes[i].slotVSShadows = shaderData.getUniformBlockIndex(UniformBlockType::VS_SHADOWS);
                mesh.submeshes[i].slotFSShadows = shaderData.getUniformBlockIndex(UniformBlockType::FS_SHADOWS);
                mesh.submeshes[i].slotFSShadowProjection = shaderData.getUniformBlockIndex(UniformBlockType::FS_SHADOW_PROJECTION);
                mesh.submeshes[i].slotFSPointShadows = shaderData.getUniformBlockIndex(UniformBlockType::FS_POINT_SHADOWS);
            }
        }
        if (p_light2d){
            mesh.submeshes[i].slotFSLighting2D = shaderData.getUniformBlockIndex(UniformBlockType::FS_LIGHTING2D);
        }
        if (mesh.submeshes[i].hasTextureRect){
            mesh.submeshes[i].slotVSSprite = shaderData.getUniformBlockIndex(UniformBlockType::SPRITE_VS_PARAMS);
        }
        if (mesh.submeshes[i].hasSkinning){
            mesh.submeshes[i].slotVSSkinning = shaderData.getUniformBlockIndex(UniformBlockType::VS_SKINNING);
            if (!bindSkinningResource(render, shaderData, mesh))
                return false;
        }
        if (mesh.submeshes[i].hasMorphTarget){
            mesh.submeshes[i].slotVSMorphTarget = shaderData.getUniformBlockIndex(UniformBlockType::VS_MORPHTARGET);
        }
        // custom blocks of a forked shader (none in the built-in)
        mesh.submeshes[i].customVSParams.resolve(shaderData, "u_vs_customParams");
        mesh.submeshes[i].customFSParams.resolve(shaderData, "u_fs_customParams");
        mesh.submeshes[i].customVSParams.writeValues(mesh.shaderUniforms, inheritedUniforms);
        mesh.submeshes[i].customFSParams.writeValues(mesh.shaderUniforms, inheritedUniforms);

        if (!loadPBRTextures(mesh.submeshes[i].material, shaderData, mesh.submeshes[i].render, mesh.receiveLights)){
            return false;
        }
        loadShadowTextures(shaderData, mesh.submeshes[i].render, mesh.receiveLights, p_receiveShadows);
        loadShadow2DTexture(shaderData, mesh.submeshes[i].render, p_shadows2d);

        if (p_ibl){
            auto skyArray = scene->getComponentArray<SkyComponent>();
            TextureRender* irradiance = &emptyCubeBlack;
            TextureRender* prefiltered = &emptyCubeBlack;
            if (skyArray->size() > 0){
                SkyComponent& sky = skyArray->getComponentFromIndex(0);
                if (sky.irradianceMap && sky.prefilteredMap){
                    irradiance = sky.irradianceMap.get();
                    prefiltered = sky.prefilteredMap.get();
                }
            }
            render.addTexture(shaderData.getTextureIndex(TextureShaderType::IRRADIANCEMAP), ShaderStageType::FRAGMENT, irradiance);
            render.addTexture(shaderData.getTextureIndex(TextureShaderType::PREFILTEREDMAP), ShaderStageType::FRAGMENT, prefiltered);
            render.addTexture(shaderData.getTextureIndex(TextureShaderType::REFLECTIONPROBE), ShaderStageType::FRAGMENT, &emptyCubeBlack);
        }

        if (terrain){
            mesh.submeshes[i].slotVSTerrain = shaderData.getUniformBlockIndex(UniformBlockType::TERRAIN_VS_PARAMS);
            if (p_terrainSurface){
                mesh.submeshes[i].slotFSTerrainLayers = shaderData.getUniformBlockIndex(UniformBlockType::FS_TERRAIN_LAYERS);
            }

            if (!loadTerrainTextures(entity, *terrain, mesh.submeshes[i].render, shaderData)){
                return false;
            }

            terrain->needUpdateTexture = false;

            // bind view 0 as the load-time default; per-view buffers (same layout) are
            // swapped in at draw via ObjectRender::replaceVertexBuffer
            for (auto const &attr : terrain->views[0].nodesbuffer[i].getAttributes()) {
                render.addAttribute(shaderData.getAttrIndex(attr.first), terrain->views[0].nodesbuffer[i].getRender(), attr.second.getElements(), attr.second.getDataType(), terrain->views[0].nodesbuffer[i].getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
            }
        }

        mesh.submeshes[i].needUpdateTexture = false;
        mesh.submeshes[i].needUpdateDepthTexture = false;
        mesh.submeshes[i].needUpdateGBufferTexture = false;

        const bool alphaBlend = usesAlphaBlend(mesh.submeshes[i].material);
        mesh.submeshes[i].alphaBlend = alphaBlend;
        if (mesh.autoTransparency && !mesh.transparent && alphaBlend){
            mesh.transparent = true;
        }
    
        unsigned int indexCount = 0;
        // an indexed submesh draws indexCount elements even when that count is zero;
        // only a submesh without any index data falls back to the whole vertex buffer
        bool indexed = false;

        for (auto const& buf : buffers){
            if (buf.second->isRenderAttributes()) {
                if (buf.second->getType() == BufferType::INDEX_BUFFER){
                    indexed = true;
                    indexCount = buf.second->getCount();
                    Attribute indexattr = buf.second->getAttributes()[AttributeType::INDEX];
                    render.setIndex(buf.second->getRender(), indexattr.getDataType(), indexattr.getOffset());
                }else{
                    for (auto const &attr : buf.second->getAttributes()) {
                        render.addAttribute(shaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                    }
                }
            }
        }

        for (auto const& attr : mesh.submeshes[i].attributes){
            if (bufferNameToRender.count(attr.second.getBufferName())){
                if (attr.first == AttributeType::INDEX){
                    indexed = true;
                    indexCount = attr.second.getCount();
                    render.setIndex(bufferNameToRender[attr.second.getBufferName()], attr.second.getDataType(), attr.second.getOffset());
                }else{
                    render.addAttribute(shaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                }
            }else{
                Log::error("Cannot load submesh attribute from buffer name: %s", attr.second.getBufferName().c_str());
            }
        }

        if (instmesh){
            for (auto const &attr : instmesh->buffer.getAttributes()) {
                render.addAttribute(shaderData.getAttrIndex(attr.first), instmesh->buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), instmesh->buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
            }
        }

        if (indexed){
            // An empty indexed submesh (a tilemap rect with no tile placed on it)
            // must draw nothing: falling back to the mesh vertex count would read
            // indices belonging to the other submeshes, and past the index buffer.
            mesh.submeshes[i].vertexCount = indexCount;
        }else{
            mesh.submeshes[i].vertexCount = mesh.vertexCount;
        }

        bool faceCulling = disableFaceCulling ? false : mesh.submeshes[i].faceCulling;
        if (!render.endLoad(pipelines, faceCulling, !alphaBlend,
                            mesh.cullingMode, mesh.windingOrder)){
            return false;
        }

        //----------Start depth shader---------------
        if (needDepthPass){
            ObjectRender& depthRender = mesh.submeshes[i].depthRender;

            depthRender.beginLoad(mesh.submeshes[i].primitiveType);

            depthRender.setShader(mesh.submeshes[i].depthShader.get());
            ShaderData& depthShaderData = mesh.submeshes[i].depthShader.get()->shaderData;

            mesh.submeshes[i].slotVSDepthParams = depthShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_VS_PARAMS);
            depthShaderData.getUniformBlockMembers("u_vs_depthParams", mesh.submeshes[i].sizeVSDepthParams);
            if (p_depthAlphaMask){
                mesh.submeshes[i].slotFSDepthMaterial = depthShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_FS_MATERIAL);
            }
            if (p_instanceFade){
                mesh.submeshes[i].slotVSDepthFade = depthShaderData.getUniformBlockIndex(UniformBlockType::PBR_VS_FADE);
            }

            if (mesh.submeshes[i].hasSkinning){
                mesh.submeshes[i].slotVSDepthSkinning = depthShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_VS_SKINNING);
                if (!bindSkinningResource(depthRender, depthShaderData, mesh))
                    return false;
            }
            if (mesh.submeshes[i].hasMorphTarget){
                mesh.submeshes[i].slotVSDepthMorphTarget = depthShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_VS_MORPHTARGET);
            }

            if (p_depthTexture){
                if (!loadDepthTexture(mesh.submeshes[i].material, depthShaderData, depthRender)){
                    return false;
                }
            }

            if (terrain){
                mesh.submeshes[i].slotVSDepthTerrain = depthShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_TERRAIN_VS_PARAMS);

                if (!loadTerrainHeightTexture(*terrain, depthRender, depthShaderData)){
                    return false;
                }

                // shadow depth pass always renders the main camera's selection (view 0)
                for (auto const &attr : terrain->views[0].nodesbuffer[i].getAttributes()) {
                    depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), terrain->views[0].nodesbuffer[i].getRender(), attr.second.getElements(), attr.second.getDataType(), terrain->views[0].nodesbuffer[i].getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                }
            }

            for (auto const& buf : buffers){
                if (buf.second->isRenderAttributes()) {

                    if (buf.second->getType() == BufferType::INDEX_BUFFER){
                        indexCount = buf.second->getCount();
                        Attribute indexattr = buf.second->getAttributes()[AttributeType::INDEX];
                        depthRender.setIndex(buf.second->getRender(), indexattr.getDataType(), indexattr.getOffset());
                    }else{
                        for (auto const &attr : buf.second->getAttributes()){
                            if (attr.first == AttributeType::POSITION || attr.first == AttributeType::TEXCOORD1){
                                depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                            }
                            if (mesh.submeshes[i].hasSkinning){
                                if (attr.first == AttributeType::BONEIDS || attr.first == AttributeType::BONEWEIGHTS){
                                    depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                                }
                            }
                            if (mesh.submeshes[i].hasMorphTarget){
                                if (attr.first == AttributeType::MORPHTARGET0 || attr.first == AttributeType::MORPHTARGET1 ||
                                    attr.first == AttributeType::MORPHTARGET2 || attr.first == AttributeType::MORPHTARGET3 ||
                                    attr.first == AttributeType::MORPHTARGET4 || attr.first == AttributeType::MORPHTARGET5 ||
                                    attr.first == AttributeType::MORPHTARGET6 || attr.first == AttributeType::MORPHTARGET7 ||
                                    attr.first == AttributeType::MORPHNORMAL0 || attr.first == AttributeType::MORPHNORMAL1 ||
                                    attr.first == AttributeType::MORPHNORMAL2 || attr.first == AttributeType::MORPHNORMAL3 ||
                                    attr.first == AttributeType::MORPHTANGENT0 || attr.first == AttributeType::MORPHTANGENT1){
                                    depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                                }
                            }
                        }
                    }
                }
            }

            for (auto const& attr : mesh.submeshes[i].attributes){
                if (bufferNameToRender.count(attr.second.getBufferName())){
                    if (attr.first == AttributeType::INDEX){
                        depthRender.setIndex(bufferNameToRender[attr.second.getBufferName()], attr.second.getDataType(), attr.second.getOffset());
                    }else if (attr.first == AttributeType::POSITION || attr.first == AttributeType::TEXCOORD1){
                        depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                    }
                    if (mesh.submeshes[i].hasSkinning){
                        if (attr.first == AttributeType::BONEIDS || attr.first == AttributeType::BONEWEIGHTS){
                            depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                        }
                    }
                    if (mesh.submeshes[i].hasMorphTarget){
                        if (attr.first == AttributeType::MORPHTARGET0 || attr.first == AttributeType::MORPHTARGET1 ||
                            attr.first == AttributeType::MORPHTARGET2 || attr.first == AttributeType::MORPHTARGET3 ||
                            attr.first == AttributeType::MORPHTARGET4 || attr.first == AttributeType::MORPHTARGET5 ||
                            attr.first == AttributeType::MORPHTARGET6 || attr.first == AttributeType::MORPHTARGET7 ||
                            attr.first == AttributeType::MORPHNORMAL0 || attr.first == AttributeType::MORPHNORMAL1 ||
                            attr.first == AttributeType::MORPHNORMAL2 || attr.first == AttributeType::MORPHNORMAL3 ||
                            attr.first == AttributeType::MORPHTANGENT0 || attr.first == AttributeType::MORPHTANGENT1){
                            depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                        }
                    }
                }else{
                    Log::error("Cannot load (depth) submesh attribute from buffer name: %s", attr.second.getBufferName().c_str());
                }
            }

            if (instmesh){
                for (auto const &attr : instmesh->buffer.getAttributes()) {
                    depthRender.addAttribute(depthShaderData.getAttrIndex(attr.first), instmesh->buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), instmesh->buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                }
            }

            CullingMode depthCullingMode = mesh.cullingMode;
            bool depthFaceCulling = (mesh.submeshes[i].textureShadow)? false : mesh.submeshes[i].faceCulling;

            uint16_t depthPipelines = PIP_DEPTH | PIP_SHADOW_DEPTH;
            if (needPrepass){
                if (pipelines & PIP_DEFAULT) depthPipelines |= PIP_ZPREPASS;
                if (pipelines & PIP_RTT) depthPipelines |= PIP_ZPREPASS_RTT;
            }
            // the prepass writes the depth the color pass will test, so it culls like it
            if (!depthRender.endLoad(depthPipelines, depthFaceCulling, true, depthCullingMode, mesh.windingOrder, faceCulling)){
                return false;
            }
        }
        //----------End depth shader---------------

        //----------Start G-buffer shader---------------
        if (scene->isSSREnabled()){
            ObjectRender& gbufferRender = mesh.submeshes[i].gbufferRender;

            gbufferRender.beginLoad(mesh.submeshes[i].primitiveType);

            gbufferRender.setShader(mesh.submeshes[i].gbufferShader.get());
            ShaderData& gbufferShaderData = mesh.submeshes[i].gbufferShader.get()->shaderData;

            mesh.submeshes[i].slotVSGBufferParams = gbufferShaderData.getUniformBlockIndex(UniformBlockType::GBUFFER_VS_PARAMS);
            mesh.submeshes[i].slotFSGBufferMaterial = gbufferShaderData.getUniformBlockIndex(UniformBlockType::GBUFFER_FS_MATERIAL);

            if (mesh.submeshes[i].hasSkinning){
                mesh.submeshes[i].slotVSGBufferSkinning = gbufferShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_VS_SKINNING);
                if (!bindSkinningResource(gbufferRender, gbufferShaderData, mesh))
                    return false;
            }
            if (mesh.submeshes[i].hasMorphTarget){
                mesh.submeshes[i].slotVSGBufferMorphTarget = gbufferShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_VS_MORPHTARGET);
            }

            if (!loadGBufferTextures(mesh.submeshes[i].material, gbufferShaderData, gbufferRender)){
                return false;
            }

            if (terrain){
                mesh.submeshes[i].slotVSGBufferTerrain = gbufferShaderData.getUniformBlockIndex(UniformBlockType::DEPTH_TERRAIN_VS_PARAMS);

                // The painted G-buffer variant samples the layers too, so it binds the
                // same blend maps and detail array the color pass does
                if (p_terrainSurface && mesh.submeshes[i].hasNormal){
                    mesh.submeshes[i].slotFSGBufferTerrainLayers = gbufferShaderData.getUniformBlockIndex(UniformBlockType::FS_TERRAIN_LAYERS);
                    if (!loadTerrainTextures(entity, *terrain, gbufferRender, gbufferShaderData)){
                        return false;
                    }
                }else if (!loadTerrainHeightTexture(*terrain, gbufferRender, gbufferShaderData)){
                    return false;
                }

                for (auto const &attr : terrain->views[0].nodesbuffer[i].getAttributes()) {
                    gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), terrain->views[0].nodesbuffer[i].getRender(), attr.second.getElements(), attr.second.getDataType(), terrain->views[0].nodesbuffer[i].getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                }
            }

            unsigned int gbufferIndexCount = 0;
            for (auto const& buf : buffers){
                if (buf.second->isRenderAttributes()) {
                    if (buf.second->getType() == BufferType::INDEX_BUFFER){
                        gbufferIndexCount = buf.second->getCount();
                        Attribute indexattr = buf.second->getAttributes()[AttributeType::INDEX];
                        gbufferRender.setIndex(buf.second->getRender(), indexattr.getDataType(), indexattr.getOffset());
                    }else{
                        for (auto const &attr : buf.second->getAttributes()){
                            if (attr.first == AttributeType::POSITION || attr.first == AttributeType::TEXCOORD1 || attr.first == AttributeType::NORMAL){
                                gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                            }
                            if (mesh.submeshes[i].hasSkinning){
                                if (attr.first == AttributeType::BONEIDS || attr.first == AttributeType::BONEWEIGHTS){
                                    gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                                }
                            }
                            if (mesh.submeshes[i].hasMorphTarget){
                                if (attr.first == AttributeType::MORPHTARGET0 || attr.first == AttributeType::MORPHTARGET1 ||
                                    attr.first == AttributeType::MORPHTARGET2 || attr.first == AttributeType::MORPHTARGET3 ||
                                    attr.first == AttributeType::MORPHTARGET4 || attr.first == AttributeType::MORPHTARGET5 ||
                                    attr.first == AttributeType::MORPHTARGET6 || attr.first == AttributeType::MORPHTARGET7 ||
                                    attr.first == AttributeType::MORPHNORMAL0 || attr.first == AttributeType::MORPHNORMAL1 ||
                                    attr.first == AttributeType::MORPHNORMAL2 || attr.first == AttributeType::MORPHNORMAL3 ||
                                    attr.first == AttributeType::MORPHTANGENT0 || attr.first == AttributeType::MORPHTANGENT1){
                                    gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), buf.second->getRender(), attr.second.getElements(), attr.second.getDataType(), buf.second->getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                                }
                            }
                        }
                    }
                }
            }

            for (auto const& attr : mesh.submeshes[i].attributes){
                if (bufferNameToRender.count(attr.second.getBufferName())){
                    if (attr.first == AttributeType::INDEX){
                        gbufferRender.setIndex(bufferNameToRender[attr.second.getBufferName()], attr.second.getDataType(), attr.second.getOffset());
                    }else if (attr.first == AttributeType::POSITION || attr.first == AttributeType::TEXCOORD1 || attr.first == AttributeType::NORMAL){
                        gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                    }
                    if (mesh.submeshes[i].hasSkinning){
                        if (attr.first == AttributeType::BONEIDS || attr.first == AttributeType::BONEWEIGHTS){
                            gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                        }
                    }
                    if (mesh.submeshes[i].hasMorphTarget){
                        if (attr.first == AttributeType::MORPHTARGET0 || attr.first == AttributeType::MORPHTARGET1 ||
                            attr.first == AttributeType::MORPHTARGET2 || attr.first == AttributeType::MORPHTARGET3 ||
                            attr.first == AttributeType::MORPHTARGET4 || attr.first == AttributeType::MORPHTARGET5 ||
                            attr.first == AttributeType::MORPHTARGET6 || attr.first == AttributeType::MORPHTARGET7 ||
                            attr.first == AttributeType::MORPHNORMAL0 || attr.first == AttributeType::MORPHNORMAL1 ||
                            attr.first == AttributeType::MORPHNORMAL2 || attr.first == AttributeType::MORPHNORMAL3 ||
                            attr.first == AttributeType::MORPHTANGENT0 || attr.first == AttributeType::MORPHTANGENT1){
                            gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), bufferNameToRender[attr.second.getBufferName()], attr.second.getElements(), attr.second.getDataType(), bufferStride[attr.second.getBufferName()], attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                        }
                    }
                }
            }

            if (instmesh){
                for (auto const &attr : instmesh->buffer.getAttributes()) {
                    gbufferRender.addAttribute(gbufferShaderData.getAttrIndex(attr.first), instmesh->buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), instmesh->buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
                }
            }

            CullingMode gbufferCullingMode = mesh.cullingMode;
            bool gbufferFaceCulling = (mesh.submeshes[i].textureShadow)? false : mesh.submeshes[i].faceCulling;

            if (!gbufferRender.endLoad(PIP_GBUFFER, gbufferFaceCulling, true, gbufferCullingMode, mesh.windingOrder)){
                return false;
            }
        }
        //----------End G-buffer shader---------------
    }

    mesh.needReload = false;
    mesh.needUpdateShaderUniforms = false;
    mesh.needUpdateAABB = true;
    mesh.loadCalled = true;

    // A reload recreates the terrain's per-view CDLOD node buffers empty; force a
    // re-selection next frame (the main-camera cut at view 0 is otherwise only
    // re-run on camera/transform movement), so the terrain doesn't vanish until
    // the camera moves after a settings change such as toggling SSAO.
    if (terrain){
        if (Transform* terrainTransform = scene->findComponent<Transform>(entity)){
            terrainTransform->needUpdate = true;
        }
    }

    updateMeshLods(entity, mesh, true);
    if (instmesh){
        instanceViewsDirty = true;
    }

    SystemRender::addQueueCommand(&changeLoaded, new check_load_t{scene, entity});

    return true;
}

// Levels come from MeshLodPool asynchronously; a load rehashes the geometry so a mesh
// that changed never draws stale levels. A mesh that should not have levels drops them
void RenderSystem::updateMeshLods(Entity entity, MeshComponent& mesh, bool rehash){
    if (!scene->isMeshLodEnabled() || !mesh.lodEnabled || scene->findComponent<TerrainComponent>(entity) || scene->findComponent<TilemapComponent>(entity)){
        releaseMeshLods(mesh);
        return;
    }

    std::map<std::string, Buffer*> buffers;
    if (mesh.buffer.getSize() > 0)
        buffers["vertices"] = &mesh.buffer;
    if (mesh.indices.getSize() > 0)
        buffers["indices"] = &mesh.indices;
    for (int i = 0; i < mesh.numExternalBuffers; i++)
        buffers[mesh.eBuffers[i].getName()] = &mesh.eBuffers[i];

    for (unsigned int i = 0; i < mesh.numSubmeshes; i++){
        Submesh& submesh = mesh.submeshes[i];

        if (!rehash && submesh.lodKey != 0){
            pollMeshLod(mesh, submesh);
            continue;
        }

        LodAttributes attributes;
        if (!findLodAttributes(submesh, buffers, attributes)){
            if (releaseSubmeshLod(submesh))
                updateMeshLodErrors(mesh);
            continue;
        }

        // every simplifier input takes part in the key, and the index type the levels are packed in
        const uint32_t layout[3] = {(uint32_t)attributes.index.attribute->getDataType(), attributes.index.attribute->getCount(), attributes.position.attribute->getCount()};
        uint64_t key = MeshLodPool::hash(0, layout, sizeof(layout));
        key = attributeHash(key, attributes.index);
        key = attributeHash(key, attributes.position);
        key = attributeHash(key, attributes.normal);
        key = attributeHash(key, attributes.uv);
        if (key == submesh.lodKey){
            pollMeshLod(mesh, submesh);
            continue;
        }

        submesh.lodKey = key;
        submesh.lodData.reset();
        submesh.lodIndices.getRender()->destroyBuffer();

        if (!MeshLodPool::has(key)){
            MeshLodSource source;
            if (readLodSource(attributes, source)){
                MeshLodPool::build(key, std::move(source));
            }
        }
        pollMeshLod(mesh, submesh);
    }
}

RenderSystem::LodAttribute RenderSystem::findSubmeshAttribute(Submesh& submesh, std::map<std::string, Buffer*>& buffers, AttributeType type){
    auto it = submesh.attributes.find(type);
    if (it != submesh.attributes.end()){
        auto buf = buffers.find(it->second.getBufferName());
        if (buf == buffers.end())
            return {};
        return {buf->second, &it->second};
    }
    for (auto const& buf : buffers){
        if (buf.second->isRenderAttributes() && buf.second->getAttribute(type)){
            return {buf.second, buf.second->getAttribute(type)};
        }
    }
    return {};
}

// the position and index attributes a level can be built over, with the normal and uv
// attributes that guide the collapses when present
bool RenderSystem::findLodAttributes(Submesh& submesh, std::map<std::string, Buffer*>& buffers, LodAttributes& attributes){
    if (submesh.primitiveType != PrimitiveType::TRIANGLES)
        return false;

    attributes.position = findSubmeshAttribute(submesh, buffers, AttributeType::POSITION);
    attributes.index = findSubmeshAttribute(submesh, buffers, AttributeType::INDEX);
    if (!attributes.position.attribute || !attributes.index.attribute)
        return false;
    if (attributes.position.attribute->getDataType() != AttributeDataType::FLOAT || attributes.position.attribute->getElements() != 3)
        return false;
    AttributeDataType indexType = attributes.index.attribute->getDataType();
    if (indexType != AttributeDataType::UNSIGNED_SHORT && indexType != AttributeDataType::UNSIGNED_INT)
        return false;
    if (attributes.index.attribute->getCount() % 3 != 0)
        return false;

    unsigned int vertexCount = attributes.position.attribute->getCount();
    attributes.normal = findSubmeshAttribute(submesh, buffers, AttributeType::NORMAL);
    attributes.uv = findSubmeshAttribute(submesh, buffers, AttributeType::TEXCOORD1);
    for (LodAttribute* optional : {&attributes.normal, &attributes.uv}){
        if (optional->attribute && (optional->attribute->getDataType() != AttributeDataType::FLOAT || optional->attribute->getCount() != vertexCount))
            *optional = {};
    }
    return true;
}

uint64_t RenderSystem::attributeHash(uint64_t seed, const LodAttribute& attribute){
    if (!attribute.attribute)
        return MeshLodPool::hash(seed, "", 1);
    size_t stride = std::max(attribute.buffer->getStride(), 1u);
    size_t begin = std::min((size_t)attribute.attribute->getOffset(), attribute.buffer->getSize());
    size_t end = std::min(begin + (size_t)attribute.attribute->getCount() * stride, attribute.buffer->getSize());
    return MeshLodPool::hash(seed, attribute.buffer->getData() + begin, end - begin);
}

bool RenderSystem::readLodSource(const LodAttributes& attributes, MeshLodSource& source){
    unsigned int vertexCount = attributes.position.attribute->getCount();
    unsigned int indexCount = attributes.index.attribute->getCount();
    bool normal = attributes.normal.attribute != nullptr;
    bool uv = attributes.uv.attribute != nullptr;

    source.indexType = attributes.index.attribute->getDataType();
    source.indices.reserve(indexCount);
    for (unsigned int k = 0; k < indexCount; k++){
        uint32_t value = (source.indexType == AttributeDataType::UNSIGNED_INT)
            ? attributes.index.buffer->getUInt32(attributes.index.attribute, k)
            : attributes.index.buffer->getUInt16(attributes.index.attribute, k);
        if (value >= vertexCount)
            return false;
        source.indices.push_back(value);
    }

    source.positions.reserve(vertexCount * 3);
    source.attributeCount = (normal ? 3 : 0) + (uv ? 2 : 0);
    source.attributes.reserve(vertexCount * source.attributeCount);
    for (unsigned int v = 0; v < vertexCount; v++){
        Vector3 p = attributes.position.buffer->getVector3(attributes.position.attribute, v);
        source.positions.insert(source.positions.end(), {p.x, p.y, p.z});
        if (normal){
            Vector3 n = attributes.normal.buffer->getVector3(attributes.normal.attribute, v);
            source.attributes.insert(source.attributes.end(), {n.x, n.y, n.z});
        }
        if (uv){
            Vector2 t = attributes.uv.buffer->getVector2(attributes.uv.attribute, v);
            source.attributes.insert(source.attributes.end(), {t.x, t.y});
        }
    }

    return true;
}

void RenderSystem::pollMeshLod(MeshComponent& mesh, Submesh& submesh){
    if (!submesh.lodData){
        submesh.lodData = MeshLodPool::get(submesh.lodKey);
        if (!submesh.lodData)
            return;
        submesh.lodIndices.setData(submesh.lodData->indices.data(), submesh.lodData->indices.size());
        submesh.lodIndices.setType(BufferType::INDEX_BUFFER);
        submesh.lodIndices.setUsage(BufferUsage::IMMUTABLE);
        updateMeshLodErrors(mesh);
    }

    if (submesh.lodData->numLevels > 1 && !submesh.lodIndices.getRender()->isCreated()){
        submesh.lodIndices.getRender()->createBuffer(submesh.lodIndices.getSize(), submesh.lodIndices.getData(), BufferType::INDEX_BUFFER, BufferUsage::IMMUTABLE);
    }
}

// true when the submesh had levels; builds already started still finish, and the pool
// keeps its copy until pools are cleared
bool RenderSystem::releaseSubmeshLod(Submesh& submesh){
    if (submesh.lodKey == 0 && !submesh.lodData && !submesh.lodIndices.getRender()->isCreated())
        return false;
    submesh.lodIndices.getRender()->destroyBuffer();
    submesh.lodIndices.clearAll();
    submesh.lodData.reset();
    submesh.lodKey = 0;
    return true;
}

void RenderSystem::releaseMeshLods(MeshComponent& mesh){
    bool hadLods = false;
    for (unsigned int i = 0; i < mesh.numSubmeshes; i++)
        hadLods |= releaseSubmeshLod(mesh.submeshes[i]);
    if (hadLods)
        updateMeshLodErrors(mesh);
}

bool RenderSystem::hasPendingMeshLods() const{
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            if (isMeshLodPending(mesh.submeshes[s]))
                return true;
        }
    }
    return false;
}

// a submesh still waiting for its levels, or for their GPU buffer
bool RenderSystem::isMeshLodPending(Submesh& submesh){
    if (submesh.lodKey == 0)
        return false;
    if (!submesh.lodData)
        return true;
    return submesh.lodData->numLevels > 1 && !submesh.lodIndices.getRender()->isCreated();
}

// a level is only as good as its worst submesh
void RenderSystem::updateMeshLodErrors(MeshComponent& mesh){
    mesh.numLods = 1;
    for (unsigned int l = 0; l < MAX_MESH_LODS; l++)
        mesh.lodError[l] = 0;
    for (unsigned int i = 0; i < mesh.numSubmeshes; i++){
        const MeshLodData* data = mesh.submeshes[i].lodData.get();
        if (!data)
            continue;
        mesh.numLods = std::max(mesh.numLods, data->numLevels);
        for (unsigned int l = 1; l < data->numLevels; l++)
            mesh.lodError[l] = std::max(mesh.lodError[l], data->levels[l].error);
    }
    instanceViewsDirty = true;
}

// world units one pixel of the main view spans; a change rebuilds the instance views
void RenderSystem::updateMainLodView(CameraComponent& camera, Transform& cameraTransform){
    float viewHeight = Engine::getViewRect().getHeight();
    if (isFixedResolutionActive())
        viewHeight = (float)scene->getFixedResolutionHeight();
    viewHeight = std::max(viewHeight, 1.0f);

    LodView view;
    view.enabled = scene->isMeshLodEnabled();
    view.threshold = scene->getMeshLodThreshold();
    view.origin = cameraTransform.worldPosition;
    if (camera.type == CameraType::CAMERA_PERSPECTIVE){
        view.perUnit = 2.0f * std::tan(camera.yfov * 0.5f) / viewHeight;
    }else{
        view.constant = std::abs(camera.topClip - camera.bottomClip) / viewHeight;
    }

    // Every input here is the player's own view: what the lights see is unaffected by the camera
    // moving or by its pixels per world unit changing.
    if (view.enabled != mainLodView.enabled || view.threshold != mainLodView.threshold ||
        view.origin != mainLodView.origin || view.perUnit != mainLodView.perUnit || view.constant != mainLodView.constant){
        mainInstanceViewDirty = true;
    }
    mainLodView = view;}

unsigned int RenderSystem::selectMeshLod(const MeshComponent& mesh, const LodView* view, float distance, float worldScale) const{
    if (!view || !view->enabled || !mesh.lodEnabled || mesh.numLods < 2)
        return 0;

    float allowed = view->threshold * (view->perUnit * std::max(distance, 0.0f) + view->constant) / std::max(mesh.lodBias, 0.001f);
    unsigned int level = 0;
    for (unsigned int l = 1; l < mesh.numLods; l++){
        if (mesh.lodError[l] * worldScale > allowed)
            break;
        level = l;
    }
    return level;
}

LightCamera RenderSystem::shadowTileCamera(const LightComponent& light, int cameraIndex, int tile){
    LightCamera camera = light.cameras[cameraIndex];
    if (tile < 0 || tile >= SHADOW_VIRTUAL_PAGES_PER_MAP || light.type == LightType::POINT)
        return camera;

    const Matrix4 crop = shadowTileCrop(tile);
    camera.lightProjectionMatrix = crop * camera.lightProjectionMatrix;
    camera.lightViewProjectionMatrix = crop * camera.lightViewProjectionMatrix;
    updateCameraFrustumPlanes(camera.lightViewProjectionMatrix, camera.frustumPlanes);
    return camera;
}

RenderSystem::LodView RenderSystem::shadowLodView(const LightComponent& light, int cameraIndex, int slot, int tile){
    LodView view;
    view.enabled = scene->isMeshLodEnabled();
    view.threshold = SHADOW_LOD_TEXELS;

    const LightCamera camera = shadowTileCamera(light, cameraIndex, tile);
    const Matrix4& projection = camera.lightProjectionMatrix;
    float texel = 2.0f / (std::abs(projection[1][1]) * std::max(getShadowAtlasSlotRect(slot).getHeight(), 1.0f));
    if (light.type == LightType::DIRECTIONAL){
        view.constant = texel;
    }else{
        view.origin = camera.lightViewMatrix.inverse() * Vector3(0, 0, 0);
        view.perUnit = texel;
    }
    return view;
}

// projective atlas slots the shadow pass draws this frame
std::vector<RenderSystem::ShadowSlot> RenderSystem::collectShadowSlots(){
    std::vector<ShadowSlot> slots;
    if (!hasShadows || !hasShadowAtlas)
        return slots;

    auto lights = scene->getComponentArray<LightComponent>();
    for (size_t l = 0; l < lights->size(); l++){
        LightComponent& light = lights->getComponentFromIndex(l);
        if (light.type == LightType::POINT || light.intensity <= 0 || !light.shadows || light.shadowMapIndex < 0)
            continue;
        int numShadowCameras = (light.type == LightType::DIRECTIONAL) ? (int)light.numShadowCascades : 1;
        for (int c = 0; c < numShadowCameras; c++){
            const int logicalPage = light.shadowMapIndex + c;
            for (int tile = 0; tile < SHADOW_VIRTUAL_PAGES_PER_MAP; tile++){
                const int virtualPage = shadowVirtualPageIndex(logicalPage, tile);
                const int physicalPage = (virtualPage >= 0 && virtualPage < MAX_SHADOW_VIRTUAL_PAGES) ?
                    shadowVirtualToPhysical[virtualPage] : -1;
                if (physicalPage >= 0 && physicalPage < shadowAtlasUsedSlots)
                    slots.push_back({physicalPage, &light, c, tile});
            }
        }
    }
    return slots;
}

// culled slice of every instanced mesh for the main camera and each shadow slot;
// runs when a list or a camera changed, so a still frame uploads nothing
void RenderSystem::buildInstanceViews(CameraComponent& mainCamera, Transform& mainCameraTransform){
    if (!instanceViewsDirty && !mainInstanceViewDirty && !shadowInstanceViewsDirty)
        return;

    // Walking around the scene changes what the player sees, not what the lights see, and moving
    // a light is the mirror case. Rebuilding both together re-culled and re-sorted every shadow
    // cascade for every instanced mesh on every frame the camera turned.
    const bool rebuildMain = instanceViewsDirty || mainInstanceViewDirty;
    const bool rebuildShadow = instanceViewsDirty || shadowInstanceViewsDirty;
    instanceViewsDirty = false;
    mainInstanceViewDirty = false;
    shadowInstanceViewsDirty = false;

    std::vector<ShadowSlot> shadowSlots = collectShadowSlots();
    if (rebuildMain){
        instanceViews[0].data.clear();
    }
    if (rebuildShadow){
        for (int v = 1; v < 1 + MAX_SHADOW_ATLAS_SLOTS; v++){
            instanceViews[v].data.clear();
        }
    }

    // hidden entities are built too: the passes skip them, but their slices must stay valid
    auto instmeshes = scene->getComponentArray<InstancedMeshComponent>();
    for (int i = 0; i < instmeshes->size(); i++){
        InstancedMeshComponent& instmesh = instmeshes->getComponentFromIndex(i);

        // A slot that stopped being a shadow slot is not rebuilt below, so its slice has to be
        // dropped here: its data was just cleared and a stale range would index past it.
        if (rebuildShadow){
            for (int v = 1; v < 1 + MAX_SHADOW_ATLAS_SLOTS; v++){
                instmesh.views[v] = InstanceViewRange();
            }
        }

        MeshComponent* mesh = scene->findComponent<MeshComponent>(instmeshes->getEntity(i));
        Transform* transform = scene->findComponent<Transform>(instmeshes->getEntity(i));
        if (!mesh || !transform || !mesh->loaded){
            for (InstanceViewRange& range : instmesh.views)
                range = InstanceViewRange();
            continue;
        }

        if (rebuildMain){
            // the main camera keeps the order where it is the layering: blended batches and 2D
            bool keepOrder = mesh->transparent || !sortsByDistance(mainCamera);
            const Vector3& eye = mainCameraTransform.worldPosition;
            buildInstanceView(0, instmesh, *mesh, *transform, mainCamera.effectiveFarClip, mainCamera.frustumPlanes, mainLodView,
                eye, eye, Vector3::ZERO, keepOrder);
        }

        if (!rebuildShadow)
            continue;

        for (const ShadowSlot& shadow : shadowSlots){
            if (!mesh->castShadows){
                instmesh.views[1 + shadow.slot] = InstanceViewRange();
                continue;
            }
            const LightCamera lightCamera = shadowTileCamera(*shadow.light, shadow.cameraIndex, shadow.tile);
            LodView lodView = shadowLodView(*shadow.light, shadow.cameraIndex, shadow.slot, shadow.tile);
            Vector3 direction = (shadow.light->type == LightType::DIRECTIONAL) ? shadow.light->worldDirection : Vector3::ZERO;
            buildInstanceView(1 + shadow.slot, instmesh, *mesh, *transform, lightCamera.nearFar.y, lightCamera.frustumPlanes, lodView,
                mainCameraTransform.worldPosition, lodView.origin, direction, false);
        }
    }

    for (int v = 0; v < 1 + MAX_SHADOW_ATLAS_SLOTS; v++){
        const bool rebuilt = (v == 0) ? rebuildMain : rebuildShadow;
        if (rebuilt){
            instanceViews[v].needUpload = !instanceViews[v].data.empty();
        }
    }
}

// instances inside the frustum and the cull distance from the eye, nearest first (along
// direction when given), grouped by level
void RenderSystem::buildInstanceView(int viewIndex, InstancedMeshComponent& instmesh, MeshComponent& mesh, Transform& transform, const float cameraFar, const Plane frustumPlanes[6], const LodView& lodView, const Vector3& eye, const Vector3& origin, const Vector3& direction, bool keepOrder){
    struct Candidate{
        unsigned int index;
        unsigned int lod;
        float key;
    };
    static std::vector<Candidate> candidates;
    candidates.clear();

    InstanceViewRange& range = instmesh.views[viewIndex];
    range = InstanceViewRange();
    if (instmesh.renderBounds.size() < instmesh.numVisible)
        return;

    const Vector3& worldScale = transform.worldScale;
    const float entityScale = std::max(std::abs(worldScale.x), std::max(std::abs(worldScale.y), std::abs(worldScale.z)));
    const bool limitDistance = std::isfinite(instmesh.cullDistance) && instmesh.cullDistance > 0.0f;

    for (unsigned int i = 0; i < instmesh.numVisible; i++){
        const InstanceBounds& bounds = instmesh.renderBounds[i];
        const Vector3 center = transform.modelMatrix * bounds.center;
        const float radius = bounds.radius * entityScale;
        if (instmesh.cullInstances){
            if (!isInsideCamera(cameraFar, frustumPlanes, center, radius))
                continue;
            if (limitDistance && (center - eye).length() - radius > instmesh.cullDistance)
                continue;
        }

        Candidate candidate;
        candidate.index = i;
        candidate.lod = 0;
        candidate.key = (float)i;
        if (!keepOrder){
            float distance = (center - lodView.origin).length() - radius;
            candidate.lod = selectMeshLod(mesh, &lodView, distance, bounds.scale * entityScale);
            candidate.key = (direction == Vector3::ZERO) ? (center - origin).squaredLength() : center.dotProduct(direction);
        }
        candidates.push_back(candidate);
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b){
        return a.lod != b.lod ? a.lod < b.lod : a.key < b.key;
    });

    InstanceView& view = instanceViews[viewIndex];
    range.offset = (unsigned int)view.data.size();
    for (const Candidate& candidate : candidates){
        view.data.push_back(instmesh.renderInstances[candidate.index]);
        range.lodCount[candidate.lod]++;
    }
}

// instances and detail level a pass draws a mesh with; false when there is nothing to draw
bool RenderSystem::selectMeshDraw(MeshComponent& mesh, Transform& transform, InstancedMeshComponent* instmesh, bool ownLod, int instanceView, const LodView* lodView, MeshDraw& draw){
    if (instmesh){
        const InstanceViewRange* range = (instanceView >= 0) ? &instmesh->views[instanceView] : nullptr;
        draw.instanceCount = range ? range->count() : instmesh->numVisible;
        draw.instanceFirst = range ? range->offset : 0;
        // unpainted foliage chunks make this common
        if (draw.instanceCount == 0)
            return false;
    }

    if (!ownLod && lodView){
        float worldScale = std::max(std::abs(transform.worldScale.x), std::max(std::abs(transform.worldScale.y), std::abs(transform.worldScale.z)));
        float distance = mesh.worldAABB.isNull() ? 0.0f : mesh.worldAABB.distance(lodView->origin);
        draw.lod = selectMeshLod(mesh, lodView, distance, worldScale);
    }

    return true;
}

// the slice of a view buffer, or the whole component buffer, starting at instance first
void RenderSystem::bindInstances(ObjectRender& render, InstancedMeshComponent& instmesh, int instanceView, unsigned int first){
    BufferRender* buffer = (instanceView >= 0) ? instanceViews[instanceView].buffer.getRender() : instmesh.buffer.getRender();
    render.replaceVertexBuffer(instmesh.buffer.getRender(), buffer, (size_t)first * instmesh.buffer.getStride());
}

// a culled view draws each level bucket of its slice; anything else draws every instance at one level
void RenderSystem::drawSubmeshGeometry(ObjectRender& render, MeshComponent& mesh, unsigned int submeshIndex, InstancedMeshComponent* instmesh, int instanceView, unsigned int lod, unsigned int instanceCount){
    Submesh& submesh = mesh.submeshes[submeshIndex];
    const MeshLodData* lodData = submesh.lodData.get();
    bool lodReady = lodData && lodData->numLevels > 1 && submesh.lodIndices.getRender()->isCreated();

    auto drawLevel = [&](unsigned int level, unsigned int instances, unsigned int first){
        if (instances == 0)
            return;
        if (instmesh)
            bindInstances(render, *instmesh, instanceView, first);
        level = lodReady ? std::min(level, lodData->numLevels - 1) : 0;
        if (level > 0){
            render.setIndexBuffer(submesh.lodIndices.getRender());
            render.draw(lodData->levels[level].indexBase, lodData->levels[level].indexCount, instances);
            render.resetIndexBuffer();
        }else{
            render.draw(0, submesh.vertexCount, instances);
        }
    };

    if (instmesh && instanceView >= 0){
        const InstanceViewRange& range = instmesh->views[instanceView];
        unsigned int first = range.offset;
        for (unsigned int l = 0; l < MAX_MESH_LODS; l++){
            drawLevel(l, range.lodCount[l], first);
            first += range.lodCount[l];
        }
    }else{
        drawLevel(lod, instanceCount, 0);
    }
}

// Uploads the dirty CPU-side buffers of a mesh, before the first pass of the frame
// that draws it. Every pass needs this, not only the color one that comes last: a
// rebuilt tilemap reorders its indices, so a shadow or G-buffer pass drawing chunk
// ranges would slice the previous upload with the new offsets. Sokol allows a single
// update per buffer per frame, which the flag already guarantees.
void RenderSystem::updateMeshBuffers(MeshComponent& mesh){
    if (mesh.needUpdateBones){
        const unsigned int size = static_cast<unsigned int>(
            mesh.bonesMatrix.size() * sizeof(Matrix4));
        if (mesh.bonesBuffer.isCreated())
            mesh.bonesBuffer.updateBuffer(size, mesh.bonesMatrix.data());
        if (mesh.bonesTexture.isCreated())
            mesh.bonesTexture.updateTexture(mesh.bonesMatrix.data(), size);
        mesh.needUpdateBones = false;
    }

    if (!mesh.needUpdateBuffer)
        return;

    if (mesh.buffer.getUsage() != BufferUsage::IMMUTABLE)
        mesh.buffer.getRender()->updateBuffer(mesh.buffer.getSize(), mesh.buffer.getData());
    if (mesh.indices.getUsage() != BufferUsage::IMMUTABLE)
        mesh.indices.getRender()->updateBuffer(mesh.indices.getSize(), mesh.indices.getData());
    for (int i = 0; i < mesh.numExternalBuffers; i++){
        if (mesh.eBuffers[i].getUsage() != BufferUsage::IMMUTABLE)
            mesh.eBuffers[i].getRender()->updateBuffer(mesh.eBuffers[i].getSize(), mesh.eBuffers[i].getData());
    }

    mesh.needUpdateBuffer = false;
}

// Once per frame, before any pass: the depth passes take their instance count from here.
void RenderSystem::updateInstanceBuffers(){
    auto instmeshes = scene->getComponentArray<InstancedMeshComponent>();

    for (int i = 0; i < instmeshes->size(); i++){
        InstancedMeshComponent& instmesh = instmeshes->getComponentFromIndex(i);

        if (!instmesh.needUpdateBuffer)
            continue;

        // setData here because component can change order and lose reference
        if (instmesh.numVisible > 0 && !instmesh.renderInstances.empty()){
            instmesh.buffer.setData((unsigned char*)(&instmesh.renderInstances[0]), sizeof(InstanceRenderData)*instmesh.numVisible);
            instmesh.buffer.getRender()->updateBuffer(instmesh.buffer.getSize(), instmesh.buffer.getData());
        }

        instmesh.needUpdateBuffer = false;
    }

    for (InstanceView& view : instanceViews){
        if (!view.needUpload)
            continue;
        view.needUpload = false;

        const size_t size = view.data.size() * sizeof(InstanceRenderData);
        BufferRender* render = view.buffer.getRender();
        // grows with headroom and never shrinks
        if (render->isCreated() && view.capacity < size)
            render->destroyBuffer();
        if (!render->isCreated()){
            view.capacity = size + size / 2;
            render->createBuffer(view.capacity, nullptr, BufferType::VERTEX_BUFFER, BufferUsage::STREAM);
        }
        view.buffer.setData((unsigned char*)view.data.data(), size);
        render->updateBuffer(size, view.buffer.getData());
    }
}

// Uploads a terrain view's CDLOD node selection, before the first pass of the frame
// that draws it — the same rule as updateMeshBuffers. The depth and G-buffer passes
// draw the main camera's selection and take their instance count from the CPU-side
// buffer, so without this they would draw this frame's node count out of the previous
// frame's node data.
void RenderSystem::updateTerrainNodesBuffer(TerrainComponent& terrain, int viewIndex){
    if (!terrain.views[viewIndex].needUpdateNodesBuffer)
        return;

    for (int s = 0; s < 2; s++){
        terrain.views[viewIndex].nodesbuffer[s].getRender()->updateBuffer(terrain.views[viewIndex].nodesbuffer[s].getSize(), terrain.views[viewIndex].nodesbuffer[s].getData());
    }

    terrain.views[viewIndex].needUpdateNodesBuffer = false;
}

bool RenderSystem::drawMesh(Entity entity, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType, InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, int terrainView, int instanceView, const LodView* lodView){
    if (mesh.loaded && !mesh.needReload){

        if (terrain && terrain->needUpdateTexture){
            return false;
        }

        if (cullsBatch(mesh, instmesh) && !isInsideCamera(camera, mesh.worldAABB)) {
            return false;
        }

        // Buffers already uploaded this frame by updateInstanceBuffers().
        MeshDraw draw;
        if (!selectMeshDraw(mesh, transform, instmesh, terrain || tilemap, instanceView, lodView, draw)){
            return false;
        }
        unsigned int instanceCount = draw.instanceCount;

        updateMeshBuffers(mesh);
        writeCustomUniforms(mesh, scene);

        if (terrain){
            updateTerrainNodesBuffer(*terrain, terrainView);
        }

        for (int i = 0; i < mesh.numSubmeshes; i++){
            // an indexed submesh without indices has nothing to draw (a tilemap rect
            // with no tile placed on it). Its texture state is cleared here because
            // gaining indices always reloads the mesh, which reloads the textures.
            if (mesh.submeshes[i].vertexCount == 0){
                mesh.submeshes[i].needUpdateTexture = false;
                continue;
            }

            // per-chunk culling: skip the submesh entirely when no chunk is visible
            if (tilemap && !selectTilemapChunks(*tilemap, i, camera.effectiveFarClip, camera.frustumPlanes))
                continue;

            ObjectRender& render = mesh.submeshes[i].render;

            if (terrain){
                instanceCount = terrain->views[terrainView].nodesbuffer[i].getCount();
                // swap in this view's node instance buffer (no-op for view 0)
                render.replaceVertexBuffer(terrain->views[0].nodesbuffer[i].getRender(), terrain->views[terrainView].nodesbuffer[i].getRender());
            }

            bool needUpdateFramebuffer = checkPBRFrabebufferUpdate(mesh.submeshes[i].material);

            if (mesh.submeshes[i].needUpdateTexture || needUpdateFramebuffer){
                ShaderData& shaderData = mesh.submeshes[i].shader.get()->shaderData;
                if (loadPBRTextures(mesh.submeshes[i].material, shaderData, mesh.submeshes[i].render, mesh.receiveLights)){
                    mesh.submeshes[i].needUpdateTexture = false;
                }
            }

            // the new texture is still loading and the old one may be released already
            if (mesh.submeshes[i].needUpdateTexture)
                continue;

            if (!render.beginDraw(pipType)){
                mesh.needReload = true;
                return false;
            }

            if (mesh.submeshes[i].hasIBL){
                TextureRender* probeTexture = &emptyCubeBlack;
                // Probe assignment is per renderable. Its bounds center is a
                // better representative point than the entity origin for meshes
                // whose geometry is offset in local space.
                Vector3 probeSamplePosition = meshProbeSamplePosition(mesh, transform);
                selectReflectionProbe(probeSamplePosition, fs_reflection_probe, probeTexture);
                ShaderData& probeShaderData = mesh.submeshes[i].shader.get()->shaderData;
                render.addTexture(probeShaderData.getTextureIndex(TextureShaderType::REFLECTIONPROBE), ShaderStageType::FRAGMENT, probeTexture);
                if (mesh.submeshes[i].slotFSReflectionProbe != -1){
                    render.applyUniformBlock(mesh.submeshes[i].slotFSReflectionProbe, sizeof(fs_reflection_probe_t), &fs_reflection_probe);
                }
            }

            if (hasFog){
                render.applyUniformBlock(mesh.submeshes[i].slotFSFog, sizeof(float) * 8, &fs_fog);
            }

            if (mesh.submeshes[i].slotFSMirror != -1){
                render.applyUniformBlock(mesh.submeshes[i].slotFSMirror, sizeof(float) * 16, &mesh.mirrorViewProjection);
            }

            // a submesh is lit (uses the lighting block + full PBR params) unless it
            // was compiled as MATERIAL_UNLIT (property bit 0)
            bool submeshLit = !(mesh.submeshes[i].shaderProperties & (1u << 0));

            if (submeshLit){
                ShaderData& lightingShaderData = mesh.submeshes[i].shader.get()->shaderData;
                loadSpotMaskTexture(lightingShaderData, render);
                render.applyUniformBlock(mesh.submeshes[i].slotFSLighting, sizeof(fs_lighting_t), &fs_lighting);
                const int lightGridSlot = lightingShaderData.getStorageBufferIndex(StorageBufferType::FS_LIGHT_GRID);
                if (lightGridSlot != -1 && clusteredLightGridReady){
                    render.addStorageBuffer(lightGridSlot, ShaderStageType::FRAGMENT, &clusteredLightGridBuffer);
                }
                if (hasShadows && (mesh.submeshes[i].shaderProperties & (1u << 4))){
                    render.applyUniformBlock(mesh.submeshes[i].slotVSShadows, sizeof(shadow_projection_t), &shadowProjection);
                    render.applyUniformBlock(mesh.submeshes[i].slotFSShadowProjection, sizeof(shadow_projection_t), &shadowProjection);
                    render.applyUniformBlock(mesh.submeshes[i].slotFSShadows, sizeof(fs_shadows_t), &fs_shadows);
                    render.applyUniformBlock(mesh.submeshes[i].slotFSPointShadows, sizeof(fs_point_shadows_t), &fs_point_shadows);
                }
                // USE_SSAO (property bit 21): bind the screen-space AO texture for
                // this view (blurred AO for the main camera, empty white otherwise)
                if (mesh.submeshes[i].shaderProperties & (1u << 21)){
                    ShaderData& ssaoShaderData = mesh.submeshes[i].shader.get()->shaderData;
                    render.addTexture(ssaoShaderData.getTextureIndex(TextureShaderType::SSAOTEXTURE), ShaderStageType::FRAGMENT,
                                      currentSSAOTexture ? currentSSAOTexture : &emptyWhite);
                }
            }

            // USE_LIGHT2D (property bit 22): dedicated 2D light path; composes with
            // both unlit (sprites) and lit submeshes, so it is applied independently
            if (mesh.submeshes[i].shaderProperties & (1u << 22)){
                render.applyUniformBlock(mesh.submeshes[i].slotFSLighting2D, sizeof(fs_lighting2d_t), &fs_lighting2d);
            }

            if (mesh.submeshes[i].hasTextureRect){
                render.applyUniformBlock(mesh.submeshes[i].slotVSSprite, sizeof(float) * 4, &mesh.submeshes[i].textureRect);
            }

            if (mesh.submeshes[i].hasSkinning){
                applySkinningUniform(render, mesh.submeshes[i].slotVSSkinning, mesh, mesh.submeshes[i]);
            }

            if (mesh.submeshes[i].hasMorphTarget){
                if (!mesh.submeshes[i].hasMorphNormal && !mesh.submeshes[i].hasMorphTangent){
                    render.applyUniformBlock(mesh.submeshes[i].slotVSMorphTarget, sizeof(float) * MAX_MORPHTARGETS, &mesh.morphWeights);
                }else{
                    render.applyUniformBlock(mesh.submeshes[i].slotVSMorphTarget, sizeof(float) * MAX_MORPHTARGETS / 2, &mesh.morphWeights);
                }
            }

            if (submeshLit){
                render.applyUniformBlock(mesh.submeshes[i].slotFSParams, sizeof(float) * 12, &mesh.submeshes[i].material);
            }else if (mesh.submeshes[i].shaderProperties & (1u << 24)){
                // Unlit alpha-mask variants include the metallic/roughness padding
                // and alphaCutoff from the shared CPU material prefix.
                render.applyUniformBlock(mesh.submeshes[i].slotFSParams, sizeof(float) * 8, &mesh.submeshes[i].material);
            }else{
                render.applyUniformBlock(mesh.submeshes[i].slotFSParams, sizeof(float) * 4, &mesh.submeshes[i].material);
            }

            if (mesh.submeshes[i].slotFSMaterialExtensions != -1){
                const Material& material = mesh.submeshes[i].material;
                fs_material_extensions_t extensions;
                extensions.clearcoat = Vector4(
                    std::clamp(material.clearcoatFactor, 0.0f, 1.0f),
                    std::clamp(material.clearcoatRoughness, 0.0f, 1.0f), 0.0f, 0.0f);
                extensions.sheen = Vector4(
                    std::max(material.sheenColorFactor.x, 0.0f),
                    std::max(material.sheenColorFactor.y, 0.0f),
                    std::max(material.sheenColorFactor.z, 0.0f),
                    std::clamp(material.sheenRoughnessFactor, 0.0f, 1.0f));
                extensions.transmission = Vector4(
                    std::clamp(material.transmissionFactor, 0.0f, 1.0f),
                    std::max(material.thicknessFactor, 0.0f),
                    std::max(material.attenuationDistance, 0.0f), 1.5f);
                extensions.attenuation = Vector4(
                    std::max(material.attenuationColor.x, 0.0f),
                    std::max(material.attenuationColor.y, 0.0f),
                    std::max(material.attenuationColor.z, 0.0f), 0.0f);
                extensions.anisotropy = Vector4(
                    std::clamp(material.anisotropyStrength, -1.0f, 1.0f),
                    material.anisotropyRotation, 0.0f, 0.0f);
                render.applyUniformBlock(mesh.submeshes[i].slotFSMaterialExtensions,
                                         sizeof(extensions), &extensions);
            }

            if (mesh.submeshes[i].slotVSFade != -1 && instmesh){
                applyInstanceFadeUniform(render, mesh.submeshes[i].slotVSFade, *instmesh);
            }

            if (mesh.submeshes[i].slotFSHeight != -1){
                // heightScale, minLayers, maxLayers and the zeroed pad are four
                // contiguous floats at the head of the material's height block
                render.applyUniformBlock(mesh.submeshes[i].slotFSHeight, sizeof(float) * 4,
                                         &mesh.submeshes[i].material.heightScale);
            }

            if (mesh.submeshes[i].slotFSTexCoordSets != -1){
                // per-texture UV set selector (0 = a_texcoord1, 1 = a_texcoord2):
                // set0 = baseColor, metallicRoughness, occlusion, emissive;
                // set1.x = normal, set1.y = height
                const Material& mat = mesh.submeshes[i].material;
                int32_t texCoordSets[8] = {
                    mat.baseColorTexCoord, mat.metallicRoughnessTexCoord, mat.occlusionTexCoord, mat.emissiveTexCoord,
                    mat.normalTexCoord, mat.heightTexCoord, 0, 0
                };
                render.applyUniformBlock(mesh.submeshes[i].slotFSTexCoordSets, sizeof(texCoordSets), &texCoordSets);
            }

            if (terrain){
                // morph from this view's eye position (paired with its node selection)
                terrain->eyePos = terrain->views[terrainView].nodesEyePos;
                render.applyUniformBlock(mesh.submeshes[i].slotVSTerrain, sizeof(float) * 8, &(terrain->eyePos));
                applyTerrainLayersUniform(render, mesh.submeshes[i].slotFSTerrainLayers, entity, *terrain);
            }

            //model, normal and mvp matrix
            render.applyUniformBlock(mesh.submeshes[i].slotVSParams, sizeof(float) * 48, &transform.modelMatrix);

            applyCustomUniforms(render, mesh.submeshes[i].customVSParams, frameTime, passResolution);
            applyCustomUniforms(render, mesh.submeshes[i].customFSParams, frameTime, passResolution);

            if (tilemap && !tilemapDrawRanges.empty()){
                if (instmesh){
                    bindInstances(render, *instmesh, instanceView, draw.instanceFirst);
                }
                for (const TilemapDrawRange& range : tilemapDrawRanges){
                    render.draw(range.offset, range.count, instanceCount);
                }
            }else{
                drawSubmeshGeometry(render, mesh, i, instmesh, instanceView, draw.lod, instanceCount);
            }
        }
    }

    return true;
}

bool RenderSystem::drawMeshDepth(MeshComponent& mesh, Transform& transform, const float cameraFar, const Plane frustumPlanes[6], vs_depth_t vsDepthParams, InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, bool forSSAO, PipelineType pipelineType, int instanceView, const LodView* lodView){
    // shadow passes only draw casters; the SSAO depth pre-pass draws every opaque mesh
    if (mesh.loaded && !mesh.needReload && (mesh.castShadows || forSSAO)){

        if (terrain && terrain->needUpdateTexture){
            return false;
        }

        if (cullsBatch(mesh, instmesh) && !isInsideCamera(cameraFar, frustumPlanes, mesh.worldAABB)) {
            return false;
        }

        MeshDraw draw;
        if (!selectMeshDraw(mesh, transform, instmesh, terrain || tilemap, instanceView, lodView, draw)){
            return false;
        }
        unsigned int instanceCount = draw.instanceCount;

        updateMeshBuffers(mesh);
        writeCustomUniforms(mesh, scene);

        // the depth pass renders the main camera's selection (view 0)
        if (terrain){
            updateTerrainNodesBuffer(*terrain, 0);
        }

        for (int i = 0; i < mesh.numSubmeshes; i++){
            if (mesh.submeshes[i].vertexCount == 0){
                mesh.submeshes[i].needUpdateDepthTexture = false;
                continue;
            }

            if (tilemap && !selectTilemapChunks(*tilemap, i, cameraFar, frustumPlanes))
                continue;

            ObjectRender& depthRender = mesh.submeshes[i].depthRender;

            if (mesh.submeshes[i].needUpdateDepthTexture &&
                (mesh.submeshes[i].depthShaderProperties & (1u << 0))){
                ShaderData& depthShaderData = mesh.submeshes[i].depthShader.get()->shaderData;
                if (loadDepthTexture(mesh.submeshes[i].material, depthShaderData, mesh.submeshes[i].depthRender)){
                    mesh.submeshes[i].needUpdateDepthTexture = false;
                }
            }else if (mesh.submeshes[i].needUpdateDepthTexture){
                mesh.submeshes[i].needUpdateDepthTexture = false;
            }

            if (mesh.submeshes[i].needUpdateDepthTexture)
                continue;

            if (!depthRender.beginDraw(pipelineType)){
                mesh.needReload = true;
                return false;
            }

            if (terrain){
                instanceCount = terrain->views[0].nodesbuffer[i].getCount();
                depthRender.replaceVertexBuffer(terrain->views[0].nodesbuffer[i].getRender(), terrain->views[0].nodesbuffer[i].getRender());
            }

            //model, mvp matrix
            depthRender.applyUniformBlock(mesh.submeshes[i].slotVSDepthParams, std::min((unsigned int)sizeof(vs_depth_t), mesh.submeshes[i].sizeVSDepthParams), &vsDepthParams);

            applyCustomUniforms(depthRender, mesh.submeshes[i].customVSDepthParams, frameTime, passResolution);
            applyCustomUniforms(depthRender, mesh.submeshes[i].customFSDepthParams, frameTime, passResolution);

            if (mesh.submeshes[i].slotVSDepthFade != -1 && instmesh){
                applyInstanceFadeUniform(depthRender, mesh.submeshes[i].slotVSDepthFade, *instmesh);
            }

            if (mesh.submeshes[i].slotFSDepthMaterial != -1){
                const Material& material = mesh.submeshes[i].material;
                Vector4 alphaParams(material.baseColorFactor.w, material.alphaCutoff, 0.0f, 0.0f);
                depthRender.applyUniformBlock(mesh.submeshes[i].slotFSDepthMaterial, sizeof(Vector4), &alphaParams);
            }

            if (mesh.submeshes[i].hasSkinning){
                applySkinningUniform(depthRender, mesh.submeshes[i].slotVSDepthSkinning, mesh, mesh.submeshes[i]);
            }
            if (mesh.submeshes[i].hasMorphTarget){
                if (!mesh.submeshes[i].hasMorphNormal && !mesh.submeshes[i].hasMorphTangent){
                    depthRender.applyUniformBlock(mesh.submeshes[i].slotVSDepthMorphTarget, sizeof(float) * MAX_MORPHTARGETS, &mesh.morphWeights);
                }else{
                    depthRender.applyUniformBlock(mesh.submeshes[i].slotVSDepthMorphTarget, sizeof(float) * MAX_MORPHTARGETS / 2, &mesh.morphWeights);
                }
            }

            if (terrain){
                // shadow depth renders the main camera's selection (view 0)
                terrain->eyePos = terrain->views[0].nodesEyePos;
                depthRender.applyUniformBlock(mesh.submeshes[i].slotVSDepthTerrain, sizeof(float) * 8, &(terrain->eyePos));
            }

            if (tilemap && !tilemapDrawRanges.empty()){
                if (instmesh){
                    bindInstances(depthRender, *instmesh, instanceView, draw.instanceFirst);
                }
                for (const TilemapDrawRange& range : tilemapDrawRanges){
                    depthRender.draw(range.offset, range.count, instanceCount);
                }
            }else{
                drawSubmeshGeometry(depthRender, mesh, i, instmesh, instanceView, draw.lod, instanceCount);
            }
        }
    }

    return true;
}

void RenderSystem::loadSSAO(){
    if (ssaoLoaded)
        return;

    // the SSAO fullscreen shaders may still be building (async in the editor)
    ssaoShader = ShaderPool::get(ShaderType::SSAO, 0);
    ssaoBlurShader = ShaderPool::get(ShaderType::SSAO_BLUR, 0);
    if (!ssaoShader || !ssaoShader->isCreated() || !ssaoBlurShader || !ssaoBlurShader->isCreated())
        return;

    // hemisphere sample kernel, weighted toward the origin (more near-field samples).
    // It is constant, so write it straight into the uniform block once here.
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distm11(-1.0f, 1.0f);

    for (int i = 0; i < SSAO_KERNEL_SIZE; i++){
        Vector3 sample(distm11(rng), distm11(rng), dist01(rng)); // +Z hemisphere
        sample.normalize();
        sample *= dist01(rng);
        float scale = (float)i / (float)SSAO_KERNEL_SIZE;
        scale = 0.1f + (scale * scale) * (1.0f - 0.1f); // lerp(0.1, 1.0, scale^2)
        sample *= scale;
        fs_ssao.kernel[i] = Vector4(sample.x, sample.y, sample.z, 0.0f);
    }

    // 4x4 rotation-noise texture: random vectors in the tangent XY plane (z=0).
    // Pixel layout matches the empty textures: 0xAABBGGRR in memory.
    const int noiseDim = 4;
    uint32_t noisePixels[noiseDim * noiseDim];
    for (int i = 0; i < noiseDim * noiseDim; i++){
        uint8_t r = (uint8_t)((distm11(rng) * 0.5f + 0.5f) * 255.0f);
        uint8_t g = (uint8_t)((distm11(rng) * 0.5f + 0.5f) * 255.0f);
        noisePixels[i] = (0xFFu << 24) | (0x80u << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    }

    void* ndata[6]; size_t nsize[6];
    ndata[0] = (void*)noisePixels; nsize[0] = noiseDim * noiseDim * 4;
    ssaoNoiseTexture.createTexture(
            "ssao|noise", noiseDim, noiseDim, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, ndata, nsize,
            TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);

    // fullscreen ssao pass (vertexless: no attributes added)
    ssaoRender.beginLoad(PrimitiveType::TRIANGLES);
    ssaoRender.setShader(ssaoShader.get());
    ssaoSlotParams = ssaoShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::SSAO_FS_PARAMS);
    if (!ssaoRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    // fullscreen blur pass
    ssaoBlurRender.beginLoad(PrimitiveType::TRIANGLES);
    ssaoBlurRender.setShader(ssaoBlurShader.get());
    ssaoBlurSlotParams = ssaoBlurShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::SSAO_BLUR_FS_PARAMS);
    if (!ssaoBlurRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    ssaoLoaded = true;
}

void RenderSystem::destroySSAO(){
    if (ssaoLoaded){
        ssaoRender.destroy();
        ssaoBlurRender.destroy();
        ssaoNoiseTexture.destroyTexture();
    }
    if (ssaoShader){
        ssaoShader.reset();
        ShaderPool::remove(ShaderType::SSAO, 0);
    }
    if (ssaoBlurShader){
        ssaoBlurShader.reset();
        ShaderPool::remove(ShaderType::SSAO_BLUR, 0);
    }

    ssaoDepthFramebuffer.destroy();
    ssaoFramebuffer.destroy();
    ssaoBlurFramebuffer.destroy();

    ssaoWidth = 0;
    ssaoHeight = 0;
    ssaoSlotParams = -1;
    ssaoBlurSlotParams = -1;
    ssaoLoaded = false;
    currentSSAOTexture = NULL;
}

// only the depth pre-pass target, for water and post-process depth without SSAO
bool RenderSystem::ensureDepthPrePassFramebuffer(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;

    if (ssaoDepthFramebuffer.isCreated() && ssaoDepthFramebuffer.getWidth() == width && ssaoDepthFramebuffer.getHeight() == height)
        return true;

    ssaoDepthFramebuffer.destroy();

    // depth pre-pass target: packed depth in color, sampled point-exact
    ssaoDepthFramebuffer.setWidth(width);
    ssaoDepthFramebuffer.setHeight(height);
    ssaoDepthFramebuffer.setMinFilter(TextureFilter::NEAREST);
    ssaoDepthFramebuffer.setMagFilter(TextureFilter::NEAREST);
    ssaoDepthFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    ssaoDepthFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    ssaoDepthFramebuffer.create();

    return ssaoDepthFramebuffer.isCreated();
}

bool RenderSystem::ensureSSAOFramebuffers(unsigned int width, unsigned int height){
    if (!ensureDepthPrePassFramebuffer(width, height))
        return false;

    if (ssaoFramebuffer.isCreated() && ssaoBlurFramebuffer.isCreated() && ssaoWidth == width && ssaoHeight == height)
        return true;

    ssaoFramebuffer.destroy();
    ssaoBlurFramebuffer.destroy();

    ssaoFramebuffer.setWidth(width);
    ssaoFramebuffer.setHeight(height);
    ssaoFramebuffer.setMinFilter(TextureFilter::NEAREST);
    ssaoFramebuffer.setMagFilter(TextureFilter::NEAREST);
    ssaoFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    ssaoFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    ssaoFramebuffer.create();

    // blurred AO is sampled bilinearly by the lit meshes
    ssaoBlurFramebuffer.setWidth(width);
    ssaoBlurFramebuffer.setHeight(height);
    ssaoBlurFramebuffer.setMinFilter(TextureFilter::LINEAR);
    ssaoBlurFramebuffer.setMagFilter(TextureFilter::LINEAR);
    ssaoBlurFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    ssaoBlurFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    ssaoBlurFramebuffer.create();

    ssaoWidth = width;
    ssaoHeight = height;

    return ssaoFramebuffer.isCreated() && ssaoBlurFramebuffer.isCreated();
}

void RenderSystem::renderSSAO(CameraComponent& camera, TextureRender* sharedDepth){
    loadSSAO();
    if (!ssaoLoaded)
        return;

    unsigned int w = (unsigned int)Engine::getViewRect().getWidth();
    unsigned int h = (unsigned int)Engine::getViewRect().getHeight();
    if (isFixedResolutionActive() && !camera.renderToTexture){
        // the main color pass renders into the fixed-resolution target, so the
        // AO buffer must match it (meshes convert gl_FragCoord with these sizes)
        w = scene->getFixedResolutionWidth();
        h = scene->getFixedResolutionHeight();
    }
    if (!ensureSSAOFramebuffers(w, h))
        return;

    Matrix4 renderProj = camera.projectionMatrix;

    // --- 1. depth + normal source: reuse the SSR G-buffer (packed depth in color[0],
    // view-space normal in color[1]) when available, otherwise run the SSAO depth
    // pre-pass and reconstruct the normal from the depth gradient in the shader ---
    bool useGBufferNormal = (sharedDepth != nullptr);
    TextureRender* depthTexture = sharedDepth;
    TextureRender* normalTexture; // G-buffer normal when shared, else an unused placeholder
    if (!depthTexture){
        renderDepthPrePass(camera);
        depthTexture = &ssaoDepthFramebuffer.getRender().getColorTexture();
        normalTexture = depthTexture; // placeholder binding; not sampled (params.w = 0)
    }else{
        normalTexture = &gbufferFramebuffer.getRender().getColorAttachmentTexture(1);
    }

    // --- 2. ssao pass (kernel was filled once in loadSSAO) ---
    fs_ssao.projection = renderProj;
    fs_ssao.invProjection = renderProj.inverse();
    fs_ssao.params = Vector4(scene->getSSAORadius(), scene->getSSAOBias(), scene->getSSAOIntensity(), useGBufferNormal ? 1.0f : 0.0f);
    // xy = noise tiling (4x4), zw = inverse depth-texture size (neighbour taps for normals)
    fs_ssao.noiseScale = Vector4((float)w / 4.0f, (float)h / 4.0f, 1.0f / (float)w, 1.0f / (float)h);

    ssaoPassRender.setClearColor(Vector4(1.0, 1.0, 1.0, 1.0));
    ssaoPassRender.startRenderPass(&ssaoFramebuffer.getRender());
    if (ssaoRender.beginDraw(PIP_RTT)){
        ShaderData& sd = ssaoShader.get()->shaderData;
        ssaoRender.addTexture(sd.getTextureIndex(TextureShaderType::DEPTHTEXTURE), ShaderStageType::FRAGMENT, depthTexture);
        ssaoRender.addTexture(sd.getTextureIndex(TextureShaderType::GBUFFERTEXTURE), ShaderStageType::FRAGMENT, normalTexture);
        ssaoRender.addTexture(sd.getTextureIndex(TextureShaderType::NOISETEXTURE), ShaderStageType::FRAGMENT, &ssaoNoiseTexture);
        ssaoRender.applyUniformBlock(ssaoSlotParams, sizeof(fs_ssao_t), &fs_ssao);
        ssaoRender.draw(0, 3, 1);
    }
    ssaoPassRender.endRenderPass();

    // --- 3. blur pass ---
    fs_ssao_blur.texelSize = Vector4(1.0f / (float)w, 1.0f / (float)h, 0.0, 0.0);

    ssaoPassRender.setClearColor(Vector4(1.0, 1.0, 1.0, 1.0));
    ssaoPassRender.startRenderPass(&ssaoBlurFramebuffer.getRender());
    if (ssaoBlurRender.beginDraw(PIP_RTT)){
        ShaderData& bd = ssaoBlurShader.get()->shaderData;
        ssaoBlurRender.addTexture(bd.getTextureIndex(TextureShaderType::SSAOTEXTURE), ShaderStageType::FRAGMENT, &ssaoFramebuffer.getRender().getColorTexture());
        ssaoBlurRender.applyUniformBlock(ssaoBlurSlotParams, sizeof(fs_ssao_blur_t), &fs_ssao_blur);
        ssaoBlurRender.draw(0, 3, 1);
    }
    ssaoPassRender.endRenderPass();

    currentSSAOTexture = &ssaoBlurFramebuffer.getRender().getColorTexture();
}

void RenderSystem::loadContactShadows(){
    if (contactShadowsLoaded)
        return;

    contactShadowsShader = ShaderPool::get(ShaderType::CONTACT_SHADOWS, 0);
    if (!contactShadowsShader || !contactShadowsShader->isCreated())
        return;

    contactShadowsRender.beginLoad(PrimitiveType::TRIANGLES);
    contactShadowsRender.setShader(contactShadowsShader.get());
    contactShadowsSlotParams = contactShadowsShader->shaderData.getUniformBlockIndex(
        UniformBlockType::CONTACT_SHADOWS_FS_PARAMS);
    if (!contactShadowsRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    contactShadowsLoaded = true;
}

void RenderSystem::destroyContactShadows(){
    if (contactShadowsLoaded)
        contactShadowsRender.destroy();
    if (contactShadowsShader){
        contactShadowsShader.reset();
        ShaderPool::remove(ShaderType::CONTACT_SHADOWS, 0);
    }

    contactShadowsFramebuffer.destroy();
    contactShadowsLoaded = false;
    contactShadowsHDR = false;
    contactShadowsWidth = 0;
    contactShadowsHeight = 0;
    contactShadowsSlotParams = -1;
}

bool RenderSystem::ensureContactShadowsFramebuffer(unsigned int width, unsigned int height, bool hdr){
    if (width == 0 || height == 0)
        return false;

    if (contactShadowsFramebuffer.isCreated() && contactShadowsWidth == width &&
            contactShadowsHeight == height && contactShadowsHDR == hdr)
        return true;

    contactShadowsFramebuffer.destroy();
    const ColorFormat format = hdr ? ColorFormat::RGBA16F : ColorFormat::RGBA;
    contactShadowsFramebuffer.setColorAttachments(1, &format);
    contactShadowsFramebuffer.setWidth(width);
    contactShadowsFramebuffer.setHeight(height);
    contactShadowsFramebuffer.setMinFilter(TextureFilter::LINEAR);
    contactShadowsFramebuffer.setMagFilter(TextureFilter::LINEAR);
    contactShadowsFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    contactShadowsFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    contactShadowsFramebuffer.create();

    contactShadowsWidth = width;
    contactShadowsHeight = height;
    contactShadowsHDR = hdr && contactShadowsFramebuffer.isCreated();
    return contactShadowsFramebuffer.isCreated() && (!hdr || contactShadowsHDR);
}

void RenderSystem::renderContactShadows(CameraComponent& camera, Transform& cameraTransform,
        Framebuffer* sceneColorTarget, TextureRender* depthTexture){
    if (!contactShadowsLoaded || !sceneColorTarget || !sceneColorTarget->isCreated() ||
            !depthTexture || !contactShadowsFramebuffer.isCreated())
        return;

    auto lights = scene->getComponentArray<LightComponent>();
    const LightComponent* strongestDirectional = nullptr;
    float strongestScore = 0.0f;
    for (int i = 0; i < lights->size(); i++){
        const LightComponent& light = lights->getComponentFromIndex(i);
        if (light.type != LightType::DIRECTIONAL || light.intensity <= 0.0f)
            continue;
        const float score = light.intensity * std::max(0.1f, light.color.length());
        if (!strongestDirectional || score > strongestScore){
            strongestDirectional = &light;
            strongestScore = score;
        }
    }
    if (!strongestDirectional)
        return;

    Vector3 lightDirection = -strongestDirectional->worldDirection;
    if (lightDirection.squaredLength() < 0.000001f)
        return;
    lightDirection.normalize();

    fs_contactShadows.viewProjection = camera.viewProjectionMatrix;
    fs_contactShadows.invViewProjection = camera.viewProjectionMatrix.inverse();
    fs_contactShadows.cameraPosition = Vector4(cameraTransform.worldPosition.x,
                                               cameraTransform.worldPosition.y,
                                               cameraTransform.worldPosition.z, 1.0f);
    fs_contactShadows.lightDirection = Vector4(lightDirection.x, lightDirection.y, lightDirection.z, 0.0f);
    fs_contactShadows.params = Vector4(scene->getContactShadowLength(),
                                       (float)scene->getContactShadowSteps(),
                                       scene->getContactShadowThickness(),
                                       scene->getContactShadowIntensity());

    contactShadowsPassRender.setClearColor(scene->getBackgroundColor());
    contactShadowsPassRender.startRenderPass(&contactShadowsFramebuffer.getRender());
    if (contactShadowsRender.beginDraw(PIP_RTT)){
        ShaderData& sd = contactShadowsShader->shaderData;
        contactShadowsRender.addTexture(sd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE),
                                        ShaderStageType::FRAGMENT,
                                        &sceneColorTarget->getRender().getColorTexture());
        contactShadowsRender.addTexture(sd.getTextureIndex(TextureShaderType::DEPTHTEXTURE),
                                        ShaderStageType::FRAGMENT, depthTexture);
        contactShadowsRender.applyUniformBlock(contactShadowsSlotParams,
                                               sizeof(fs_contact_shadows_t), &fs_contactShadows);
        contactShadowsRender.draw(0, 3, 1);
    }
    contactShadowsPassRender.endRenderPass();

    loadBlit();
    if (blitLoaded){
        renderBlit(&contactShadowsFramebuffer.getRender().getColorTexture(),
                   &sceneColorTarget->getRender(), nullptr);
    }
}

void RenderSystem::loadSSGI(){
    if (ssgiLoaded)
        return;

    ssgiShader = ShaderPool::get(ShaderType::SSGI, 0);
    if (!ssgiShader || !ssgiShader->isCreated())
        return;

    ssgiRender.beginLoad(PrimitiveType::TRIANGLES);
    ssgiRender.setShader(ssgiShader.get());
    ssgiSlotParams = ssgiShader->shaderData.getUniformBlockIndex(UniformBlockType::SSGI_FS_PARAMS);
    if (!ssgiRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    ssgiLoaded = true;
}

void RenderSystem::destroySSGI(){
    if (ssgiLoaded)
        ssgiRender.destroy();
    if (ssgiShader){
        ssgiShader.reset();
        ShaderPool::remove(ShaderType::SSGI, 0);
    }

    ssgiFramebuffer.destroy();
    ssgiLoaded = false;
    ssgiHDR = false;
    ssgiWidth = 0;
    ssgiHeight = 0;
    ssgiSlotParams = -1;
}

bool RenderSystem::ensureSSGIFramebuffer(unsigned int width, unsigned int height, bool hdr){
    if (width == 0 || height == 0)
        return false;

    if (ssgiFramebuffer.isCreated() && ssgiWidth == width && ssgiHeight == height && ssgiHDR == hdr)
        return true;

    ssgiFramebuffer.destroy();
    const ColorFormat format = hdr ? ColorFormat::RGBA16F : ColorFormat::RGBA;
    ssgiFramebuffer.setColorAttachments(1, &format);
    ssgiFramebuffer.setWidth(width);
    ssgiFramebuffer.setHeight(height);
    ssgiFramebuffer.setMinFilter(TextureFilter::LINEAR);
    ssgiFramebuffer.setMagFilter(TextureFilter::LINEAR);
    ssgiFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    ssgiFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    ssgiFramebuffer.create();

    ssgiWidth = width;
    ssgiHeight = height;
    ssgiHDR = hdr && ssgiFramebuffer.isCreated();
    return ssgiFramebuffer.isCreated() && (!hdr || ssgiHDR);
}

void RenderSystem::renderSSGI(CameraComponent& camera, Framebuffer* sceneColorTarget, TextureRender* depthTexture){
    if (!ssgiLoaded || !sceneColorTarget || !sceneColorTarget->isCreated() ||
            !depthTexture || !ssgiFramebuffer.isCreated())
        return;

    fsSSGI.projection = camera.projectionMatrix;
    fsSSGI.invProjection = camera.projectionMatrix.inverse();
    fsSSGI.params0 = Vector4(scene->getSSGIRadius(), (float)scene->getSSGISteps(),
                             scene->getSSGIIntensity(), scene->getSSGIThickness());
    fsSSGI.params1 = Vector4(1.0f / (float)ssgiWidth, 1.0f / (float)ssgiHeight,
                             std::fmod(frameTime * 0.73f, 6.2831853f), 0.0f);

    ssgiPassRender.setClearColor(scene->getBackgroundColor());
    ssgiPassRender.startRenderPass(&ssgiFramebuffer.getRender());
    if (ssgiRender.beginDraw(PIP_RTT)){
        ShaderData& sd = ssgiShader->shaderData;
        ssgiRender.addTexture(sd.getTextureIndexByName("u_sceneColorTexture"),
                              ShaderStageType::FRAGMENT,
                              &sceneColorTarget->getRender().getColorTexture());
        ssgiRender.addTexture(sd.getTextureIndexByName("u_depthTexture"),
                              ShaderStageType::FRAGMENT, depthTexture);
        ssgiRender.applyUniformBlock(ssgiSlotParams, sizeof(fs_ssgi_t), &fsSSGI);
        ssgiRender.draw(0, 3, 1);
    }
    ssgiPassRender.endRenderPass();

    loadBlit();
    if (blitLoaded){
        renderBlit(&ssgiFramebuffer.getRender().getColorTexture(),
                   &sceneColorTarget->getRender(), nullptr);
    }
}

void RenderSystem::loadDecals(bool hdr){
    if (decalsLoaded && decalsHDR == hdr)
        return;
    if (decalsLoaded)
        destroyDecals();

    decalShader = ShaderPool::get(ShaderType::DECAL, 0);
    if (!decalShader || !decalShader->isCreated())
        return;

    decalRender.beginLoad(PrimitiveType::TRIANGLES);
    decalRender.setShader(decalShader.get());
    decalRender.setRenderTargetColorFormat(hdr ? ColorFormat::RGBA16F : ColorFormat::RGBA);
    decalSlotParams = decalShader->shaderData.getUniformBlockIndex(UniformBlockType::DECAL_FS_PARAMS);
    if (!decalRender.endLoad(PIP_RTT_NODEPTH, false, false, CullingMode::BACK, WindingOrder::CCW))
        return;

    decalPassRender.setLoadActionLoad();
    decalsHDR = hdr;
    decalsLoaded = true;
}

void RenderSystem::destroyDecals(){
    if (decalsLoaded)
        decalRender.destroy();
    if (decalShader){
        decalShader.reset();
        ShaderPool::remove(ShaderType::DECAL, 0);
    }
    decalsLoaded = false;
    decalsHDR = false;
    decalSlotParams = -1;
}

void RenderSystem::renderDecals(CameraComponent& camera, Framebuffer* sceneColorTarget,
        TextureRender* depthTexture){
    if (!decalsLoaded || !sceneColorTarget || !sceneColorTarget->isCreated() || !depthTexture)
        return;

    auto decals = scene->getComponentArray<DecalComponent>();
    if (!decals || decals->size() == 0)
        return;

    fsDecal.invViewProjection = camera.viewProjectionMatrix.inverse();
    decalPassRender.startRenderPass(&sceneColorTarget->getRender());
    if (decalRender.beginDraw(PIP_RTT_NODEPTH)){
        ShaderData& shaderData = decalShader->shaderData;
        const std::pair<int, int> depthSlot = shaderData.getTextureIndex(TextureShaderType::DEPTHTEXTURE);
        const std::pair<int, int> decalTextureSlot = shaderData.getTextureIndex(TextureShaderType::DECALTEXTURE);
        decalRender.addTexture(depthSlot, ShaderStageType::FRAGMENT, depthTexture);

        for (size_t i = 0; i < decals->size(); i++){
            DecalComponent& decal = decals->getComponentFromIndex(i);
            if (!decal.enabled || decal.opacity <= 0.0f ||
                    std::abs(decal.size.x) < 0.0001f || std::abs(decal.size.y) < 0.0001f ||
                    std::abs(decal.size.z) < 0.0001f)
                continue;

            Transform* transform = scene->findComponent<Transform>(decals->getEntity(i));
            if (!transform || !transform->visible)
                continue;

            if (!decal.texture.empty())
                decal.texture.load();
            decalRender.addTexture(decalTextureSlot, ShaderStageType::FRAGMENT,
                                   decal.texture.empty() ? &emptyWhite : decal.texture.getRender(&emptyWhite));

            const Matrix4 decalModel = transform->modelMatrix * Matrix4::scaleMatrix(decal.size);
            fsDecal.inverseDecalModel = decalModel.inverse();
            fsDecal.colorOpacity = Vector4(decal.color.x, decal.color.y, decal.color.z,
                                           std::clamp(decal.opacity, 0.0f, 1.0f));
            fsDecal.params = Vector4(std::max(0.0f, decal.depthFade),
                                     isRenderingFlipped(camera) ? 1.0f : 0.0f, 0.0f, 0.0f);
            decalRender.applyUniformBlock(decalSlotParams, sizeof(fs_decal_t), &fsDecal);
            decalRender.draw(0, 3, 1);
        }
    }
    decalPassRender.endRenderPass();
}

void RenderSystem::renderDepthPrePass(CameraComponent& camera){
    // Logical (un-flipped) matrices: flipY reverses winding but PIP_DEPTH does not
    // compensate (only PIP_RTT reverses winding on GL). The depth buffer therefore
    // stays in logical orientation; SSAO/SSR consumers flip Y on GL when sampling.
    Matrix4 renderVP = camera.viewProjectionMatrix;

    ssaoPassRender.setClearColor(Vector4(1.0, 1.0, 1.0, 1.0)); // background -> depth ~1.0
    ssaoPassRender.startRenderPass(&ssaoDepthFramebuffer.getRender());
    passResolution = Vector2((float)ssaoDepthFramebuffer.getWidth(), (float)ssaoDepthFramebuffer.getHeight());

    auto transforms = scene->getComponentArray<Transform>();
    for (int i = 0; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);
        Entity entity = transforms->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (!signature.test(scene->getComponentId<MeshComponent>()))
            continue;

        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        if (!transform.visible || mesh.transparent)
            continue;

        InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);

        vs_depth_t params = {transform.modelMatrix, renderVP, renderVP * transform.modelMatrix};
        drawMeshDepth(mesh, transform, camera.effectiveFarClip, camera.frustumPlanes, params, instmesh, terrain, tilemap, true, PIP_DEPTH, 0, &mainLodView);
    }
    ssaoPassRender.endRenderPass();

    depthPrePassRendered = true;
}

bool RenderSystem::ensureGBufferFramebuffer(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;

    if (gbufferFramebuffer.isCreated() && gbufferFramebuffer.getWidth() == width && gbufferFramebuffer.getHeight() == height)
        return true;

    gbufferFramebuffer.destroy();

    // MRT: color[0] = packed depth (point-sampled like the depth pre-pass),
    //      color[1] = view-space normal (oct .rg) + roughness (.b) + metallic (.a),
    //      color[2] = linear base color (.rgb) + hasIBL flag (.a)
    ColorFormat formats[3] = { ColorFormat::RGBA, ColorFormat::RGBA, ColorFormat::RGBA };
    gbufferFramebuffer.setColorAttachments(3, formats);
    gbufferFramebuffer.setWidth(width);
    gbufferFramebuffer.setHeight(height);
    gbufferFramebuffer.setMinFilter(TextureFilter::NEAREST);
    gbufferFramebuffer.setMagFilter(TextureFilter::NEAREST);
    gbufferFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    gbufferFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    gbufferFramebuffer.create();

    return gbufferFramebuffer.isCreated();
}

bool RenderSystem::drawMeshGBuffer(Entity entity, MeshComponent& mesh, Transform& transform, const float cameraFar, const Plane frustumPlanes[6], vs_gbuffer_t vsGBufferParams, bool hasLocalProbe, InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, int instanceView, const LodView* lodView){
    if (!mesh.loaded || mesh.needReload)
        return true;

    if (terrain && terrain->needUpdateTexture){
        return false;
    }

    if (cullsBatch(mesh, instmesh) && !isInsideCamera(cameraFar, frustumPlanes, mesh.worldAABB)) {
        return false;
    }

    MeshDraw draw;
    if (!selectMeshDraw(mesh, transform, instmesh, terrain || tilemap, instanceView, lodView, draw)){
        return false;
    }
    unsigned int instanceCount = draw.instanceCount;

    updateMeshBuffers(mesh);

    // the G-buffer pass renders the main camera's selection (view 0)
    if (terrain){
        updateTerrainNodesBuffer(*terrain, 0);
    }

    for (int i = 0; i < mesh.numSubmeshes; i++){
        // the G-buffer shader/render only exist when SSR was enabled at load time; a
        // mesh loaded before SSR was toggled on is skipped until it reloads
        if (!mesh.submeshes[i].gbufferShader)
            continue;

        if (mesh.submeshes[i].vertexCount == 0){
            mesh.submeshes[i].needUpdateGBufferTexture = false;
            continue;
        }

        if (tilemap && !selectTilemapChunks(*tilemap, i, cameraFar, frustumPlanes))
            continue;

        ObjectRender& gbufferRender = mesh.submeshes[i].gbufferRender;

        if (mesh.submeshes[i].needUpdateGBufferTexture){
            ShaderData& gbufferShaderData = mesh.submeshes[i].gbufferShader.get()->shaderData;
            if (loadGBufferTextures(mesh.submeshes[i].material, gbufferShaderData, mesh.submeshes[i].gbufferRender)){
                mesh.submeshes[i].needUpdateGBufferTexture = false;
            }
        }

        if (mesh.submeshes[i].needUpdateGBufferTexture)
            continue;

        if (!gbufferRender.beginDraw(PIP_GBUFFER)){
            mesh.needReload = true;
            return false;
        }

        gbufferRender.applyUniformBlock(mesh.submeshes[i].slotVSGBufferParams, sizeof(vs_gbuffer_t), &vsGBufferParams);

        fs_gbuffer_material_t mat;
        // IBL source encoding: 0 = none, 0.5 = sky, 1 = local probe. The SSR
        // composite can exactly replace sky IBL; local probes need a deferred
        // cubemap array to be reconstructed per pixel, so they use additive SSR.
        float iblSource = mesh.submeshes[i].hasIBL ? (hasLocalProbe ? 1.0f : 0.5f) : 0.0f;
        const Material& material = mesh.submeshes[i].material;
        const float alphaCutoff = usesAlphaMask(material, mesh.submeshes[i].textureShadow)
            ? material.alphaCutoff
            : -1.0f;
        mat.params = Vector4(material.roughnessFactor, material.metallicFactor, iblSource, alphaCutoff);
        mat.baseColorFactor = mesh.submeshes[i].material.baseColorFactor;
        gbufferRender.applyUniformBlock(mesh.submeshes[i].slotFSGBufferMaterial, sizeof(fs_gbuffer_material_t), &mat);

        if (mesh.submeshes[i].hasSkinning){
            applySkinningUniform(gbufferRender, mesh.submeshes[i].slotVSGBufferSkinning, mesh, mesh.submeshes[i]);
        }
        if (mesh.submeshes[i].hasMorphTarget){
            if (!mesh.submeshes[i].hasMorphNormal && !mesh.submeshes[i].hasMorphTangent){
                gbufferRender.applyUniformBlock(mesh.submeshes[i].slotVSGBufferMorphTarget, sizeof(float) * MAX_MORPHTARGETS, &mesh.morphWeights);
            }else{
                gbufferRender.applyUniformBlock(mesh.submeshes[i].slotVSGBufferMorphTarget, sizeof(float) * MAX_MORPHTARGETS / 2, &mesh.morphWeights);
            }
        }

        if (terrain){
            terrain->eyePos = terrain->views[0].nodesEyePos;
            gbufferRender.applyUniformBlock(mesh.submeshes[i].slotVSGBufferTerrain, sizeof(float) * 8, &(terrain->eyePos));
            applyTerrainLayersUniform(gbufferRender, mesh.submeshes[i].slotFSGBufferTerrainLayers, entity, *terrain);
            instanceCount = terrain->views[0].nodesbuffer[i].getCount();
            gbufferRender.replaceVertexBuffer(terrain->views[0].nodesbuffer[i].getRender(), terrain->views[0].nodesbuffer[i].getRender());
        }

        if (tilemap && !tilemapDrawRanges.empty()){
            if (instmesh){
                bindInstances(gbufferRender, *instmesh, instanceView, draw.instanceFirst);
            }
            for (const TilemapDrawRange& range : tilemapDrawRanges){
                gbufferRender.draw(range.offset, range.count, instanceCount);
            }
        }else{
            drawSubmeshGeometry(gbufferRender, mesh, i, instmesh, instanceView, draw.lod, instanceCount);
        }
    }

    return true;
}

void RenderSystem::renderGBufferPass(CameraComponent& camera){
    // Same logical orientation as renderDepthPrePass: PIP_GBUFFER (like PIP_DEPTH)
    // does not reverse winding on GL, so color[0] depth matches the depth pre-pass
    // and SSR's existing Y-flip logic applies unchanged. color[1] carries the
    // view-space normal (octahedral) + roughness + metallic at the same texel layout.
    Matrix4 renderVP = camera.viewProjectionMatrix;
    Matrix4 viewMatrix = camera.viewMatrix;

    gbufferPassRender.setClearColor(Vector4(1.0, 1.0, 1.0, 1.0)); // color[0] background -> depth ~1.0
    gbufferPassRender.startRenderPass(&gbufferFramebuffer.getRender());

    auto transforms = scene->getComponentArray<Transform>();
    for (int i = 0; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);
        Entity entity = transforms->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (!signature.test(scene->getComponentId<MeshComponent>()))
            continue;

        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        if (!transform.visible || mesh.transparent)
            continue;

        InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);

        fs_reflection_probe_t probeParams;
        TextureRender* probeTexture = nullptr;
        Vector3 probeSamplePosition = meshProbeSamplePosition(mesh, transform);
        bool hasLocalProbe = selectReflectionProbe(probeSamplePosition, probeParams, probeTexture)
            && probeParams.position_weight.w > 0.001f;

        // view-space normal matrix = transpose(inverse(view * model))
        Matrix4 viewModel = viewMatrix * transform.modelMatrix;
        Matrix4 normalMatrix = viewModel.inverse().transpose();

        vs_gbuffer_t params = {transform.modelMatrix, renderVP, normalMatrix};
        drawMeshGBuffer(entity, mesh, transform, camera.effectiveFarClip, camera.frustumPlanes, params, hasLocalProbe, instmesh, terrain, tilemap, 0, &mainLodView);
    }
    gbufferPassRender.endRenderPass();
}

void RenderSystem::loadSSR(){
    if (ssrLoaded)
        return;

    // fullscreen SSR shaders may still be building (async in the editor)
    ssrShader = ShaderPool::get(ShaderType::SSR, 0);
    ssrBlurShader = ShaderPool::get(ShaderType::SSR_BLUR, 0);
    compositeShader = ShaderPool::get(ShaderType::COMPOSITE, 0);
    if (!ssrShader || !ssrShader->isCreated() || !ssrBlurShader || !ssrBlurShader->isCreated() || !compositeShader || !compositeShader->isCreated())
        return;

    // fullscreen ssr pass (vertexless: no attributes added)
    ssrRender.beginLoad(PrimitiveType::TRIANGLES);
    ssrRender.setShader(ssrShader.get());
    ssrSlotParams = ssrShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::SSR_FS_PARAMS);
    if (!ssrRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    // fullscreen glossy blur pass
    ssrBlurRender.beginLoad(PrimitiveType::TRIANGLES);
    ssrBlurRender.setShader(ssrBlurShader.get());
    ssrBlurSlotParams = ssrBlurShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::SSR_BLUR_FS_PARAMS);
    if (!ssrBlurRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    // composite targets either an offscreen framebuffer (PIP_RTT) or the swapchain
    // (PIP_DEFAULT), so build both pipeline variants.
    compositeRender.beginLoad(PrimitiveType::TRIANGLES);
    compositeRender.setShader(compositeShader.get());
    compositeSlotParams = compositeShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::COMPOSITE_FS_PARAMS);
    if (!compositeRender.endLoad(PIP_RTT | PIP_DEFAULT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    ssrLoaded = true;
}

void RenderSystem::destroySSR(){
    if (ssrLoaded){
        ssrRender.destroy();
        ssrBlurRender.destroy();
        compositeRender.destroy();
    }
    if (ssrShader){
        ssrShader.reset();
        ShaderPool::remove(ShaderType::SSR, 0);
    }
    if (ssrBlurShader){
        ssrBlurShader.reset();
        ShaderPool::remove(ShaderType::SSR_BLUR, 0);
    }
    if (compositeShader){
        compositeShader.reset();
        ShaderPool::remove(ShaderType::COMPOSITE, 0);
    }

    gbufferFramebuffer.destroy();
    sceneColorFramebuffer.destroy();
    ssrFramebuffer.destroy();
    ssrBlurFramebuffer.destroy();
    ssrCompositeFramebuffer.destroy();

    ssrWidth = 0;
    ssrHeight = 0;
    ssrCompositeHDR = false;
    ssrSlotParams = -1;
    ssrBlurSlotParams = -1;
    compositeSlotParams = -1;
    ssrLoaded = false;
}

// the redirected color pass target, of SSR or of a water copying the scene
bool RenderSystem::ensureSceneColorFramebuffer(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;

    // The chain is decided from the setting, not from hdrLoaded: a toggle rebuilds the target
    // once, while a mid-frame dependency on the load would drop the scene color of the frame
    // that loads the shaders.
    const bool wantHDR = scene && scene->isHdrEnabled();

    if (sceneColorFramebuffer.isCreated() && sceneColorFramebuffer.getWidth() == width &&
            sceneColorFramebuffer.getHeight() == height && sceneColorHDR == wantHDR)
        return true;

    sceneColorFramebuffer.destroy();

    // offscreen opaque scene color (sampled bilinearly by ssr.frag); has a depth
    // attachment so the redirected main color pass can depth-test normally - the MRT path
    // taken to get a pixel format attaches depth too (SokolFramebuffer.cpp:143).
    //
    // With the chain on, the attachment is 16-bit float. mesh.frag still closes in
    // linearTosRGB (no mesh variant is compiled without it), but a float target does not
    // clamp an emissive or a 2D light above 1.0, so the bright pass and the tone map read
    // the real range instead of a wall at white, at 16-bit precision instead of 8.
    ColorFormat format = wantHDR ? ColorFormat::RGBA16F : ColorFormat::RGBA;
    sceneColorFramebuffer.setColorAttachments(1, &format);
    sceneColorFramebuffer.setWidth(width);
    sceneColorFramebuffer.setHeight(height);
    sceneColorFramebuffer.setMinFilter(TextureFilter::LINEAR);
    sceneColorFramebuffer.setMagFilter(TextureFilter::LINEAR);
    sceneColorFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    sceneColorFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    sceneColorFramebuffer.create();

    sceneColorHDR = wantHDR && sceneColorFramebuffer.isCreated();

    if (wantHDR && !sceneColorHDR){
        Log::error("HDR scene color target could not be created as RGBA16F at %ux%u; the HDR chain is disabled for this target",
                   width, height);
    }

    // once per (re)build, so a headless run can prove which target it drew into
    Log::verbose("scene color target: %s at %ux%u", sceneColorHDR ? "RGBA16F" : "RGBA8",
                 width, height);

    return sceneColorFramebuffer.isCreated() && (!wantHDR || sceneColorHDR);
}

bool RenderSystem::ensureSSRFramebuffers(unsigned int width, unsigned int height){
    if (!ensureSceneColorFramebuffer(width, height))
        return false;

    if (ssrFramebuffer.isCreated() && ssrBlurFramebuffer.isCreated() &&
            ssrCompositeFramebuffer.isCreated() && ssrWidth == width && ssrHeight == height &&
            ssrCompositeHDR == sceneColorHDR)
        return true;

    ssrFramebuffer.destroy();
    ssrBlurFramebuffer.destroy();
    ssrCompositeFramebuffer.destroy();

    // reflection color + mask
    ssrFramebuffer.setWidth(width);
    ssrFramebuffer.setHeight(height);
    ssrFramebuffer.setMinFilter(TextureFilter::LINEAR);
    ssrFramebuffer.setMagFilter(TextureFilter::LINEAR);
    ssrFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    ssrFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    ssrFramebuffer.create();

    // glossy-blurred reflection (sampled by the composite when blur > 0)
    ssrBlurFramebuffer.setWidth(width);
    ssrBlurFramebuffer.setHeight(height);
    ssrBlurFramebuffer.setMinFilter(TextureFilter::LINEAR);
    ssrBlurFramebuffer.setMagFilter(TextureFilter::LINEAR);
    ssrBlurFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    ssrBlurFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    ssrBlurFramebuffer.create();

    const ColorFormat compositeFormat = sceneColorHDR ? ColorFormat::RGBA16F : ColorFormat::RGBA;
    ssrCompositeFramebuffer.setColorAttachments(1, &compositeFormat);
    ssrCompositeFramebuffer.setWidth(width);
    ssrCompositeFramebuffer.setHeight(height);
    ssrCompositeFramebuffer.setMinFilter(TextureFilter::LINEAR);
    ssrCompositeFramebuffer.setMagFilter(TextureFilter::LINEAR);
    ssrCompositeFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    ssrCompositeFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    ssrCompositeFramebuffer.create();

    ssrWidth = width;
    ssrHeight = height;
    ssrCompositeHDR = sceneColorHDR && ssrCompositeFramebuffer.isCreated();

    return ssrFramebuffer.isCreated() && ssrBlurFramebuffer.isCreated() &&
        ssrCompositeFramebuffer.isCreated();
}

void RenderSystem::renderSSR(CameraComponent& camera, FramebufferRender* destination,
                             bool compositeIntoSceneColor){
    unsigned int w = ssrWidth;
    unsigned int h = ssrHeight;
    if (w == 0 || h == 0)
        return;

    // The depth buffer is in logical orientation; the offscreen scene color was
    // rendered with the flip an offscreen target uses, so on GL the two differ by a
    // Y flip (isRenderingFlipped, always true on GL here).
    float flipGL = isRenderingFlipped(camera) ? 1.0f : 0.0f;
    Matrix4 renderProj = camera.projectionMatrix;

    // --- 1. ssr pass: march the depth buffer, sample offscreen scene color ---
    fs_ssr.projection = renderProj;
    fs_ssr.invProjection = renderProj.inverse();
    fs_ssr.params = Vector4(scene->getSSRMaxDistance(), scene->getSSRThickness(), scene->getSSRIntensity(), (float)scene->getSSRMaxSteps());
    // misc.w = glossy-blur amount, used only to scale the march jitter (0 => sharp, no jitter)
    fs_ssr.misc = Vector4(1.0f / (float)w, 1.0f / (float)h, flipGL, scene->getSSRBlur());

    ssrPassRender.setClearColor(Vector4(0.0, 0.0, 0.0, 0.0));
    ssrPassRender.startRenderPass(&ssrFramebuffer.getRender());
    if (ssrRender.beginDraw(PIP_RTT)){
        ShaderData& sd = ssrShader.get()->shaderData;
        ssrRender.addTexture(sd.getTextureIndex(TextureShaderType::DEPTHTEXTURE), ShaderStageType::FRAGMENT, &gbufferFramebuffer.getRender().getColorAttachmentTexture(0));
        ssrRender.addTexture(sd.getTextureIndex(TextureShaderType::GBUFFERTEXTURE), ShaderStageType::FRAGMENT, &gbufferFramebuffer.getRender().getColorAttachmentTexture(1));
        ssrRender.addTexture(sd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT, &sceneColorFramebuffer.getRender().getColorTexture());
        ssrRender.applyUniformBlock(ssrSlotParams, sizeof(fs_ssr_t), &fs_ssr);
        ssrRender.draw(0, 3, 1);
    }
    ssrPassRender.endRenderPass();

    // --- 2. optional glossy blur pass (premultiplied) ---
    // ssrBlur in [0..1] maps to a pixel spread radius; 0 keeps sharp mirror
    // reflections and skips the pass entirely. The blur ramps smoothly with the
    // radius (the shader scales its tap offsets by it), so there is no hard step.
    const float ssrBlurMaxRadius = 24.0f;
    float blurRadius = scene->getSSRBlur() * ssrBlurMaxRadius;
    TextureRender* reflectionTexture = &ssrFramebuffer.getRender().getColorTexture();
    if (scene->getSSRBlur() > 0.001f && scene->getSSRDebugMode() == 0){
        // blurRadius is the MAX radius (full roughness); the blur shader scales it
        // per-pixel by the G-buffer roughness. w = flip (gbuffer is in depth space).
        fs_ssr_blur.params = Vector4(1.0f / (float)w, 1.0f / (float)h, blurRadius, flipGL);

        ssrPassRender.setClearColor(Vector4(0.0, 0.0, 0.0, 0.0));
        ssrPassRender.startRenderPass(&ssrBlurFramebuffer.getRender());
        if (ssrBlurRender.beginDraw(PIP_RTT)){
            ShaderData& bd = ssrBlurShader.get()->shaderData;
            ssrBlurRender.addTexture(bd.getTextureIndex(TextureShaderType::SSRTEXTURE), ShaderStageType::FRAGMENT, &ssrFramebuffer.getRender().getColorTexture());
            ssrBlurRender.addTexture(bd.getTextureIndex(TextureShaderType::GBUFFERTEXTURE), ShaderStageType::FRAGMENT, &gbufferFramebuffer.getRender().getColorAttachmentTexture(1));
            ssrBlurRender.applyUniformBlock(ssrBlurSlotParams, sizeof(fs_ssr_blur_t), &fs_ssr_blur);
            ssrBlurRender.draw(0, 3, 1);
        }
        ssrPassRender.endRenderPass();

        reflectionTexture = &ssrBlurFramebuffer.getRender().getColorTexture();
    }

    // --- 3. composite pass: energy-conserving SSR-over-IBL -> real destination ---
    // The composite recomputes the IBL specular term per pixel (for surfaces flagged
    // hasIBL in the G-buffer) and replaces it with the SSR reflection, so the env
    // reflection already in the scene is not double-counted.
    fs_composite.invProjection = renderProj.inverse();
    fs_composite.invView = camera.viewMatrix.inverse();

    // the GL swapchain keeps row 0 at the bottom while the offscreen sources do not,
    // so a swapchain destination samples them flipped (renderBlit does the same)
    // HDR must receive SSR before bloom and tone mapping. The dedicated target also
    // avoids sampling and writing the same scene-color texture in one pass.
    FramebufferRender* compositeDestination = destination;
    if (compositeIntoSceneColor){
        compositeDestination = &ssrCompositeFramebuffer.getRender();
    }
    float flipDest = (!compositeDestination && Engine::isOpenGL()) ? 1.0f : 0.0f;
    fs_composite.params = Vector4(scene->getSSRIntensity(), flipGL, (float)scene->getSSRDebugMode(), flipDest);

    // environment color/rotation, matching mesh.frag's lighting.envColor; the prefiltered
    // GGX map comes from the sky (fall back to a black cube when there is no IBL sky)
    fs_composite.envColor = Vector4(0.0, 0.0, 0.0, 0.0);
    TextureRender* prefilteredTexture = &emptyCubeBlack;
    {
        auto skys = scene->getComponentArray<SkyComponent>();
        if (skys->size() > 0){
            SkyComponent& sky = skys->getComponentFromIndex(0);
            if (sky.prefilteredMap){
                prefilteredTexture = sky.prefilteredMap.get();
                Vector3 linColor = Color::sRGBToLinear(Vector3(sky.color.x, sky.color.y, sky.color.z));
                float giIntensity = scene->getGlobalIlluminationIntensity();
                fs_composite.envColor = Vector4(linColor.x * giIntensity, linColor.y * giIntensity,
                                                linColor.z * giIntensity, Angle::defaultToRad(sky.rotation));
            }
        }
    }

    ssrPassRender.setClearColor(scene->getBackgroundColor());
    PipelineType compositePip;
    if (compositeDestination){
        ssrPassRender.startRenderPass(compositeDestination);
        compositePip = PIP_RTT;
    }else{
        ssrPassRender.startRenderPass();
        ssrPassRender.applyViewport(Engine::getViewRect());
        compositePip = PIP_DEFAULT;
    }
    if (compositeRender.beginDraw(compositePip)){
        ShaderData& cd = compositeShader.get()->shaderData;
        compositeRender.addTexture(cd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT, &sceneColorFramebuffer.getRender().getColorTexture());
        compositeRender.addTexture(cd.getTextureIndex(TextureShaderType::SSRTEXTURE), ShaderStageType::FRAGMENT, reflectionTexture);
        compositeRender.addTexture(cd.getTextureIndex(TextureShaderType::DEPTHTEXTURE), ShaderStageType::FRAGMENT, &gbufferFramebuffer.getRender().getColorAttachmentTexture(0));
        compositeRender.addTexture(cd.getTextureIndex(TextureShaderType::GBUFFERTEXTURE), ShaderStageType::FRAGMENT, &gbufferFramebuffer.getRender().getColorAttachmentTexture(1));
        compositeRender.addTexture(cd.getTextureIndex(TextureShaderType::GBUFFERALBEDOTEXTURE), ShaderStageType::FRAGMENT, &gbufferFramebuffer.getRender().getColorAttachmentTexture(2));
        compositeRender.addTexture(cd.getTextureIndex(TextureShaderType::PREFILTEREDMAP), ShaderStageType::FRAGMENT, prefilteredTexture);
        compositeRender.applyUniformBlock(compositeSlotParams, sizeof(fs_composite_t), &fs_composite);
        compositeRender.draw(0, 3, 1);
    }
    ssrPassRender.endRenderPass();

    if (compositeIntoSceneColor){
        loadBlit();
        if (blitLoaded){
            renderBlit(&ssrCompositeFramebuffer.getRender().getColorTexture(),
                       &sceneColorFramebuffer.getRender(), nullptr);
        }
    }
}

void RenderSystem::loadBlit(){
    if (blitLoaded)
        return;

    // the fullscreen blit shader may still be building (async in the editor)
    blitShader = ShaderPool::get(ShaderType::BLIT, 0);
    if (!blitShader || !blitShader->isCreated())
        return;

    // the blit targets either an offscreen framebuffer (PIP_RTT) or the swapchain
    // (PIP_DEFAULT, exported builds); PIP_RTT_NODEPTH copies into a camera pass in progress
    // and PIP_RTT_NOBLEND into the scene copy
    blitRender.beginLoad(PrimitiveType::TRIANGLES);
    blitRender.setShader(blitShader.get());
    blitSlotParams = blitShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::BLIT_FS_PARAMS);
    if (!blitRender.endLoad(PIP_RTT | PIP_DEFAULT | PIP_RTT_NODEPTH | PIP_RTT_NOBLEND, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    blitLoaded = true;
}

void RenderSystem::destroyBlit(){
    if (blitLoaded){
        blitRender.destroy();
    }
    if (blitShader){
        blitShader.reset();
        ShaderPool::remove(ShaderType::BLIT, 0);
    }

    fixedResFramebuffer.destroy();
    sceneCopyFramebuffer.destroy();

    fixedResWidth = 0;
    fixedResHeight = 0;
    blitSlotParams = -1;
    blitLoaded = false;
}

bool RenderSystem::ensureFixedResFramebuffer(unsigned int width, unsigned int height, TextureFilter filter){
    if (width == 0 || height == 0)
        return false;

    if (fixedResFramebuffer.isCreated() && fixedResWidth == width && fixedResHeight == height
            && fixedResFramebuffer.getMinFilter() == filter)
        return true;

    fixedResFramebuffer.destroy();

    // offscreen scene color at the fixed game resolution; the min/mag filter is
    // what the upscale blit samples with (nearest = crisp pixels, linear = smooth)
    fixedResFramebuffer.setWidth(width);
    fixedResFramebuffer.setHeight(height);
    fixedResFramebuffer.setMinFilter(filter);
    fixedResFramebuffer.setMagFilter(filter);
    fixedResFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    fixedResFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    fixedResFramebuffer.create();

    fixedResWidth = width;
    fixedResHeight = height;

    return fixedResFramebuffer.isCreated();
}

void RenderSystem::renderBlit(TextureRender* source, FramebufferRender* destination, const Rect* viewport){
    // bars outside the viewport keep the background color (same as the direct
    // path, which clears the whole destination before applying the viewport)
    blitPassRender.setClearColor(scene->getBackgroundColor());

    if (destination){
        // source and destination are both flipped on GL, so no un-flip needed
        blitPassRender.startRenderPass(destination);
    }else{
        blitPassRender.startRenderPass();
    }
    if (viewport){
        blitPassRender.applyViewport(*viewport);
    }

    // the source was rendered flipped (PIP_RTT) but the GL swapchain is not
    drawBlit(source, destination ? PIP_RTT : PIP_DEFAULT, !destination && Engine::isOpenGL());
    blitPassRender.endRenderPass();
}

void RenderSystem::drawBlit(TextureRender* source, PipelineType pipeline, bool flipY){
    if (blitRender.beginDraw(pipeline)){
        ShaderData& bd = blitShader.get()->shaderData;
        blitRender.addTexture(bd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT, source);
        fs_blit.params = Vector4(flipY ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        blitRender.applyUniformBlock(blitSlotParams, sizeof(fs_blit_t), &fs_blit);
        blitRender.draw(0, 3, 1);
    }
}

void RenderSystem::renderFixedResolutionBlit(FramebufferRender* destination){
    // an explicit destination (the FXAA target) wins over the captured framebuffer, which is
    // what the chain presents when no anti-aliasing pass follows the upscale
    if (!destination && Engine::getFramebuffer()){
        if (!Engine::getFramebuffer()->isCreated()){
            Engine::getFramebuffer()->create();
        }
        destination = &Engine::getFramebuffer()->getRender();
    }
    const Rect viewport = Engine::getViewRect();
    renderBlit(&fixedResFramebuffer.getRender().getColorTexture(), destination, &viewport);
}

bool RenderSystem::ensureSceneCopyFramebuffer(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;

    if (sceneCopyFramebuffer.isCreated() && sceneCopyFramebuffer.getWidth() == width && sceneCopyFramebuffer.getHeight() == height)
        return true;

    sceneCopyFramebuffer.destroy();
    sceneCopyFramebuffer.setWidth(width);
    sceneCopyFramebuffer.setHeight(height);
    sceneCopyFramebuffer.setMinFilter(TextureFilter::LINEAR);
    sceneCopyFramebuffer.setMagFilter(TextureFilter::LINEAR);
    sceneCopyFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    sceneCopyFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    sceneCopyFramebuffer.create();

    return sceneCopyFramebuffer.isCreated();
}

void RenderSystem::renderSceneCopy(Framebuffer* source){
    // unblended, the captured alpha was already applied
    blitPassRender.startRenderPass(&sceneCopyFramebuffer.getRender());
    drawBlit(&source->getRender().getColorTexture(), PIP_RTT_NOBLEND, false);
    blitPassRender.endRenderPass();
}

void RenderSystem::needReloadPostProcess(){
    postProcessNeedReload = true;
}

void RenderSystem::needUpdatePostProcessUniforms(){
    postProcessNeedUpdateUniforms = true;
}

void RenderSystem::needUpdateDefaultShaderUniforms(ShaderType type){
    switch (type){
        case ShaderType::MESH:   flagInheritedUniforms(scene->getComponentArray<MeshComponent>());   break;
        case ShaderType::UI:     flagInheritedUniforms(scene->getComponentArray<UIComponent>());     break;
        case ShaderType::SKYBOX: flagInheritedUniforms(scene->getComponentArray<SkyComponent>());    break;
        case ShaderType::POINTS: flagInheritedUniforms(scene->getComponentArray<PointsComponent>()); break;
        case ShaderType::LINES:  flagInheritedUniforms(scene->getComponentArray<LinesComponent>());  break;
        case ShaderType::WATER:  flagInheritedUniforms(scene->getComponentArray<WaterComponent>());  break;
        default: break;
    }
}

void RenderSystem::loadPostProcess(){
    if (postProcessLoaded && !postProcessNeedReload)
        return;

    // a pass whose shader is still building aborts the attempt, so the partial
    // chain is always dropped before rebuilding
    for (int i = 0; i < (int)postProcessPasses.size(); i++){
        postProcessPasses[i].render.destroy();
        postProcessPasses[i].shader.reset();
    }
    postProcessPasses.clear();
    postProcessNeedReload = false;
    postProcessLoaded = false;
    postProcessNeedsDepth = false;
    postProcessNeedsGBuffer = false;

    const std::vector<PostProcessPass>& passes = scene->getPostProcessPasses();

    for (int i = 0; i < (int)passes.size(); i++){
        if (!passes[i].enabled)
            continue;

        PostProcessRuntime pass;
        pass.passIndex = i;

        // an empty path is the built-in passthrough, also the fallback of a failed fork
        pass.customId = ShaderPool::registerCustomShader(passes[i].shader);
        pass.shader = ShaderPool::get(ShaderType::POSTPROCESS, 0, pass.customId);
        if (pass.customId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::POSTPROCESS, 0, pass.customId)){
            // the pool already logged the failure; the panel marks the pass
            pass.customId = 0;
            pass.shader = ShaderPool::get(ShaderType::POSTPROCESS, 0);
        }
        // shaders may still be building (async in the editor)
        if (!pass.shader || !pass.shader->isCreated())
            return;

        pass.render.beginLoad(PrimitiveType::TRIANGLES);
        pass.render.setShader(pass.shader.get());
        // the chain never targets the swapchain directly, renderPostProcess blits for that
        if (!pass.render.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
            return;

        ShaderData& sd = pass.shader.get()->shaderData;
        pass.slotSceneColor = sd.getTextureIndexByName("u_sceneColorTexture");
        pass.slotDepth = sd.getTextureIndexByName("u_depthTexture");
        pass.slotGBuffer = sd.getTextureIndexByName("u_gbufferTexture");
        pass.slotSSAO = sd.getTextureIndexByName("u_ssaoTexture");

        if (pass.slotDepth.first != -1)
            postProcessNeedsDepth = true;
        if (pass.slotGBuffer.first != -1)
            postProcessNeedsGBuffer = true;

        // stored by name, so a renamed or removed member just drops
        if (pass.params.resolve(sd, "u_fs_postParams")){
            pass.params.writeValues(passes[i].uniforms);
        }

        postProcessPasses.push_back(std::move(pass));
    }

    postProcessLoaded = true;
    postProcessNeedUpdateUniforms = false;
}

// rewrites the pass values from the scene chain, keeping the compiled shaders
void RenderSystem::updatePostProcessUniforms(){
    if (!postProcessNeedUpdateUniforms || !postProcessLoaded)
        return;

    const std::vector<PostProcessPass>& passes = scene->getPostProcessPasses();
    for (int i = 0; i < (int)postProcessPasses.size(); i++){
        PostProcessRuntime& pass = postProcessPasses[i];
        if (pass.passIndex < (int)passes.size()){
            pass.params.writeValues(passes[pass.passIndex].uniforms);
        }
    }

    postProcessNeedUpdateUniforms = false;
}

void RenderSystem::destroyPostProcess(){
    for (int i = 0; i < (int)postProcessPasses.size(); i++){
        postProcessPasses[i].render.destroy();
        postProcessPasses[i].shader.reset();
        ShaderPool::remove(ShaderType::POSTPROCESS, 0, postProcessPasses[i].customId);
    }
    postProcessPasses.clear();

    postProcessFramebuffer[0].destroy();
    postProcessFramebuffer[1].destroy();

    postProcessWidth = 0;
    postProcessHeight = 0;
    postProcessNeedsDepth = false;
    postProcessNeedsGBuffer = false;
    postProcessNeedReload = false;
    postProcessNeedUpdateUniforms = false;
    postProcessLoaded = false;
}

bool RenderSystem::ensurePostProcessFramebuffers(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;

    if (postProcessFramebuffer[0].isCreated() && postProcessWidth == width && postProcessHeight == height)
        return true;

    // the chain ping-pongs between the two; its last pass writes the real destination
    for (int i = 0; i < 2; i++){
        postProcessFramebuffer[i].destroy();
        postProcessFramebuffer[i].setWidth(width);
        postProcessFramebuffer[i].setHeight(height);
        postProcessFramebuffer[i].setMinFilter(TextureFilter::LINEAR);
        postProcessFramebuffer[i].setMagFilter(TextureFilter::LINEAR);
        postProcessFramebuffer[i].setWrapU(TextureWrap::CLAMP_TO_EDGE);
        postProcessFramebuffer[i].setWrapV(TextureWrap::CLAMP_TO_EDGE);
        postProcessFramebuffer[i].create();
    }

    postProcessWidth = width;
    postProcessHeight = height;

    return postProcessFramebuffer[0].isCreated() && postProcessFramebuffer[1].isCreated();
}

void RenderSystem::renderPostProcess(FramebufferRender* destination){
    // depth is the SSR G-buffer or the chain's own pre-pass; the G-buffer
    // normal/roughness only exists while SSR is on
    TextureRender* depthTexture = &emptyBlack;
    TextureRender* gbufferTexture = &emptyBlack;
    if (postProcessNeedsGBuffer && gbufferFramebuffer.isCreated()){
        gbufferTexture = &gbufferFramebuffer.getRender().getColorAttachmentTexture(1);
    }
    if (postProcessNeedsDepth){
        if (gbufferFramebuffer.isCreated()){
            depthTexture = &gbufferFramebuffer.getRender().getColorAttachmentTexture(0);
        }else if (ssaoDepthFramebuffer.isCreated()){
            depthTexture = &ssaoDepthFramebuffer.getRender().getColorTexture();
        }
    }
    TextureRender* ssaoTexture = currentSSAOTexture ? currentSSAOTexture : &emptyWhite;

    // the color pass (or the SSR composite) was redirected into buffer 0
    TextureRender* input = &postProcessFramebuffer[0].getRender().getColorTexture();
    int pingPong = 1;

    updatePostProcessUniforms();

    for (int i = 0; i < (int)postProcessPasses.size(); i++){
        PostProcessRuntime& pass = postProcessPasses[i];
        bool writeDestination = destination && (i + 1 == (int)postProcessPasses.size());

        if (writeDestination){
            postProcessPassRender.startRenderPass(destination);
        }else{
            postProcessPassRender.startRenderPass(&postProcessFramebuffer[pingPong].getRender());
        }

        if (pass.render.beginDraw(PIP_RTT)){
            pass.render.addTexture(pass.slotSceneColor, ShaderStageType::FRAGMENT, input);
            pass.render.addTexture(pass.slotDepth, ShaderStageType::FRAGMENT, depthTexture);
            pass.render.addTexture(pass.slotGBuffer, ShaderStageType::FRAGMENT, gbufferTexture);
            pass.render.addTexture(pass.slotSSAO, ShaderStageType::FRAGMENT, ssaoTexture);
            applyCustomUniforms(pass.render, pass.params, frameTime, Vector2((float)postProcessWidth, (float)postProcessHeight));
            pass.render.draw(0, 3, 1);
        }
        postProcessPassRender.endRenderPass();

        if (!writeDestination){
            input = &postProcessFramebuffer[pingPong].getRender().getColorTexture();
            pingPong = 1 - pingPong;
        }
    }

    // the swapchain is written by the blit pass, which handles the GL orientation flip
    if (!destination){
        const Rect viewport = Engine::getViewRect();
        renderBlit(input, nullptr, &viewport);
    }
}

// Bloom + tone map. One load for the whole HDR chain: a pass whose shader is still
// building (async in the editor) aborts the attempt, so a half-built chain is never drawn.
void RenderSystem::loadHDR(){
    if (hdrLoaded)
        return;

    bloomThresholdShader = ShaderPool::get(ShaderType::BLOOM_THRESHOLD, 0);
    bloomDownsampleShader = ShaderPool::get(ShaderType::BLOOM_DOWNSAMPLE, 0);
    bloomUpsampleShader = ShaderPool::get(ShaderType::BLOOM_UPSAMPLE, 0);
    bloomCompositeShader = ShaderPool::get(ShaderType::BLOOM_COMPOSITE, 0);
    tonemapShader = ShaderPool::get(ShaderType::TONEMAP, 0);

    if (!bloomThresholdShader || !bloomThresholdShader->isCreated() ||
            !bloomDownsampleShader || !bloomDownsampleShader->isCreated() ||
            !bloomUpsampleShader || !bloomUpsampleShader->isCreated() ||
            !bloomCompositeShader || !bloomCompositeShader->isCreated())
        return;

    // every bloom pass writes an offscreen level
    ColorFormat bloomTarget = ColorFormat::RGBA16F;
    bloomThresholdRender.setRenderTargetColorFormat(bloomTarget);
    bloomThresholdRender.beginLoad(PrimitiveType::TRIANGLES);
    bloomThresholdRender.setShader(bloomThresholdShader.get());
    bloomThresholdSlotParams = bloomThresholdShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::BLOOM_THRESHOLD_FS_PARAMS);
    if (!bloomThresholdRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    bloomDownsampleRender.setRenderTargetColorFormat(bloomTarget);
    bloomDownsampleRender.beginLoad(PrimitiveType::TRIANGLES);
    bloomDownsampleRender.setShader(bloomDownsampleShader.get());
    bloomDownsampleSlotParams = bloomDownsampleShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::BLOOM_DOWNSAMPLE_FS_PARAMS);
    if (!bloomDownsampleRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    bloomUpsampleRender.setRenderTargetColorFormat(bloomTarget);
    bloomUpsampleRender.beginLoad(PrimitiveType::TRIANGLES);
    bloomUpsampleRender.setShader(bloomUpsampleShader.get());
    bloomUpsampleSlotParams = bloomUpsampleShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::BLOOM_UPSAMPLE_FS_PARAMS);
    if (!bloomUpsampleRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    bloomCompositeRender.setRenderTargetColorFormat(bloomTarget);
    bloomCompositeRender.beginLoad(PrimitiveType::TRIANGLES);
    bloomCompositeRender.setShader(bloomCompositeShader.get());
    bloomCompositeSlotParams = bloomCompositeShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::BLOOM_COMPOSITE_FS_PARAMS);
    if (!bloomCompositeRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    if (!tonemapShader || !tonemapShader->isCreated())
        return;

    // the tone map ends the chain: an offscreen destination or the swapchain
    tonemapRender.beginLoad(PrimitiveType::TRIANGLES);
    tonemapRender.setShader(tonemapShader.get());
    tonemapSlotParams = tonemapShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::TONEMAP_FS_PARAMS);
    if (!tonemapRender.endLoad(PIP_RTT | PIP_DEFAULT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    // Eye adaptation writes one 1x1 texel that the tone map samples. It is optional: a scene
    // that never turns auto exposure on does not run it, and a failed load must not take the
    // bloom chain down with it.
    exposureShader = ShaderPool::get(ShaderType::EXPOSURE, 0);
    if (exposureShader && exposureShader->isCreated()){
        exposureRender.setRenderTargetColorFormat(ColorFormat::R16F);
        exposureRender.beginLoad(PrimitiveType::TRIANGLES);
        exposureRender.setShader(exposureShader.get());
        exposureSlotParams = exposureShader->shaderData.getUniformBlockIndex(UniformBlockType::EXPOSURE_FS_PARAMS);
        exposureLoaded = exposureRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW);
    }

    bloomLoaded = true;
    tonemapLoaded = true;
    hdrLoaded = true;
}

void RenderSystem::destroyHDR(){
    if (hdrLoaded){
        bloomThresholdRender.destroy();
        bloomDownsampleRender.destroy();
        bloomUpsampleRender.destroy();
        bloomCompositeRender.destroy();
        tonemapRender.destroy();
        if (exposureLoaded)
            exposureRender.destroy();
    }

    if (lut3DLoaded)
        lut3DTexture.destroyTexture();
    lut3DLoaded = false;
    lut3DSize = 0;
    lut3DPath.clear();
    lut3DPixels.clear();
    if (bloomThresholdShader){
        bloomThresholdShader.reset();
        ShaderPool::remove(ShaderType::BLOOM_THRESHOLD, 0);
    }
    if (bloomDownsampleShader){
        bloomDownsampleShader.reset();
        ShaderPool::remove(ShaderType::BLOOM_DOWNSAMPLE, 0);
    }
    if (bloomUpsampleShader){
        bloomUpsampleShader.reset();
        ShaderPool::remove(ShaderType::BLOOM_UPSAMPLE, 0);
    }
    if (bloomCompositeShader){
        bloomCompositeShader.reset();
        ShaderPool::remove(ShaderType::BLOOM_COMPOSITE, 0);
    }
    if (tonemapShader){
        tonemapShader.reset();
        ShaderPool::remove(ShaderType::TONEMAP, 0);
    }
    if (exposureShader){
        exposureShader.reset();
        ShaderPool::remove(ShaderType::EXPOSURE, 0);
    }

    bloomThresholdFramebuffer.destroy();
    for (int i = 0; i < MAX_BLOOM_LEVELS; i++){
        bloomDownsampleFramebuffers[i].destroy();
        bloomUpsampleFramebuffers[i].destroy();
    }
    hdrFramebuffer.destroy();
    for (int i = 0; i < 2; i++)
        exposureFramebuffers[i].destroy();

    bloomWidth = 0;
    bloomHeight = 0;
    hdrWidth = 0;
    hdrHeight = 0;
    activeBloomMipLevels = 0;
    bloomThresholdSlotParams = -1;
    bloomDownsampleSlotParams = -1;
    bloomUpsampleSlotParams = -1;
    bloomCompositeSlotParams = -1;
    tonemapSlotParams = -1;
    exposureSlotParams = -1;
    exposureWriteIndex = 0;
    bloomLoaded = false;
    tonemapLoaded = false;
    exposureLoaded = false;
    hdrLoaded = false;
}

// level k of the chain: k = 0 is the half-resolution bright pass, k >= 1 halves again
static void bloomLevelSize(unsigned int width, unsigned int height, int k, unsigned int& outWidth, unsigned int& outHeight){
    outWidth = width;
    outHeight = height;
    for (int i = 0; i <= k; i++){
        outWidth = outWidth > 2 ? outWidth / 2 : 1;
        outHeight = outHeight > 2 ? outHeight / 2 : 1;
    }
}

bool RenderSystem::ensureHDRFramebuffers(unsigned int width, unsigned int height, int levels){
    if (width == 0 || height == 0 || levels < 1)
        return false;

    if (levels > MAX_BLOOM_LEVELS)
        levels = MAX_BLOOM_LEVELS;

    if (hdrFramebuffer.isCreated() && hdrWidth == width && hdrHeight == height && activeBloomMipLevels == levels)
        return true;

    hdrFramebuffer.destroy();
    for (int i = 0; i < MAX_BLOOM_LEVELS; i++){
        bloomDownsampleFramebuffers[i].destroy();
        bloomUpsampleFramebuffers[i].destroy();
    }

    // the bloom targets are sampled with a magnifying filter, which is the cheap half
    // of the upsample; float format because the bright pass is not clamped to 1.0
    ColorFormat hdr = ColorFormat::RGBA16F;

    bloomThresholdFramebuffer.setColorAttachments(1, &hdr);
    bloomLevelSize(width, height, 0, bloomWidth, bloomHeight);
    bloomThresholdFramebuffer.setWidth(bloomWidth);
    bloomThresholdFramebuffer.setHeight(bloomHeight);
    bloomThresholdFramebuffer.setMinFilter(TextureFilter::LINEAR);
    bloomThresholdFramebuffer.setMagFilter(TextureFilter::LINEAR);
    bloomThresholdFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    bloomThresholdFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    bloomThresholdFramebuffer.create();

    for (int k = 1; k < levels; k++){
        unsigned int lw, lh, pw, ph;
        bloomLevelSize(width, height, k, lw, lh);
        bloomLevelSize(width, height, k - 1, pw, ph);

        Framebuffer* down = &bloomDownsampleFramebuffers[k - 1];
        down->setColorAttachments(1, &hdr);
        down->setWidth(lw);
        down->setHeight(lh);
        down->setMinFilter(TextureFilter::LINEAR);
        down->setMagFilter(TextureFilter::LINEAR);
        down->setWrapU(TextureWrap::CLAMP_TO_EDGE);
        down->setWrapV(TextureWrap::CLAMP_TO_EDGE);
        down->create();

        // the accumulated level of the upsample chain, at the size it writes into,
        // which is the level above the one being read (up[k-1] holds level k-1)
        Framebuffer* up = &bloomUpsampleFramebuffers[k - 1];
        up->setColorAttachments(1, &hdr);
        up->setWidth(pw);
        up->setHeight(ph);
        up->setMinFilter(TextureFilter::LINEAR);
        up->setMagFilter(TextureFilter::LINEAR);
        up->setWrapU(TextureWrap::CLAMP_TO_EDGE);
        up->setWrapV(TextureWrap::CLAMP_TO_EDGE);
        up->create();
    }

    hdrFramebuffer.setColorAttachments(1, &hdr);
    hdrFramebuffer.setWidth(width);
    hdrFramebuffer.setHeight(height);
    hdrFramebuffer.setMinFilter(TextureFilter::LINEAR);
    hdrFramebuffer.setMagFilter(TextureFilter::LINEAR);
    hdrFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    hdrFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    hdrFramebuffer.create();

    hdrWidth = width;
    hdrHeight = height;
    activeBloomMipLevels = levels;

    // once per (re)build, so a headless run can prove the chain really exists by its log
    Log::verbose("HDR chain ready: %d bloom levels at %ux%u", levels, width, height);
    hdrFeatureMask = 0;

    return hdrFramebuffer.isCreated() && bloomThresholdFramebuffer.isCreated();
}

bool RenderSystem::ensureColorGradingLUT(){
    const std::string& path = scene->getColorGradingLUTPath();
    if (path == lut3DPath)
        return lut3DLoaded;

    if (lut3DLoaded)
        lut3DTexture.destroyTexture();
    lut3DLoaded = false;
    lut3DSize = 0;
    lut3DPath = path;
    lut3DPixels.clear();

    if (path.empty())
        return false;

    TextureData source(path.c_str());
    const int size = source.getHeight();
    const int width = source.getWidth();
    if (!source.getData() || source.getColorFormat() != ColorFormat::RGBA ||
            source.getChannels() != 4 || size < 2 || width != size * size || size > 64){
        Log::error("Color grading LUT must be an RGBA strip with dimensions N*N x N (2 <= N <= 64): %s",
                   path.c_str());
        return false;
    }

    const unsigned char* sourcePixels = static_cast<const unsigned char*>(source.getData());
    const size_t sliceBytes = static_cast<size_t>(size) * static_cast<size_t>(size) * 4u;
    lut3DPixels.resize(sliceBytes * static_cast<size_t>(size));
    for (int blue = 0; blue < size; blue++){
        unsigned char* destination = lut3DPixels.data() + sliceBytes * static_cast<size_t>(blue);
        for (int green = 0; green < size; green++){
            for (int red = 0; red < size; red++){
                const size_t sourceIndex = (static_cast<size_t>(green) * static_cast<size_t>(width) +
                    static_cast<size_t>(blue * size + red)) * 4u;
                const size_t destinationIndex = (static_cast<size_t>(green) * static_cast<size_t>(size) +
                    static_cast<size_t>(red)) * 4u;
                std::copy_n(sourcePixels + sourceIndex, 4u, destination + destinationIndex);
            }
        }
    }

    std::vector<void*> slices(static_cast<size_t>(size));
    std::vector<size_t> sliceSizes(static_cast<size_t>(size), sliceBytes);
    for (int blue = 0; blue < size; blue++)
        slices[static_cast<size_t>(blue)] = lut3DPixels.data() + sliceBytes * static_cast<size_t>(blue);

    lut3DLoaded = lut3DTexture.createTexture("scene|color-grading|" + path, size, size,
        ColorFormat::RGBA, TextureType::TEXTURE_ARRAY, size, slices.data(), sliceSizes.data(),
        TextureFilter::LINEAR, TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE,
        TextureWrap::CLAMP_TO_EDGE);
    if (!lut3DLoaded){
        lut3DSize = 0;
        lut3DPixels.clear();
        Log::error("Failed to upload color grading LUT: %s", path.c_str());
    }else{
        lut3DSize = size;
        Log::verbose("Color grading LUT ready: %s (%dx%dx%d)", path.c_str(), size, size, size);
    }
    return lut3DLoaded;
}

// Two 1x1 targets, swapped every frame: the adaptation pass reads the previous frame's texel and
// writes the next one's, so eye adaptation needs no CPU readback and no third buffer to copy into.
bool RenderSystem::ensureExposureFramebuffers(){
    if (exposureFramebuffers[0].isCreated() && exposureFramebuffers[1].isCreated())
        return true;

    ColorFormat format = ColorFormat::R16F;
    for (int i = 0; i < 2; i++){
        Framebuffer* fbo = &exposureFramebuffers[i];
        if (fbo->isCreated())
            continue;
        fbo->setColorAttachments(1, &format);
        fbo->setWidth(1);
        fbo->setHeight(1);
        fbo->setMinFilter(TextureFilter::NEAREST);
        fbo->setMagFilter(TextureFilter::NEAREST);
        fbo->setWrapU(TextureWrap::CLAMP_TO_EDGE);
        fbo->setWrapV(TextureWrap::CLAMP_TO_EDGE);
        fbo->create();
    }
    const bool ready = exposureFramebuffers[0].isCreated() && exposureFramebuffers[1].isCreated();
    if (ready)
        Log::verbose("eye adaptation ready: 1x1 meter over two ping-pong targets");
    return ready;
}

// sceneColorFramebuffer -> bloom chain -> HDR target -> tone map -> destination
void RenderSystem::renderHDR(FramebufferRender* destination){
    if (!hdrLoaded || !bloomLoaded || !sceneColorFramebuffer.isCreated())
        return;

    const int levels = activeBloomMipLevels;
    TextureRender* sceneColor = &sceneColorFramebuffer.getRender().getColorTexture();

    // The four HDR features switch independently and the frame loop would otherwise print every
    // frame: log the set only when it changes, so a headless probe gets one line per toggle.
    const unsigned featureMask = (scene->isBloomEnabled() ? 1u : 0u)
        | (scene->isTonemapEnabled() ? 2u : 0u)
        | (scene->isColorGradingEnabled() ? 4u : 0u)
        | (scene->isAutoExposureEnabled() ? 8u : 0u);
    if (featureMask != hdrFeatureMask){
        hdrFeatureMask = featureMask;
        Log::verbose("HDR features: bloom %d tone map %d grading %d auto exposure %d",
                    scene->isBloomEnabled() ? 1 : 0, scene->isTonemapEnabled() ? 1 : 0,
                    scene->isColorGradingEnabled() ? 1 : 0, scene->isAutoExposureEnabled() ? 1 : 0);
    }

    const bool hasLUT = scene->isColorGradingEnabled() && ensureColorGradingLUT();

    // The scene target is sRGB-encoded whatever its pixel format: mesh.frag ends in
    // linearTosRGB and no mesh variant without it is compiled, so the chain decodes on entry
    // and the tone map re-encodes on exit. The float target does not change that - it only
    // stops an encoded value above 1.0 from clamping (see ensureSceneColorFramebuffer), which
    // is why this is not sceneColorHDR ? 0.0f : 1.0f.
    const float srgbSource = 1.0f;

    bloomPassRender.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));

    if (scene->isBloomEnabled() && levels > 0){
        // bright pass, straight into level 0
        fs_bloom_threshold.params = Vector4(scene->getBloomThreshold(), scene->getBloomSoftKnee(),
                                             srgbSource, 0.0f);
        bloomPassRender.startRenderPass(&bloomThresholdFramebuffer.getRender());
        if (bloomThresholdRender.beginDraw(PIP_RTT)){
            ShaderData& sd = bloomThresholdShader.get()->shaderData;
            bloomThresholdRender.addTexture(sd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT, sceneColor);
            bloomThresholdRender.applyUniformBlock(bloomThresholdSlotParams, sizeof(fs_bloom_threshold_t), &fs_bloom_threshold);
            bloomThresholdRender.draw(0, 3, 1);
        }
        bloomPassRender.endRenderPass();

        // level k reads level k-1
        for (int k = 1; k < levels; k++){
            unsigned int sw, sh;
            bloomLevelSize(sceneColorFramebuffer.getWidth(), sceneColorFramebuffer.getHeight(), k - 1, sw, sh);
            fs_bloom_downsample.texelSize = Vector4(1.0f / (float)sw, 1.0f / (float)sh, 0.0f, 0.0f);

            Framebuffer* src = k - 1 == 0 ? &bloomThresholdFramebuffer : &bloomDownsampleFramebuffers[k - 2];
            bloomPassRender.startRenderPass(&bloomDownsampleFramebuffers[k - 1].getRender());
            if (bloomDownsampleRender.beginDraw(PIP_RTT)){
                ShaderData& sd = bloomDownsampleShader.get()->shaderData;
                bloomDownsampleRender.addTexture(sd.getTextureIndex(TextureShaderType::BLOOMTEXTURE0), ShaderStageType::FRAGMENT,
                                                 &src->getRender().getColorTexture());
                bloomDownsampleRender.applyUniformBlock(bloomDownsampleSlotParams, sizeof(fs_bloom_downsample_t), &fs_bloom_downsample);
                bloomDownsampleRender.draw(0, 3, 1);
            }
            bloomPassRender.endRenderPass();
        }

        // accumulate from the smallest level up: the result of the previous step is the
        // wide source, the downsampled level of this size is the detail added to it
        for (int k = levels - 2; k >= 0; k--){
            unsigned int sw, sh;
            Framebuffer* wide;
            if (k == levels - 2){
                wide = levels == 1 ? &bloomThresholdFramebuffer : &bloomDownsampleFramebuffers[levels - 2];
            }else{
                wide = &bloomUpsampleFramebuffers[k + 1];
            }
            // wide is the level below this one, so its size is what the tent filter steps by
            bloomLevelSize(sceneColorFramebuffer.getWidth(), sceneColorFramebuffer.getHeight(), k + 1, sw, sh);

            Framebuffer* detail = k == 0 ? &bloomThresholdFramebuffer : &bloomDownsampleFramebuffers[k - 1];

            fs_bloom_upsample.texelSize = Vector4(1.0f / (float)sw, 1.0f / (float)sh, 1.0f, 0.0f);

            bloomPassRender.startRenderPass(&bloomUpsampleFramebuffers[k].getRender());
            if (bloomUpsampleRender.beginDraw(PIP_RTT)){
                ShaderData& sd = bloomUpsampleShader.get()->shaderData;
                bloomUpsampleRender.addTexture(sd.getTextureIndex(TextureShaderType::BLOOMTEXTURE0), ShaderStageType::FRAGMENT,
                                               &wide->getRender().getColorTexture());
                bloomUpsampleRender.addTexture(sd.getTextureIndex(TextureShaderType::BLOOMTEXTURE1), ShaderStageType::FRAGMENT,
                                                &detail->getRender().getColorTexture());
                bloomUpsampleRender.applyUniformBlock(bloomUpsampleSlotParams, sizeof(fs_bloom_upsample_t), &fs_bloom_upsample);
                bloomUpsampleRender.draw(0, 3, 1);
            }
            bloomPassRender.endRenderPass();
        }
    }

    // composite: scene + bloom, still linear, into the HDR target
    TextureRender* bloom = levels == 1 ? &bloomThresholdFramebuffer.getRender().getColorTexture()
                                       : &bloomUpsampleFramebuffers[0].getRender().getColorTexture();

    fs_bloom_composite.params = Vector4(scene->isBloomEnabled() ? scene->getBloomIntensity() : 0.0f,
                                        srgbSource, scene->getBloomDirtIntensity(), 0.0f);

    bloomPassRender.startRenderPass(&hdrFramebuffer.getRender());
    if (bloomCompositeRender.beginDraw(PIP_RTT)){
        ShaderData& sd = bloomCompositeShader.get()->shaderData;
        bloomCompositeRender.addTexture(sd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT, sceneColor);
        bloomCompositeRender.addTexture(sd.getTextureIndex(TextureShaderType::BLOOMTEXTURE0), ShaderStageType::FRAGMENT,
                                        scene->isBloomEnabled() ? bloom : &emptyBlack);
        bloomCompositeRender.applyUniformBlock(bloomCompositeSlotParams, sizeof(fs_bloom_composite_t), &fs_bloom_composite);
        bloomCompositeRender.draw(0, 3, 1);
    }
    bloomPassRender.endRenderPass();

    // Eye adaptation reads the finished HDR target: the exposure has to be decided on the light
    // the scene actually emits, before the curve compresses it. It writes a 1x1 texel and the tone
    // map samples that texel, so the loop closes on the GPU with no readback stall.
    int exposureLatest = -1;
    if (scene->isAutoExposureEnabled() && exposureLoaded && ensureExposureFramebuffers()){
        const int write = exposureWriteIndex;
        const int read = 1 - write;

        fs_exposure.params = Vector4(exp2(scene->getAutoExposureMinEV()), exp2(scene->getAutoExposureMaxEV()),
                                     scene->getAutoExposureSpeed(), scene->getExposureCompensation());
        fs_exposure.adapt = Vector4(Engine::getDeltatime(), scene->getExposure(), 0.0f, 0.0f);

        exposurePassRender.setClearColor(Vector4(scene->getExposure(), 0.0f, 0.0f, 1.0f));
        exposurePassRender.startRenderPass(&exposureFramebuffers[write].getRender());
        if (exposureRender.beginDraw(PIP_RTT)){
            ShaderData& sd = exposureShader.get()->shaderData;
            exposureRender.addTexture(sd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT,
                                      &hdrFramebuffer.getRender().getColorTexture());
            exposureRender.addTexture(sd.getTextureIndex(TextureShaderType::EXPOSURETEXTURE), ShaderStageType::FRAGMENT,
                                      &exposureFramebuffers[read].getRender().getColorTexture());
            exposureRender.applyUniformBlock(exposureSlotParams, sizeof(fs_exposure_t), &fs_exposure);
            exposureRender.draw(0, 3, 1);
        }
        exposurePassRender.endRenderPass();

        exposureWriteIndex = read;
        exposureLatest = write;
    }

    // tone map: linear HDR in, display-encoded out, to the real destination
    const bool toSwapchain = destination == nullptr;
    if (toSwapchain){
        tonemapPassRender.setClearColor(scene->getBackgroundColor());
        tonemapPassRender.startRenderPass();
        tonemapPassRender.applyViewport(Engine::getViewRect());
    }else{
        tonemapPassRender.startRenderPass(destination);
    }
    if (tonemapRender.beginDraw(toSwapchain ? PIP_DEFAULT : PIP_RTT)){
        ShaderData& sd = tonemapShader.get()->shaderData;
        fs_tonemap.params = Vector4(scene->getExposure(),
                                    scene->isTonemapEnabled() ? (float)scene->getTonemapMode() : -1.0f,
                                    11.2f,
                                    toSwapchain && Engine::isOpenGL() ? 1.0f : 0.0f);
        fs_tonemap.grade = Vector4(scene->getColorGradingSaturation(), scene->getColorGradingContrast(),
                                   scene->getColorGradingBrightness(), scene->getColorGradingHueShift());
        fs_tonemap.grade2 = Vector4(scene->getColorGradingTemperature(), scene->getColorGradingTint(),
                                    scene->isColorGradingEnabled() ? 1.0f : 0.0f,
                                    exposureLatest >= 0 ? 1.0f : 0.0f);
        fs_tonemap.lut = Vector4(hasLUT ? 1.0f : 0.0f, static_cast<float>(lut3DSize), 0.0f, 0.0f);
        tonemapRender.addTexture(sd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT,
                                 &hdrFramebuffer.getRender().getColorTexture());
        // the sampler is declared either way, so give it something inert when auto exposure is off
        tonemapRender.addTexture(sd.getTextureIndex(TextureShaderType::EXPOSURETEXTURE), ShaderStageType::FRAGMENT,
                                 exposureLatest >= 0 ? &exposureFramebuffers[exposureLatest].getRender().getColorTexture()
                                                     : &emptyBlack);
        tonemapRender.addTexture(sd.getTextureIndex(TextureShaderType::LUT3DTEXTURE), ShaderStageType::FRAGMENT,
                                 hasLUT ? &lut3DTexture : &emptyArrayWhite);
        tonemapRender.applyUniformBlock(tonemapSlotParams, sizeof(fs_tonemap_t), &fs_tonemap);
        tonemapRender.draw(0, 3, 1);
    }
    tonemapPassRender.endRenderPass();
}

void RenderSystem::loadFXAA(){
    if (fxaaLoaded)
        return;

    fxaaShader = ShaderPool::get(ShaderType::FXAA, 0);
    if (!fxaaShader || !fxaaShader->isCreated())
        return;

    // like the tone map it may write an offscreen target or the swapchain, so both are built
    fxaaRender.beginLoad(PrimitiveType::TRIANGLES);
    fxaaRender.setShader(fxaaShader.get());
    fxaaSlotParams = fxaaShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::FXAA_FS_PARAMS);
    if (!fxaaRender.endLoad(PIP_RTT | PIP_DEFAULT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    fxaaLoaded = true;
    Log::verbose("FXAA ready");
}

void RenderSystem::destroyFXAA(){
    if (fxaaLoaded){
        fxaaRender.destroy();
        fxaaLoaded = false;
    }
    if (fxaaShader){
        fxaaShader.reset();
        ShaderPool::remove(ShaderType::FXAA, 0);
    }

    fxaaFramebuffer.destroy();
    fxaaWidth = 0;
    fxaaHeight = 0;
    fxaaSlotParams = -1;
}

bool RenderSystem::ensureFXAAFramebuffer(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;

    if (fxaaFramebuffer.isCreated() && fxaaWidth == width && fxaaHeight == height)
        return true;

    fxaaFramebuffer.destroy();

    // The FXAA taps sit on this target's own texel grid, so the filter has to be the smooth
    // one: along-edge samples land between texels and NEAREST would drop them onto the very
    // pixels the pass is trying to blend.
    fxaaFramebuffer.setWidth(width);
    fxaaFramebuffer.setHeight(height);
    fxaaFramebuffer.setMinFilter(TextureFilter::LINEAR);
    fxaaFramebuffer.setMagFilter(TextureFilter::LINEAR);
    fxaaFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    fxaaFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    fxaaFramebuffer.create();

    fxaaWidth = width;
    fxaaHeight = height;

    return fxaaFramebuffer.isCreated();
}

// presents fxaaFramebuffer to destination (the swapchain when null)
void RenderSystem::renderFXAA(FramebufferRender* destination){
    if (!fxaaLoaded || !fxaaFramebuffer.isCreated())
        return;

    const bool toSwapchain = destination == nullptr;
    if (toSwapchain){
        fxaaPassRender.setClearColor(scene->getBackgroundColor());
        fxaaPassRender.startRenderPass();
        fxaaPassRender.applyViewport(Engine::getViewRect());
    }else{
        fxaaPassRender.startRenderPass(destination);
    }
    if (fxaaRender.beginDraw(toSwapchain ? PIP_DEFAULT : PIP_RTT)){
        ShaderData& sd = fxaaShader.get()->shaderData;
        // The scene target is top-left and the GL swapchain is bottom-left, so only the
        // swapchain draw has to un-flip its coordinates - and the taps follow uv, not the
        // texel grid, so the flip costs the filter nothing.
        fs_fxaa.params = Vector4(1.0f / (float)fxaaWidth, 1.0f / (float)fxaaHeight,
                                 scene->getFXAAIntensity(),
                                 toSwapchain && Engine::isOpenGL() ? 1.0f : 0.0f);
        fxaaRender.addTexture(sd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT,
                              &fxaaFramebuffer.getRender().getColorTexture());
        fxaaRender.applyUniformBlock(fxaaSlotParams, sizeof(fs_fxaa_t), &fs_fxaa);
        fxaaRender.draw(0, 3, 1);
    }
    fxaaPassRender.endRenderPass();
}

// Depth of field: circle of confusion from the shared depth pre-pass, a half-resolution
// separable Gaussian weighted by it, then the mix back over the scene color.
void RenderSystem::loadDOF(){
    if (dofLoaded)
        return;

    dofCocShader = ShaderPool::get(ShaderType::DOF_COC, 0);
    dofBlurShader = ShaderPool::get(ShaderType::DOF_BLUR, 0);
    dofCompositeShader = ShaderPool::get(ShaderType::DOF_COMPOSITE, 0);

    // a pass whose shader is still building (async in the editor) aborts the attempt, so a
    // half-built chain is never drawn - the next frame retries
    if (!dofCocShader || !dofCocShader->isCreated() ||
            !dofBlurShader || !dofBlurShader->isCreated() ||
            !dofCompositeShader || !dofCompositeShader->isCreated())
        return;

    dofCocRender.beginLoad(PrimitiveType::TRIANGLES);
    dofCocRender.setShader(dofCocShader.get());
    dofCocSlotParams = dofCocShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::DOF_COC_FS_PARAMS);
    if (!dofCocRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    dofBlurRender.beginLoad(PrimitiveType::TRIANGLES);
    dofBlurRender.setShader(dofBlurShader.get());
    dofBlurSlotParams = dofBlurShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::DOF_BLUR_FS_PARAMS);
    if (!dofBlurRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    // offscreen only: the result is copied back into the scene color, never presented
    dofCompositeRender.beginLoad(PrimitiveType::TRIANGLES);
    dofCompositeRender.setShader(dofCompositeShader.get());
    dofCompositeSlotParams = dofCompositeShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::DOF_COMPOSITE_FS_PARAMS);
    if (!dofCompositeRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    dofLoaded = true;
    Log::verbose("DOF ready");
}

void RenderSystem::destroyDOF(){
    if (dofLoaded){
        dofCocRender.destroy();
        dofBlurRender.destroy();
        dofCompositeRender.destroy();
        dofLoaded = false;
    }
    if (dofCocShader){
        dofCocShader.reset();
        ShaderPool::remove(ShaderType::DOF_COC, 0);
    }
    if (dofBlurShader){
        dofBlurShader.reset();
        ShaderPool::remove(ShaderType::DOF_BLUR, 0);
    }
    if (dofCompositeShader){
        dofCompositeShader.reset();
        ShaderPool::remove(ShaderType::DOF_COMPOSITE, 0);
    }

    dofCocFramebuffer.destroy();
    dofBlurFramebuffer[0].destroy();
    dofBlurFramebuffer[1].destroy();
    dofSceneFramebuffer.destroy();

    dofWidth = 0;
    dofHeight = 0;
    dofHalfWidth = 0;
    dofHalfHeight = 0;
    dofCocSlotParams = -1;
    dofBlurSlotParams = -1;
    dofCompositeSlotParams = -1;
}

bool RenderSystem::ensureDOFFramebuffers(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;

    if (dofSceneFramebuffer.isCreated() && dofWidth == width && dofHeight == height)
        return true;

    dofCocFramebuffer.destroy();
    dofBlurFramebuffer[0].destroy();
    dofBlurFramebuffer[1].destroy();
    dofSceneFramebuffer.destroy();

    // Half the scene: a full-resolution blur would cost the window the CoC is there to
    // limit, and the composite magnifies the result with LINEAR, which is the cheap half
    // of the upsample. The blur reads its own target's texel grid through the CoC, so the
    // two have to stay the same size.
    dofHalfWidth = width > 2 ? width / 2 : 1;
    dofHalfHeight = height > 2 ? height / 2 : 1;

    Framebuffer* targets[] = {&dofCocFramebuffer, &dofBlurFramebuffer[0], &dofBlurFramebuffer[1]};
    for (Framebuffer* target : targets){
        target->setWidth(dofHalfWidth);
        target->setHeight(dofHalfHeight);
        target->setMinFilter(TextureFilter::LINEAR);
        target->setMagFilter(TextureFilter::LINEAR);
        target->setWrapU(TextureWrap::CLAMP_TO_EDGE);
        target->setWrapV(TextureWrap::CLAMP_TO_EDGE);
        target->create();
    }

    dofSceneFramebuffer.setWidth(width);
    dofSceneFramebuffer.setHeight(height);
    dofSceneFramebuffer.setMinFilter(TextureFilter::LINEAR);
    dofSceneFramebuffer.setMagFilter(TextureFilter::LINEAR);
    dofSceneFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    dofSceneFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    dofSceneFramebuffer.create();

    dofWidth = width;
    dofHeight = height;

    return dofCocFramebuffer.isCreated() && dofBlurFramebuffer[0].isCreated()
            && dofBlurFramebuffer[1].isCreated() && dofSceneFramebuffer.isCreated();
}

// Reads and rewrites sceneColorTarget in place, so every stage after this one keeps
// reading the same buffer it always did. SSR is excluded: its composite writes the
// destination rather than the scene color, so there would be nothing here to round-trip.
void RenderSystem::renderDOF(CameraComponent& camera, Framebuffer* sceneColorTarget, TextureRender* depthTexture){
    if (!dofLoaded || !sceneColorTarget || !sceneColorTarget->isCreated() || !depthTexture ||
            !dofCocFramebuffer.isCreated() || !dofBlurFramebuffer[0].isCreated() ||
            !dofBlurFramebuffer[1].isCreated() || !dofSceneFramebuffer.isCreated())
        return;

    const float flipGL = isRenderingFlipped(camera) ? 1.0f : 0.0f;

    // --- 1. circle of confusion, in scene-color space, from the pre-pass depth ---
    fs_dof_coc.invProjection = camera.projectionMatrix.inverse();
    fs_dof_coc.params0 = Vector4(scene->getDOFFocusDistance(), scene->getDOFAperture(),
                                 scene->getDOFFocusRange(), scene->getDOFMaxBlur());
    fs_dof_coc.params1 = Vector4(flipGL, 0.0f, 0.0f, 0.0f);

    dofPassRender.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    dofPassRender.startRenderPass(&dofCocFramebuffer.getRender());
    if (dofCocRender.beginDraw(PIP_RTT)){
        ShaderData& sd = dofCocShader.get()->shaderData;
        dofCocRender.addTexture(sd.getTextureIndexByName("u_depthTexture"), ShaderStageType::FRAGMENT, depthTexture);
        dofCocRender.applyUniformBlock(dofCocSlotParams, sizeof(fs_dof_coc_t), &fs_dof_coc);
        dofCocRender.draw(0, 3, 1);
    }
    dofPassRender.endRenderPass();

    TextureRender* cocTexture = &dofCocFramebuffer.getRender().getColorTexture();
    TextureRender* sharpTexture = &sceneColorTarget->getRender().getColorTexture();
    const float aspect = (float)dofWidth / (float)dofHeight;

    ShaderData& blurSd = dofBlurShader.get()->shaderData;
    const std::pair<int, int> blurSceneSlot = blurSd.getTextureIndexByName("u_sceneColorTexture");
    const std::pair<int, int> blurDepthSlot = blurSd.getTextureIndexByName("u_depthTexture");

    // --- 2. the two axes of the separable blur, each reading the previous result ---
    // The radius cap costs nothing here: it clamps taps that are already fixed in number.
    const float radiusCap = scene->getDOFMaxBlur();

    fs_dof_blur.params = Vector4(aspect, 0.0f, radiusCap, (float)dofHalfHeight);
    dofPassRender.startRenderPass(&dofBlurFramebuffer[0].getRender());
    if (dofBlurRender.beginDraw(PIP_RTT)){
        dofBlurRender.addTexture(blurSceneSlot, ShaderStageType::FRAGMENT, sharpTexture);
        dofBlurRender.addTexture(blurDepthSlot, ShaderStageType::FRAGMENT, cocTexture);
        dofBlurRender.applyUniformBlock(dofBlurSlotParams, sizeof(fs_dof_blur_t), &fs_dof_blur);
        dofBlurRender.draw(0, 3, 1);
    }
    dofPassRender.endRenderPass();

    fs_dof_blur.params = Vector4(aspect, 1.0f, radiusCap, (float)dofHalfHeight);
    dofPassRender.startRenderPass(&dofBlurFramebuffer[1].getRender());
    if (dofBlurRender.beginDraw(PIP_RTT)){
        dofBlurRender.addTexture(blurSceneSlot, ShaderStageType::FRAGMENT,
                                 &dofBlurFramebuffer[0].getRender().getColorTexture());
        dofBlurRender.addTexture(blurDepthSlot, ShaderStageType::FRAGMENT, cocTexture);
        dofBlurRender.applyUniformBlock(dofBlurSlotParams, sizeof(fs_dof_blur_t), &fs_dof_blur);
        dofBlurRender.draw(0, 3, 1);
    }
    dofPassRender.endRenderPass();

    // --- 3. mix the blurred half-res copy back over the sharp scene ---
    ShaderData& compSd = dofCompositeShader.get()->shaderData;
    dofPassRender.startRenderPass(&dofSceneFramebuffer.getRender());
    if (dofCompositeRender.beginDraw(PIP_RTT)){
        dofCompositeRender.addTexture(compSd.getTextureIndexByName("u_sceneColorTexture"), ShaderStageType::FRAGMENT, sharpTexture);
        dofCompositeRender.addTexture(compSd.getTextureIndexByName("u_dofTexture"), ShaderStageType::FRAGMENT,
                                      &dofBlurFramebuffer[1].getRender().getColorTexture());
        dofCompositeRender.addTexture(compSd.getTextureIndexByName("u_depthTexture"), ShaderStageType::FRAGMENT, cocTexture);
        // the composite only writes an offscreen target, so no swapchain flip; the far-field
        // bias is left at a neutral 1 until the settings carry one
        fs_dof_composite.params = Vector4(0.0f, 1.0f, 1.0f, 0.0f);
        dofCompositeRender.applyUniformBlock(dofCompositeSlotParams, sizeof(fs_dof_composite_t), &fs_dof_composite);
        dofCompositeRender.draw(0, 3, 1);
    }
    dofPassRender.endRenderPass();

    // back into the scene color, which this pass read as its sharp input - the two are
    // different textures, so neither the composite nor this copy samples what it writes
    loadBlit();
    if (blitLoaded){
        renderBlit(&dofSceneFramebuffer.getRender().getColorTexture(), &sceneColorTarget->getRender(), nullptr);
    }
}

// Atmosphere: one fullscreen pass that integrates the height fog along the view ray and
// marches the light shafts toward the sun. Like DOF it reads the offscreen scene color and
// the shared depth, writes its own full-res target, and blits the result back, so the
// post-process chain, the tone map, the upscale and FXAA all keep reading the same buffer.
void RenderSystem::loadAtmosphere(){
    if (atmosphereLoaded)
        return;

    atmosphereShader = ShaderPool::get(ShaderType::ATMOSPHERE, 0);
    // a pass whose shader is still building (async in the editor) aborts the attempt, so the
    // next frame retries instead of drawing a frame with no atmosphere at all
    if (!atmosphereShader || !atmosphereShader->isCreated())
        return;

    atmosphereRender.beginLoad(PrimitiveType::TRIANGLES);
    atmosphereRender.setShader(atmosphereShader.get());
    atmosphereSlotParams = atmosphereShader.get()->shaderData.getUniformBlockIndex(UniformBlockType::ATMOSPHERE_FS_PARAMS);
    if (!atmosphereRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    atmosphereLoaded = true;
    Log::verbose("Atmosphere ready");
}

void RenderSystem::destroyAtmosphere(){
    if (atmosphereLoaded){
        atmosphereRender.destroy();
        atmosphereLoaded = false;
    }
    if (atmosphereShader){
        atmosphereShader.reset();
        ShaderPool::remove(ShaderType::ATMOSPHERE, 0);
    }

    atmosphereFramebuffer.destroy();
    atmosphereWidth = 0;
    atmosphereHeight = 0;
    atmosphereSlotParams = -1;
}

bool RenderSystem::atmosphereRequested() const{
    const FogComponent* fog = disableFog ? nullptr : scene->findComponentFromIndex<FogComponent>(0);
    return fog && (fog->heightFog || fog->volumetricFog || fog->godRays || fog->cloudsEnabled);
}

bool RenderSystem::ensureAtmosphereFramebuffer(unsigned int width, unsigned int height){
    if (width == 0 || height == 0)
        return false;

    if (atmosphereFramebuffer.isCreated() && atmosphereWidth == width && atmosphereHeight == height)
        return true;

    atmosphereFramebuffer.destroy();

    // Full resolution: the shafts are the only screen-space effect whose edges are the
    // feature - a half-res march would feather an occluder over metres of screen.
    atmosphereFramebuffer.setWidth(width);
    atmosphereFramebuffer.setHeight(height);
    atmosphereFramebuffer.setMinFilter(TextureFilter::LINEAR);
    atmosphereFramebuffer.setMagFilter(TextureFilter::LINEAR);
    atmosphereFramebuffer.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    atmosphereFramebuffer.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    atmosphereFramebuffer.create();

    atmosphereWidth = width;
    atmosphereHeight = height;

    if (atmosphereFramebuffer.isCreated())
        Log::verbose("Atmosphere pass ready at %ux%u", width, height);

    return atmosphereFramebuffer.isCreated();
}

// One closed-form integral plus the sun's own screen position, so the whole look is driven
// from the FogComponent and the strongest directional light in the scene.
bool RenderSystem::buildAtmosphereParams(CameraComponent& camera, Transform& cameraTransform){
    FogComponent* fog = disableFog ? nullptr : scene->findComponentFromIndex<FogComponent>(0);
    if (!fog || (!fog->heightFog && !fog->volumetricFog && !fog->godRays && !fog->cloudsEnabled))
        return false;

    // The sun is whichever directional light shines hardest. Shafts need a sun to point at,
    // but a scene lit only by point lights still gets the aerial perspective, so nothing
    // here is allowed to require one.
    LightComponent* sun = nullptr;
    auto lights = scene->getComponentArray<LightComponent>();
    for (size_t i = 0; i < lights->size(); i++){
        LightComponent& light = lights->getComponentFromIndex(i);
        if (light.type != LightType::DIRECTIONAL || light.intensity <= 0.0f)
            continue;
        if (!sun || light.intensity > sun->intensity)
            sun = &light;
    }

    const Vector3& eye = cameraTransform.worldPosition;

    // The depth pre-pass stays in logical orientation (renderDepthPrePass), so this is the
    // unprojection its own texels need; the scene-color flip is the separate fog1.w flag.
    fs_atmosphere.invViewProjection = camera.viewProjectionMatrix.inverse();
    fs_atmosphere.cameraPos = Vector4(eye.x, eye.y, eye.z, 1.0f);

    // worldDirection is where the light travels; the phase and the sun's position want the
    // opposite way, toward the disk.
    Vector3 toSun = Vector3(0.0f, 1.0f, 0.0f);
    // Saturing, not linear: a light author can set anywhere from 1 to 1000, and the in-scatter
    // has to stay inside the range the tone map maps gracefully. Still monotonic, so a
    // brighter sun always fogs brighter.
    float sunWeight = 0.0f;
    if (sun){
        Vector3 travelled = sun->worldDirection;
        if (travelled.length() > 1e-6f)
            toSun = -travelled.normalized();
        sunWeight = sun->intensity > 0.0f ? sun->intensity / (1.0f + sun->intensity) : 0.0f;
    }
    fs_atmosphere.sunDirection = Vector4(toSun.x, toSun.y, toSun.z, sunWeight);

    // Projected as a far point because a directional light has no position. Behind the
    // camera (w <= 0) the antipode is used, which keeps the shafts arriving from the side
    // the sun really is on instead of from a wrapped-around coordinate.
    {
        Vector3 point = eye + toSun * 5000.0f;
        Vector4 clip = camera.viewProjectionMatrix * Vector4(point.x, point.y, point.z, 1.0f);
        if (clip.w <= 0.0f){
            point = eye - toSun * 5000.0f;
            clip = camera.viewProjectionMatrix * Vector4(point.x, point.y, point.z, 1.0f);
        }
        if (clip.w > 0.0f){
            const float ndcX = clip.x / clip.w;
            const float ndcY = clip.y / clip.w;
            // into the depth target's own orientation, which is what depthUVToNDCY expects
            const float uvY = Engine::isOpenGL() ? ndcY * 0.5f + 0.5f : 0.5f - ndcY * 0.5f;
            fs_atmosphere.sunScreen = Vector4(std::clamp(ndcX * 0.5f + 0.5f, 0.0f, 1.0f),
                                              std::clamp(uvY, 0.0f, 1.0f), 0.0f, 0.0f);
        }
    }

    fs_atmosphere.fog0 = Vector4(fog->density, fog->fogHeightFalloff, fog->fogGroundHeight, fog->fogFarRange);
    fs_atmosphere.fog1 = Vector4(fog->fogSunAmount, fog->fogScatteringG,
                                 (fog->heightFog || fog->volumetricFog) ? 1.0f : 0.0f,
                                 isRenderingFlipped(camera) ? 1.0f : 0.0f);
    fs_atmosphere.fogColor = Vector4(fog->color.x, fog->color.y, fog->color.z, 0.0f);
    fs_atmosphere.fogNoise = Vector4(fog->fogNoiseScale, fog->fogNoiseStrength,
                                     fog->fogNoiseSpeed, fog->fogNoiseEnabled ? 1.0f : 0.0f);
    fs_atmosphere.fogWind = Vector4(fog->fogNoiseWind.x, 0.0f, fog->fogNoiseWind.y,
                                    (float)Engine::getSystemTime());
    fs_atmosphere.fogVolume = Vector4(
        fog->volumetricFog ? 1.0f : 0.0f,
        (float)std::clamp(fog->volumetricFogSteps, 4u, 64u),
        std::max(fog->volumetricFogIntensity, 0.0f),
        (float)Engine::getSystemTime());

    if (sun)
        fs_atmosphere.sunColor = Vector4(sun->color.x, sun->color.y, sun->color.z, 0.0f);
    else
        fs_atmosphere.sunColor = Vector4(fog->fogSunColor.x, fog->fogSunColor.y, fog->fogSunColor.z, 0.0f);

    fs_atmosphere.rays0 = Vector4(fog->rayIntensity, fog->rayThreshold, fog->raySoftness, fog->rayDecay);
    fs_atmosphere.rays1 = Vector4(fog->rayDensity, fog->godRays ? 1.0f : 0.0f, 0.0f, 0.0f);
    fs_atmosphere.clouds0 = Vector4(fog->cloudBaseHeight, fog->cloudThickness,
                                    fog->cloudCoverage, fog->cloudDensity);
    fs_atmosphere.clouds1 = Vector4(fog->cloudScale, fog->cloudSpeed,
                                    fog->cloudSunAmount, fog->cloudsEnabled ? 1.0f : 0.0f);
    fs_atmosphere.cloudWind = Vector4(fog->cloudWind.x, 0.0f, fog->cloudWind.y, 0.0f);
    fs_atmosphere.cloudColor = Vector4(fog->cloudColor.x, fog->cloudColor.y,
                                       fog->cloudColor.z, (float)Engine::getSystemTime());

    return true;
}

void RenderSystem::renderAtmosphere(CameraComponent& camera, Transform& cameraTransform,
        Framebuffer* sceneColorTarget, TextureRender* depthTexture){
    if (!atmosphereLoaded || !sceneColorTarget || !sceneColorTarget->isCreated() ||
            !depthTexture || !atmosphereFramebuffer.isCreated())
        return;

    if (!buildAtmosphereParams(camera, cameraTransform))
        return;

    atmospherePassRender.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    atmospherePassRender.startRenderPass(&atmosphereFramebuffer.getRender());
    if (atmosphereRender.beginDraw(PIP_RTT)){
        ShaderData& sd = atmosphereShader.get()->shaderData;
        const std::pair<int, int> sceneSlot = sd.getTextureIndexByName("u_sceneColorTexture");
        const std::pair<int, int> depthSlot = sd.getTextureIndexByName("u_depthTexture");
        atmosphereRender.addTexture(sceneSlot, ShaderStageType::FRAGMENT,
                                    &sceneColorTarget->getRender().getColorTexture());
        atmosphereRender.addTexture(depthSlot, ShaderStageType::FRAGMENT, depthTexture);
        atmosphereRender.applyUniformBlock(atmosphereSlotParams, sizeof(fs_atmosphere_t), &fs_atmosphere);
        atmosphereRender.draw(0, 3, 1);
    }
    atmospherePassRender.endRenderPass();

    // the pass read sceneColorTarget and wrote its own texture, so this copy is not a
    // sample-what-you-write hazard; every later stage then sees the fogged scene
    loadBlit();
    if (blitLoaded){
        renderBlit(&atmosphereFramebuffer.getRender().getColorTexture(), &sceneColorTarget->getRender(), nullptr);
    }
}

void RenderSystem::loadTemporalEffects(){
    if (velocityLoaded && taaLoaded && motionBlurLoaded)
        return;

    velocityShader = ShaderPool::get(ShaderType::VELOCITY, 0);
    taaShader = ShaderPool::get(ShaderType::TAA, 0);
    motionBlurShader = ShaderPool::get(ShaderType::MOTIONBLUR, 0);
    if (!velocityShader || !velocityShader->isCreated() ||
            !taaShader || !taaShader->isCreated() ||
            !motionBlurShader || !motionBlurShader->isCreated())
        return;

    velocityRender.beginLoad(PrimitiveType::TRIANGLES);
    velocityRender.setShader(velocityShader.get());
    velocitySlotParams = velocityShader->shaderData.getUniformBlockIndex(UniformBlockType::VELOCITY_FS_PARAMS);
    if (!velocityRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    taaRender.beginLoad(PrimitiveType::TRIANGLES);
    taaRender.setShader(taaShader.get());
    taaSlotParams = taaShader->shaderData.getUniformBlockIndex(UniformBlockType::TAA_FS_PARAMS);
    if (!taaRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    motionBlurRender.beginLoad(PrimitiveType::TRIANGLES);
    motionBlurRender.setShader(motionBlurShader.get());
    motionBlurSlotParams = motionBlurShader->shaderData.getUniformBlockIndex(UniformBlockType::MOTIONBLUR_FS_PARAMS);
    if (!motionBlurRender.endLoad(PIP_RTT, false, true, CullingMode::BACK, WindingOrder::CCW))
        return;

    velocityLoaded = true;
    taaLoaded = true;
    motionBlurLoaded = true;
    Log::verbose("Temporal anti-aliasing and motion blur ready");
}

void RenderSystem::destroyTemporalEffects(){
    if (velocityLoaded)
        velocityRender.destroy();
    if (taaLoaded)
        taaRender.destroy();
    if (motionBlurLoaded)
        motionBlurRender.destroy();

    if (velocityShader){
        velocityShader.reset();
        ShaderPool::remove(ShaderType::VELOCITY, 0);
    }
    if (taaShader){
        taaShader.reset();
        ShaderPool::remove(ShaderType::TAA, 0);
    }
    if (motionBlurShader){
        motionBlurShader.reset();
        ShaderPool::remove(ShaderType::MOTIONBLUR, 0);
    }

    velocityFramebuffer.destroy();
    taaHistoryFramebuffers[0].destroy();
    taaHistoryFramebuffers[1].destroy();
    motionBlurFramebuffer.destroy();
    velocityLoaded = false;
    taaLoaded = false;
    motionBlurLoaded = false;
    temporalWidth = 0;
    temporalHeight = 0;
    temporalInputWidth = 0;
    temporalInputHeight = 0;
    velocitySlotParams = -1;
    taaSlotParams = -1;
    motionBlurSlotParams = -1;
    taaHistoryIndex = 0;
    taaHistoryValid = false;
    previousViewProjectionValid = false;
}

bool RenderSystem::ensureTemporalFramebuffers(unsigned int inputWidth, unsigned int inputHeight,
        unsigned int outputWidth, unsigned int outputHeight, bool hdr){
    if (inputWidth == 0 || inputHeight == 0 || outputWidth == 0 || outputHeight == 0)
        return false;

    if (temporalInputWidth == inputWidth && temporalInputHeight == inputHeight &&
            temporalWidth == outputWidth && temporalHeight == outputHeight &&
            velocityFramebuffer.isCreated() && taaHistoryFramebuffers[0].isCreated() &&
            taaHistoryFramebuffers[1].isCreated() && motionBlurFramebuffer.isCreated())
        return true;

    velocityFramebuffer.destroy();
    taaHistoryFramebuffers[0].destroy();
    taaHistoryFramebuffers[1].destroy();
    motionBlurFramebuffer.destroy();

    const ColorFormat historyFormat = hdr ? ColorFormat::RGBA16F : ColorFormat::RGBA;
    auto configure = [](Framebuffer& target, unsigned int width, unsigned int height, ColorFormat format){
        target.setColorAttachments(1, &format);
        target.setWidth(width);
        target.setHeight(height);
        target.setMinFilter(TextureFilter::LINEAR);
        target.setMagFilter(TextureFilter::LINEAR);
        target.setWrapU(TextureWrap::CLAMP_TO_EDGE);
        target.setWrapV(TextureWrap::CLAMP_TO_EDGE);
        target.create();
    };

    configure(velocityFramebuffer, inputWidth, inputHeight, ColorFormat::RGBA);
    configure(taaHistoryFramebuffers[0], outputWidth, outputHeight, historyFormat);
    configure(taaHistoryFramebuffers[1], outputWidth, outputHeight, historyFormat);
    configure(motionBlurFramebuffer, inputWidth, inputHeight, historyFormat);

    temporalInputWidth = inputWidth;
    temporalInputHeight = inputHeight;
    temporalWidth = outputWidth;
    temporalHeight = outputHeight;
    taaHistoryIndex = 0;
    taaHistoryValid = false;
    return velocityFramebuffer.isCreated() && taaHistoryFramebuffers[0].isCreated() &&
        taaHistoryFramebuffers[1].isCreated() && motionBlurFramebuffer.isCreated();
}

void RenderSystem::renderVelocity(CameraComponent& camera, TextureRender* depthTexture){
    if (!velocityLoaded || !velocityFramebuffer.isCreated() || !depthTexture)
        return;

    fs_velocity.prevViewProjection = previousViewProjectionValid
        ? previousViewProjection : camera.viewProjectionMatrix;
    fs_velocity.invCurrentViewProjection = camera.viewProjectionMatrix.inverse();
    fs_velocity.params = Vector4(scene->getTAAVelocityScale(), scene->getTAAVelocityClamp(), 0.0f, 0.0f);

    temporalPassRender.setClearColor(Vector4(0.5f, 0.5f, 0.0f, 1.0f));
    temporalPassRender.startRenderPass(&velocityFramebuffer.getRender());
    if (velocityRender.beginDraw(PIP_RTT)){
        ShaderData& sd = velocityShader->shaderData;
        velocityRender.addTexture(sd.getTextureIndexByName("u_depthTexture"), ShaderStageType::FRAGMENT, depthTexture);
        velocityRender.applyUniformBlock(velocitySlotParams, sizeof(fs_velocity_t), &fs_velocity);
        velocityRender.draw(0, 3, 1);
    }
    temporalPassRender.endRenderPass();
}

void RenderSystem::renderTAA(CameraComponent& camera, Framebuffer* sceneColorTarget, TextureRender* depthTexture,
        bool copyBack){
    if (!taaLoaded || !sceneColorTarget || !sceneColorTarget->isCreated() || !depthTexture ||
            !taaHistoryFramebuffers[0].isCreated() || !taaHistoryFramebuffers[1].isCreated())
        return;

    renderVelocity(camera, depthTexture);
    const int writeIndex = 1 - taaHistoryIndex;
    if (!taaHistoryValid){
        loadBlit();
        if (blitLoaded)
            renderBlit(&sceneColorTarget->getRender().getColorTexture(),
                       &taaHistoryFramebuffers[writeIndex].getRender(), nullptr);
        taaHistoryIndex = writeIndex;
        taaHistoryValid = true;
        return;
    }

    fs_taa.params0 = Vector4(scene->getTAABlend(), scene->getTAAVelocityScale(),
                             (float)temporalWidth, (float)temporalHeight);
    fs_taa.params1 = Vector4((float)temporalInputWidth, (float)temporalInputHeight, 0.0f, 0.0f);

    temporalPassRender.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    temporalPassRender.startRenderPass(&taaHistoryFramebuffers[writeIndex].getRender());
    if (taaRender.beginDraw(PIP_RTT)){
        ShaderData& sd = taaShader->shaderData;
        taaRender.addTexture(sd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT,
                             &sceneColorTarget->getRender().getColorTexture());
        taaRender.addTexture(sd.getTextureIndex(TextureShaderType::DEPTHTEXTURE), ShaderStageType::FRAGMENT, depthTexture);
        taaRender.addTexture(sd.getTextureIndex(TextureShaderType::VELOCITYTEXTURE), ShaderStageType::FRAGMENT,
                             &velocityFramebuffer.getRender().getColorTexture());
        taaRender.addTexture(sd.getTextureIndex(TextureShaderType::PREVSCENECOLORTEXTURE), ShaderStageType::FRAGMENT,
                             &taaHistoryFramebuffers[taaHistoryIndex].getRender().getColorTexture());
        taaRender.applyUniformBlock(taaSlotParams, sizeof(fs_taa_t), &fs_taa);
        taaRender.draw(0, 3, 1);
    }
    temporalPassRender.endRenderPass();

    if (copyBack){
        loadBlit();
        if (blitLoaded)
            renderBlit(&taaHistoryFramebuffers[writeIndex].getRender().getColorTexture(),
                       &sceneColorTarget->getRender(), nullptr);
    }
    taaHistoryIndex = writeIndex;
}

void RenderSystem::renderMotionBlur(Framebuffer* sceneColorTarget, TextureRender* depthTexture){
    if (!motionBlurLoaded || !sceneColorTarget || !sceneColorTarget->isCreated() || !depthTexture ||
            !motionBlurFramebuffer.isCreated() || !velocityFramebuffer.isCreated())
        return;

    fs_motion_blur.params = Vector4(scene->getMotionBlurIntensity(),
                                    (float)scene->getMotionBlurSamples(),
                                    scene->getMotionBlurVelocityClamp(),
                                    scene->getMotionBlurDepthThreshold());
    temporalPassRender.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    temporalPassRender.startRenderPass(&motionBlurFramebuffer.getRender());
    if (motionBlurRender.beginDraw(PIP_RTT)){
        ShaderData& sd = motionBlurShader->shaderData;
        motionBlurRender.addTexture(sd.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT,
                                    &sceneColorTarget->getRender().getColorTexture());
        motionBlurRender.addTexture(sd.getTextureIndex(TextureShaderType::VELOCITYTEXTURE), ShaderStageType::FRAGMENT,
                                    &velocityFramebuffer.getRender().getColorTexture());
        motionBlurRender.addTexture(sd.getTextureIndex(TextureShaderType::DEPTHTEXTURE), ShaderStageType::FRAGMENT, depthTexture);
        motionBlurRender.applyUniformBlock(motionBlurSlotParams, sizeof(fs_motion_blur_t), &fs_motion_blur);
        motionBlurRender.draw(0, 3, 1);
    }
    temporalPassRender.endRenderPass();

    loadBlit();
    if (blitLoaded)
        renderBlit(&motionBlurFramebuffer.getRender().getColorTexture(), &sceneColorTarget->getRender(), nullptr);
}

void RenderSystem::presentFramebufferToSwapchain(Framebuffer* source){
    if (!source || !source->isCreated())
        return;

    loadBlit();
    if (!blitLoaded)
        return;

    // the scene passes already letterboxed into the composite, so copy it 1:1
    const Rect viewport(0, 0, (float)System::instance().getScreenWidth(), (float)System::instance().getScreenHeight());
    renderBlit(&source->getRender().getColorTexture(), nullptr, &viewport);
}

void RenderSystem::destroyMesh(Entity entity, MeshComponent& mesh, bool clearAssets){
    InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
    TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
    const bool preserveAssets = mesh.needReload && !clearAssets;

    if (!mesh.loaded)
        return;

    Engine::addMeshDropStats();

    for (int i = 0; i < mesh.numSubmeshes; i++){

        Submesh& submesh = mesh.submeshes[i];
        if (!preserveAssets){
            //Destroy shader
            if (submesh.shader){
                submesh.shader.reset();
                ShaderPool::remove(ShaderType::MESH, submesh.shaderProperties, submesh.customShaderId);
            }
            // depth shader may have been loaded for shadows and/or SSAO
            if (submesh.depthShader){
                submesh.depthShader.reset();
                ShaderPool::remove(ShaderType::DEPTH, submesh.depthShaderProperties, submesh.customDepthShaderId);
            }
            // G-buffer shader may have been loaded for SSR
            if (submesh.gbufferShader){
                submesh.gbufferShader.reset();
                ShaderPool::remove(ShaderType::GBUFFER, submesh.gbufferShaderProperties);
            }

            //Destroy texture
            submesh.material.baseColorTexture.destroy();
            submesh.material.metallicRoughnessTexture.destroy();
            submesh.material.normalTexture.destroy();
            submesh.material.occlusionTexture.destroy();
            submesh.material.emissiveTexture.destroy();
        }

        if (terrain){
            //Destroy terrain texture
            if (!preserveAssets){
                terrain->heightMap.destroy();
                for (Texture& blendMap : terrain->blendMaps){
                    blendMap.destroy();
                }
                for (TerrainSurfaceLayer& layer : terrain->surfaceLayers){
                    forEachTerrainLayerTexture(layer, [](Texture& texture){ texture.destroy(); });
                }
                destroyTerrainDetailArray(entity);
            }

            //Destroy terrain buffer
            for (int v = 0; v < MAX_TERRAIN_VIEWS; v++){
                for (int s = 0; s < 2; s++){
                    terrain->views[v].nodesbuffer[s].getRender()->destroyBuffer();
                }
            }
        }

        if (instmesh){
            //Destroy instmesh buffer
            if (!preserveAssets){
                instmesh->buffer.clearAll();
            }
            instmesh->buffer.getRender()->destroyBuffer();
        }

        submesh.lodIndices.getRender()->destroyBuffer();
        if (!preserveAssets){
            submesh.lodIndices.clearAll();
            submesh.lodData.reset();
            submesh.lodKey = 0;
        }

        //Destroy render
        submesh.render.destroy();
        submesh.depthRender.destroy();
        submesh.gbufferRender.destroy();

        //Shaders uniforms
        submesh.slotVSParams = -1;
        submesh.slotFSParams = -1;
        submesh.slotFSMaterialExtensions = -1;
        submesh.slotFSTexCoordSets = -1;
        submesh.slotFSHeight = -1;
        submesh.slotVSFade = -1;
        submesh.slotFSLighting = -1;
        submesh.slotFSReflectionProbe = -1;
        submesh.slotVSSprite = -1;
        submesh.slotVSShadows = -1;
        submesh.slotFSShadows = -1;
        submesh.slotFSShadowProjection = -1;
        submesh.slotFSPointShadows = -1;
        submesh.slotVSSkinning = -1;
        submesh.slotVSMorphTarget = -1;
        submesh.slotVSTerrain = -1;
        submesh.slotFSTerrainLayers = -1;

        submesh.slotVSDepthParams = -1;
        submesh.slotVSDepthFade = -1;
        submesh.slotFSDepthMaterial = -1;
        submesh.slotVSDepthSkinning = -1;
        submesh.slotVSDepthMorphTarget = -1;
        submesh.slotVSDepthTerrain = -1;

        submesh.slotVSGBufferParams = -1;
        submesh.slotFSGBufferMaterial = -1;
        submesh.slotVSGBufferSkinning = -1;
        submesh.slotVSGBufferMorphTarget = -1;
        submesh.slotVSGBufferTerrain = -1;
        submesh.slotFSGBufferTerrainLayers = -1;

        submesh.customVSParams.clear();
        submesh.customFSParams.clear();
        submesh.customVSDepthParams.clear();
        submesh.customFSDepthParams.clear();
    }

    //Destroy buffer
    if (!preserveAssets){
        mesh.buffer.clearAll();
        mesh.indices.clearAll();
    }
    mesh.buffer.getRender()->destroyBuffer();
    mesh.indices.getRender()->destroyBuffer();
    mesh.bonesBuffer.destroyBuffer();
    mesh.bonesTexture.destroyTexture();
    for (int i = 0; i < mesh.numExternalBuffers; i++){
        if (!preserveAssets){
            mesh.eBuffers[i].clearAll();
        }
        mesh.eBuffers[i].getRender()->destroyBuffer();
    }

    if (mesh.needReload){
        // Reload destroys GPU objects immediately; reset load state synchronously so
        // update() can call loadMesh in the same frame.
        // Queuing changeDestroy would leave mesh.loaded true until later, so draw()
        // could run with stale bindings and trip Sokol validation.
        mesh.loaded = false;
        mesh.loadCalled = false;
    }else{
        SystemRender::addQueueCommand(&changeDestroy, new check_load_t{scene, entity});
    }
}

void RenderSystem::prepareMeshForDataReload(Entity entity, MeshComponent& mesh){
    if (!mesh.loaded)
        return;

    // A structural model reload replaces submesh and external-buffer counts. Tear down using
    // the old counts before MeshSystem mutates them, while retaining the synchronous reload state.
    mesh.needReload = true;
    destroyMesh(entity, mesh, true);
}

bool RenderSystem::loadUI(Entity entity, UIComponent& ui, uint16_t pipelines, bool isText){

    if (!Engine::isViewLoaded()) 
        return false;

    ui.texture.setAlphaBorderAuto(true);

    ObjectRender& render = ui.render;

    render.beginLoad(ui.primitiveType);

    bool hasUITexture = !ui.texture.empty();

    bool p_hasTexture = false;
    bool p_vertexColorVec4 = true;
    bool p_hasFontAtlasTexture = false;
    if (hasUITexture){
        if (isText){
            p_vertexColorVec4 = false;
            p_hasFontAtlasTexture = true;
        }else{
            p_hasTexture = true;
        }
    }

    ui.shaderProperties = ShaderPool::getUIProperties(p_hasTexture, p_hasFontAtlasTexture, false, p_vertexColorVec4);
    const std::string& uiShaderSrc = ui.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::UI) : ui.customShader;
    ui.customShaderId = ShaderPool::registerCustomShader(uiShaderSrc);
    ui.shader = ShaderPool::get(ShaderType::UI, ui.shaderProperties, ui.customShaderId);
    if (!ui.shader->isCreated()){
        // A custom shader that failed to compile falls back to the built-in shader (so the
        // object stays visible and loading finishes); retried after its source changes.
        if (ui.customShaderId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::UI, ui.shaderProperties, ui.customShaderId)){
            Log::error("Custom shader '%s' failed to compile; using the built-in shader", uiShaderSrc.c_str());
            ui.customShaderId = 0;
            ui.shader = ShaderPool::get(ShaderType::UI, ui.shaderProperties, 0);
        }
        if (!ui.shader->isCreated())
            return false;
    }
    render.setShader(ui.shader.get());
    ShaderData& shaderData = ui.shader.get()->shaderData;

    ui.slotVSParams = shaderData.getUniformBlockIndex(UniformBlockType::UI_VS_PARAMS);
    ui.slotFSParams = shaderData.getUniformBlockIndex(UniformBlockType::UI_FS_PARAMS);
    ui.customVSParams.resolve(shaderData, "u_vs_customParams");
    ui.customFSParams.resolve(shaderData, "u_fs_customParams");
    const ShaderUniformValues* inheritedUniforms = scene->getInheritedShaderUniforms(ShaderType::UI, ui.customShader);
    ui.customVSParams.writeValues(ui.shaderUniforms, inheritedUniforms);
    ui.customFSParams.writeValues(ui.shaderUniforms, inheritedUniforms);
    ui.needUpdateShaderUniforms = false;

    size_t bufferSize;
    size_t minBufferSize;

    bufferSize = ui.buffer.getSize();
    minBufferSize = ui.minBufferCount * ui.buffer.getStride();
    if (minBufferSize > bufferSize)
        bufferSize = minBufferSize;

    if (bufferSize == 0)
        return false;

    ui.buffer.getRender()->createBuffer(bufferSize, ui.buffer.getData(), ui.buffer.getType(), ui.buffer.getUsage());
    if (ui.buffer.isRenderAttributes()) {
        for (auto const &attr : ui.buffer.getAttributes()) {
            render.addAttribute(shaderData.getAttrIndex(attr.first), ui.buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), ui.buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
        }
    }
    if (ui.buffer.getUsage() != BufferUsage::IMMUTABLE){
        ui.needUpdateBuffer = true;
    }

    bufferSize = ui.indices.getSize();
    minBufferSize = ui.minIndicesCount * ui.indices.getStride();
    if (minBufferSize > bufferSize)
        bufferSize = minBufferSize;

    if (ui.indices.getCount() > 0){
        ui.indices.getRender()->createBuffer(bufferSize, ui.indices.getData(), ui.indices.getType(), ui.indices.getUsage());
        ui.vertexCount = ui.indices.getCount();
        Attribute indexattr = ui.indices.getAttributes()[AttributeType::INDEX];
        render.setIndex(ui.indices.getRender(), indexattr.getDataType(), indexattr.getOffset());
        if (ui.indices.getUsage() != BufferUsage::IMMUTABLE){
            ui.needUpdateBuffer = true;
        }
    }else{
        ui.vertexCount = ui.buffer.getCount();
    }

    if (TextureRender* textureRender = ui.texture.getRender(&emptyWhite)){
        if (!textureRender->isCreated()){
            return false;
        }
        render.addTexture(shaderData.getTextureIndex(TextureShaderType::UI), ShaderStageType::FRAGMENT, textureRender);
    }
    
    ui.needUpdateTexture = false;

    if (!render.endLoad(pipelines, false, true, CullingMode::BACK, WindingOrder::CCW)){
        return false;
    }

    ui.needReload = false;
    ui.needUpdateAABB = true;
    ui.loadCalled = true;
    SystemRender::addQueueCommand(&changeLoaded, new check_load_t{scene, entity});

    return true;
}

bool RenderSystem::drawUI(UIComponent& ui, Transform& transform, PipelineType pipType){
    if (ui.loaded && ui.buffer.getSize() > 0){

        if (ui.needUpdateTexture || ui.texture.isFramebufferOutdated()){
            ShaderData& shaderData = ui.shader.get()->shaderData;
            ui.texture.setAlphaBorderAuto(true);
            if (TextureRender* textureRender = ui.texture.getRender(&emptyWhite))
                if (textureRender->isCreated()){
                    ui.render.addTexture(shaderData.getTextureIndex(TextureShaderType::UI), ShaderStageType::FRAGMENT, textureRender);
                    ui.needUpdateTexture = false;
                }

            if (ui.needUpdateTexture)
                return true;
        }

        if (ui.needUpdateBuffer){
            ui.buffer.getRender()->updateBuffer(ui.buffer.getSize(), ui.buffer.getData());

            if (ui.indices.getCount() > 0){
                ui.indices.getRender()->updateBuffer(ui.indices.getSize(), ui.indices.getData());
                ui.vertexCount = ui.indices.getCount();
            }else{
                ui.vertexCount = ui.buffer.getCount();
            }

            ui.needUpdateBuffer = false;
        }

        ObjectRender& render = ui.render;

        if (!render.beginDraw(pipType)){
            ui.needReload = true;
            return false;
        }
        render.applyUniformBlock(ui.slotVSParams, sizeof(float) * 16, &transform.modelViewProjectionMatrix);
        //Color
        render.applyUniformBlock(ui.slotFSParams, sizeof(float) * 4, &ui.color);
        applyCustomUniforms(render, ui, scene, ShaderType::UI, frameTime, passResolution);
        render.draw(0, ui.vertexCount, 1);

    }

    return true;
}

void RenderSystem::destroyUI(Entity entity, UIComponent& ui){
    TextComponent* text = scene->findComponent<TextComponent>(entity);

    if (!ui.loaded)
        return;

    if (!ui.needReload){
        //Destroy shader
        if (ui.shader){
            ui.shader.reset();
            ShaderPool::remove(ShaderType::UI, ui.shaderProperties, ui.customShaderId);
        }

        //Destroy texture
        ui.texture.destroy();
    }

    //Destroy render
    ui.render.destroy();

    //Destroy buffer
    if (!ui.needReload){
        ui.buffer.clearAll();
        ui.indices.clearAll();
    }
    ui.buffer.getRender()->destroyBuffer();
    ui.indices.getRender()->destroyBuffer();

    if (text){
        text->needReloadAtlas = true;
    }

    //Shaders uniforms
    ui.slotVSParams = -1;
    ui.slotFSParams = -1;
    ui.customVSParams.clear();
    ui.customFSParams.clear();

    SystemRender::addQueueCommand(&changeDestroy, new check_load_t{scene, entity});
}

bool RenderSystem::loadPoints(Entity entity, PointsComponent& points, uint16_t pipelines){

    if (!Engine::isViewLoaded()) 
        return false;

    points.texture.setAlphaBorderAuto(true);

    ObjectRender& render = points.render;

    render.beginLoad(PrimitiveType::POINTS);

    if (points.autoTransparency && !points.transparent){
        if (points.texture.isTransparent()){ // Particle color is not tested here
            points.transparent = true;
        }
    }

    bool hasPointsTexture = !points.texture.empty();

    bool p_hasTexture = false;
    bool p_hasTextureRect = false;
    if (hasPointsTexture){
        p_hasTexture = true;
        if (points.hasTextureRect){
            p_hasTextureRect = true;
        }
    }

    points.shaderProperties = ShaderPool::getPointsProperties(p_hasTexture, false, true, p_hasTextureRect, points.gpuSimulation);
    const std::string& pointsShaderSrc = points.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::POINTS) : points.customShader;
    points.customShaderId = ShaderPool::registerCustomShader(pointsShaderSrc);
    points.shader = ShaderPool::get(ShaderType::POINTS, points.shaderProperties, points.customShaderId);
    if (!points.shader->isCreated()){
        // A custom shader that failed to compile falls back to the built-in shader (so the
        // object stays visible and loading finishes); retried after its source changes.
        if (points.customShaderId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::POINTS, points.shaderProperties, points.customShaderId)){
            Log::error("Custom shader '%s' failed to compile; using the built-in shader", pointsShaderSrc.c_str());
            points.customShaderId = 0;
            points.shader = ShaderPool::get(ShaderType::POINTS, points.shaderProperties, 0);
        }
        if (!points.shader->isCreated())
            return false;
    }
    render.setShader(points.shader.get());
    ShaderData& shaderData = points.shader.get()->shaderData;

    points.slotVSParams = shaderData.getUniformBlockIndex(UniformBlockType::POINTS_VS_PARAMS);
    points.customVSParams.resolve(shaderData, "u_vs_customParams");
    points.customFSParams.resolve(shaderData, "u_fs_customParams");
    const ShaderUniformValues* inheritedUniforms = scene->getInheritedShaderUniforms(ShaderType::POINTS, points.customShader);
    points.customVSParams.writeValues(points.shaderUniforms, inheritedUniforms);
    points.customFSParams.writeValues(points.shaderUniforms, inheritedUniforms);
    points.needUpdateShaderUniforms = false;

    points.buffer.clear();
    points.buffer.addAttribute(AttributeType::POSITION, 3, 0);
    points.buffer.addAttribute(AttributeType::COLOR, 4, 3 * sizeof(float));
    points.buffer.addAttribute(AttributeType::POINTSIZE, 1, 7 * sizeof(float));
    points.buffer.addAttribute(AttributeType::POINTROTATION, 1, 8 * sizeof(float));
    points.buffer.addAttribute(AttributeType::TEXTURERECT, 4, 9 * sizeof(float));
    if (points.gpuSimulation){
        points.buffer.addAttribute(AttributeType::PARTICLEVELOCITY, 3, 13 * sizeof(float));
        points.buffer.addAttribute(AttributeType::PARTICLEACCELERATION, 3, 16 * sizeof(float));
        points.buffer.addAttribute(AttributeType::PARTICLESPAWNTIME, 1, 19 * sizeof(float));
        points.buffer.addAttribute(AttributeType::PARTICLELIFE, 1, 20 * sizeof(float));
    }
    // PointRenderData has a stable superset layout. Non-GPU points simply leave the
    // extra attributes unused by their shader variant.
    points.buffer.setStride(sizeof(PointRenderData));
    points.buffer.setRenderAttributes(true);
    points.buffer.setUsage(BufferUsage::STREAM);

    // Now buffer size is zero than it needed to be calculated
    size_t bufferSize = points.maxPoints * points.buffer.getStride();

    points.buffer.getRender()->createBuffer(bufferSize, points.buffer.getData(), points.buffer.getType(), points.buffer.getUsage());
    if (points.buffer.isRenderAttributes()) {
        for (auto const &attr : points.buffer.getAttributes()) {
            render.addAttribute(shaderData.getAttrIndex(attr.first), points.buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), points.buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
        }
    }

    points.needUpdateBuffer = true;

    if (TextureRender* textureRender = points.texture.getRender(&emptyWhite)){
        if (!textureRender->isCreated()){
            return false;
        }
        render.addTexture(shaderData.getTextureIndex(TextureShaderType::POINTS), ShaderStageType::FRAGMENT, textureRender);
    }

    points.needUpdateTexture = false;

    // no depth write: transparent quads would hide the points drawn after them
    if (!render.endLoad(pipelines, false, !points.transparent, CullingMode::BACK, WindingOrder::CCW)){
        return false;
    }

    points.pipelineTransparent = points.transparent;
    points.needReload = false;
    points.loadCalled = true;
    SystemRender::addQueueCommand(&changeLoaded, new check_load_t{scene, entity});

    return true;
}

bool RenderSystem::loadLines(Entity entity, LinesComponent& lines, uint16_t pipelines){

    if (!Engine::isViewLoaded()) 
        return false;

    if (lines.lines.size() == 0)
        return false;

    ObjectRender& render = lines.render;

    render.beginLoad(PrimitiveType::LINES);

    lines.shaderProperties = ShaderPool::getLinesProperties(false, true);
    const std::string& linesShaderSrc = lines.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::LINES) : lines.customShader;
    lines.customShaderId = ShaderPool::registerCustomShader(linesShaderSrc);
    lines.shader = ShaderPool::get(ShaderType::LINES, lines.shaderProperties, lines.customShaderId);
    if (!lines.shader->isCreated()){
        // A custom shader that failed to compile falls back to the built-in shader (so the
        // object stays visible and loading finishes); retried after its source changes.
        if (lines.customShaderId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::LINES, lines.shaderProperties, lines.customShaderId)){
            Log::error("Custom shader '%s' failed to compile; using the built-in shader", linesShaderSrc.c_str());
            lines.customShaderId = 0;
            lines.shader = ShaderPool::get(ShaderType::LINES, lines.shaderProperties, 0);
        }
        if (!lines.shader->isCreated())
            return false;
    }
    render.setShader(lines.shader.get());
    ShaderData& shaderData = lines.shader.get()->shaderData;

    lines.slotVSParams = shaderData.getUniformBlockIndex(UniformBlockType::LINES_VS_PARAMS);
    lines.customVSParams.resolve(shaderData, "u_vs_customParams");
    lines.customFSParams.resolve(shaderData, "u_fs_customParams");
    const ShaderUniformValues* inheritedUniforms = scene->getInheritedShaderUniforms(ShaderType::LINES, lines.customShader);
    lines.customVSParams.writeValues(lines.shaderUniforms, inheritedUniforms);
    lines.customFSParams.writeValues(lines.shaderUniforms, inheritedUniforms);
    lines.needUpdateShaderUniforms = false;

    lines.buffer.clear();
    lines.buffer.addAttribute(AttributeType::POSITION, 3, 0);
    lines.buffer.addAttribute(AttributeType::COLOR, 4, 3 * sizeof(float));
    lines.buffer.setStride(7 * sizeof(float));
    lines.buffer.setRenderAttributes(true);
    lines.buffer.setUsage(BufferUsage::DYNAMIC);

    // two points per line
    size_t bufferSize = lines.maxLines * (lines.buffer.getStride() * 2);

    if (bufferSize == 0)
        return false;

    lines.buffer.getRender()->createBuffer(bufferSize, lines.buffer.getData(), lines.buffer.getType(), lines.buffer.getUsage());
    if (lines.buffer.isRenderAttributes()) {
        for (auto const &attr : lines.buffer.getAttributes()) {
            render.addAttribute(shaderData.getAttrIndex(attr.first), lines.buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), lines.buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(), attr.second.getPerInstance());
        }
    }
    // buffer is dynamic
    lines.needUpdateBuffer = true;

    if (!render.endLoad(pipelines, false, true, CullingMode::BACK, WindingOrder::CCW)){
        return false;
    }

    lines.needReload = false;
    lines.loadCalled = true;
    SystemRender::addQueueCommand(&changeLoaded, new check_load_t{scene, entity});

    return true;
}

float RenderSystem::getPointsViewportHeight(const CameraComponent& camera) const{
    if (camera.renderToTexture && camera.framebuffer){
        return (float)camera.framebuffer->getHeight();
    }
    if (isFixedResolutionActive()){
        // the main color pass renders into the fixed-resolution target
        return (float)scene->getFixedResolutionHeight();
    }
    if (Framebuffer* engineFb = Engine::getFramebuffer()){
        if (engineFb->isCreated()){
            return (float)engineFb->getHeight();
        }
    }
    float viewHeight = Engine::getViewRect().getHeight();
    if (viewHeight > 0.0f){
        return viewHeight;
    }
    return (float)Engine::getCanvasHeight();
}

float RenderSystem::computePointsScale(const CameraComponent& camera, float viewportHeight) const{
    if (viewportHeight <= 0.0f){
        return 1.0f;
    }

    if (camera.type == CameraType::CAMERA_PERSPECTIVE){
        float tanHalfFov = tanf(camera.yfov * 0.5f);
        if (tanHalfFov <= 0.0f){
            return 1.0f;
        }
        return viewportHeight / (2.0f * tanHalfFov);
    }

    float orthoHeight = 0.0f;
    if (camera.type == CameraType::CAMERA_UI){
        orthoHeight = fabsf(camera.topClip - camera.bottomClip);
    }else{
        orthoHeight = camera.topClip - camera.bottomClip;
    }

    if (orthoHeight <= 0.0f){
        return 1.0f;
    }

    return viewportHeight / orthoHeight;
}

bool RenderSystem::drawPoints(PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType){
    if (points.loaded && points.numVisible > 0){

        if (points.needUpdateTexture || points.texture.isFramebufferOutdated()){
            ShaderData& shaderData = points.shader.get()->shaderData;
            points.texture.setAlphaBorderAuto(true);
            if (TextureRender* textureRender = points.texture.getRender(&emptyWhite)){
                if (textureRender->isCreated()){
                    points.render.addTexture(shaderData.getTextureIndex(TextureShaderType::POINTS), ShaderStageType::FRAGMENT, textureRender);
                    points.needUpdateTexture = false;
                }
            }

            if (points.needUpdateTexture)
                return true;
        }

        if (points.needUpdateBuffer){
            // setData here because component can change order and lose reference
            points.buffer.setData((unsigned char*)(&points.renderPoints[0]), sizeof(PointRenderData)*points.numVisible);
            points.buffer.getRender()->updateBuffer(points.buffer.getSize(), points.buffer.getData());
            points.needUpdateBuffer = false;
        }

        ObjectRender& render = points.render;

        if (!render.beginDraw(pipType)){
            points.needReload = true;
            return false;
        }
        vs_points_params_t vsParams = {};
        vsParams.mvpMatrix = transform.modelViewProjectionMatrix;
        vsParams.pointScale = computePointsScale(camera, getPointsViewportHeight(camera));
        vsParams.simulationTime = points.gpuSimulationTime;
        render.applyUniformBlock(points.slotVSParams, sizeof(vs_points_params_t), &vsParams);
        applyCustomUniforms(render, points, scene, ShaderType::POINTS, frameTime, passResolution);
        render.draw(0, points.numVisible, 1);
    }

    return true;
}

void RenderSystem::destroyPoints(Entity entity, PointsComponent& points){
    if (!points.loaded)
        return;

    if (!points.needReload){
        //Destroy shader
        if (points.shader){
            points.shader.reset();
            ShaderPool::remove(ShaderType::POINTS, points.shaderProperties, points.customShaderId);
        }

        //Destroy texture
        points.texture.destroy();
    }

    //Destroy render
    points.render.destroy();

    //Destroy buffer
    if (!points.needReload){
        points.buffer.clearAll();
    }
    points.buffer.getRender()->destroyBuffer();

    //Shaders uniforms
    points.slotVSParams = -1;
    points.customVSParams.clear();
    points.customFSParams.clear();

    SystemRender::addQueueCommand(&changeDestroy, new check_load_t{scene, entity});
}

bool RenderSystem::drawLines(LinesComponent& lines, Transform& transform, Transform& camTransform, PipelineType pipType){
    if (lines.loaded && lines.lines.size() > 0){

        if (lines.needUpdateBuffer){
            // setData here because component can change order and lose reference
            lines.buffer.setData((unsigned char*)(&lines.lines[0]), sizeof(LineData)*lines.lines.size());
            lines.buffer.getRender()->updateBuffer(lines.buffer.getSize(), lines.buffer.getData());
            lines.needUpdateBuffer = false;
        }

        ObjectRender& render = lines.render;

        if (!render.beginDraw(pipType)){
            lines.needReload = true;
            return false;
        }
        render.applyUniformBlock(lines.slotVSParams, sizeof(float) * 16, &transform.modelViewProjectionMatrix);
        applyCustomUniforms(render, lines, scene, ShaderType::LINES, frameTime, passResolution);
        render.draw(0, lines.lines.size() * 2, 1);
    }

    return true;
}

void RenderSystem::destroyLines(Entity entity, LinesComponent& lines){
    if (!lines.loaded)
        return;

    if (!lines.needReload){
        //Destroy shader
        if (lines.shader){
            lines.shader.reset();
            ShaderPool::remove(ShaderType::LINES, lines.shaderProperties, lines.customShaderId);
        }
    }

    //Destroy render
    lines.render.destroy();

    //Destroy buffer
    if (!lines.needReload){
        lines.buffer.clearAll();
    }
    lines.buffer.getRender()->destroyBuffer();

    //Shaders uniforms
    lines.slotVSParams = -1;
    lines.customVSParams.clear();
    lines.customFSParams.clear();

    SystemRender::addQueueCommand(&changeDestroy, new check_load_t{scene, entity});
}

bool RenderSystem::loadSky(Entity entity, SkyComponent& sky, uint16_t pipelines){

    if (!Engine::isViewLoaded()) 
        return false;	

    sky.buffer.clear();
    sky.buffer.addAttribute(AttributeType::POSITION, 3);

    Attribute* attVertex = sky.buffer.getAttribute(AttributeType::POSITION);

    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f, -1.0f));

    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f,  1.0f));

    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f, -1.0f));

    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f,  1.0f));

    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f,  1.0f, -1.0f));

    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f, -1.0f));
    sky.buffer.addVector3(attVertex, Vector3(-1.0f, -1.0f,  1.0f));
    sky.buffer.addVector3(attVertex, Vector3(1.0f, -1.0f,  1.0f));
    ObjectRender* render = &sky.render;

    render->beginLoad(PrimitiveType::TRIANGLES);

    const std::string& skyShaderSrc = sky.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::SKYBOX) : sky.customShader;
    sky.customShaderId = ShaderPool::registerCustomShader(skyShaderSrc);
    sky.shader = ShaderPool::get(ShaderType::SKYBOX, 0, sky.customShaderId);
    if (!sky.shader->isCreated()){
        // A custom shader that failed to compile falls back to the built-in shader (so the
        // object stays visible and loading finishes); retried after its source changes.
        if (sky.customShaderId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::SKYBOX, 0, sky.customShaderId)){
            Log::error("Custom shader '%s' failed to compile; using the built-in shader", skyShaderSrc.c_str());
            sky.customShaderId = 0;
            sky.shader = ShaderPool::get(ShaderType::SKYBOX, 0, 0);
        }
        if (!sky.shader->isCreated())
            return false;
    }
    render->setShader(sky.shader.get());
    ShaderData& shaderData = sky.shader.get()->shaderData;

    sky.slotVSParams = shaderData.getUniformBlockIndex(UniformBlockType::SKY_VS_PARAMS);
    sky.slotFSParams = shaderData.getUniformBlockIndex(UniformBlockType::SKY_FS_PARAMS);
    sky.customVSParams.resolve(shaderData, "u_vs_customParams");
    sky.customFSParams.resolve(shaderData, "u_fs_customParams");
    const ShaderUniformValues* inheritedUniforms = scene->getInheritedShaderUniforms(ShaderType::SKYBOX, sky.customShader);
    sky.customVSParams.writeValues(sky.shaderUniforms, inheritedUniforms);
    sky.customFSParams.writeValues(sky.shaderUniforms, inheritedUniforms);
    sky.needUpdateShaderUniforms = false;

    if (TextureRender* textureRender = sky.texture.getRender(&emptyCubeWhite)){
        if (!textureRender->isCreated()){
            return false;
        }
        render->addTexture(shaderData.getTextureIndex(TextureShaderType::SKYCUBE), ShaderStageType::FRAGMENT, textureRender);
    } else {
        return false;
    }

    sky.needUpdateTexture = false;

    sky.buffer.getRender()->createBuffer(sky.buffer.getSize(), sky.buffer.getData(), sky.buffer.getType(), sky.buffer.getUsage());

    if (sky.buffer.isRenderAttributes()) {
        for (auto const &attr : sky.buffer.getAttributes()) {
            render->addAttribute(shaderData.getAttrIndex(attr.first), sky.buffer.getRender(), attr.second.getElements(), attr.second.getDataType(), sky.buffer.getStride(), attr.second.getOffset(), attr.second.getNormalized(),  attr.second.getPerInstance());
        }
    }

    if (!render->endLoad(pipelines, true, true, CullingMode::BACK, WindingOrder::CCW)){
        return false;
    }

    sky.needReload = false;
    sky.loadCalled = true;
    SystemRender::addQueueCommand(&changeLoaded, new check_load_t{scene, entity});

    return true;
}

bool RenderSystem::drawSky(SkyComponent& sky, PipelineType pipType){
    if (sky.loaded){

        if (sky.needUpdateTexture || sky.texture.isFramebufferOutdated()){
            ShaderData& shaderData = sky.shader.get()->shaderData;
            if (TextureRender* textureRender = sky.texture.getRender(&emptyCubeWhite)){
                if (textureRender->isCreated()){
                    sky.render.addTexture(shaderData.getTextureIndex(TextureShaderType::SKYCUBE), ShaderStageType::FRAGMENT, textureRender);
                    sky.needUpdateTexture = false;
                }
            }

            if (sky.needUpdateTexture)
                return true;
        }

        ObjectRender& render = sky.render;

        if (!render.beginDraw(pipType)){
            sky.needReload = true;
            return false;
        }
        render.applyUniformBlock(sky.slotVSParams, sizeof(float) * 16, &sky.skyViewProjectionMatrix);
        render.applyUniformBlock(sky.slotFSParams, sizeof(float) * 4, &sky.color);
        applyCustomUniforms(render, sky, scene, ShaderType::SKYBOX, frameTime, passResolution);
        render.draw(0, 36, 1);
    }

    return true;
}

void RenderSystem::destroySky(Entity entity, SkyComponent& sky){
    if (!sky.loaded)
        return;

    if (!sky.needReload){
        //Destroy shader
        if (sky.shader){
            sky.shader.reset();
            ShaderPool::remove(ShaderType::SKYBOX, 0, sky.customShaderId);
        }

        //Destroy texture
        sky.texture.destroy();

        //Release IBL environment maps (pooled maps are kept for reuse)
        releaseSkyEnvironment(sky);
        sky.needUpdateEnvironment = true;
    }

    //Destroy render
    sky.render.destroy();

    //Destroy buffer
    if (!sky.needReload){
        sky.buffer.clearAll();
    }
    sky.buffer.getRender()->destroyBuffer();

    //Shaders uniforms
    sky.slotVSParams = -1;
    sky.slotFSParams = -1;
    sky.customVSParams.clear();
    sky.customFSParams.clear();

    SystemRender::addQueueCommand(&changeDestroy, new check_load_t{scene, entity});
}

namespace {
    constexpr float WATER_TWO_PI = 6.2831853f;
    constexpr float WATER_GRAVITY = 9.81f;

    struct WaterWave{
        Vector2 direction; // on world XZ
        float amplitude;
        float wavelength;
    };

    // --- the sea the wind raises ---------------------------------------------------------------
    // Fetch-limited growth in the classic non-dimensional form (Sverdrup-Munk-Bretzun), written so
    // that every number here means metres and seconds:
    //
    //   Hs = min(0.0016 * U * sqrt(X / g), 0.0246 * U * U)   significant wave height
    //   Tp = min(0.72 * (g * X / U^2)^0.34 * U / g, 0.83 * U) peak period
    //   Lp = g * Tp^2 / (2 * PI)                              its deep-water wavelength
    //
    // U is the wind over the water and X the fetch it blows along, so one breeze ripples a canal and
    // heaves an ocean - which no hand-authored wave height can do on its own. The second term of each
    // pair is the fully developed sea: past it, more fetch buys nothing but time.
    const float SEA_GROWTH = 0.0016f;
    const float SEA_DEVELOPED = 0.0246f;
    const float SEA_PERIOD_GROWTH = 0.72f;
    const float SEA_PERIOD_DEVELOPED = 0.83f;

    // A water smaller than this has no room to build a wind sea at all. The growth law is honest
    // about that: 500 m of lake under a 6 m/s breeze is a handful of centimetres, and a template that
    // drew 0.2 m waves would answer the wind by going flat. So the shortfall is charged where it
    // belongs - the fetch, which is what a game scene cannot state - and never to the height, which
    // is then free to keep its physical scaling. 18 km is the fetch at which the law returns the
    // 0.4 m sea that the authored default of 0.2 m crest-to-crest is drawn against.
    const float SEA_MIN_FETCH = 18000.0f;

    // Seconds to close most of the gap to the sea the wind could sustain, and most of the way back
    // down. A real swell outlives the weather that raised it by hours; this is the same story told
    // inside a scene - long enough that a two-second lull changes nothing anybody can see.
    const float SEA_RISE_TIME = 4.0f;
    const float SEA_FALL_TIME = 24.0f;

    // The wind the water feels, smoothed only enough that one frame's spike is not weather. It keeps
    // its own short constant because whitecaps are the part of a gust that arrives with it: run this
    // through the swell's clock and a squall would foam up minutes after it had passed.
    const float SEA_WIND_TIME = 2.0f;

    // Whitecaps: the foam a wind tears off its own crests. They start where waves begin to spill,
    // around 8 m/s, and cover the sea by 20. They follow the wind and not the swell, because that is
    // what a whitecap is - it arrives with the gust and is gone behind it.
    float getWhitecapFactor(float windSpeed){
        return std::clamp((windSpeed - 8.0f) / 12.0f, 0.0f, 1.0f);
    }

    // an angle has no middle until the way round it is picked: the short arc, so a wind veering from
    // 350 to 10 degrees turns the sea that way rather than spinning it through the year
    float mixAngle(float from, float to, float share){
        float delta = to - from;
        while (delta > WATER_TWO_PI * 0.5f)
            delta -= WATER_TWO_PI;
        while (delta < -WATER_TWO_PI * 0.5f)
            delta += WATER_TWO_PI;
        return from + delta * share;
    }

    float mixValue(float from, float to, float share){
        return from + (to - from) * share;
    }

    // how far a value walks toward a target in `seconds`, at the rate the sea does when it is
    // rising (fast) or when the wind has dropped (slow) - which is the whole of D2
    float seaApproach(float current, float target, float seconds){
        const float tau = (target > current) ? SEA_RISE_TIME : SEA_FALL_TIME;
        return current + (target - current) * (1.0f - std::exp(-seconds / tau));
    }

    // The sea state the scene's wind is building under this water, integrated in place. Called from
    // the frame step only, so the shader upload, the CPU surface sample that floats bodies, the
    // bounds and the foam all read one answer instead of each asking the weather again.
    void advanceWindSea(Scene& scene, WaterComponent& water, float seconds, float elapsed){
        const float influence = std::clamp(water.windInfluence, 0.0f, 1.0f);
        const bool windOn = scene.isWindEnabled() && influence > 0.0f;

        float targetWind = 0.0f;
        float targetAngle = water.seaAngle;
        float targetHeight = 0.0f;
        float targetLength = 0.0f;

        if (windOn){
            // one sample at the middle of the water: the sea state is one number per water, and the
            // gusts that cross it are already in the waves' own sum
            const Vector3 centre = water.worldAABB.getCenter();
            const Vector2 blow = Wind::sample(scene, Vector2(centre.x, centre.z), elapsed);
            targetWind = blow.length();

            if (targetWind > 0.001f){
                targetAngle = std::atan2(-blow.y, blow.x); // the same reading as Vector2(cos a, -sin a)

                // what the wind blows over: the extent along it, unless the author knew better
                float fetch = water.fetch;
                if (fetch <= 0.0f){
                    const float along = std::fabs(water.size.x * std::cos(targetAngle)) +
                            std::fabs(water.size.y * std::sin(targetAngle));
                    fetch = std::max(along, 1.0f);
                }
                fetch = std::max(fetch, SEA_MIN_FETCH);

                const float fetchNumber = WATER_GRAVITY * fetch / (targetWind * targetWind);
                targetHeight = std::min(SEA_GROWTH * targetWind * std::sqrt(fetch / WATER_GRAVITY),
                        SEA_DEVELOPED * targetWind * targetWind);
                const float period = std::min(SEA_PERIOD_GROWTH * std::pow(fetchNumber, 0.34f) *
                        targetWind / WATER_GRAVITY, SEA_PERIOD_DEVELOPED * targetWind);
                targetLength = WATER_GRAVITY * period * period / WATER_TWO_PI;
            }
        }

        water.seaWind = water.seaWind + (targetWind - water.seaWind) * (1.0f - std::exp(-seconds / SEA_WIND_TIME));
        water.seaHeight = seaApproach(water.seaHeight, targetHeight, seconds);
        water.seaLength = seaApproach(water.seaLength, targetLength, seconds);
        water.seaAngle = mixAngle(water.seaAngle, targetAngle, 1.0f - std::exp(-seconds / SEA_RISE_TIME));
        water.seaMix = seaApproach(water.seaMix, windOn ? influence : 0.0f, seconds);
    }

    // The wave numbers as they are drawn: the author's, mixed toward the wind sea by the smoothed
    // influence. Everything that reads a wave goes through these four, so none of them can disagree
    // with the others - which is the contract water.vert and sumWaterWaves have always had.
    float getDrawnWaveHeight(const WaterComponent& water){
        const float share = std::clamp(water.seaMix, 0.0f, 1.0f);
        // Hs is the crest-to-trough of the biggest few waves of a real sea; the contract of the wave
        // array is that its four Gerstner sums, in phase, reach `height`, so the wind sea enters at
        // half its height.
        return std::max(mixValue(water.waveHeight, water.seaHeight * 0.5f, share), 0.0f);
    }

    float getDrawnWaveLength(const WaterComponent& water){
        const float share = std::clamp(water.seaMix, 0.0f, 1.0f);
        return std::max(mixValue(water.waveLength, water.seaLength, share), 0.01f);
    }

    float getDrawnWaveAngle(const WaterComponent& water){
        const float share = std::clamp(water.seaMix, 0.0f, 1.0f);
        return mixAngle(Angle::defaultToRad(water.waveDirection), water.seaAngle, share);
    }

    float getDrawnWaveSpeed(const WaterComponent& water){
        const float share = std::clamp(water.seaMix, 0.0f, 1.0f);
        // with the wind in charge the dispersion is the whole story - sqrt(g*k) is how fast a wave of
        // that length travels - so waveSpeed, a taste multiplier, goes to 1.0 instead of doubling it
        return mixValue(water.waveSpeed, 1.0f, share);
    }

    // the waves water.vert sums, from the authored wave settings and the sea the wind is raising
    void getWaterWaves(const WaterComponent& water, WaterWave (&waves)[WATER_WAVE_COUNT]){
        static const float lengthScale[WATER_WAVE_COUNT] = {1.0f, 0.61f, 0.38f, 0.24f};
        static const float angleOffset[WATER_WAVE_COUNT] = {0.0f, 0.47f, -0.39f, 0.83f}; // radians

        float scaleSum = 0.0f;
        for (int i = 0; i < WATER_WAVE_COUNT; i++){
            scaleSum += lengthScale[i];
        }

        const float baseAngle = getDrawnWaveAngle(water);
        const float length = getDrawnWaveLength(water);
        const float height = getDrawnWaveHeight(water);

        for (int i = 0; i < WATER_WAVE_COUNT; i++){
            const float angle = baseAngle + angleOffset[i];
            waves[i].direction = Vector2(std::cos(angle), -std::sin(angle)); // +X rotated around Y
            waves[i].wavelength = length * lengthScale[i];
            waves[i].amplitude = height * lengthScale[i] / scaleSum; // crests in phase reach waveHeight
        }
    }

    // crest pinch, capped before the waves fold over
    float getWaterSteepness(const WaterComponent& water, const WaterWave (&waves)[WATER_WAVE_COUNT]){
        float slope = 0.0f;
        for (int i = 0; i < WATER_WAVE_COUNT; i++){
            slope += (WATER_TWO_PI / waves[i].wavelength) * waves[i].amplitude;
        }

        // a blow pinches the crests as it raises them, up to the shape a storm sea keeps
        const float windShare = getWhitecapFactor(water.seaWind) * std::clamp(water.seaMix, 0.0f, 1.0f);
        const float steepness = std::clamp(mixValue(water.waveSteepness, 0.9f, windShare), 0.0f, 1.0f);
        return (slope > 0.0f) ? std::min(steepness, 1.0f / slope) : steepness;
    }

    // deep water dispersion: longer waves travel faster
    float getWaterWaveFrequency(const WaterComponent& water, const WaterWave& wave){
        const float k = WATER_TWO_PI / wave.wavelength;
        return std::sqrt(WATER_GRAVITY * k) * getDrawnWaveSpeed(water);
    }

    // the wave sum of water.vert at the world point p: the surface offset, and its slopes along x and z
    Vector3 sumWaterWaves(const WaterComponent& water, const WaterWave (&waves)[WATER_WAVE_COUNT], float steepness, const Vector2& p, Vector3& tangent, Vector3& binormal){
        Vector3 offset(0.0f, 0.0f, 0.0f);
        tangent = Vector3(1.0f, 0.0f, 0.0f);
        binormal = Vector3(0.0f, 0.0f, 1.0f);

        for (int i = 0; i < WATER_WAVE_COUNT; i++){
            const Vector2& d = waves[i].direction;
            const float a = waves[i].amplitude;
            const float k = WATER_TWO_PI / waves[i].wavelength;
            const float f = k * (d.x * p.x + d.y * p.y) - water.wavePhase[i];
            const float s = std::sin(f);
            const float c = std::cos(f);
            const float qa = steepness * a;

            offset += Vector3(qa * d.x * c, a * s, qa * d.y * c);
            tangent += Vector3(-qa * k * d.x * d.x * s, a * k * d.x * c, -qa * k * d.x * d.y * s);
            binormal += Vector3(-qa * k * d.x * d.y * s, a * k * d.y * c, -qa * k * d.y * d.y * s);
        }

        return offset;
    }

    // local area of a hull map: the hull bounds and two texels around them
    void getWaterExclusionMapArea(const AABB& bounds, Vector3& origin, Vector3& extent){
        const float margin = 2.0f / (float)(WATER_EXCLUSION_MAP_SIZE - 4);
        const Vector3 size = bounds.getSize();
        origin = bounds.getMinimum() - Vector3(size.x * margin, 0.0f, size.z * margin);
        extent = Vector3(std::max(size.x * (1.0f + 2.0f * margin), 1.0e-6f), std::max(size.y, 1.0e-6f),
                std::max(size.z * (1.0f + 2.0f * margin), 1.0e-6f));
    }

    // world to 0-1 across the volume: its box, or the area of its hull map
    Matrix4 getWaterExclusionSpace(const WaterExclusionComponent& exclusion, const Matrix4& modelMatrix){
        Vector3 origin = exclusion.aabb.getMinimum();
        Vector3 extent = exclusion.aabb.getSize();
        if (exclusion.shape == WaterExclusionShape::HULL){
            getWaterExclusionMapArea(exclusion.hullBounds, origin, extent);
        }

        return Matrix4::scaleMatrix(Vector3(1.0f / extent.x, 1.0f / extent.y, 1.0f / extent.z)) *
                Matrix4::translateMatrix(-origin) * modelMatrix.inverse();
    }

    unsigned char toUnorm8(float value){
        return (unsigned char)std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f);
    }

    // The hull from above: its bottom and top over each texel and how far the texel is outside
    // its vertical walls, all linear between texels, so filtering keeps the edges straight
    void buildWaterExclusionMap(WaterExclusionComponent& exclusion){
        Vector3 origin;
        Vector3 extent;
        getWaterExclusionMapArea(exclusion.hullBounds, origin, extent);

        const int size = WATER_EXCLUSION_MAP_SIZE;
        const float wallRange = 4.0f * std::max(extent.x, extent.z) / (float)size; // four texels each way
        exclusion.hullMap.resize((size_t)size * size * 4);
        for (int z = 0; z < size; z++){
            for (int x = 0; x < size; x++){
                const float px = origin.x + extent.x * ((float)x + 0.5f) / (float)size;
                const float pz = origin.z + extent.z * ((float)z + 0.5f) / (float)size;
                float bottom = std::numeric_limits<float>::lowest();
                float top = std::numeric_limits<float>::max();
                float wall = -wallRange;
                for (const Vector4& plane : exclusion.hullPlanes){
                    const float rest = plane.w - plane.x * px - plane.z * pz;
                    if (plane.y > 0.001f){
                        top = std::min(top, rest / plane.y);
                    }else if (plane.y < -0.001f){
                        bottom = std::max(bottom, rest / plane.y);
                    }else{
                        wall = std::max(wall, -rest / std::sqrt(plane.x * plane.x + plane.z * plane.z));
                    }
                }

                // an empty column stays empty, which clamping each height could turn into [1, 1]
                unsigned char* texel = &exclusion.hullMap[((size_t)z * size + x) * 4];
                texel[0] = (bottom > top) ? 255 : toUnorm8((bottom - origin.y) / extent.y);
                texel[1] = (bottom > top) ? 0 : toUnorm8((top - origin.y) / extent.y);
                texel[2] = toUnorm8(0.5f + wall / (2.0f * wallRange));
                texel[3] = 255;
            }
        }
    }
}

// the surface offset over the world point (x, z), from the grid point the waves move onto it,
// plus the depth field where the water simulates
Vector3 RenderSystem::getWaterSurfaceOffset(const WaterComponent& water, float x, float z, Vector3* normal, float* depth){
    WaterWave waves[WATER_WAVE_COUNT];
    getWaterWaves(water, waves);
    const float steepness = getWaterSteepness(water, waves);

    // Newton steps on p + offset(p) = (x, z), a map the steepness cap keeps from folding
    Vector2 p(x, z);
    Vector3 tangent, binormal;
    Vector3 offset = sumWaterWaves(water, waves, steepness, p, tangent, binormal);
    for (int i = 0; i < 8; i++){
        const Vector2 residual(p.x + offset.x - x, p.y + offset.z - z);
        if (residual.squaredLength() < 1.0e-8f)
            break;

        const float det = tangent.x * binormal.z - binormal.x * tangent.z;
        if (det > 1.0e-4f){
            p.x -= (binormal.z * residual.x - binormal.x * residual.y) / det;
            p.y -= (tangent.x * residual.y - tangent.z * residual.x) / det;
        }else{
            p -= residual;
        }
        offset = sumWaterWaves(water, waves, steepness, p, tangent, binormal);
    }

    if (normal){
        *normal = binormal.crossProduct(tangent).normalized();
    }
    // a level plane holds water everywhere inside its rect
    if (depth)
        *depth = std::numeric_limits<float>::max();

    if (WaterSystem::isSimulated(water)){
        float standing = 0.0f;
        Vector3 fieldNormal(0.0f, 1.0f, 0.0f);
        float field = 0.0f;
        // dry, or past the field: the plane it would have been, and nothing to float on
        if (WaterSystem::getSimSurface(water, x, z, field, fieldNormal, standing)){
            offset.y += field;
            if (normal)
                *normal = Vector3(normal->x + fieldNormal.x, normal->y, normal->z + fieldNormal.z).normalized();
        }else{
            offset.y = 0.0f;
            standing = 0.0f;
        }
        if (depth)
            *depth = standing;
    }

    return offset;
}

// From the world pose, which the physics steps keep current for a body, unlike the model
// matrix. A hull tests its exact planes, which its map only approximates.
bool RenderSystem::isInsideWaterExclusion(const WaterExclusionComponent& exclusion, const Transform& transform, const Vector3& point){
    const Vector3& scale = transform.worldScale;
    if (exclusion.aabb.isNull() || scale.x * scale.y * scale.z == 0.0f)
        return false;

    const Matrix4 modelMatrix = Matrix4::translateMatrix(transform.worldPosition) *
            transform.worldRotation.getRotationMatrix() * Matrix4::scaleMatrix(scale);
    if (!(modelMatrix * exclusion.aabb).contains(point))
        return false;

    if (exclusion.shape == WaterExclusionShape::HULL){
        const Vector3 local = modelMatrix.inverse() * point;
        for (const Vector4& plane : exclusion.hullPlanes){
            if (plane.x * local.x + plane.y * local.y + plane.z * local.z > plane.w)
                return false;
        }
        return !exclusion.hullPlanes.empty();
    }

    const Vector3 q = getWaterExclusionSpace(exclusion, modelMatrix) * point;
    if (q.x < 0.0f || q.x > 1.0f || q.y < 0.0f || q.y > 1.0f || q.z < 0.0f || q.z > 1.0f)
        return false;

    return exclusion.shape != WaterExclusionShape::SPHERE || (q - Vector3(0.5f, 0.5f, 0.5f)).squaredLength() <= 0.25f;
}

// Built-in ripples: a sum of sines on whole wave numbers, so the map tiles seamlessly
std::shared_ptr<TextureRender> RenderSystem::getDefaultWaterNormalMap(){
    static const std::string id = "water|normal|default";

    if (std::shared_ptr<TextureRender> cached = TexturePool::get(id)){
        return cached;
    }

    struct Ripple{
        int kx;
        int ky;
        float slope;
        float phase;
    };

    // fixed seed: the same ripples on every run and platform
    uint32_t seed = 1337u;
    auto random01 = [&seed](){
        seed = seed * 1664525u + 1013904223u;
        return (float)(seed >> 8) * (1.0f / 16777216.0f);
    };

    std::vector<Ripple> ripples;
    float meanSquare = 0.0f;
    while (ripples.size() < 40){
        const float angle = random01() * WATER_TWO_PI;
        const float cycles = 3.0f + random01() * 15.0f;
        const int kx = (int)std::lround(std::cos(angle) * cycles);
        const int ky = (int)std::lround(std::sin(angle) * cycles);
        if (kx == 0 && ky == 0)
            continue;
        const float k2 = (float)(kx * kx + ky * ky);
        const float slope = WATER_TWO_PI / std::pow(k2, 0.7f);
        ripples.push_back({kx, ky, slope, random01() * WATER_TWO_PI});
        meanSquare += 0.5f * slope * slope * k2;
    }

    // slopes scaled to a fixed root mean square, calm on average and steep in places
    const float scale = 0.45f / std::sqrt(meanSquare);

    const int size = 256;
    std::vector<uint32_t> pixels((size_t)size * size);
    for (int y = 0; y < size; y++){
        for (int x = 0; x < size; x++){
            float dx = 0.0f;
            float dy = 0.0f;
            for (const Ripple& ripple : ripples){
                const float c = ripple.slope * std::cos(WATER_TWO_PI * (ripple.kx * x + ripple.ky * y) / size + ripple.phase);
                dx += c * ripple.kx;
                dy += c * ripple.ky;
            }
            Vector3 normal(-dx * scale, -dy * scale, 1.0f);
            normal.normalize();
            const uint32_t r = (uint32_t)std::lround((normal.x * 0.5f + 0.5f) * 255.0f);
            const uint32_t g = (uint32_t)std::lround((normal.y * 0.5f + 0.5f) * 255.0f);
            const uint32_t b = (uint32_t)std::lround((normal.z * 0.5f + 0.5f) * 255.0f);
            pixels[y * size + x] = (0xFFu << 24) | (b << 16) | (g << 8) | r;
        }
    }

    void* data[1] = {pixels.data()};
    size_t dataSize[1] = {pixels.size() * sizeof(uint32_t)};

    std::shared_ptr<TextureRender> render = std::make_shared<TextureRender>();
    // mipmapped, so distant ripples average out instead of shimmering
    if (!render->createTexture(id, size, size, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data, dataSize,
            TextureFilter::LINEAR_MIPMAP_LINEAR, TextureFilter::LINEAR, TextureWrap::REPEAT, TextureWrap::REPEAT)){
        render->destroyTexture();
        return NULL;
    }

    TexturePool::add(id, render);
    return render;
}

// One texel per cell of the field, filtered linear so a drawn grid finer than the field
// still reads a smooth surface. It starts dry, so a water whose first step has not run
// shows no surface instead of whatever the allocator left in the image.
bool RenderSystem::updateWaterField(WaterComponent& water){
    if (!water.simulate){
        if (water.simTextureCells != 0){
            water.simTexture.destroyTexture();
            water.simTextureCells = 0;
        }
        return true;
    }

    const unsigned int cells = WaterSystem::resolutionOf(water);
    if (water.simTextureCells != cells || !water.simTexture.isCreated()){
        water.simTexture.destroyTexture();
        if (!water.simTexture.createDynamicTexture("water-field", (int)cells, (int)cells,
                TextureFilter::LINEAR, TextureFilter::LINEAR)){
            water.simTextureCells = 0;
            return false;
        }
        water.simTextureCells = cells;
        water.simDirty = true;
    }
    return true;
}

bool RenderSystem::loadWater(Entity entity, WaterComponent& water, uint16_t pipelines){

    if (!Engine::isViewLoaded())
        return false;

    ObjectRender& render = water.render;

    render.beginLoad(PrimitiveType::TRIANGLES);

    const bool receiveShadows = hasLights && hasShadows && water.receiveShadows;
    water.shaderProperties = ShaderPool::getWaterProperties(hasFog, water.depthEffects, water.planarReflection, receiveShadows, water.refraction, water.simulate);
    const std::string& waterShaderSrc = water.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::WATER) : water.customShader;
    water.customShaderId = ShaderPool::registerCustomShader(waterShaderSrc);
    water.shader = ShaderPool::get(ShaderType::WATER, water.shaderProperties, water.customShaderId);
    if (!water.shader->isCreated()){
        // A custom shader that failed to compile falls back to the built-in shader (so the
        // object stays visible and loading finishes); retried after its source changes.
        if (water.customShaderId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::WATER, water.shaderProperties, water.customShaderId)){
            Log::error("Custom shader '%s' failed to compile; using the built-in shader", waterShaderSrc.c_str());
            water.customShaderId = 0;
            water.shader = ShaderPool::get(ShaderType::WATER, water.shaderProperties, 0);
        }
        if (!water.shader->isCreated())
            return false;
    }
    render.setShader(water.shader.get());
    ShaderData& shaderData = water.shader.get()->shaderData;

    // built-in ripples as fallback, recreated after a view loss clears the pools
    if (!water.defaultNormalMap || !water.defaultNormalMap->isCreated()){
        water.defaultNormalMap = getDefaultWaterNormalMap();
    }
    TextureRender* normalRender = water.normalTexture.getRender(water.defaultNormalMap.get());
    if (!normalRender || !normalRender->isCreated())
        return false;
    render.addTexture(shaderData.getTextureIndex(TextureShaderType::WATERNORMAL), ShaderStageType::FRAGMENT, normalRender);
    water.needUpdateTexture = false;

    water.slotVSParams = shaderData.getUniformBlockIndex(UniformBlockType::WATER_VS_PARAMS);
    water.slotVSSimParams = -1;
    if (water.simulate){
        if (!updateWaterField(water))
            return false;
        water.slotVSSimParams = shaderData.getUniformBlockIndex(UniformBlockType::WATER_SIM_VS_PARAMS);
    }
    water.slotFSParams = shaderData.getUniformBlockIndex(UniformBlockType::WATER_FS_PARAMS);
    // fields are only appended, so a fork of an older version gets the ones it knows
    unsigned int fsParamsSize = 0;
    shaderData.getUniformBlockMembers("u_fs_waterParams", fsParamsSize);
    water.fsParamsSize = std::min(fsParamsSize, (unsigned int)sizeof(fs_water_t));
    if (water.customShaderId != 0 && fsParamsSize < sizeof(fs_water_t)){
        Log::warn("Custom shader '%s' is a fork of an older water shader; fork it again for the newer effects", waterShaderSrc.c_str());
    }
    water.slotFSLighting = shaderData.getUniformBlockIndex(UniformBlockType::FS_LIGHTING);
    water.slotFSFog = hasFog ? shaderData.getUniformBlockIndex(UniformBlockType::FS_FOG) : -1;
    if (receiveShadows){
        water.slotVSShadows = shaderData.getUniformBlockIndex(UniformBlockType::VS_SHADOWS);
        water.slotFSShadows = shaderData.getUniformBlockIndex(UniformBlockType::FS_SHADOWS);
        water.slotFSShadowProjection = shaderData.getUniformBlockIndex(UniformBlockType::FS_SHADOW_PROJECTION);
        water.slotFSPointShadows = shaderData.getUniformBlockIndex(UniformBlockType::FS_POINT_SHADOWS);
    }
    water.customVSParams.resolve(shaderData, "u_vs_customParams");
    water.customFSParams.resolve(shaderData, "u_fs_customParams");
    const ShaderUniformValues* inheritedUniforms = scene->getInheritedShaderUniforms(ShaderType::WATER, water.customShader);
    water.customVSParams.writeValues(water.shaderUniforms, inheritedUniforms);
    water.customFSParams.writeValues(water.shaderUniforms, inheritedUniforms);
    water.needUpdateShaderUniforms = false;

    // grid on local XZ, kept by reloads that only change the shader
    const unsigned int cells = std::clamp(water.subdivisions, 1u, (unsigned int)MAX_WATER_SUBDIVISIONS);
    if (water.indexCount == 0 || water.gridCells != cells || water.gridSize != water.size){
        const unsigned int row = cells + 1;
        const float width = water.size.x;
        const float depth = water.size.y;

        water.buffer.clearAll();
        water.buffer.addAttribute(AttributeType::POSITION, 3);
        Attribute* positions = water.buffer.getAttribute(AttributeType::POSITION);
        for (unsigned int z = 0; z < row; z++){
            for (unsigned int x = 0; x < row; x++){
                water.buffer.addVector3(positions, Vector3(
                    width * ((float)x / (float)cells - 0.5f),
                    0.0f,
                    depth * ((float)z / (float)cells - 0.5f)));
            }
        }

        // finer grids pass the 16-bit index limit
        water.indices.clearAll();
        water.indices.addAttribute(AttributeType::INDEX, AttributeDataType::UNSIGNED_INT, 1, 0);
        water.indices.setStride(sizeof(uint32_t));
        Attribute* attIndex = water.indices.getAttribute(AttributeType::INDEX);
        for (unsigned int z = 0; z < cells; z++){
            for (unsigned int x = 0; x < cells; x++){
                const uint32_t i0 = z * row + x;
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + row;
                const uint32_t i3 = i2 + 1;
                // counter-clockwise seen from above
                water.indices.addUInt32(attIndex, i0);
                water.indices.addUInt32(attIndex, i2);
                water.indices.addUInt32(attIndex, i1);
                water.indices.addUInt32(attIndex, i1);
                water.indices.addUInt32(attIndex, i2);
                water.indices.addUInt32(attIndex, i3);
            }
        }

        water.indexCount = cells * cells * 6;
        water.gridCells = cells;
        water.gridSize = water.size;
    }
    Attribute* attVertex = water.buffer.getAttribute(AttributeType::POSITION);

    // a retried load may already hold them
    water.buffer.getRender()->destroyBuffer();
    water.indices.getRender()->destroyBuffer();
    if (!water.buffer.getRender()->createBuffer(water.buffer.getSize(), water.buffer.getData(), water.buffer.getType(), water.buffer.getUsage()) ||
        !water.indices.getRender()->createBuffer(water.indices.getSize(), water.indices.getData(), water.indices.getType(), water.indices.getUsage())){
        Log::error("Cannot create the water grid buffers of entity %u", entity);
        water.buffer.getRender()->destroyBuffer();
        water.indices.getRender()->destroyBuffer();
        return false;
    }

    render.addAttribute(shaderData.getAttrIndex(AttributeType::POSITION), water.buffer.getRender(), attVertex->getElements(), attVertex->getDataType(), water.buffer.getStride(), attVertex->getOffset(), attVertex->getNormalized(), attVertex->getPerInstance());
    render.setIndex(water.indices.getRender(), AttributeDataType::UNSIGNED_INT, 0);

    // no culling to show it from below, depth written for the transparencies drawn after it
    if (!render.endLoad(pipelines, false, true, CullingMode::BACK, WindingOrder::CCW)){
        return false;
    }

    water.needReload = false;
    water.loadCalled = true;
    SystemRender::addQueueCommand(&changeWaterLoaded, new check_load_t{scene, entity});

    return true;
}

void RenderSystem::advanceWater(WaterComponent& water, float seconds){
    // the sea first: the phases below advance with the wavelengths the wind is leaving behind
    advanceWindSea(*scene, water, seconds, water.time + seconds);

    WaterWave waves[WATER_WAVE_COUNT];
    getWaterWaves(water, waves);

    // integrated so speed changes do not jump, wrapped to keep precision
    for (int i = 0; i < WATER_WAVE_COUNT; i++){
        water.wavePhase[i] = std::fmod(water.wavePhase[i] + getWaterWaveFrequency(water, waves[i]) * seconds, WATER_TWO_PI);
    }

    // the two ripple layers drift apart, in normal map tiles
    const float travel = water.rippleSpeed * seconds / std::max(water.normalScale, 0.01f);
    const Vector2 direction = waves[0].direction;
    const Vector2 across(direction.x * 0.6f - direction.y * 0.8f, direction.x * 0.8f + direction.y * 0.6f);
    water.rippleOffset[0] += direction * travel;
    water.rippleOffset[1] += across * (travel * 0.75f);
    for (int i = 0; i < 2; i++){
        water.rippleOffset[i].x -= std::floor(water.rippleOffset[i].x);
        water.rippleOffset[i].y -= std::floor(water.rippleOffset[i].y);
    }

    water.time = std::fmod(water.time + seconds, 10000.0f);
}

void RenderSystem::advanceWaters(){
    auto waters = scene->getComponentArray<WaterComponent>();
    for (int i = 0; i < waters->size(); i++){
        WaterComponent& water = waters->getComponentFromIndex(i);
        if (water.pendingTime > 0.0f){
            advanceWater(water, water.pendingTime);
            water.pendingTime = 0.0f;
        }
    }
}

void RenderSystem::updateWater(Entity entity, WaterComponent& water, Transform& transform, uint16_t pipelines, double dt){
    // applied by advanceWaters()
    if (!waterPaused)
        water.pendingTime += (float)dt;

    // the variant follows the scene fog and shadows, and the water switches
    if (water.loaded && !water.needReload &&
            water.shaderProperties != ShaderPool::getWaterProperties(hasFog, water.depthEffects, water.planarReflection,
                hasLights && hasShadows && water.receiveShadows, water.refraction, water.simulate)){
        water.needReload = true;
    }

    if (water.loaded && water.needReload){
        destroyWater(entity, water);
    }
    if (!water.loadCalled){
        loadWater(entity, water, pipelines);
    }
    // with the water, so a first dive does not stall and a fork shows its uniforms and errors
    if (water.loaded && !water.needReload && water.underwater){
        loadUnderwater(water);
    }

    // the resolution is authored at runtime too, so the texel grid follows it
    if (water.loaded)
        updateWaterField(water);

    // bounds grow by the wave height up, down and sideways, and by the ground a field
    // lifts the surface onto
    const float waves = getDrawnWaveHeight(water);
    float top = waves;
    float bottom = -waves;
    if (water.simulate){
        const float ripple = std::max(water.simRippleStrength, 0.0f);
        top = water.simFloorMax + water.simWaterMax + waves + ripple;
        bottom = std::min(water.simFloorMin, 0.0f) - waves - ripple;
    }

    const float halfWidth = std::abs(water.size.x) * 0.5f;
    const float halfDepth = std::abs(water.size.y) * 0.5f;
    water.aabb = AABB(-halfWidth, bottom, -halfDepth, halfWidth, top, halfDepth);

    AABB flat(-halfWidth, bottom, -halfDepth, halfWidth, top, halfDepth);
    water.worldAABB = transform.modelMatrix * flat;
    // a body is still found by the grid point the waves move onto it, which is off by up
    // to the steepness times the amplitude
    water.worldAABB.setExtents(
        water.worldAABB.getMinimum() - Vector3(waves, 0.0f, waves),
        water.worldAABB.getMaximum() + Vector3(waves, 0.0f, waves));
}

bool RenderSystem::drawWater(Entity entity, WaterComponent& water, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType, bool mainCamera){
    if (!water.loaded || water.needReload || water.indexCount == 0)
        return false;

    if (!isInsideCamera(camera, water.worldAABB))
        return false;

    ObjectRender& render = water.render;
    ShaderData& shaderData = water.shader.get()->shaderData;

    if (water.needUpdateTexture || water.normalTexture.isFramebufferOutdated()){
        TextureRender* normalRender = water.normalTexture.getRender(water.defaultNormalMap.get());
        if (normalRender && normalRender->isCreated()){
            render.addTexture(shaderData.getTextureIndex(TextureShaderType::WATERNORMAL), ShaderStageType::FRAGMENT, normalRender);
            water.needUpdateTexture = false;
        }

        if (water.needUpdateTexture)
            return false;
    }

    if (!render.beginDraw(pipType)){
        water.needReload = true;
        return false;
    }

    // textures owned elsewhere can be recreated any frame, so they are bound on every draw
    TextureRender* skyTexture = &emptyCubeWhite;
    TextureRender* irradiance = &emptyCubeBlack;
    Vector4 envColor;
    bool iblAmbient = false;
    auto skys = scene->getComponentArray<SkyComponent>();
    if (skys->size() > 0){
        SkyComponent& sky = skys->getComponentFromIndex(0);
        if (sky.loaded){
            TextureRender* texture = sky.texture.getRender(&emptyCubeWhite);
            if (texture && texture->isCreated()){
                skyTexture = texture;
            }
        }
        if (hasIBL && sky.irradianceMap){
            irradiance = sky.irradianceMap.get();
            iblAmbient = true;
        }
        Vector3 linColor = Color::sRGBToLinear(Vector3(sky.color.x, sky.color.y, sky.color.z));
        // water mirrors the sky, so it dims with the same indirect-light knob the meshes use
        float giIntensity = scene->getGlobalIlluminationIntensity();
        envColor = Vector4(linColor.x * giIntensity, linColor.y * giIntensity,
                           linColor.z * giIntensity, Angle::defaultToRad(sky.rotation));
    }else{
        // no sky, reflects the background color
        Vector4 background = scene->getBackgroundColor();
        Vector3 linColor = Color::sRGBToLinear(Vector3(background.x, background.y, background.z));
        envColor = Vector4(linColor.x, linColor.y, linColor.z, 0.0f);
    }
    render.addTexture(shaderData.getTextureIndex(TextureShaderType::SKYCUBE), ShaderStageType::FRAGMENT, skyTexture);
    render.addTexture(shaderData.getTextureIndex(TextureShaderType::IRRADIANCEMAP), ShaderStageType::FRAGMENT, irradiance);
    loadSpotMaskTexture(shaderData, render);
    if (water.shaderProperties & (1u << 3)){ // 'Shw'
        loadShadowTextures(shaderData, render, true, true);
    }

    render.addTexture(shaderData.getTextureIndex(TextureShaderType::DEPTHTEXTURE), ShaderStageType::FRAGMENT, currentSceneDepthTexture ? currentSceneDepthTexture : &emptyWhite);

    if (water.planarReflection){
        TextureRender* reflection = &emptyBlack;
        CameraComponent* refCam = scene->findComponent<CameraComponent>(getMirrorCamera(entity));
        if (refCam && refCam->framebuffer && refCam->framebuffer->isCreated()){
            reflection = &refCam->framebuffer->getRender().getColorTexture();
        }
        render.addTexture(shaderData.getTextureIndex(TextureShaderType::WATERREFLECTION), ShaderStageType::FRAGMENT, reflection);
    }

    if (water.shaderProperties & (1u << 4)){ // 'Rfr'
        render.addTexture(shaderData.getTextureIndex(TextureShaderType::WATERREFRACTION), ShaderStageType::FRAGMENT, currentSceneCopy ? currentSceneCopy : &emptyBlack);
    }

    render.addTexture(shaderData.getTextureIndex(TextureShaderType::WATEREXCLUSION), ShaderStageType::FRAGMENT,
            (waterExclusionMapLayers > 0) ? &waterExclusionMaps : &emptyArrayWhite);

    WaterWave waves[WATER_WAVE_COUNT];
    getWaterWaves(water, waves);

    vs_water_sim_t vsSim;
    if (water.simulate){
        if (!water.simTexture.isCreated() && !updateWaterField(water))
            return false;

        const size_t texels = (size_t)water.simTextureCells * water.simTextureCells * 4;
        if (water.simDirty && water.simTexels.size() == texels){
            water.simTexture.updateTexture(water.simTexels.data(), texels * sizeof(float));
            water.simDirty = false;
        }

        const float cells = std::max(water.simTextureCells, 1u);
        const Vector2 span = water.simCell * Vector2(cells, cells);
        vsSim.simMap = Vector4(1.0f / std::max(span.x, 0.001f), 1.0f / std::max(span.y, 0.001f),
                1.0f / cells, 1.0f / cells);
        vsSim.simOrigin = Vector4(water.simOrigin.x, water.simOrigin.y, 0.0f, 0.0f);
        vsSim.simCell = Vector4(water.simCell.x, water.simCell.y, std::max(water.simDryDepth, 0.0001f), 0.0f);
        vsSim.simTune = Vector4(1.0f, 0.0f, 0.0f, 0.0f);

        std::pair<int, int> slotTex = shaderData.getTextureIndex(TextureShaderType::WATERFIELD);
        if (slotTex.first == -1)
            return false;
        render.addTexture(slotTex, ShaderStageType::VERTEX, &water.simTexture);
    }

    vs_water_t vsParams;
    vsParams.modelMatrix = transform.modelMatrix;
    vsParams.viewProjectionMatrix = camera.viewProjectionMatrix;
    if (isRenderingFlipped(camera)){
        vsParams.viewProjectionMatrix = Matrix4::scaleMatrix(Vector3(1, -1, 1)) * vsParams.viewProjectionMatrix;
    }
    for (int i = 0; i < WATER_WAVE_COUNT; i++){
        vsParams.waves[i] = Vector4(waves[i].direction.x, waves[i].direction.y, waves[i].amplitude, waves[i].wavelength);
    }
    vsParams.phases = Vector4(water.wavePhase[0], water.wavePhase[1], water.wavePhase[2], water.wavePhase[3]);
    vsParams.params = Vector4(getWaterSteepness(water, waves), 0.0f, 0.0f, 0.0f);

    fs_water_t fsParams;
    fsParams.viewProjection = camera.viewProjectionMatrix;
    fsParams.invViewProjection = camera.viewProjectionMatrix.inverse();
    fsParams.reflectionViewProjection = water.reflectionViewProjection;
    fsParams.shallowColor = Vector4(water.shallowColor.x, water.shallowColor.y, water.shallowColor.z, water.depthFade);
    fsParams.deepColor = Vector4(water.deepColor.x, water.deepColor.y, water.deepColor.z, currentSceneDepthTexture ? 1.0f : 0.0f);
    fsParams.foamColor = Vector4(water.foamColor.x, water.foamColor.y, water.foamColor.z, water.shoreFoam);
    // the wind's own foam, on top of whatever the author drew: whitecaps come with the gust, so this
    // reads the smoothed wind speed and not the slow swell. The fragment scales crestFoam by three
    // before it clamps, so half a whitecap is already a sea going over.
    const float windFoam = getWhitecapFactor(water.seaWind) * std::clamp(water.seaMix, 0.0f, 1.0f);
    fsParams.ripples = Vector4(1.0f / std::max(water.normalScale, 0.01f), water.normalStrength, getDrawnWaveHeight(water),
            std::clamp(water.crestFoam + 0.5f * windFoam, 0.0f, 1.0f));
    fsParams.rippleOffsets = Vector4(water.rippleOffset[0].x, water.rippleOffset[0].y, water.rippleOffset[1].x, water.rippleOffset[1].y);
    fsParams.surface = Vector4(water.reflectivity, water.specularIntensity, std::clamp(water.roughness, 0.0f, 1.0f), water.reflectionDistortion);
    fsParams.envColor = envColor;
    fsParams.eyePos = Vector4(camTransform.worldPosition.x, camTransform.worldPosition.y, camTransform.worldPosition.z, water.time);
    // the planar reflection is rendered for the main camera, other views reflect the sky,
    // and a pass that faded its scene for an eye inside the water fades the surface too
    fsParams.flags = Vector4(hasLights ? 1.0f : 0.0f, iblAmbient ? 1.0f : 0.0f, (mainCamera && water.planarReflection) ? 1.0f : 0.0f,
            (currentSceneCopy && &water == currentUnderwater) ? 1.0f : 0.0f);
    // only a pass that copied its scene refracts, the others blend over it
    fsParams.refraction = Vector4(water.refractionDistortion, (water.refraction && currentSceneCopy) ? 1.0f : 0.0f, 0.0f, 0.0f);
    fsParams.refractionRect = currentSceneCopyRect;
    fsParams.caustics = Vector4(std::max(water.caustics, 0.0f), 1.0f / std::max(water.causticsScale, 0.01f), 0.0f, 0.0f);
    fsParams.waveScale = Vector4(getDrawnWaveLength(water), 0.0f, 0.0f, 0.0f);
    setWaterExclusionParams(water, camTransform.worldPosition, fsParams);

    render.applyUniformBlock(water.slotVSParams, sizeof(vs_water_t), &vsParams);
    if (water.simulate)
        render.applyUniformBlock(water.slotVSSimParams, sizeof(vs_water_sim_t), &vsSim);
    render.applyUniformBlock(water.slotFSParams, water.fsParamsSize, &fsParams);
    render.applyUniformBlock(water.slotFSLighting, sizeof(fs_lighting_t), &fs_lighting);
    const int lightGridSlot = shaderData.getStorageBufferIndex(StorageBufferType::FS_LIGHT_GRID);
    if (lightGridSlot != -1 && clusteredLightGridReady){
        render.addStorageBuffer(lightGridSlot, ShaderStageType::FRAGMENT, &clusteredLightGridBuffer);
    }
    if (water.slotFSFog != -1){
        render.applyUniformBlock(water.slotFSFog, sizeof(float) * 8, &fs_fog);
    }
    render.applyUniformBlock(water.slotVSShadows, sizeof(shadow_projection_t), &shadowProjection);
    render.applyUniformBlock(water.slotFSShadowProjection, sizeof(shadow_projection_t), &shadowProjection);
    render.applyUniformBlock(water.slotFSShadows, sizeof(fs_shadows_t), &fs_shadows);
    render.applyUniformBlock(water.slotFSPointShadows, sizeof(fs_point_shadows_t), &fs_point_shadows);
    writeCustomUniforms(water, scene);
    applyCustomUniforms(render, water.customVSParams, frameTime, passResolution);
    applyCustomUniforms(render, water.customFSParams, frameTime, passResolution);

    render.draw(0, water.indexCount, 1);

    return true;
}

void RenderSystem::destroyWater(Entity entity, WaterComponent& water){
    if (!water.loaded)
        return;

    if (!water.needReload){
        //Destroy shader
        if (water.shader){
            water.shader.reset();
            ShaderPool::remove(ShaderType::WATER, water.shaderProperties, water.customShaderId);
        }
        if (water.underwaterShader){
            water.underwaterShader.reset();
            ShaderPool::remove(ShaderType::UNDERWATER, 0, water.customUnderwaterShaderId);
        }

        //Destroy texture
        water.normalTexture.destroy();
        water.simTexture.destroyTexture();
        water.simTextureCells = 0;

        // the pool keeps the built-in ripples while another water still uses them
        if (water.defaultNormalMap){
            water.defaultNormalMap.reset();
            TexturePool::remove("water|normal|default");
        }
    }

    //Destroy render
    water.render.destroy();
    if (water.underwaterLoaded){
        water.underwaterRender.destroy();
        water.underwaterLoaded = false;
    }

    //Destroy buffers
    if (!water.needReload){
        water.buffer.clearAll();
        water.indices.clearAll();
        water.indexCount = 0;
    }
    water.buffer.getRender()->destroyBuffer();
    water.indices.getRender()->destroyBuffer();

    //Shaders uniforms
    water.slotVSParams = -1;
    water.slotVSSimParams = -1;
    water.slotFSParams = -1;
    water.fsParamsSize = 0;
    water.slotFSLighting = -1;
    water.slotFSFog = -1;
    water.slotVSShadows = -1;
    water.slotFSShadows = -1;
    water.slotFSShadowProjection = -1;
    water.slotFSPointShadows = -1;
    water.customVSParams.clear();
    water.customFSParams.clear();
    water.slotUnderwaterParams = -1;
    water.customUnderwaterParams.clear();

    SystemRender::addQueueCommand(&changeWaterDestroy, new check_load_t{scene, entity});
}

// A box across a water surface from the eye, and within its area, is covered by the water
bool RenderSystem::isBehindWater(const std::vector<WaterRenderData>& waters, const AABB& box){
    if (box.isNull() || !box.isFinite())
        return false;

    for (const WaterRenderData& waterData : waters){
        const AABB& area = waterData.water->worldAABB;
        if (box.getMaximum().x < area.getMinimum().x || box.getMinimum().x > area.getMaximum().x ||
                box.getMaximum().z < area.getMinimum().z || box.getMinimum().z > area.getMaximum().z)
            continue;

        // the surface this pass draws under the box center
        const Vector3 center = box.getCenter();
        float depth = 0.0f;
        const float surface = waterData.transform->worldPosition.y + getWaterSurfaceOffset(*waterData.water, center.x, center.z, nullptr, &depth).y;
        if (depth <= 0.0f)
            continue; // the field ran dry under the box, so it sorts as if there were no water
        if (waterData.eyeAbove ? (box.getMaximum().y < surface) : (box.getMinimum().y > surface))
            return true;
    }

    return false;
}

// the water with the eye below its surface and within its area, if it shows the view from inside
WaterComponent* RenderSystem::findUnderwater(const Vector3& eye){
    // a dry volume, like a cabin below the waterline, keeps the view clear
    auto exclusions = scene->getComponentArray<WaterExclusionComponent>();
    for (int i = 0; i < exclusions->size(); i++){
        Transform* transform = scene->findComponent<Transform>(exclusions->getEntity(i));
        if (transform && isInsideWaterExclusion(exclusions->getComponentFromIndex(i), *transform, eye))
            return NULL;
    }

    auto waters = scene->getComponentArray<WaterComponent>();
    for (int i = 0; i < waters->size(); i++){
        WaterComponent& water = waters->getComponentFromIndex(i);
        Transform* transform = scene->findComponent<Transform>(waters->getEntity(i));
        if (!water.underwater || !water.loaded || water.needReload || !transform || !transform->visible)
            continue;

        const AABB& area = water.worldAABB;
        if (eye.x < area.getMinimum().x || eye.x > area.getMaximum().x ||
                eye.z < area.getMinimum().z || eye.z > area.getMaximum().z)
            continue;

        float depth = 0.0f;
        const float surface = transform->worldPosition.y + getWaterSurfaceOffset(water, eye.x, eye.z, nullptr, &depth).y;
        // standing in the dry part of a field is not being inside the water
        if (depth > 0.0f && eye.y < surface)
            return &water;
    }

    return NULL;
}

// hulls follow the meshes they are built from, and their maps share one texture array
void RenderSystem::updateWaterExclusions(){
    auto exclusions = scene->getComponentArray<WaterExclusionComponent>();
    int layers = 0;
    for (int i = 0; i < exclusions->size(); i++){
        WaterExclusionComponent& exclusion = exclusions->getComponentFromIndex(i);
        Entity entity = exclusions->getEntity(i);

        AABB bounds;
        int layer = -1;
        if (exclusion.shape == WaterExclusionShape::HULL){
            const uint64_t signature = PhysicsSystem::getMeshSignature(scene, entity);
            if (signature != exclusion.hullSignature){
                exclusion.hullSignature = signature;
                exclusion.hullBounds.setNull();
                if (PhysicsSystem::getMeshConvexHull(scene, entity, exclusion.hullPlanes, exclusion.hullPoints)){
                    for (const Vector3& point : exclusion.hullPoints){
                        exclusion.hullBounds.merge(point);
                    }
                    buildWaterExclusionMap(exclusion);
                }else{
                    exclusion.hullMap.clear();
                }
                needUpdateWaterExclusionMaps = true;
            }
            if (!exclusion.hullMap.empty()){
                bounds = exclusion.hullBounds;
                layer = layers++;
            }
        }else{
            const Vector3 half(std::abs(exclusion.size.x) * 0.5f, std::abs(exclusion.size.y) * 0.5f, std::abs(exclusion.size.z) * 0.5f);
            bounds = AABB(exclusion.center - half, exclusion.center + half);
        }
        if (exclusion.hullLayer != layer){
            exclusion.hullLayer = layer;
            needUpdateWaterExclusionMaps = true;
        }

        // sized or scaled to nothing it keeps nothing out, and has no inverse to test with
        Transform* transform = scene->findComponent<Transform>(entity);
        const Vector3 scale = transform ? transform->worldScale : Vector3::ZERO;
        exclusion.aabb = (bounds.volume() * scale.x * scale.y * scale.z != 0.0f) ? bounds : AABB();
        exclusion.worldAABB = exclusion.aabb.isNull() ? AABB() : transform->modelMatrix * exclusion.aabb;
    }

    if (layers != waterExclusionMapLayers){
        needUpdateWaterExclusionMaps = true;
    }
    if (!needUpdateWaterExclusionMaps || !Engine::isViewLoaded())
        return;
    needUpdateWaterExclusionMaps = false;

    if (waterExclusionMapLayers > 0){
        waterExclusionMaps.destroyTexture();
        waterExclusionMapLayers = 0;
    }
    if (layers == 0)
        return;

    std::vector<void*> data(layers);
    std::vector<size_t> sizes(layers);
    for (int i = 0; i < exclusions->size(); i++){
        WaterExclusionComponent& exclusion = exclusions->getComponentFromIndex(i);
        if (exclusion.hullLayer >= 0){
            data[exclusion.hullLayer] = exclusion.hullMap.data();
            sizes[exclusion.hullLayer] = exclusion.hullMap.size();
        }
    }

    if (waterExclusionMaps.createTexture("water|exclusion|maps", WATER_EXCLUSION_MAP_SIZE, WATER_EXCLUSION_MAP_SIZE,
            ColorFormat::RGBA, TextureType::TEXTURE_ARRAY, layers, data.data(), sizes.data(),
            TextureFilter::LINEAR, TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE)){
        waterExclusionMapLayers = layers;
    }else{
        waterExclusionMaps.destroyTexture();
    }
}

// the volumes over the water nearest to the eye
void RenderSystem::setWaterExclusionParams(const WaterComponent& water, const Vector3& eye, fs_water_t& params){
    std::vector<std::pair<float, int>> nearest;
    auto exclusions = scene->getComponentArray<WaterExclusionComponent>();
    for (int i = 0; i < exclusions->size(); i++){
        const WaterExclusionComponent& exclusion = exclusions->getComponentFromIndex(i);
        if (!exclusion.worldAABB.intersects(water.worldAABB))
            continue;
        // a hull waits for its map in the texture
        if (exclusion.shape == WaterExclusionShape::HULL && exclusion.hullLayer >= waterExclusionMapLayers)
            continue;

        nearest.push_back({exclusion.worldAABB.squaredDistance(eye), i});
    }
    std::sort(nearest.begin(), nearest.end());

    int count = 0;
    for (const auto& candidate : nearest){
        const WaterExclusionComponent& exclusion = exclusions->getComponentFromIndex(candidate.second);
        Transform* transform = scene->findComponent<Transform>(exclusions->getEntity(candidate.second));
        if (!transform)
            continue;

        const Matrix4 space = getWaterExclusionSpace(exclusion, transform->modelMatrix);
        for (int r = 0; r < 3; r++){
            params.exclusionRows[count * 3 + r] = space.row(r);
        }
        params.exclusionShapes[count] = Vector4((float)(int)exclusion.shape, (float)exclusion.hullLayer, 0.0f, 0.0f);
        if (++count == MAX_WATER_EXCLUSIONS)
            break;
    }
    params.exclusionCount = Vector4((float)count, 0.0f, 0.0f, 0.0f);
}

// unloaded with the water in destroyWater
bool RenderSystem::loadUnderwater(WaterComponent& water){
    if (water.underwaterLoaded)
        return true;

    water.customUnderwaterShaderId = ShaderPool::registerCustomShader(water.customUnderwaterShader);
    water.underwaterShader = ShaderPool::get(ShaderType::UNDERWATER, 0, water.customUnderwaterShaderId);
    if (!water.underwaterShader->isCreated()){
        // as in loadWater, a fork that failed to compile falls back to the built-in shader
        if (water.customUnderwaterShaderId != 0 && ShaderPool::isShaderBuildFailed(ShaderType::UNDERWATER, 0, water.customUnderwaterShaderId)){
            Log::error("Custom shader '%s' failed to compile; using the built-in shader", water.customUnderwaterShader.c_str());
            water.customUnderwaterShaderId = 0;
            water.underwaterShader = ShaderPool::get(ShaderType::UNDERWATER, 0);
        }
        // may still be building (async in the editor)
        if (!water.underwaterShader->isCreated())
            return false;
    }

    ShaderData& shaderData = water.underwaterShader.get()->shaderData;
    water.underwaterRender.beginLoad(PrimitiveType::TRIANGLES);
    water.underwaterRender.setShader(water.underwaterShader.get());
    water.slotUnderwaterParams = shaderData.getUniformBlockIndex(UniformBlockType::UNDERWATER_FS_PARAMS);
    water.customUnderwaterParams.resolve(shaderData, "u_fs_customParams");
    water.customUnderwaterParams.writeValues(water.shaderUniforms, scene->getInheritedShaderUniforms(ShaderType::WATER, water.customShader));
    if (!water.underwaterRender.endLoad(PIP_RTT_NODEPTH, false, true, CullingMode::BACK, WindingOrder::CCW))
        return false;

    water.underwaterLoaded = true;
    return true;
}

void RenderSystem::drawUnderwater(WaterComponent& water, CameraComponent& camera, Transform& cameraTransform){
    ObjectRender& render = water.underwaterRender;
    if (!render.beginDraw(PIP_RTT_NODEPTH))
        return;

    // the body light of water.frag, with only the directional lights
    TextureRender* irradiance = &emptyCubeBlack;
    Vector3 skyColor(1.0f, 1.0f, 1.0f);
    Vector3 light(1.0f, 1.0f, 1.0f);
    if (hasLights){
        auto skys = scene->getComponentArray<SkyComponent>();
        if (hasIBL && skys->size() > 0 && skys->getComponentFromIndex(0).irradianceMap){
            SkyComponent& sky = skys->getComponentFromIndex(0);
            irradiance = sky.irradianceMap.get();
            skyColor = Color::sRGBToLinear(Vector3(sky.color.x, sky.color.y, sky.color.z));
            light = Vector3(0.0f, 0.0f, 0.0f);
        }else{
            light = scene->getGlobalIlluminationColorLinear() * scene->getGlobalIlluminationIntensity();
        }

        auto lights = scene->getComponentArray<LightComponent>();
        for (size_t i = 0; i < lights->size(); i++){
            const LightComponent& sceneLight = lights->getComponentFromIndex(i);
            if (sceneLight.type == LightType::DIRECTIONAL){
                const float fromAbove = std::max(-sceneLight.worldDirection.normalized().y, 0.0f);
                light += sceneLight.color * (sceneLight.intensity * fromAbove * 2.0f / WATER_TWO_PI);
            }
        }
    }

    fs_underwater_t params;
    params.invViewProjection = camera.viewProjectionMatrix.inverse();
    params.shallowColor = Vector4(water.shallowColor.x, water.shallowColor.y, water.shallowColor.z, water.depthFade);
    params.deepColor = Vector4(water.deepColor.x, water.deepColor.y, water.deepColor.z, currentSceneDepthTexture ? 1.0f : 0.0f);
    params.light = Vector4(light.x, light.y, light.z, 0.0f);
    params.envColor = Vector4(skyColor.x, skyColor.y, skyColor.z, 0.0f);
    params.eyePos = Vector4(cameraTransform.worldPosition.x, cameraTransform.worldPosition.y, cameraTransform.worldPosition.z, 0.0f);
    params.sceneRect = currentSceneCopyRect;

    ShaderData& shaderData = water.underwaterShader.get()->shaderData;
    render.addTexture(shaderData.getTextureIndex(TextureShaderType::SCENECOLORTEXTURE), ShaderStageType::FRAGMENT, currentSceneCopy);
    render.addTexture(shaderData.getTextureIndex(TextureShaderType::DEPTHTEXTURE), ShaderStageType::FRAGMENT, currentSceneDepthTexture ? currentSceneDepthTexture : &emptyWhite);
    render.addTexture(shaderData.getTextureIndex(TextureShaderType::IRRADIANCEMAP), ShaderStageType::FRAGMENT, irradiance);
    render.applyUniformBlock(water.slotUnderwaterParams, sizeof(fs_underwater_t), &params);
    writeCustomUniforms(water, scene);
    applyCustomUniforms(render, water.customUnderwaterParams, frameTime, passResolution);
    render.draw(0, 3, 1);
}

void RenderSystem::destroyLight(LightComponent& light){
    // shadow maps live in the RenderSystem-owned atlases, so a light owns no GPU
    // resources of its own to release here
    light.shadowMapIndex = -1;
}

void RenderSystem::destroyCamera(CameraComponent& camera, bool entityDestroyed){
    if (camera.framebuffer){
        if (entityDestroyed){
            delete camera.framebuffer; // destroy framebuffer in destructor
        }else{
            camera.framebuffer->destroy();
        }
    }
}

// Rect::fitOnRect moves the origin without shrinking the opposite edge, which widens a
// scissor that starts outside the bounds
static Rect intersectScissor(const Rect& rect, const Rect& bounds){
    float boundsRight = bounds.getX() + bounds.getWidth();
    float boundsTop = bounds.getY() + bounds.getHeight();

    float left = std::min(std::max(rect.getX(), bounds.getX()), boundsRight);
    float bottom = std::min(std::max(rect.getY(), bounds.getY()), boundsTop);
    float right = std::max(std::min(rect.getX() + rect.getWidth(), boundsRight), left);
    float top = std::max(std::min(rect.getY() + rect.getHeight(), boundsTop), bottom);

    return Rect(left, bottom, right - left, top - bottom);
}

Rect RenderSystem::getScissorRect(UILayoutComponent& layout, ImageComponent& img, Transform& transform, const Rect& passViewport){
    // the scissor is in the pass target pixels, so it follows the viewport the color pass
    // applied: offscreen targets start at (0,0), a swapchain keeps the letterbox offset
    float viewX = passViewport.getX();
    float viewY = passViewport.getY();
    float viewWidth = passViewport.getWidth();
    float viewHeight = passViewport.getHeight();

    if (viewWidth <= 0.0f || viewHeight <= 0.0f)
        return Rect(0, 0, 0, 0);

    float localLeft = (float) img.patchMarginLeft;
    float localTop = (float) img.patchMarginTop;
    float localRight = (float) layout.width - (float) img.patchMarginRight;
    float localBottom = (float) layout.height - (float) img.patchMarginBottom;

    if (localRight < localLeft)
        localRight = localLeft;
    if (localBottom < localTop)
        localBottom = localTop;

    Vector4 corners[4] = {
        transform.modelViewProjectionMatrix * Vector4(localLeft, localTop, 0.0f, 1.0f),
        transform.modelViewProjectionMatrix * Vector4(localRight, localTop, 0.0f, 1.0f),
        transform.modelViewProjectionMatrix * Vector4(localRight, localBottom, 0.0f, 1.0f),
        transform.modelViewProjectionMatrix * Vector4(localLeft, localBottom, 0.0f, 1.0f)
    };

    // a corner behind the eye has no screen position and a negative w mirrors the rect,
    // so the quad is clipped against w > 0 first
    const float EPSILON = 1e-5f;
    Vector4 visible[8];
    int visibleCount = 0;

    for (int i = 0; i < 4; i++){
        const Vector4& current = corners[i];
        const Vector4& next = corners[(i + 1) % 4];
        bool currentInFront = current.w > EPSILON;
        bool nextInFront = next.w > EPSILON;

        if (currentInFront)
            visible[visibleCount++] = current;

        if (currentInFront != nextInFront)
            visible[visibleCount++] = current + (next - current) * ((EPSILON - current.w) / (next.w - current.w));
    }

    if (visibleCount == 0)
        return Rect(0, 0, 0, 0);

    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;

    for (int i = 0; i < visibleCount; i++){
        float screenX = viewX + (((visible[i].x / visible[i].w) + 1.0f) * 0.5f * viewWidth);
        float screenY = viewY + (((visible[i].y / visible[i].w) + 1.0f) * 0.5f * viewHeight);

        if (i == 0){
            minX = maxX = screenX;
            minY = maxY = screenY;
        }else{
            minX = std::min(minX, screenX);
            minY = std::min(minY, screenY);
            maxX = std::max(maxX, screenX);
            maxY = std::max(maxY, screenY);
        }
    }

    return intersectScissor(Rect(minX, minY, maxX - minX, maxY - minY), passViewport);
}

void RenderSystem::updateTransform(Transform& transform, Entity entity){
    // UI grows around its pivot, so its origin, the top-left corner, moves
    Vector3 origin = transform.position;
    UILayoutComponent* layout = scene->findComponent<UILayoutComponent>(entity);
    if (layout && layout->pivot != Vector2(0, 0)){
        origin += getUIPivotShift(*layout, transform.rotation, transform.scale);
    }

    Matrix4 translateMatrix = Matrix4::translateMatrix(origin);
    Matrix4 rotationMatrix = transform.rotation.getRotationMatrix();
    Matrix4 scaleMatrix = Matrix4::scaleMatrix(transform.scale);

    transform.localMatrix = translateMatrix * rotationMatrix * scaleMatrix;

    Transform* transformParent = NULL;
    if (transform.parent != NULL_ENTITY){
        transformParent = scene->findComponent<Transform>(transform.parent);
    }

    if (transformParent){
        transform.modelMatrix = transformParent->modelMatrix * transform.localMatrix;

        transform.worldPosition = transformParent->modelMatrix * origin;
        transform.worldScale = transformParent->worldScale * transform.scale;
        transform.worldRotation = transformParent->worldRotation * transform.rotation;
    }else{
        transform.modelMatrix = transform.localMatrix;

        transform.worldPosition = origin;
        transform.worldScale = transform.scale;
        transform.worldRotation = transform.rotation;
    }

    if (hasLights){
        transform.normalMatrix = transform.modelMatrix.inverse().transpose();
    }
}

void RenderSystem::updateCamera(CameraComponent& camera, Transform& transform){
    //Update ProjectionMatrix
    if (camera.type == CameraType::CAMERA_UI){
        camera.projectionMatrix = Matrix4::orthoMatrix(camera.leftClip, camera.rightClip, camera.topClip, camera.bottomClip, camera.nearClip, camera.farClip);
        camera.effectiveFarClip = camera.farClip;
    }else if (camera.type == CameraType::CAMERA_ORTHO) {
        camera.projectionMatrix = Matrix4::orthoMatrix(camera.leftClip, camera.rightClip, camera.bottomClip, camera.topClip, camera.nearClip, camera.farClip);
        camera.effectiveFarClip = camera.farClip;
    }else if (camera.type == CameraType::CAMERA_PERSPECTIVE){
        // Capping the projection is what makes the scene render distance a culling distance:
        // the frustum planes are built from this matrix below, so the far plane moves in with
        // it and geometry past it stops reaching the draw calls at all.
        Vector2 nearFar = getRenderedNearFar(camera, scene->getEffectiveRenderDistance());
        // a farClip of 0 is the culling tests' "no far plane" sentinel; capping it here would
        // hand them a near-zero one instead
        camera.effectiveFarClip = camera.farClip == 0.0f ? 0.0f : nearFar.y;
        camera.projectionMatrix = Matrix4::perspectiveMatrix(camera.yfov, camera.aspect, nearFar.x, nearFar.y);
    }

    if (camera.useTarget){
        camera.direction = (transform.position - camera.target).normalize();
        camera.right = (camera.up.crossProduct(camera.direction)).normalize();
        //camera.up = camera.direction.crossProduct(camera.right); // no need to align, keep "up" always same

        if (transform.parent != NULL_ENTITY){
            camera.worldTarget = transform.modelMatrix * (camera.target - transform.position);
            camera.worldUp = ((transform.modelMatrix * camera.up) - (transform.modelMatrix * Vector3(0,0,0))).normalize();
        }else{
            camera.worldTarget = camera.target;
            camera.worldUp = camera.up;
        }

        //Update ViewMatrix
        camera.viewMatrix = Matrix4::lookAtMatrix(transform.worldPosition, camera.worldTarget, camera.worldUp);
    }else{
        camera.direction = transform.rotation * Vector3(0, 0, 1);
        camera.right = transform.rotation * Vector3(1, 0, 0);

        camera.worldTarget = transform.worldPosition - (transform.worldRotation * Vector3(0, 0, 1));

        //Update ViewMatrix
        camera.viewMatrix = transform.worldRotation.inverse().getRotationMatrix();
        camera.viewMatrix.translateInPlace(-transform.worldPosition.x, -transform.worldPosition.y, -transform.worldPosition.z);
    }

    // planar reflection drives the view matrix directly (mainView * reflect);
    // projection above still comes from the copied perspective params
    if (camera.hasCustomViewMatrix){
        camera.viewMatrix = camera.customViewMatrix;
    }

    camera.worldRight = Vector3(camera.viewMatrix[0][0], camera.viewMatrix[1][0], camera.viewMatrix[2][0]);
    camera.worldUp = Vector3(camera.viewMatrix[0][1], camera.viewMatrix[1][1], camera.viewMatrix[2][1]);
    camera.worldDirection = Vector3(camera.viewMatrix[0][2], camera.viewMatrix[1][2], camera.viewMatrix[2][2]);

    //Update ViewProjectionMatrix
    camera.viewProjectionMatrix = camera.projectionMatrix * camera.viewMatrix;

    updateCameraFrustumPlanes(camera.viewProjectionMatrix, camera.frustumPlanes);
}

// Creates the internal render-to-texture camera a mirror (or a reflecting water)
// drives: a hidden system entity (not serialized, not shown in the editor) whose
// framebuffer feeds the mirror mesh base texture. Returns the camera entity.
Entity RenderSystem::createMirrorCamera(Entity mirrorEntity){
    const bool mirror = scene->findComponent<MirrorComponent>(mirrorEntity) != NULL;
    if (!mirror && !scene->findComponent<WaterComponent>(mirrorEntity))
        return NULL_ENTITY;

    const Entity existing = getMirrorCamera(mirrorEntity);
    if (existing != NULL_ENTITY && scene->isEntityCreated(existing))
        return existing;

    Entity camEntity = scene->createSystemEntity();
    scene->addComponent<Transform>(camEntity, {});
    scene->addComponent<CameraComponent>(camEntity, {});

    CameraComponent& cam = scene->getComponent<CameraComponent>(camEntity);
    cam.type = CameraType::CAMERA_PERSPECTIVE;
    cam.renderToTexture = true;
    cam.autoResize = false; // driven entirely by updateMirrors
    // ripples distort a water reflection, so half the canvas is enough there
    const int divisor = mirror ? 1 : 2;
    int w = (Engine::getCanvasWidth() > 0) ? std::max(Engine::getCanvasWidth() / divisor, 1) : 1024;
    int h = (Engine::getCanvasHeight() > 0) ? std::max(Engine::getCanvasHeight() / divisor, 1) : 1024;
    cam.framebuffer->setWidth(w);
    cam.framebuffer->setHeight(h);
    // keep authored fields in sync so RenderSystem::update doesn't resize it back
    cam.framebufferWidth = w;
    cam.framebufferHeight = h;

    mirrorCameras[mirrorEntity] = camEntity;
    return camEntity;
}

Entity RenderSystem::getMirrorCamera(Entity mirrorEntity) const{
    auto it = mirrorCameras.find(mirrorEntity);
    return it != mirrorCameras.end() ? it->second : NULL_ENTITY;
}

void RenderSystem::destroyMirrorCamera(Entity entity){
    // Ownership leaves the map before entity destruction dispatches removal callbacks.
    auto camera = mirrorCameras.extract(entity);
    if (camera.empty())
        return;

    const Entity cameraEntity = camera.mapped();
    if (cameraEntity == NULL_ENTITY || !scene->isEntityCreated(cameraEntity))
        return;

    // Detach the borrowed framebuffer before destroying the camera that owns it.
    if (CameraComponent* cameraComponent = scene->findComponent<CameraComponent>(cameraEntity)){
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
            if (mesh->numSubmeshes > 0){
                Texture& baseTexture = mesh->submeshes[0].material.baseColorTexture;
                if (baseTexture.getFramebuffer() == cameraComponent->framebuffer){
                    baseTexture = Texture();
                }
            }
        }
    }
    scene->destroyEntity(cameraEntity);
}

// Drives each mirror's reflection camera: its view = mainView * reflect(plane),
// so the reflected scene is rendered into that camera's framebuffer. Runs after
// the main camera is updated and before the draw pass. The handedness flip from
// the reflection is compensated by invertCulling on the reflection camera.
// Reflecting waters get the same camera, mirrored across their water level.
void RenderSystem::updateMirrors(Entity mainCameraEntity){
    auto mirrors = scene->getComponentArray<MirrorComponent>();
    auto waters = scene->getComponentArray<WaterComponent>();

    // a water that stopped reflecting the scene gives its camera back
    bool reflectingWater = false;
    for (size_t i = 0; i < waters->size(); i++){
        Entity entity = waters->getEntity(i);
        if (waters->getComponentFromIndex(i).planarReflection){
            reflectingWater = true;
        }else if (getMirrorCamera(entity) != NULL_ENTITY && !scene->findComponent<MirrorComponent>(entity)){
            destroyMirrorCamera(entity);
        }
    }

    if (mirrors->size() == 0 && !reflectingWater)
        return;

    CameraComponent* mainCameraPtr = scene->findComponent<CameraComponent>(mainCameraEntity);
    if (!mainCameraPtr)
        return;

    // copy main camera state before any camera creation: creating a reflection
    // camera below can reallocate the camera/transform arrays and dangle references
    const Matrix4 mainView = mainCameraPtr->viewMatrix;
    // main camera eye in world space (reflected per-mirror below); derived from the
    // view matrix so it doesn't depend on the (possibly dangling) camera component
    const Vector3 mainEye = mainView.inverse() * Vector3(0.0f, 0.0f, 0.0f);
    const CameraType mainType = mainCameraPtr->type;
    const float mYfov = mainCameraPtr->yfov, mAspect = mainCameraPtr->aspect;
    const float mNear = mainCameraPtr->nearClip, mFar = mainCameraPtr->farClip;
    const float mLeft = mainCameraPtr->leftClip, mRight = mainCameraPtr->rightClip;
    const float mBottom = mainCameraPtr->bottomClip, mTop = mainCameraPtr->topClip;
    // mainCameraPtr must not be used past here (may dangle after camera creation)

    // Creating a reflection camera appends to the camera/transform arrays but never
    // the mirror or water arrays, so indexed iteration over them stays valid. The plane
    // is copied, as the owner's transform may move.
    auto driveReflectionCamera = [&](Entity owner, Vector3 worldNormal, Vector3 worldPoint) -> CameraComponent* {
        Entity camEntity = getMirrorCamera(owner);
        if (camEntity == NULL_ENTITY || !scene->isEntityCreated(camEntity)){
            camEntity = createMirrorCamera(owner);
            // the loop counting render-to-texture cameras has run, but draw() still
            // renders this one in the same frame
            hasMultipleCameras = true;
        }

        CameraComponent* refCam = scene->findComponent<CameraComponent>(camEntity);
        Transform* refCamTransform = scene->findComponent<Transform>(camEntity);
        if (!refCam || !refCamTransform)
            return NULL;

        Plane plane(worldNormal, worldPoint);

        // reflection view keeps the handedness flip (true mirror); winding is
        // restored by invertCulling so reflected geometry shows front faces
        Matrix4 reflectMat = Matrix4::reflectMatrix(plane);
        refCam->customViewMatrix = mainView * reflectMat;
        refCam->hasCustomViewMatrix = true;

        // Give the reflection camera the reflected eye position. The custom view
        // matrix overrides the view, but distance-based effects still read the
        // transform's worldPosition (terrain CDLOD detail + morph origin, transparent
        // sorting, per-camera lighting eye) — without this they'd key off the origin
        // and the reflected terrain would morph at the wrong LOD (visible cracks).
        refCamTransform->worldPosition = reflectMat * mainEye;
        refCam->invertCulling = true;
        refCam->renderToTexture = true;

        // match the main camera projection so the reflection lines up 1:1
        refCam->type = mainType;
        refCam->yfov = mYfov;
        refCam->aspect = mAspect;
        refCam->nearClip = mNear;
        refCam->farClip = mFar;
        refCam->leftClip = mLeft;
        refCam->rightClip = mRight;
        refCam->bottomClip = mBottom;
        refCam->topClip = mTop;

        // recompute the reflection camera matrices with the override applied
        updateCamera(*refCam, *refCamTransform);

        // oblique near-plane clipping: move the reflection camera's near plane onto
        // the mirror plane so geometry behind the mirror doesn't leak into the
        // reflection. The clip plane (mirror plane, normal toward the front/reflecting
        // side) is taken to the reflection camera's view space via inverse-transpose.
        // Applied ONLY to viewProjectionMatrix (used by meshes), NOT projectionMatrix:
        // the skybox renders from projectionMatrix and the oblique depth would clip it.
        // Only the depth row changes, so projective sampling (clip.xy/w) is unaffected.
        Vector4 worldClipPlane(worldNormal.x, worldNormal.y, worldNormal.z, plane.d);
        Vector4 viewClipPlane = refCam->viewMatrix.inverse().transpose() * worldClipPlane;
        Matrix4 obliqueProjection = refCam->projectionMatrix.obliqueNearClip(viewClipPlane);
        refCam->viewProjectionMatrix = obliqueProjection * refCam->viewMatrix;
        updateCameraFrustumPlanes(refCam->viewProjectionMatrix, refCam->frustumPlanes);

        return refCam;
    };

    for (size_t i = 0; i < mirrors->size(); i++){
        Entity entity = mirrors->getEntity(i);

        Transform* mirrorTransform = scene->findComponent<Transform>(entity);
        if (!mirrorTransform)
            continue;

        // mirror plane in world space (entity position, world-rotated normal)
        Vector3 worldNormal = (mirrorTransform->worldRotation * mirrors->getComponentFromIndex(i).normal).normalize();

        CameraComponent* refCam = driveReflectionCamera(entity, worldNormal, mirrorTransform->worldPosition);
        if (!refCam)
            continue;

        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (mesh && mesh->numSubmeshes > 0){
            // (re)bind the reflection framebuffer to the mesh base texture every frame.
            // Doing this per-frame (not just at camera creation) re-heals the mirror
            // after a play-stop snapshot restore, which clears the framebuffer binding
            // (framebuffer textures aren't serialized). One-shot: no rebind once matched.
            Texture& baseTex = mesh->submeshes[0].material.baseColorTexture;
            if (baseTex.getFramebuffer() != refCam->framebuffer){
                baseTex.setFramebuffer(refCam->framebuffer);
                mesh->needReload = true;
            }

            // hand the reflection VP to the mirror mesh for projective sampling
            // (USE_MIRROR shader); logical matrix, the shader handles the texture origin
            mesh->mirrorViewProjection = refCam->viewProjectionMatrix;
        }
    }

    for (size_t i = 0; i < waters->size(); i++){
        if (!waters->getComponentFromIndex(i).planarReflection)
            continue;

        Entity entity = waters->getEntity(i);
        Transform* waterTransform = scene->findComponent<Transform>(entity);
        if (!waterTransform)
            continue;

        // mirrored across the water level
        CameraComponent* refCam = driveReflectionCamera(entity, Vector3(0.0f, 1.0f, 0.0f), waterTransform->worldPosition);
        if (!refCam)
            continue;

        // logical matrix for the projective lookup in water.frag
        waters->getComponentFromIndex(i).reflectionViewProjection = refCam->viewProjectionMatrix;
    }
}

void RenderSystem::updateSkyViewProjection(SkyComponent& sky, CameraComponent& camera){
    Matrix4 skyViewMatrix = camera.viewMatrix;

    skyViewMatrix.set(3,0,0);
    skyViewMatrix.set(3,1,0);
    skyViewMatrix.set(3,2,0);
    skyViewMatrix.set(3,3,1);
    skyViewMatrix.set(2,3,0);
    skyViewMatrix.set(1,3,0);
    skyViewMatrix.set(0,3,0);

    Matrix4 rotationMatrix = Quaternion(0, sky.rotation, 0).getRotationMatrix();

    sky.skyViewProjectionMatrix = camera.projectionMatrix * skyViewMatrix * rotationMatrix;

    if (isRenderingFlipped(camera)){
        static const Matrix4 flipYMatrix = Matrix4::scaleMatrix(Vector3(1, -1, 1));
        sky.skyViewProjectionMatrix = flipYMatrix * sky.skyViewProjectionMatrix;
    }

    sky.needUpdateSky = false;
}

void RenderSystem::updatePoints(PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform){
    float worldScale = std::max(transform.worldScale.x, std::max(transform.worldScale.y, transform.worldScale.z));

    points.numVisible = 0;
    points.aabb.setNull();
    size_t pointsSize = (points.points.size() < points.maxPoints)? points.points.size() : points.maxPoints;
    if (points.renderPoints.size() < pointsSize){
        points.renderPoints.resize(pointsSize);
    }

    for (int i = 0; i < pointsSize; i++){
        if (points.points[i].visible){
            PointRenderData& renderPoint = points.renderPoints[points.numVisible];
            renderPoint.position = points.points[i].position;
            renderPoint.color = points.points[i].color;
            renderPoint.size = points.points[i].size * worldScale;
            renderPoint.rotation = points.points[i].rotation;
            renderPoint.textureRect = points.points[i].textureRect;
            renderPoint.particleVelocity = points.points[i].particleVelocity;
            renderPoint.particleAcceleration = points.points[i].particleAcceleration;
            renderPoint.particleSpawnTime = points.points[i].particleSpawnTime;
            renderPoint.particleLife = points.points[i].particleLife;
            points.aabb.merge(renderPoint.position);
            if (points.gpuSimulation){
                const float life = std::max(0.0f, renderPoint.particleLife);
                const Vector3 endPosition = renderPoint.position + renderPoint.particleVelocity * life +
                    renderPoint.particleAcceleration * (0.5f * life * life);
                points.aabb.merge(endPosition);
            }
            points.numVisible++;
        }
    }
    points.renderPoints.resize(points.numVisible);

    if (points.loaded)
        points.needUpdateBuffer = true;
}

void RenderSystem::sortPoints(PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform){
    Vector3 camDir = (camTransform.worldPosition - camera.worldTarget).normalize();

    auto comparePoints = [&transform, &camDir](const PointRenderData& a, const PointRenderData& b) -> bool {
        return (transform.modelMatrix * a.position).dotProduct(camDir) < (transform.modelMatrix * b.position).dotProduct(camDir);
    };
    std::sort(points.renderPoints.begin(), points.renderPoints.end(), comparePoints);

    if (points.loaded)
        points.needUpdateBuffer = true;
}

void RenderSystem::updateTerrain(TerrainComponent& terrain, Transform& transform, CameraComponent& camera, Transform& cameraTransform, int viewIndex){
    if (terrain.heightMapLoaded){
        for (int i = 0; i < terrain.numNodes; i++){
            terrain.nodes[i].visible = false;
        }

        for (int s = 0; s < 2; s++){
            terrain.views[viewIndex].nodesbuffer[s].clear();
        }

        for (int i = 0; i < (terrain.rootGridSize*terrain.rootGridSize); i++){
            terrainNodeLODSelect(terrain, transform, camera, cameraTransform, terrain.nodes[terrain.grid[i]], terrain.levels-1, viewIndex);
        }

        terrain.views[viewIndex].needUpdateNodesBuffer = true;

        terrain.views[viewIndex].nodesEyePos = Vector3(cameraTransform.worldPosition.x, cameraTransform.worldPosition.y, cameraTransform.worldPosition.z);
    }
}

AABB RenderSystem::getTerrainNodeAABB(Transform& transform, TerrainNode& terrainNode){
    float halfSize = terrainNode.size / 2.0f;
    Vector3 localMin(terrainNode.position.x - halfSize, terrainNode.minHeight, terrainNode.position.y - halfSize);
    Vector3 localMax(terrainNode.position.x + halfSize, terrainNode.maxHeight, terrainNode.position.y + halfSize);

    AABB box(localMin, localMax);
    box.transform(transform.modelMatrix);
    return box;
};

bool RenderSystem::isTerrainNodeInSphere(Vector3 position, float radius, const AABB& box) {
    float r2 = radius*radius;

    Vector3 c1 = box.getMinimum();
    Vector3 c2 = box.getMaximum();
    Vector3 distV;

    if (position.x < c1.x) distV.x = (position.x - c1.x);
    else if (position.x > c2.x) distV.x = (position.x - c2.x);
    if (position.y < c1.y) distV.y = (position.y - c1.y);
    else if (position.y > c2.y) distV.y = (position.y - c2.y);
    if (position.z < c1.z) distV.z = (position.z - c1.z);
    else if (position.z > c2.z) distV.z = (position.z - c2.z);

    float dist2 = distV.dotProduct(distV);

    return dist2 <= r2;
}

// appends one selected CDLOD node's per-instance data (position, size, range, resolution)
static void appendTerrainNode(InterleavedBuffer& buffer, const TerrainNode& node){
    buffer.addVector2(AttributeType::TERRAINNODEPOSITION, node.position);
    buffer.addFloat(AttributeType::TERRAINNODESIZE, node.size);
    buffer.addFloat(AttributeType::TERRAINNODERANGE, node.currentRange);
    buffer.addFloat(AttributeType::TERRAINNODERESOLUTION, node.resolution);
}

bool RenderSystem::terrainNodeLODSelect(TerrainComponent& terrain, Transform& transform, CameraComponent& camera, Transform& cameraTransform, TerrainNode& terrainNode, int lodLevel, int viewIndex){
    terrainNode.currentRange = terrain.ranges[lodLevel];

    AABB box = getTerrainNodeAABB(transform, terrainNode);

    if (!isTerrainNodeInSphere(cameraTransform.worldPosition, terrain.ranges[lodLevel], box)) {
        // no node or child nodes were selected
        return false;
    }

    if (!isInsideCamera(camera, box)) {
        // return true to parent node does not select itself over area
        return true;
    }

    if( lodLevel == 0 ) {
        //Full resolution
        terrainNode.resolution = terrain.resolution;
        terrainNode.visible = true;
        appendTerrainNode(terrain.views[viewIndex].nodesbuffer[0], terrainNode);

        return true;
    } else {

        if( !isTerrainNodeInSphere(cameraTransform.worldPosition, terrain.ranges[lodLevel-1], box) ) {
            //Full resolution
            terrainNode.resolution = terrain.resolution;
            terrainNode.visible = true;
            appendTerrainNode(terrain.views[viewIndex].nodesbuffer[0], terrainNode);
        } else {
            for (int i = 0; i < 4; i++) {
                TerrainNode& child = terrain.nodes[terrainNode.childs[i]];
                if (!terrainNodeLODSelect(terrain, transform, camera, cameraTransform, child, lodLevel-1, viewIndex)){
                    //Half resolution
                    child.resolution = terrain.resolution / 2;
                    child.currentRange = terrainNode.currentRange;
                    child.visible = true;
                    appendTerrainNode(terrain.views[viewIndex].nodesbuffer[1], child);
                }
            }
        }

        return true;
    }

}

// Fills tilemapDrawRanges with the index runs of a tilemap submesh that survive
// frustum culling, merging chunks that are adjacent in the index buffer so a visible
// region costs as few draw calls as it can. Returns false when nothing of the submesh
// is visible; an empty range list on success means "draw the submesh whole", the
// fallback for a submesh the chunk data does not cover.
bool RenderSystem::selectTilemapChunks(TilemapComponent& tilemap, unsigned int submeshIndex, const float cameraFar, const Plane frustumPlanes[6]){
    tilemapDrawRanges.clear();

    if ((submeshIndex + 1) >= tilemap.chunkStart.size())
        return true;

    const unsigned int begin = tilemap.chunkStart[submeshIndex];
    const unsigned int end = tilemap.chunkStart[submeshIndex + 1];

    for (unsigned int c = begin; c < end; c++){
        const TilemapChunk& chunk = tilemap.chunks[c];

        if (!isInsideCamera(cameraFar, frustumPlanes, chunk.worldAABB))
            continue;

        if (!tilemapDrawRanges.empty() && (tilemapDrawRanges.back().offset + tilemapDrawRanges.back().count) == chunk.offset){
            tilemapDrawRanges.back().count += chunk.count;
        }else{
            tilemapDrawRanges.push_back({chunk.offset, chunk.count});
        }
    }

    return !tilemapDrawRanges.empty();
}

void RenderSystem::updateCameraSize(Entity entity){
    Signature signature = scene->getSignature(entity);
    if (signature.test(scene->getComponentId<CameraComponent>())){
        CameraComponent& camera = scene->getComponent<CameraComponent>(entity);
        
        Rect rect;
        if (camera.renderToTexture) {
            rect = Rect(0, 0, camera.framebuffer->getWidth(), camera.framebuffer->getHeight());
        }else{
            rect = Rect(0, 0, Engine::getCanvasWidth(), Engine::getCanvasHeight());
        }

        if (camera.autoResize){
            float newLeft = rect.getX();
            float newBottom = rect.getY();
            float newRight = rect.getWidth();
            float newTop = rect.getHeight();
            if (newRight <= 0.0f || newTop <= 0.0f) return;
            float newAspect = newRight / newTop;

            if ((camera.leftClip != newLeft) || (camera.bottomClip != newBottom) || (camera.rightClip != newRight) || (camera.topClip != newTop) || (camera.aspect != newAspect)){
                camera.leftClip = newLeft;
                camera.bottomClip = newBottom;
                camera.rightClip = newRight;
                camera.topClip = newTop;
                camera.aspect = newAspect;

                camera.needUpdate = true;
            }
        }
    }
}

bool RenderSystem::isInsideCamera(const float cameraFar, const Plane frustumPlanes[6], const AABB& box){
    if (box.isNull() || box.isInfinite())
        return false;

    Vector3 centre = box.getCenter();
    Vector3 halfSize = box.getHalfSize();

    for (int plane = 0; plane < 6; ++plane){
        if (plane == FRUSTUM_PLANE_FAR && cameraFar == 0)
            continue;

        Plane::Side side = frustumPlanes[plane].getSide(centre, halfSize);
        if (side == Plane::Side::NEGATIVE_SIDE){
            return false;
        }
    }

    return true;
}

bool RenderSystem::isInsideCamera(CameraComponent& camera, const AABB& box){
    return isInsideCamera(camera.effectiveFarClip, camera.frustumPlanes, box);
}

bool RenderSystem::isInsideCamera(CameraComponent& camera, const Vector3& point){
    for (int plane = 0; plane < 6; ++plane){
        if (plane == FRUSTUM_PLANE_FAR && camera.effectiveFarClip == 0)
            continue;

        if (camera.frustumPlanes[plane].getSide(point) == Plane::Side::NEGATIVE_SIDE){
            return false;
        }
    }

    return true;
}

bool RenderSystem::isInsideCamera(CameraComponent& camera, const Vector3& center, const float& radius){
    return isInsideCamera(camera.effectiveFarClip, camera.frustumPlanes, center, radius);
}

bool RenderSystem::isInsideCamera(const float cameraFar, const Plane frustumPlanes[6], const Vector3& center, const float& radius){
    for (int plane = 0; plane < 6; ++plane){
        if (plane == FRUSTUM_PLANE_FAR && cameraFar == 0)
            continue;

        if (frustumPlanes[plane].getDistance(center) < -radius){
            return false;
        }
    }

    return true;
}

void RenderSystem::needReloadPoints() {
    auto pointsArray = scene->getComponentArray<PointsComponent>();
    for (int i = 0; i < pointsArray->size(); i++) {
        PointsComponent& points = pointsArray->getComponentFromIndex(i);
        points.needReload = true;
    }
}

void RenderSystem::needReloadLines() {
    auto linesArray = scene->getComponentArray<LinesComponent>();
    for (int i = 0; i < linesArray->size(); i++) {
        LinesComponent& lines = linesArray->getComponentFromIndex(i);
        lines.needReload = true;
    }
}

void RenderSystem::needReloadMeshes() {
    shadowAtlasContentDirty = true;
    shadowPointAtlasContentDirty = true;
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++) {
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        mesh.needReload = true;
    }
}

void RenderSystem::needReloadUIs() {
    auto uis = scene->getComponentArray<UIComponent>();
    for (int i = 0; i < uis->size(); i++) {
        UIComponent& ui = uis->getComponentFromIndex(i);
        ui.needReload = true;
    }
}

void RenderSystem::needReloadSky() {
    auto skyArray = scene->getComponentArray<SkyComponent>();
    for (int i = 0; i < skyArray->size(); i++) {
        SkyComponent& sky = skyArray->getComponentFromIndex(i);
        sky.needReload = true;
    }
}

void RenderSystem::needReloadWater() {
    auto waters = scene->getComponentArray<WaterComponent>();
    for (int i = 0; i < waters->size(); i++) {
        WaterComponent& water = waters->getComponentFromIndex(i);
        water.needReload = true;
    }
}

bool RenderSystem::isAllLoaded() const{
    size_t loaded;
    size_t total;
    getLoadCount(loaded, total);

    return loaded == total;
}

void RenderSystem::getLoadCount(size_t& loaded, size_t& total) const{
    loaded = 0;
    total = 0;

    auto count = [&](bool isLoaded){
        total++;
        if (isLoaded)
            loaded++;
    };

    // Check MeshComponents
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++) {
        const MeshComponent& mesh = meshes->getComponentFromIndex(i);
        // An empty tilemap never loads: loadMesh() has no buffers to create until a tile is added.
        if (!mesh.loaded) {
            Entity meshEntity = meshes->getEntity(i);
            const TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(meshEntity);
            if (tilemap && tilemap->numTiles == 0)
                continue;
            // Same reasoning for every other mesh that carries no data yet - there is no load
            // waiting to finish, and counting it would keep isAllLoaded() false forever, which
            // stops the editor from ever idling and holds the scene behind the loading gate.
            if (!scene->findComponent<TerrainComponent>(meshEntity) && meshHasNothingToLoad(mesh))
                continue;
        }
        count(mesh.loaded);
    }

    // Check models with no mesh of their own, their load builds the parts
    auto models = scene->getComponentArray<ModelComponent>();
    for (int i = 0; i < models->size(); i++) {
        const ModelComponent& model = models->getComponentFromIndex(i);
        Entity entity = models->getEntity(i);
        if (model.filename.empty() || !scene->findComponent<Transform>(entity) || scene->findComponent<MeshComponent>(entity))
            continue;
        count(!model.needUpdateModel);
    }

    // Check UIComponents
    auto uis = scene->getComponentArray<UIComponent>();
    for (int i = 0; i < uis->size(); i++) {
        const UIComponent& ui = uis->getComponentFromIndex(i);
        // An empty UI never loads: loadUI() retries each frame and nothing sets needReload.
        const size_t uiBufferSize = std::max(ui.buffer.getSize(),
                                             (size_t)ui.minBufferCount * ui.buffer.getStride());
        if (uiBufferSize > 0)
            count(ui.loaded);
    }

    // Check PointsComponents
    auto pointsArray = scene->getComponentArray<PointsComponent>();
    for (int i = 0; i < pointsArray->size(); i++) {
        const PointsComponent& points = pointsArray->getComponentFromIndex(i);
        count(points.loaded);
    }

    // Check LinesComponents
    auto linesArray = scene->getComponentArray<LinesComponent>();
    for (int i = 0; i < linesArray->size(); i++) {
        const LinesComponent& lines = linesArray->getComponentFromIndex(i);
        // An empty Lines never loads: addLine() only sets needReload when maxLines grows.
        if (!lines.lines.empty())
            count(lines.loaded);
    }

    // Check SkyComponents
    auto skyArray = scene->getComponentArray<SkyComponent>();
    for (int i = 0; i < skyArray->size(); i++) {
        const SkyComponent& sky = skyArray->getComponentFromIndex(i);
        count(sky.loaded);
    }

    // Check WaterComponents, one without a Transform never loads; the underwater pass
    // builds with its water, so the editor keeps drawing until it is in
    auto waters = scene->getComponentArray<WaterComponent>();
    for (int i = 0; i < waters->size(); i++) {
        if (!scene->findComponent<Transform>(waters->getEntity(i)))
            continue;
        const WaterComponent& water = waters->getComponentFromIndex(i);
        count(water.loaded);
        if (water.underwater)
            count(water.underwaterLoaded);
    }
}

void RenderSystem::updateCameraFrustumPlanes(const Matrix4 viewProjectionMatrix, Plane* frustumPlanes){

    frustumPlanes[FRUSTUM_PLANE_LEFT].normal.x = viewProjectionMatrix[0][3] + viewProjectionMatrix[0][0];
    frustumPlanes[FRUSTUM_PLANE_LEFT].normal.y = viewProjectionMatrix[1][3] + viewProjectionMatrix[1][0];
    frustumPlanes[FRUSTUM_PLANE_LEFT].normal.z = viewProjectionMatrix[2][3] + viewProjectionMatrix[2][0];
    frustumPlanes[FRUSTUM_PLANE_LEFT].d = viewProjectionMatrix[3][3] + viewProjectionMatrix[3][0];

    frustumPlanes[FRUSTUM_PLANE_RIGHT].normal.x = viewProjectionMatrix[0][3] - viewProjectionMatrix[0][0];
    frustumPlanes[FRUSTUM_PLANE_RIGHT].normal.y = viewProjectionMatrix[1][3] - viewProjectionMatrix[1][0];
    frustumPlanes[FRUSTUM_PLANE_RIGHT].normal.z = viewProjectionMatrix[2][3] - viewProjectionMatrix[2][0];
    frustumPlanes[FRUSTUM_PLANE_RIGHT].d = viewProjectionMatrix[3][3] - viewProjectionMatrix[3][0];

    frustumPlanes[FRUSTUM_PLANE_TOP].normal.x = viewProjectionMatrix[0][3] - viewProjectionMatrix[0][1];
    frustumPlanes[FRUSTUM_PLANE_TOP].normal.y = viewProjectionMatrix[1][3] - viewProjectionMatrix[1][1];
    frustumPlanes[FRUSTUM_PLANE_TOP].normal.z = viewProjectionMatrix[2][3] - viewProjectionMatrix[2][1];
    frustumPlanes[FRUSTUM_PLANE_TOP].d = viewProjectionMatrix[3][3] - viewProjectionMatrix[3][1];

    frustumPlanes[FRUSTUM_PLANE_BOTTOM].normal.x = viewProjectionMatrix[0][3] + viewProjectionMatrix[0][1];
    frustumPlanes[FRUSTUM_PLANE_BOTTOM].normal.y = viewProjectionMatrix[1][3] + viewProjectionMatrix[1][1];
    frustumPlanes[FRUSTUM_PLANE_BOTTOM].normal.z = viewProjectionMatrix[2][3] + viewProjectionMatrix[2][1];
    frustumPlanes[FRUSTUM_PLANE_BOTTOM].d = viewProjectionMatrix[3][3] + viewProjectionMatrix[3][1];

    frustumPlanes[FRUSTUM_PLANE_NEAR].normal.x = viewProjectionMatrix[0][3] + viewProjectionMatrix[0][2];
    frustumPlanes[FRUSTUM_PLANE_NEAR].normal.y = viewProjectionMatrix[1][3] + viewProjectionMatrix[1][2];
    frustumPlanes[FRUSTUM_PLANE_NEAR].normal.z = viewProjectionMatrix[2][3] + viewProjectionMatrix[2][2];
    frustumPlanes[FRUSTUM_PLANE_NEAR].d = viewProjectionMatrix[3][3] + viewProjectionMatrix[3][2];

    frustumPlanes[FRUSTUM_PLANE_FAR].normal.x = viewProjectionMatrix[0][3] - viewProjectionMatrix[0][2];
    frustumPlanes[FRUSTUM_PLANE_FAR].normal.y = viewProjectionMatrix[1][3] - viewProjectionMatrix[1][2];
    frustumPlanes[FRUSTUM_PLANE_FAR].normal.z = viewProjectionMatrix[2][3] - viewProjectionMatrix[2][2];
    frustumPlanes[FRUSTUM_PLANE_FAR].d = viewProjectionMatrix[3][3] - viewProjectionMatrix[3][2];

    for (int i=0; i<6; i++){
        float length = frustumPlanes[i].normal.normalizeL();
        frustumPlanes[i].d /= length;
    }
}

void RenderSystem::updateInstancedMesh(InstancedMeshComponent& instmesh, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform){
    instmesh.renderInstances.clear();
    instmesh.renderInstances.reserve(instmesh.instances.size());
    instmesh.renderBounds.clear();
    instmesh.renderBounds.reserve(instmesh.instances.size());

    const bool hasBounds = !mesh.verticesAABB.isNull() && !mesh.verticesAABB.isInfinite();
    const Vector3 localCenter = hasBounds ? mesh.verticesAABB.getCenter() : Vector3::ZERO;
    const float localRadius = hasBounds ? mesh.verticesAABB.getHalfSize().length() : 0.0f;

    const float cullEnd = instanceFadeCullEnd(instmesh, mesh, *scene);
    const float cullNear = instanceCullNear(instmesh);
    instmesh.lastFadeCullEnd = cullEnd;
    instmesh.lastCullNear = cullNear;
    instmesh.lastFadeCullEye = instmesh.fadeEyeLocal;
    // small margin for CPU/GPU float differences at the edge
    const float cullRadius = cullEnd + std::max(0.0001f, cullEnd * 0.00001f);
    const float cullRadiusSquared = cullRadius * cullRadius;
    const float cullNearSquared = cullNear * cullNear;

    Quaternion bRotation;
    if (instmesh.instancedBillboard){
        Vector3 camPos = camTransform.worldPosition;
        if (instmesh.instancedCylindricalBillboard){
            camPos.y = transform.worldPosition.y;
        }

        Matrix4 m1 = camera.viewMatrix.inverse();
        bRotation.fromRotationMatrix(m1);
        bRotation = transform.worldRotation.inverse() * bRotation;
    }

    // Not AABB::ZERO: that is a finite box at the origin, so merging into it stretches every
    // instanced mesh back to its own origin and defeats culling.
    mesh.aabb.setNull();
    instmesh.numVisible = 0;
    size_t instancesSize = (instmesh.instances.size() < instmesh.maxInstances)? instmesh.instances.size() : instmesh.maxInstances;
    for (int i = 0; i < instancesSize; i++){
        if (instmesh.instances[i].visible){
            // model-local XZ distance from the instance origin, as in mesh.vert
            const float dx = instmesh.instances[i].position.x - instmesh.fadeEyeLocal.x;
            const float dz = instmesh.instances[i].position.z - instmesh.fadeEyeLocal.z;
            if (cullEnd >= 0.0f && dx * dx + dz * dz > cullRadiusSquared){
                continue;
            }
            if (dx * dx + dz * dz < cullNearSquared){
                continue;
            }
            Matrix4 translateMatrix = Matrix4::translateMatrix(instmesh.instances[i].position);
            Matrix4 rotationMatrix;
            Matrix4 scaleMatrix = Matrix4::scaleMatrix(instmesh.instances[i].scale);

            if (instmesh.instancedBillboard){
                rotationMatrix = bRotation.getRotationMatrix();
            }else{
                rotationMatrix = instmesh.instances[i].rotation.getRotationMatrix();
            }

            Matrix4 instanceMatrix = translateMatrix * rotationMatrix * scaleMatrix;

            instmesh.renderInstances.push_back({});
            instmesh.renderInstances[instmesh.numVisible].instanceMatrix = instanceMatrix;
            instmesh.renderInstances[instmesh.numVisible].color = instmesh.instances[i].color;
            instmesh.renderInstances[instmesh.numVisible].textureRect = instmesh.instances[i].textureRect;
            instmesh.numVisible++;

            const Vector3& scale = instmesh.instances[i].scale;
            InstanceBounds bounds;
            bounds.scale = std::max(std::abs(scale.x), std::max(std::abs(scale.y), std::abs(scale.z)));
            bounds.center = instanceMatrix * localCenter;
            bounds.radius = localRadius * bounds.scale;
            instmesh.renderBounds.push_back(bounds);

            // Not instanceMatrix * mesh.verticesAABB: that transforms eight corners, a full matrix
            // multiply each, and it measured 0.2 us per instance - 26 ms a frame over the 30k of a
            // grass scatter. The instance's circumradius contains its rotated box, so two point
            // merges give a box that is looser but still conservative, and this box is only ever a
            // reject test.
            if (hasBounds){
                const Vector3 corner(bounds.radius, bounds.radius, bounds.radius);
                mesh.aabb.merge(bounds.center - corner);
                mesh.aabb.merge(bounds.center + corner);
            }
        }
    }

    mesh.needUpdateAABB = true;
    instanceViewsDirty = true;

    // The circumradius merge above only bounds a finite box. An infinite verticesAABB is not a
    // bound to transform but a standing "never cull me", which the old per-instance merge of an
    // unchanged infinite box used to leave behind.
    if (instmesh.numVisible > 0 && !hasBounds && mesh.verticesAABB.isInfinite())
        mesh.aabb.setInfinite();

    if (mesh.loaded)
        instmesh.needUpdateBuffer = true;
}

void RenderSystem::sortInstancedMesh(InstancedMeshComponent& instmesh, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform){
    Vector3 camDir = (camTransform.worldPosition - camera.worldTarget).normalize();

    auto comparePoints = [&transform, &camDir](const InstanceRenderData& a, const InstanceRenderData& b) -> bool {
        Vector3 positionA = Vector3(a.instanceMatrix[3][0], a.instanceMatrix[3][1], a.instanceMatrix[3][2]);
        Vector3 positionB = Vector3(b.instanceMatrix[3][0], b.instanceMatrix[3][1], b.instanceMatrix[3][2]);

        return (transform.modelMatrix * positionA).dotProduct(camDir) < (transform.modelMatrix * positionB).dotProduct(camDir);
    };

    // the bounds follow the sort so the instance views keep matching
    std::vector<unsigned int> order(instmesh.renderInstances.size());
    for (unsigned int i = 0; i < order.size(); i++)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](unsigned int a, unsigned int b){
        return comparePoints(instmesh.renderInstances[a], instmesh.renderInstances[b]);
    });
    std::vector<InstanceRenderData> sortedInstances(order.size());
    std::vector<InstanceBounds> sortedBounds(std::min(order.size(), instmesh.renderBounds.size()));
    for (unsigned int i = 0; i < order.size(); i++){
        sortedInstances[i] = instmesh.renderInstances[order[i]];
        if (order[i] < sortedBounds.size() && i < sortedBounds.size())
            sortedBounds[i] = instmesh.renderBounds[order[i]];
    }
    instmesh.renderInstances.swap(sortedInstances);
    instmesh.renderBounds.swap(sortedBounds);
    instanceViewsDirty = true;

    if (mesh.loaded)
        instmesh.needUpdateBuffer = true;
}

Vector2 RenderSystem::getCameraNearFar(const CameraComponent& camera){
    if (camera.type != CameraType::CAMERA_PERSPECTIVE){
        return Vector2(camera.nearClip, camera.farClip);
    }

    float nearClip = std::max(camera.nearClip, 0.001f);
    float farClip = std::max(camera.farClip, nearClip + 0.001f);
    return Vector2(nearClip, farClip);
}

Vector2 RenderSystem::getRenderedNearFar(const CameraComponent& camera, float renderDistance){
    Vector2 nearFar = getCameraNearFar(camera);
    // A capped far below the near would flip the projection, so the cap never bites that hard.
    if (renderDistance <= 0.0f || camera.type != CameraType::CAMERA_PERSPECTIVE || camera.farClip == 0.0f)
        return nearFar;
    nearFar.y = std::min(nearFar.y, std::max(nearFar.x + 0.001f, renderDistance));
    return nearFar;
}

void RenderSystem::configureLightShadowNearFar(LightComponent& light, const CameraComponent& camera){
    if (light.automaticShadowCamera){
        // Keep the shadow volume inside what is actually drawn: a far plane that reaches past
        // the render distance would spread the same map over ground nothing shows.
        Vector2 nearFar = getRenderedNearFar(camera, scene->getEffectiveRenderDistance());
        light.shadowCameraNearFar.x = nearFar.x;
        if (light.range == 0.0){
            light.shadowCameraNearFar.y = nearFar.y;
        }else{
            light.shadowCameraNearFar.y = light.range;
        }
    }
}

float RenderSystem::lerp(float a, float b, float fraction) {
    return (a * (1.0f - fraction)) + (b * fraction);
}

Matrix4 RenderSystem::getDirLightProjection(const Matrix4& viewMatrix, const Matrix4& sceneCameraInv, float shadowMaxDistance, const Vector3& lightDirection, const Vector3& cameraPosition){
    // Calculate extended frustum bounds
    Matrix4 extendedSceneCameraInv = sceneCameraInv;

    // Extend the frustum backwards to include more geometry
    float extension = shadowMaxDistance * 0.5f;

    Matrix4 t = viewMatrix * extendedSceneCameraInv;
    std::vector<Vector4> frustumCorners = {
        t * Vector4(-1.f, 1.f, -1.f, 1.f),
        t * Vector4(1.f, 1.f, -1.f, 1.f),
        t * Vector4(1.f, -1.f, -1.f, 1.f),
        t * Vector4(-1.f, -1.f, -1.f, 1.f),
        t * Vector4(-1.f, 1.f, 1.f, 1.f),
        t * Vector4(1.f, 1.f, 1.f, 1.f),
        t * Vector4(1.f, -1.f, 1.f, 1.f),
        t * Vector4(-1.f, -1.f, 1.f, 1.f)
    };

    // Add additional points behind the camera
    Vector3 backwardOffset = -lightDirection * extension;
    Vector4 backPoint = viewMatrix * Vector4(cameraPosition + backwardOffset, 1.0f);
    frustumCorners.push_back(backPoint);

    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();

    for (auto& p : frustumCorners){
        if (p.w != 0.0f) {
            p = p / p.w;
        }

        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
        minZ = std::min(minZ, p.z);
        maxZ = std::max(maxZ, p.z);
    }

    float paddingX = (maxX - minX) * 0.05f;
    float paddingY = (maxY - minY) * 0.05f;
    float paddingZ = (maxZ - minZ) * 0.1f;

    minX -= paddingX;
    maxX += paddingX;
    minY -= paddingY;
    maxY += paddingY;
    minZ -= paddingZ;
    maxZ += paddingZ;

    // Keep the orthographic projection locked to shadow texels. Without this
    // quantisation, sub-pixel camera motion moves the whole cascade through the
    // atlas and makes otherwise static shadows shimmer. The atlas slot is square
    // for every directional cascade, so the same texel density applies to both
    // light-space axes.
    const unsigned int resolution = std::max(1u, shadowAtlasSlotResolution);
    const float extentX = std::max(maxX - minX, 0.0001f);
    const float extentY = std::max(maxY - minY, 0.0001f);
    const float texelX = extentX / static_cast<float>(resolution);
    const float texelY = extentY / static_cast<float>(resolution);
    const float centerX = std::floor(((minX + maxX) * 0.5f) / texelX + 0.5f) * texelX;
    const float centerY = std::floor(((minY + maxY) * 0.5f) / texelY + 0.5f) * texelY;
    minX = centerX - extentX * 0.5f;
    maxX = centerX + extentX * 0.5f;
    minY = centerY - extentY * 0.5f;
    maxY = centerY + extentY * 0.5f;

    return Matrix4::orthoMatrix(minX, maxX, minY, maxY, -maxZ, -minZ);
}

// Planes around the casters that can shadow a cascade's slice of the view: the slice extruded
// toward the light, widened by the normal offset and PCF reach. Returns 0 to skip culling.
int RenderSystem::getShadowReceiverPlanes(const CameraComponent& camera, const LightComponent& light, int cascade, Plane* planes){
    Matrix4 viewProjection = camera.viewProjectionMatrix;
    if (camera.type == CameraType::CAMERA_PERSPECTIVE){
        // cascades are not blended, each one shades only its own slice
        float fov = atanf(1.f / camera.projectionMatrix[1][1]) * 2.f;
        float ratio = camera.projectionMatrix[1][1] / camera.projectionMatrix[0][0];
        const Vector2& slice = light.cameras[cascade].nearFar;
        viewProjection = Matrix4::perspectiveMatrix(fov, ratio, slice.x, slice.y) * camera.viewMatrix;
    }

    Matrix4 inverse = viewProjection.inverse();
    const Matrix4& lightProjection = light.cameras[cascade].lightProjectionMatrix;
    float texel = 2.0f / (std::max(1u, shadowAtlasSlotResolution) * std::min(std::abs(lightProjection[0][0]), std::abs(lightProjection[1][1])));
    float margin = (std::abs(light.shadowNormalBias) + 8.0f) * texel;
    if (!inverse.isValid() || !std::isfinite(margin))
        return 0;

    Vector3 corners[8];
    Vector3 center = Vector3::ZERO;
    const Vector4 clipCorners[8] = {
        Vector4(-1, 1,-1, 1), Vector4( 1, 1,-1, 1), Vector4( 1,-1,-1, 1), Vector4(-1,-1,-1, 1),
        Vector4(-1, 1, 1, 1), Vector4( 1, 1, 1, 1), Vector4( 1,-1, 1, 1), Vector4(-1,-1, 1, 1)
    };
    for (int c = 0; c < 8; c++){
        Vector4 corner = inverse * clipCorners[c];
        if (std::abs(corner.w) < 0.000001f)
            return 0;
        corners[c] = Vector3(corner.x, corner.y, corner.z) / corner.w;
        center += corners[c] / 8.0f;
    }

    int count = 0;

    // the faces the light leaves through
    Plane frustum[6];
    updateCameraFrustumPlanes(viewProjection, frustum);
    for (int f = 0; f < 6; f++){
        if (frustum[f].normal.dotProduct(light.worldDirection) < -0.00001f){
            planes[count] = frustum[f];
            planes[count].d += margin;
            count++;
        }
    }

    // and planes along the light through the silhouette edges
    const int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (const auto& edge : edges){
        Vector3 normal = (corners[edge[1]] - corners[edge[0]]).crossProduct(light.worldDirection);
        if (normal.normalizeL() < 0.000001f)
            continue;
        if (normal.dotProduct(center - corners[edge[0]]) < 0.0f)
            normal = -normal;

        Plane plane(normal, corners[edge[0]]);
        bool silhouette = true;
        for (const Vector3& corner : corners){
            if (plane.getDistance(corner) < -0.001f){
                silhouette = false;
                break;
            }
        }
        if (silhouette){
            plane.d += margin + 0.001f;
            planes[count++] = plane;
        }
    }

    return count;
}

void RenderSystem::updateLightFromScene(LightComponent& light, Transform& transform, CameraComponent& camera, Transform& cameraTransform){
    light.worldDirection = transform.worldRotation * light.direction;
    light.worldUp = (transform.worldRotation * light.getLocalSpotProjectionUp()).normalized();

    if (hasShadows && (light.intensity > 0)){
        
        Vector3 up = Vector3(0, 1, 0);
        if (light.worldDirection.crossProduct(up) == Vector3::ZERO){
            up = Vector3(0, 0, 1);
        }

        configureLightShadowNearFar(light, camera);

        //TODO: perspective aspect based on shadow map size
        if (light.type == LightType::DIRECTIONAL){

            float shadowSplitLogFactor = 0.7f;
            float shadowMaxDistance = light.shadowCameraNearFar.y;

            Matrix4 projectionMatrix[MAX_SHADOWCASCADES];
            Matrix4 viewMatrix;

            viewMatrix = Matrix4::lookAtMatrix(transform.worldPosition, light.worldDirection + transform.worldPosition, up);

            //TODO: light directional cascades is only considering main camera
            if (camera.type == CameraType::CAMERA_PERSPECTIVE) {

                float zNear = light.shadowCameraNearFar.x;
                float zFar = light.shadowCameraNearFar.y;
                float planeFarOffset = (zFar - zNear) * 0.05f;

                std::vector<float> splitFar;
                std::vector<float> splitNear;

                // Split perspective frustum to create cascades
                Matrix4 projection = camera.projectionMatrix;
                Matrix4 invProjection = projection.inverse();

                std::vector<Vector4> v1 = {
                    invProjection * Vector4(-1.f, 1.f, -1.f, 1.f),
                    invProjection * Vector4(1.f, 1.f, -1.f, 1.f),
                    invProjection * Vector4(1.f, -1.f, -1.f, 1.f),
                    invProjection * Vector4(-1.f, -1.f, -1.f, 1.f),
                    invProjection * Vector4(-1.f, 1.f, 1.f, 1.f),
                    invProjection * Vector4(1.f, 1.f, 1.f, 1.f),
                    invProjection * Vector4(1.f, -1.f, 1.f, 1.f),
                    invProjection * Vector4(-1.f, -1.f, 1.f, 1.f)
                };

                zFar = std::min(zFar, -(v1[4] / v1[4].w).z);
                zNear = -(v1[0] / v1[0].w).z;
                float fov = atanf(1.f / projection[1][1]) * 2.f;
                float ratio = projection[1][1] / projection[0][0];

                splitFar.resize(light.numShadowCascades);
                splitNear.resize(light.numShadowCascades);
                splitNear[0] = zNear;
                splitFar[light.numShadowCascades - 1] = zFar;

                float j = 1.f;
                for (auto i = 0u; i < light.numShadowCascades - 1; ++i, j += 1.f) {
                    splitFar[i] = lerp(
                        zNear + (j / (float)light.numShadowCascades) * (zFar - zNear),
                        zNear * powf(zFar / zNear, j / (float)light.numShadowCascades),
                        shadowSplitLogFactor
                    );
                    splitNear[i + 1] = splitFar[i];
                }

                // Get frustum box and create light ortho
                for (int ca = 0; ca < light.numShadowCascades; ca++) {
                    Matrix4 cascadeProjection = Matrix4::perspectiveMatrix(fov, ratio, splitNear[ca], splitFar[ca] + planeFarOffset);
                    Matrix4 sceneCameraInv = (cascadeProjection * camera.viewMatrix).inverse();

                    projectionMatrix[ca] = getDirLightProjection(viewMatrix, sceneCameraInv, shadowMaxDistance, light.worldDirection, cameraTransform.worldPosition);

                    light.cameras[ca].lightViewMatrix = viewMatrix;
                    light.cameras[ca].lightProjectionMatrix = projectionMatrix[ca];
                    light.cameras[ca].lightViewProjectionMatrix = projectionMatrix[ca] * viewMatrix;
                    light.cameras[ca].nearFar = Vector2(splitNear[ca], splitFar[ca]);
                    updateCameraFrustumPlanes(light.cameras[ca].lightViewProjectionMatrix, light.cameras[ca].frustumPlanes);
                }

            } else {
                // Orthographic camera handling remains the same
                if (light.numShadowCascades > 1){
                    light.numShadowCascades = 1;
                    Log::warn("Can not have multiple cascades shadows when using ortho scene camera. Reducing num shadow cascades to 1");
                }

                Matrix4 sceneCameraInv = camera.viewProjectionMatrix.inverse();

                projectionMatrix[0] = getDirLightProjection(viewMatrix, sceneCameraInv, shadowMaxDistance, light.worldDirection, cameraTransform.worldPosition);

                light.cameras[0].lightViewMatrix = viewMatrix;
                light.cameras[0].lightProjectionMatrix = projectionMatrix[0];
                light.cameras[0].lightViewProjectionMatrix = projectionMatrix[0] * viewMatrix;
                light.cameras[0].nearFar = Vector2(-shadowMaxDistance, shadowMaxDistance);
                updateCameraFrustumPlanes(light.cameras[0].lightViewProjectionMatrix, light.cameras[0].frustumPlanes);

            }

        }else if (light.type == LightType::SPOT){
            Matrix4 projectionMatrix;
            Matrix4 viewMatrix;

            Vector3 projectionUp = light.spotMaskReady ? light.worldUp : up;
            float projectionAspect = light.spotMaskReady ? light.spotMaskAspect : 1.0f;
            viewMatrix = Matrix4::lookAtMatrix(transform.worldPosition, light.worldDirection + transform.worldPosition, projectionUp);

            projectionMatrix = Matrix4::perspectiveMatrix(
                acos(light.outerConeCos) * 2,
                projectionAspect,
                light.shadowCameraNearFar.x,
                light.shadowCameraNearFar.y);

            light.cameras[0].lightViewMatrix = viewMatrix;
            light.cameras[0].lightProjectionMatrix = projectionMatrix;
            light.cameras[0].lightViewProjectionMatrix = projectionMatrix * viewMatrix;
            light.cameras[0].nearFar = light.shadowCameraNearFar;
            updateCameraFrustumPlanes(light.cameras[0].lightViewProjectionMatrix, light.cameras[0].frustumPlanes);

        }else if (light.type == LightType::POINT){
            Matrix4 projectionMatrix;
            Matrix4 viewMatrix[6];

            viewMatrix[0] = Matrix4::lookAtMatrix(transform.worldPosition, Vector3( 1.f, 0.f, 0.f) + transform.worldPosition, Vector3(0.f, -1.f, 0.f));
            viewMatrix[1] = Matrix4::lookAtMatrix(transform.worldPosition, Vector3(-1.f, 0.f, 0.f) + transform.worldPosition, Vector3(0.f, -1.f, 0.f));
            viewMatrix[2] = Matrix4::lookAtMatrix(transform.worldPosition, Vector3( 0.f, 1.f, 0.f) + transform.worldPosition, Vector3(0.f,  0.f, 1.f));
            viewMatrix[3] = Matrix4::lookAtMatrix(transform.worldPosition, Vector3( 0.f,-1.f, 0.f) + transform.worldPosition, Vector3(0.f,  0.f,-1.f));
            viewMatrix[4] = Matrix4::lookAtMatrix(transform.worldPosition, Vector3( 0.f, 0.f, 1.f) + transform.worldPosition, Vector3(0.f, -1.f, 0.f));
            viewMatrix[5] = Matrix4::lookAtMatrix(transform.worldPosition, Vector3( 0.f, 0.f,-1.f) + transform.worldPosition, Vector3(0.f, -1.f, 0.f));

            projectionMatrix = Matrix4::perspectiveMatrix(Angle::degToRad(90), 1, light.shadowCameraNearFar.x, light.shadowCameraNearFar.y);

            Vector2 calculedNearFar;
            float nfsub = light.shadowCameraNearFar.y - light.shadowCameraNearFar.x;
            calculedNearFar.x = (light.shadowCameraNearFar.y + light.shadowCameraNearFar.x) / nfsub * 0.5f + 0.5f;
            calculedNearFar.y =-(light.shadowCameraNearFar.y * light.shadowCameraNearFar.x) / nfsub;

            for (int f = 0; f < 6; f++){
                light.cameras[f].lightViewMatrix = viewMatrix[f];
                light.cameras[f].lightProjectionMatrix = projectionMatrix;
                light.cameras[f].lightViewProjectionMatrix = projectionMatrix * viewMatrix[f];
                light.cameras[f].nearFar = calculedNearFar;
                updateCameraFrustumPlanes(light.cameras[f].lightViewProjectionMatrix, light.cameras[f].frustumPlanes);
            }
        }
        
        
    }
}

void RenderSystem::changeLoaded(void* data){
    if (!Engine::isViewLoaded())
        return;

    check_load_t* loadObj = (check_load_t*)data;

    Scene* scene = loadObj->scene;
    Entity entity = loadObj->entity;

    Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<MeshComponent>())){
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

        mesh.loaded = true;

    }else if (signature.test(scene->getComponentId<UIComponent>())){
        UIComponent& ui = scene->getComponent<UIComponent>(entity);

        ui.loaded = true;

    }else if (signature.test(scene->getComponentId<PointsComponent>())){
        PointsComponent& points = scene->getComponent<PointsComponent>(entity);

        points.loaded = true;

    }else if (signature.test(scene->getComponentId<LinesComponent>())){
        LinesComponent& lines = scene->getComponent<LinesComponent>(entity);

        lines.loaded = true;

    }else if (signature.test(scene->getComponentId<SkyComponent>())){
        SkyComponent& sky = scene->getComponent<SkyComponent>(entity);

        sky.loaded = true;
    }

    delete (check_load_t*)data;
}

void RenderSystem::changeDestroy(void* data){
    check_load_t* loadObj = (check_load_t*)data;

    Scene* scene = loadObj->scene;
    Entity entity = loadObj->entity;

    Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<MeshComponent>())){
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

        mesh.loaded = false;
        mesh.loadCalled = false;

    }else if (signature.test(scene->getComponentId<UIComponent>())){
        UIComponent& ui = scene->getComponent<UIComponent>(entity);

        ui.loaded = false;
        ui.loadCalled = false;

    }else if (signature.test(scene->getComponentId<PointsComponent>())){
        PointsComponent& points = scene->getComponent<PointsComponent>(entity);

        points.loaded = false;
        points.loadCalled = false;

    }else if (signature.test(scene->getComponentId<LinesComponent>())){
        LinesComponent& lines = scene->getComponent<LinesComponent>(entity);

        lines.loaded = false;
        lines.loadCalled = false;

    }else if (signature.test(scene->getComponentId<SkyComponent>())){
        SkyComponent& sky = scene->getComponent<SkyComponent>(entity);

        sky.loaded = false;
        sky.loadCalled = false;
    }

    delete (check_load_t*)data;
}

void RenderSystem::changeWaterLoaded(void* data){
    check_load_t* loadObj = (check_load_t*)data;

    if (Engine::isViewLoaded()){
        if (WaterComponent* water = loadObj->scene->findComponent<WaterComponent>(loadObj->entity)){
            water->loaded = true;
        }
    }

    delete loadObj;
}

void RenderSystem::changeWaterDestroy(void* data){
    check_load_t* loadObj = (check_load_t*)data;

    if (WaterComponent* water = loadObj->scene->findComponent<WaterComponent>(loadObj->entity)){
        water->loaded = false;
        water->loadCalled = false;
    }

    delete loadObj;
}

bool RenderSystem::usesAlphaMask(const Material& material, bool textureShadow){
    return material.alphaMode == MaterialAlphaMode::MASK ||
        (material.alphaMode == MaterialAlphaMode::AUTO && textureShadow);
}

// a texture cannot be sampled in the pass that renders into it; objects showing a
// camera's framebuffer must be skipped when drawing that same camera
bool RenderSystem::samplesCameraTarget(const CameraComponent& camera, const MeshComponent& mesh){
    if (!camera.renderToTexture)
        return false;

    for (unsigned int i = 0; i < mesh.numSubmeshes; i++){
        const Material& material = mesh.submeshes[i].material;
        if (material.baseColorTexture.getFramebuffer() == camera.framebuffer ||
            material.metallicRoughnessTexture.getFramebuffer() == camera.framebuffer ||
            material.normalTexture.getFramebuffer() == camera.framebuffer ||
            material.occlusionTexture.getFramebuffer() == camera.framebuffer ||
            material.emissiveTexture.getFramebuffer() == camera.framebuffer){
            return true;
        }
    }

    return false;
}

bool RenderSystem::samplesCameraTarget(const CameraComponent& camera, const Texture& texture){
    return camera.renderToTexture && texture.getFramebuffer() == camera.framebuffer;
}

bool RenderSystem::isRenderingFlipped(const CameraComponent& camera) const{
    // Cubemap sampling follows one fixed convention on every API, and with the
    // classic capture table (renderReflectionProbeCapture) it expects GL-style
    // bottom-up face storage: flip only on the backends that natively rasterize
    // top-left. This is the inverse of the 2D RTT rule below, and its winding
    // pairs with PIP_RTT_INVERT (one reversal off PIP_RTT on every backend).
    if (capturingReflectionProbe)
        return !Engine::isOpenGL();
    // OpenGL offscreen targets are bottom-up; rendering them with a Y-flipped
    // projection makes framebuffer textures top-left origin on every backend.
    // Must match the PIP_RTT pipeline selection (its winding is reversed on GL).
    return (camera.renderToTexture || Engine::getFramebuffer() || isFixedResolutionActive() || swapchainRedirect) && Engine::isOpenGL();
}

bool RenderSystem::isFixedResolutionActive() const{
    // main scene only: layer scenes keep rendering at native resolution on top
    return !disableFixedResolution
        && scene->isFixedResolutionEnabled()
        && scene->getFixedResolutionWidth() > 0
        && scene->getFixedResolutionHeight() > 0
        && Engine::getMainScene() == scene;
}

void RenderSystem::updateSwapchainRedirect(){
    // SSR, the post-process chain and the scene copy of a water need the scene in an offscreen
    // color buffer. Without a framebuffer destination (exported builds) the color pass is redirected
    // into it and the last pass targets the swapchain. Main scene only: a layer scene
    // composites over the scene below, and its own pass would clear it.
    bool redirect = false;

    if (!Engine::getFramebuffer() && !isFixedResolutionActive()
            && Engine::getMainScene() == scene && Engine::isViewLoaded()){
        unsigned int w = (unsigned int)Engine::getViewRect().getWidth();
        unsigned int h = (unsigned int)Engine::getViewRect().getHeight();

        // the meshes render with PIP_RTT (flipped on GL) while it is on, so the
        // redirect only starts once everything it needs exists
        if (scene->isSSREnabled()){
            loadSSR();
            redirect = ssrLoaded && ensureSSRFramebuffers(w, h) && ensureGBufferFramebuffer(w, h);
        }
        if (!redirect && !scene->getPostProcessPasses().empty()){
            loadPostProcess();
            loadBlit();
            redirect = postProcessLoaded && blitLoaded && !postProcessPasses.empty()
                    && ensurePostProcessFramebuffers(w, h);
        }

        // Bloom and the tone map read the scene color offscreen too. Without this branch the
        // chain only ever runs in the editor or at a fixed resolution: an exported build draws
        // straight to the swapchain, so a scene that asks for bloom would silently get none.
        if (!redirect && scene->isHdrEnabled() && !scene->isSSREnabled()){
            loadHDR();
            redirect = hdrLoaded && ensureSceneColorFramebuffer(w, h)
                    && ensureHDRFramebuffers(w, h, scene->getBloomLevels());
        }

        // Depth of field reads the scene color and the depth pre-pass offscreen, so an
        // exported build needs the same redirect. The blit carries its own result back
        // into the scene color.
        if (!redirect && scene->isDOFEnabled() && !scene->isSSREnabled()){
            loadDOF();
            loadBlit();
            redirect = dofLoaded && blitLoaded && ensureSceneColorFramebuffer(w, h)
                    && ensureDOFFramebuffers(w, h) && ensureDepthPrePassFramebuffer(w, h);
        }

        // Height fog and light shafts read the scene color and the depth pre-pass exactly
        // like DOF does, so they need the same redirect and the same blit to put the result
        // back. SSR is excluded for the same reason: its composite writes the destination.
        if (!redirect && atmosphereRequested() && !scene->isSSREnabled()){
            loadAtmosphere();
            loadBlit();
            redirect = atmosphereLoaded && blitLoaded && ensureSceneColorFramebuffer(w, h)
                    && ensureAtmosphereFramebuffer(w, h) && ensureDepthPrePassFramebuffer(w, h);
        }

        if (!redirect && scene->isSSGIEnabled() && scene->getPostProcessPasses().empty()){
            loadSSGI();
            loadBlit();
            redirect = ssgiLoaded && blitLoaded && ensureSceneColorFramebuffer(w, h)
                    && ensureSSGIFramebuffer(w, h, sceneColorHDR)
                    && ensureDepthPrePassFramebuffer(w, h);
        }

        if (!redirect && scene->isContactShadowsEnabled() && !scene->isSSREnabled()
                && scene->getPostProcessPasses().empty()){
            loadContactShadows();
            loadBlit();
            redirect = contactShadowsLoaded && blitLoaded && ensureSceneColorFramebuffer(w, h)
                    && ensureContactShadowsFramebuffer(w, h, sceneColorHDR)
                    && ensureDepthPrePassFramebuffer(w, h);
        }

        if (!redirect && !scene->isSSREnabled() &&
                (scene->isTAAEnabled() || scene->isMotionBlurEnabled())){
            loadTemporalEffects();
            redirect = velocityLoaded && taaLoaded && motionBlurLoaded &&
                ensureSceneColorFramebuffer(w, h) &&
                ensureTemporalFramebuffers(w, h, w, h, sceneColorHDR) &&
                ensureDepthPrePassFramebuffer(w, h);
        }

        // Projected decals sample the packed depth pre-pass and composite over
        // the scene color. Exported/direct-swapchain runs need the same offscreen
        // redirect as the other screen-space passes or the decal branch is never
        // selected and the component is silently ignored.
        if (!redirect && !scene->isSSREnabled()){
            auto decals = scene->getComponentArray<DecalComponent>();
            if (decals && decals->size() > 0){
                loadBlit();
                redirect = blitLoaded && ensureSceneColorFramebuffer(w, h)
                    && ensureDepthPrePassFramebuffer(w, h);
            }
        }

        // FXAA runs on the finished image, so the scene has to be offscreen for it as well.
        // The blit is required because a scene with no bloom, no tone map and no post chain
        // still has to move its color buffer into the anti-aliased target.
        if (!redirect && scene->isFXAAEnabled()){
            loadBlit();
            loadFXAA();
            redirect = fxaaLoaded && blitLoaded && ensureSceneColorFramebuffer(w, h)
                    && ensureFXAAFramebuffer(w, h);
        }

        bool copyingWater = false;
        auto waters = scene->getComponentArray<WaterComponent>();
        for (int i = 0; i < waters->size(); i++){
            const WaterComponent& water = waters->getComponentFromIndex(i);
            copyingWater = copyingWater || water.refraction || water.underwater;
        }
        if (!redirect && copyingWater){
            loadBlit();
            redirect = blitLoaded && ensureSceneColorFramebuffer(w, h);
        }
    }

    if (redirect != swapchainRedirect){
        swapchainRedirect = redirect;
        taaHistoryValid = false;
        previousViewProjectionValid = false;
        // the flip is baked in the MVP matrices
        scene->getComponent<CameraComponent>(scene->getRenderCamera()).needUpdate = true;
    }
}

void RenderSystem::updateMVP(size_t index, Transform& transform, CameraComponent& camera, Transform& cameraTransform){
    if (transform.billboard && !transform.fakeBillboard){

        Vector3 camPos = cameraTransform.worldPosition;

        if (transform.cylindricalBillboard)
            camPos.y = transform.worldPosition.y;

        Vector3 toCamera = camPos - transform.worldPosition;
        const float EPSILON = 1e-6f;

        if (toCamera.length() > EPSILON && toCamera.crossProduct(camera.worldUp).length() > EPSILON){ // check if not parallel
            Matrix4 m1 = Matrix4::lookAtMatrix(camPos, transform.worldPosition, camera.worldUp).inverse();

            Quaternion oldRotation = transform.rotation;

            if (m1.isValid()){
                transform.rotation.fromRotationMatrix(m1);
                transform.rotation = transform.rotation * transform.billboardRotation;
                if (transform.parent != NULL_ENTITY){
                    Transform* transformParent = scene->findComponent<Transform>(transform.parent);
                    if (transformParent){
                        transform.rotation = transformParent->worldRotation.inverse() * transform.rotation;
                    }
                }
            }

            if (transform.rotation != oldRotation){
                transform.needUpdate = true;

                std::vector<Entity> parentList;
                auto transforms = scene->getComponentArray<Transform>();
                for (int i = index; i < transforms->size(); i++){
                    Transform& childTransform = transforms->getComponentFromIndex(i);

                    // Finding childs
                    if (i > index){
                        if (std::find(parentList.begin(), parentList.end(), childTransform.parent) != parentList.end()){
                            childTransform.needUpdate = true;
                        }else{
                            break;
                        }
                    }

                    if (childTransform.needUpdate){
                        Entity entity = transforms->getEntity(i);
                        parentList.push_back(entity);
                        updateTransform(childTransform, entity);
                    }
                }
            }
        }

    }

    if (transform.billboard && transform.fakeBillboard){
        
        Matrix4 modelViewMatrix = camera.viewMatrix * transform.modelMatrix;

        modelViewMatrix.set(0, 0, transform.worldScale.x);
        modelViewMatrix.set(0, 1, 0.0);
        modelViewMatrix.set(0, 2, 0.0);

        if (!transform.cylindricalBillboard) {
            modelViewMatrix.set(1, 0, 0.0);
            modelViewMatrix.set(1, 1, transform.worldScale.y);
            modelViewMatrix.set(1, 2, 0.0);
        }

        modelViewMatrix.set(2, 0, 0.0);
        modelViewMatrix.set(2, 1, 0.0);
        modelViewMatrix.set(2, 2, transform.worldScale.z);

        transform.modelViewProjectionMatrix = camera.projectionMatrix * modelViewMatrix;

    }else{

        transform.modelViewProjectionMatrix = camera.viewProjectionMatrix * transform.modelMatrix;

    }

    if (isRenderingFlipped(camera)){
        // flip only the render matrix; camera.viewProjectionMatrix stays logical
        // (top-left screen origin) for frustum planes, rays and UI hit tests
        static const Matrix4 flipYMatrix = Matrix4::scaleMatrix(Vector3(1, -1, 1));
        transform.modelViewProjectionMatrix = flipYMatrix * transform.modelViewProjectionMatrix;
    }

    transform.distanceToCamera = (cameraTransform.worldPosition - transform.worldPosition).length();
}

uint16_t RenderSystem::getScenePipelines() const{
    uint16_t pipelines = 0;

    Entity mainCameraEntity = scene->getRenderCamera();
    auto cameras = scene->getComponentArray<CameraComponent>();

    for (int i = 0; i < cameras->size(); i++){
        CameraComponent& camera = cameras->getComponentFromIndex(i);
        Entity cameraEntity = cameras->getEntity(i);

        // no Transform means no pass
        if (!scene->findComponent<Transform>(cameraEntity))
            continue;

        // with an engine framebuffer (the editor) nothing draws to the swapchain
        if (cameraEntity == mainCameraEntity && !camera.renderToTexture && !Engine::getFramebuffer()){
            pipelines |= camera.depthTest ? PIP_DEFAULT : PIP_DEFAULT_NODEPTH;
        }

        if (camera.renderToTexture || Engine::getFramebuffer() || isFixedResolutionActive() || swapchainRedirect){
            pipelines |= camera.depthTest ? PIP_RTT : PIP_RTT_NODEPTH;
        }
    }

    // mirrors and probe capture render with reversed winding (see isRenderingFlipped)
    if (scene->getComponentArray<MirrorComponent>()->size() > 0 ||
            scene->getComponentArray<ReflectionProbeComponent>()->size() > 0){
        pipelines |= PIP_RTT_INVERT;
    }

    // and so does the mirrored camera of a reflecting water
    auto waters = scene->getComponentArray<WaterComponent>();
    for (int i = 0; i < waters->size(); i++){
        if (waters->getComponentFromIndex(i).planarReflection){
            pipelines |= PIP_RTT_INVERT;
            break;
        }
    }

    return pipelines;
}

void RenderSystem::update(double dt){
    if (paused) {
        return;
    }

    checkLightsAndShadow();

    // light types can change without adding or removing a light
    bool newOnlyDirectionalLights = hasLights && allLightsDirectional(scene);
    if (onlyDirectionalLights != newOnlyDirectionalLights){
        onlyDirectionalLights = newOnlyDirectionalLights;
        needReloadMeshes();
    }

    auto transforms = scene->getComponentArray<Transform>();
    auto cameras = scene->getComponentArray<CameraComponent>();

    for (int i = 0; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);

        if (transform.parent != NULL_ENTITY){
            Transform* transformParent = scene->findComponent<Transform>(transform.parent);
            if (transformParent){
                if (transformParent->needUpdate){
                    transform.needUpdate = true;
                }

                if (transformParent->needUpdateChildVisibility){
                    transform.visible = transformParent->visible;
                    transform.needUpdateChildVisibility = true;
                }
            }
        }

        if (transform.needUpdate){
            updateTransform(transform, transforms->getEntity(i));
        }
    }

    auto models = scene->getComponentArray<ModelComponent>();
    for (int i = 0; i < models->size(); i++) {
        ModelComponent& model = models->getComponentFromIndex(i);
        for (const ModelSkinBinding& binding : model.skinBindings) {
            MeshComponent* mesh = scene->findComponent<MeshComponent>(binding.mesh);
            Transform* meshTransform = scene->findComponent<Transform>(binding.mesh);
            if (!mesh || !meshTransform)
                continue;

            bool dirty = meshTransform->needUpdate;
            for (Entity joint : binding.joints) {
                if (dirty)
                    break;
                Transform* jointTransform = scene->findComponent<Transform>(joint);
                dirty = jointTransform && jointTransform->needUpdate;
            }
            if (!dirty)
                continue;

            const Matrix4 inverseMeshTransform = meshTransform->modelMatrix.inverse();
            // Scale 0 makes this NaN; posing the skinned AABB with it trips setExtents.
            if (!inverseMeshTransform.isValid())
                continue;

            const size_t jointCount = std::min(binding.joints.size(), binding.inverseBindMatrices.size());
            for (size_t jointIndex = 0; jointIndex < jointCount &&
                    mesh->bonesMatrix.validIndex(static_cast<int>(jointIndex)); jointIndex++) {
                Transform* jointTransform = scene->findComponent<Transform>(binding.joints[jointIndex]);
                if (jointTransform)
                    mesh->bonesMatrix[jointIndex] = inverseMeshTransform * jointTransform->modelMatrix *
                        binding.inverseBindMatrices[jointIndex];
            }
            mesh->needUpdateBones = true;
            mesh->needUpdateAABB = true;
        }
    }

    updateReflectionProbes(dt);

    // the pipeline mask below depends on the redirect
    updateSwapchainRedirect();

    Entity mainCameraEntity = scene->getRenderCamera();
    uint16_t pipelines = getScenePipelines();

    hasMultipleCameras = false;
    // The cap lives inside the projection, and a projection is only rebuilt when its camera is
    // dirty - so moving the scene's render distance has to dirty them, or the distance would
    // take effect only the next time something else moves the camera.
    const float renderDistance = scene->getEffectiveRenderDistance();
    const bool renderDistanceChanged = renderDistance != appliedRenderDistance;
    appliedRenderDistance = renderDistance;
    for (int i = 0; i < cameras->size(); i++){
        CameraComponent& camera = cameras->getComponentFromIndex(i);
        Entity cameraEntity = cameras->getEntity(i);
        Transform* cameraTransformPtr = scene->findComponent<Transform>(cameraEntity);
        if (!cameraTransformPtr)
            continue;
        Transform& cameraTransform = *cameraTransformPtr;

        if (camera.renderToTexture){
            // sync authored framebuffer settings (editor/serialized) into the live
            // framebuffer; recreate it when they change so edits take effect. A
            // non-positive size is ignored so an in-use framebuffer is never left
            // destroyed (meshes sampling an uncreated framebuffer would be invalid).
            bool validSize = camera.framebufferWidth > 0 && camera.framebufferHeight > 0;

            bool settingsChanged = validSize &&
                (camera.framebuffer->getWidth() != camera.framebufferWidth ||
                 camera.framebuffer->getHeight() != camera.framebufferHeight ||
                 camera.framebuffer->getMinFilter() != camera.framebufferFilter ||
                 camera.framebuffer->getMagFilter() != camera.framebufferFilter);

            if (settingsChanged){
                camera.framebuffer->setWidth(camera.framebufferWidth);
                camera.framebuffer->setHeight(camera.framebufferHeight);
                camera.framebuffer->setMinFilter(camera.framebufferFilter);
                camera.framebuffer->setMagFilter(camera.framebufferFilter);

                if (camera.framebuffer->isCreated()){
                    camera.framebuffer->destroy();
                }
            }

            bool created = false;
            if (!camera.framebuffer->isCreated() && validSize){
                camera.framebuffer->create();
                created = true;
            }

            // autoResize cameras derive clip/aspect from the framebuffer size;
            // reconcile on first creation and after any size change, otherwise the
            // camera keeps a stale (e.g. canvas) aspect and the RTT image is distorted
            if (settingsChanged || created){
                updateCameraSize(cameraEntity);
            }
        }

        if (camera.renderToTexture && cameraEntity != mainCameraEntity){
            hasMultipleCameras = true;
        }

        if (renderDistanceChanged){
            camera.needUpdate = true;
        }

        if (cameraTransform.needUpdate){
            camera.needUpdate = true;
        }
        
        if (camera.needUpdate){
            updateCamera(camera, cameraTransform);
        }
    }

    // drive mirror reflection cameras from the (now updated) main camera, so the
    // reflection passes render with current matrices in the draw step. This may
    // create reflection camera entities (reallocating the camera/transform arrays),
    // so fetch mainCamera/mainCameraTransform references AFTER it returns.
    updateMirrors(mainCameraEntity);

    // the rest of the frame is built from the main camera view
    CameraComponent* mainCameraPtr = scene->findComponent<CameraComponent>(mainCameraEntity);
    Transform* mainCameraTransformPtr = scene->findComponent<Transform>(mainCameraEntity);
    if (!mainCameraPtr || !mainCameraTransformPtr)
        return;

    CameraComponent& mainCamera = *mainCameraPtr;
    Transform& mainCameraTransform = *mainCameraTransformPtr;
    fadeEyePosition = mainCameraTransform.worldPosition;

    updateMainLodView(mainCamera, mainCameraTransform);
    if (mainCamera.needUpdate){
        mainInstanceViewDirty = true;
    }

    // while extra cameras render, draw() rewrites the shared MVP and sky matrices
    // per camera; removing the last of them (a mirror, a reflection probe) would
    // otherwise leave the scene on that camera's view until something turns dirty
    bool multiCameraDraw = hasMultipleCameras || hasReflectionProbes;
    if (lastMultiCameraDraw && !multiCameraDraw){
        mainCamera.needUpdate = true;
    }
    lastMultiCameraDraw = multiCameraDraw;

    // the batch order is baked into the shared render arrays, so a mode change has to
    // rebuild them from the authored instances and points
    bool batchSort = sortsByDistance(mainCamera);
    bool batchSortChanged = batchSort != lastBatchSort;
    lastBatchSort = batchSort;

    // levels exist only while the scene switch is on, so a change builds or frees them
    bool meshLodToggled = scene->isMeshLodEnabled() != lastMeshLodEnabled;
    lastMeshLodEnabled = scene->isMeshLodEnabled();

    // the destination is baked into the pipelines at load, so a scene entering or
    // leaving a stack (or fixed resolution switching) has to reload with the new set
    if (pipelines != loadedPipelines){
        loadedPipelines = pipelines;

        needReloadMeshes();
        needReloadUIs();
        needReloadPoints();
        needReloadLines();
        needReloadSky();
        needReloadWater();
    }

    selectLights(mainCameraTransform.worldPosition);
    int numLights = (int)activeLights.size();
    loadLights(numLights);
    updateSpotMaskAtlas(numLights);
    loadLights2D();
    loadAndProcessFog();

    auto skys = scene->getComponentArray<SkyComponent>();
    bool newHasIBL = false;
    if (skys->size() > 0){
        SkyComponent& sky = skys->getComponentFromIndex(0);
        Entity entity = skys->getEntity(0);
        if (mainCamera.needUpdate || sky.needUpdateSky){
            if (!hasMultipleCameras){
                updateSkyViewProjection(sky, mainCamera);
            }
        }

        if (sky.needUpdateTexture){
            if (sky.texture.empty()){
                sky.needReload = true;
            }
            // texture changed: IBL environment maps must be regenerated (or destroyed)
            sky.needUpdateEnvironment = true;
        }

        // generate IBL maps before loadSky uploads (and releases) the texture pixels
        if (Engine::isViewLoaded()){
            updateSkyEnvironment(sky);
        }

        if (sky.loaded && sky.needReload){
            destroySky(entity, sky);
        }
        if (!sky.loadCalled){
            loadSky(entity, sky, pipelines);
        }

        newHasIBL = sky.envMapsLoaded;
    }

    if (hasIBL != newHasIBL){
        hasIBL = newHasIBL;
        needReloadMeshes();
    }

    // before the meshes, so they reload with depth shaders in this same frame
    bool newHasWaterDepth = false;
    auto waters = scene->getComponentArray<WaterComponent>();
    for (int i = 0; i < waters->size(); i++){
        WaterComponent& water = waters->getComponentFromIndex(i);
        Entity entity = waters->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform)
            continue;

        if (water.depthEffects){
            newHasWaterDepth = true;
        }
        updateWater(entity, water, *transform, pipelines, dt);
    }

    if (hasWaterDepth != newHasWaterDepth){
        hasWaterDepth = newHasWaterDepth;
        needReloadMeshes();
    }

    updateWaterExclusions();

    // Read once for the whole scan: each of these is a guarded function-static plus a vector
    // subscript, and this loop runs them for every entity in the scene every frame.
    const ComponentId cidMesh = scene->getComponentId<MeshComponent>();
    const ComponentId cidTilemap = scene->getComponentId<TilemapComponent>();
    const ComponentId cidUI = scene->getComponentId<UIComponent>();
    const ComponentId cidText = scene->getComponentId<TextComponent>();
    const ComponentId cidPoints = scene->getComponentId<PointsComponent>();
    const ComponentId cidLines = scene->getComponentId<LinesComponent>();
    const ComponentId cidLight = scene->getComponentId<LightComponent>();
    const ComponentId cidTerrain = scene->getComponentId<TerrainComponent>();
    const ComponentId cidSound = scene->getComponentId<SoundComponent>();
    const ComponentId cidModel = scene->getComponentId<ModelComponent>();
    const ComponentId cidBone = scene->getComponentId<BoneComponent>();

    for (int i = 0; i < transforms->size(); i++){
        Transform& transform = transforms->getComponentFromIndex(i);

        Entity entity = transforms->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(cidMesh)){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

            // A resident shadow atlas is safe to reuse only while its casters and
            // projection are stable. Loading/reloading a caster or rebuilding its
            // bounds invalidates both atlas families.
            if (mesh.castShadows && (transform.needUpdate || !mesh.loaded || mesh.needReload || mesh.needUpdateAABB)){
                shadowAtlasContentDirty = true;
                shadowPointAtlasContentDirty = true;
            }

            TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);

            InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
            if (instmesh){
                const float cullNear = instanceCullNear(*instmesh);
                if (instmesh->distanceFade || cullNear > 0.0f){
                    // The range is a model-space distance, so the eye moves into that space
                    // instead: scaling the range into world space breaks on a scaled mesh.
                    const Matrix4 inverseModel = transform.modelMatrix.inverse();
                    instmesh->fadeEyeLocal = inverseModel.isValid() ?
                        (inverseModel * fadeEyePosition) : fadeEyePosition;
                }

                bool sortTransparentInstances = mesh.transparent && batchSort;

                const float cullEnd = instanceFadeCullEnd(*instmesh, mesh, *scene);
                // An exact compare rebuilds every scatter on any frame the camera moves, which is
                // every frame while it walks. Only an eye move that can carry an instance across a
                // boundary is worth that, and the boundary is a fade measured in tens of metres.
                const float eyeTolerance = 0.02f * std::max(0.0f, std::max(cullEnd, cullNear));
                const bool eyeMoved = std::abs(instmesh->fadeEyeLocal.x - instmesh->lastFadeCullEye.x) > eyeTolerance ||
                                      std::abs(instmesh->fadeEyeLocal.z - instmesh->lastFadeCullEye.z) > eyeTolerance;
                const bool fadeSelectionChanged = cullEnd != instmesh->lastFadeCullEnd || cullNear != instmesh->lastCullNear ||
                    ((cullEnd >= 0.0f || cullNear > 0.0f) && eyeMoved);
                bool instancesNeedUpdate = instmesh->needUpdateInstances || mesh.needUpdateAABB ||
                    batchSortChanged || fadeSelectionChanged;

                if (instancesNeedUpdate && !instmesh->instancedBillboard){
                    updateInstancedMesh(*instmesh, mesh, transform, mainCamera, mainCameraTransform);
                }

                if (instancesNeedUpdate || ((mainCamera.needUpdate || transform.needUpdate) && sortTransparentInstances)){
                    // The instance buffer is shared by every camera and sokol allows one
                    // sg_update_buffer per frame (VALIDATE_UPDATEBUF_ONCE), so the main camera
                    // fixes the batch order and every other camera reuses it.
                    if (instmesh->instancedBillboard){
                        updateInstancedMesh(*instmesh, mesh, transform, mainCamera, mainCameraTransform);
                    }
                    if (sortTransparentInstances){
                        sortInstancedMesh(*instmesh, mesh, transform, mainCamera, mainCameraTransform);
                    }
                }

                instmesh->needUpdateInstances = false;

                InstanceViewSettings viewSettings = {instmesh->cullInstances, mesh.castShadows, mesh.lodEnabled, mesh.lodBias, instmesh->cullDistance};
                if (transform.needUpdate || viewSettings != instmesh->viewSettings){
                    instmesh->viewSettings = viewSettings;
                    instanceViewsDirty = true;
                }
            }

            if (meshLodToggled && mesh.loadCalled && !mesh.needReload){
                updateMeshLods(entity, mesh, true);
            }else if (mesh.loaded && !mesh.needReload){
                for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                    if (isMeshLodPending(mesh.submeshes[s])){
                        updateMeshLods(entity, mesh, false);
                        break;
                    }
                }
            }

            if (terrain && mesh.numSubmeshes > 1){
                for (unsigned int s = 1; s < mesh.numSubmeshes; s++){
                    const bool materialOutOfSync = mesh.submeshes[s].material != mesh.submeshes[0].material;
                    if (materialOutOfSync){
                        mesh.submeshes[s].material = mesh.submeshes[0].material;
                    }
                    if (mesh.submeshes[0].needUpdateTexture || materialOutOfSync){
                        mesh.submeshes[s].needUpdateTexture = true;
                    }
                }
            }

            bool expectedTransparent = false;
            for (int s = 0; s < mesh.numSubmeshes; s++){
                const MaterialAlphaMode alphaMode = mesh.submeshes[s].material.alphaMode;
                const bool alphaBlend = usesAlphaBlend(mesh.submeshes[s].material);
                expectedTransparent = expectedTransparent || alphaBlend;

                if (mesh.submeshes[s].alphaBlend != alphaBlend){
                    mesh.needReload = true;
                }

                // Explicit alpha modes own the derived shadow-cutout state. AUTO
                // remains the compatibility path for setCastShadowsWithTexture().
                if (alphaMode != MaterialAlphaMode::AUTO){
                    const bool expectedTextureShadow = alphaMode == MaterialAlphaMode::MASK;
                    if (mesh.submeshes[s].textureShadow != expectedTextureShadow){
                        mesh.submeshes[s].textureShadow = expectedTextureShadow;
                        mesh.needReload = true;
                    }
                }

                // Alpha mode can be changed directly by scripts/generated code, so
                // shader variant reconciliation must not depend on a texture update.
                const bool shaderAlphaMask = (mesh.submeshes[s].shaderProperties & (1u << 24)) != 0;
                const bool shaderAlphaOpaque = (mesh.submeshes[s].shaderProperties & (1u << 25)) != 0;
                if (shaderAlphaMask != (alphaMode == MaterialAlphaMode::MASK) ||
                    shaderAlphaOpaque != (alphaMode == MaterialAlphaMode::ALPHA_OPAQUE)) {
                    mesh.needReload = true;
                }

                // A layer gains or loses its own surface from the editor, a script or an AI
                // action, and only a reload can swap the terrain variant that reads it
                if (terrain){
                    const bool shaderTerrainSurface = (mesh.submeshes[s].shaderProperties & (1u << 27)) != 0;
                    if (shaderTerrainSurface != hasTerrainSurfaceLayers(*terrain)){
                        mesh.needReload = true;
                    }
                }

                if (mesh.submeshes[s].needUpdateTexture){
                    if (terrain){
                        transform.needUpdate = true;
                    }
                    if (mesh.submeshes[s].textureShadow){
                        mesh.submeshes[s].needUpdateDepthTexture = true;
                    }
                    if (mesh.submeshes[s].gbufferShader){
                        mesh.submeshes[s].needUpdateGBufferTexture = true;
                    }
                    if (checkPBRTextures(mesh.submeshes[s].material, mesh.receiveLights)){
                        if (!(mesh.submeshes[s].shaderProperties & (1 << 1)) && !(mesh.submeshes[s].shaderProperties & (1 << 2))){ // not 'Uv1' and not 'Uv2'
                            mesh.needReload = true;
                        }
                    }else{
                        if ((mesh.submeshes[s].shaderProperties & (1 << 1)) || (mesh.submeshes[s].shaderProperties & (1 << 2))){ // 'Uv1' or 'Uv2'
                            mesh.needReload = true;
                        }
                    }
                    if (!mesh.submeshes[s].material.normalTexture.empty()){
                        if (!(mesh.submeshes[s].shaderProperties & (1 << 7))){ // not 'Nmp'
                            mesh.needReload = true;
                        }
                    }else{
                        if (mesh.submeshes[s].shaderProperties & (1 << 7)){ // 'Nmp'
                            mesh.needReload = true;
                        }
                    }

                }
            }

            if (mesh.autoTransparency && mesh.transparent != expectedTransparent){
                mesh.needReload = true;
            }

            if (mesh.loaded && mesh.needReload){
                destroyMesh(entity, mesh);
            }
            // A terrain builds its buffers from the heightmap, so an empty terrain mesh is a load
            // waiting on data rather than one with no data - keep retrying that one.
            const bool nothingToLoad = !terrain && meshHasNothingToLoad(mesh);
            if (!mesh.loadCalled && !nothingToLoad){
                constexpr int STUCK_ATTEMPTS = 8;
                constexpr int RETRY_FRAMES = 30;

                if (mesh.loadRetryDelay > 0){
                    mesh.loadRetryDelay--;
                }else if (loadMesh(entity, mesh, pipelines, instmesh, terrain)){
                    mesh.loadRetries = 0;
                }else{
                    // An async shader still compiling fails the same way, so the wait is paced
                    // rather than abandoned - and a mesh that never loads says which it is.
                    mesh.loadRetries++;
                    if (mesh.loadRetries == STUCK_ATTEMPTS){
                        Log::warn("Mesh of '%s' has failed to load %i times, retrying every %i frames",
                                  scene->getEntityName(entity).c_str(), STUCK_ATTEMPTS, RETRY_FRAMES);
                    }
                    if (mesh.loadRetries >= STUCK_ATTEMPTS)
                        mesh.loadRetryDelay = RETRY_FRAMES;
                }
            }
            if (mesh.needUpdateAABB || transform.needUpdate){
                // Bones cancel out this mesh's model matrix, so the shader draws at
                // modelMatrix * bonesMatrix[b] * vertex and modelMatrix * aabb mis-sizes the box
                // once posed. Each bone bounds only its own share: the whole aabb per bone works
                // too, but unions body-sized boxes over the skeleton. Local space so the editor
                // draws the same box.
                mesh.skinnedAABB.setNull();
                if (!instmesh){
                    const size_t boneBounds = std::min(mesh.bonesAABB.size(), mesh.bonesMatrix.size());
                    for (size_t b = 0; b < boneBounds; b++){
                        if (!mesh.bonesAABB[b].isNull()){
                            mesh.skinnedAABB.merge(mesh.bonesMatrix[b] * mesh.bonesAABB[b]);
                        }
                    }

                    if (mesh.skinnedAABB.isNull()){ // no per-bone bounds to pose
                        const Matrix4 identityMatrix;
                        for (size_t b = 0; b < mesh.bonesMatrix.size(); b++){
                            const Matrix4& bone = mesh.bonesMatrix[b];
                            if (bone != identityMatrix){
                                mesh.skinnedAABB.merge(bone * mesh.aabb);
                            }
                        }
                    }
                }
                mesh.worldAABB = mesh.skinnedAABB.isNull() ? (transform.modelMatrix * mesh.aabb)
                                                           : (transform.modelMatrix * mesh.skinnedAABB);

                mesh.needUpdateAABB = false;

                // Tilemap chunk bounds are refreshed with the mesh AABB: rebuilding a
                // tilemap recalculates that AABB, and MeshSystem updates before this
                // system, so new chunks are always current before draw().
                if (signature.test(cidTilemap)){
                    TilemapComponent& tilemap = scene->getComponent<TilemapComponent>(entity);
                    for (TilemapChunk& chunk : tilemap.chunks){
                        chunk.worldAABB = transform.modelMatrix * chunk.aabb;
                    }
                }
            }
        }else if (signature.test(cidUI)){
            UIComponent& ui = scene->getComponent<UIComponent>(entity);

            bool isText = false;
            if (signature.test(cidText)){
                isText = true;
            }

            if (ui.needUpdateTexture){
                uint32_t textureProperty = isText ? (1 << 1) : (1 << 0); // 'Ftx' for texts, 'Tex' otherwise
                if (!ui.texture.empty()){
                    if (!(ui.shaderProperties & textureProperty)){
                        ui.needReload = true;
                    }
                }else{
                    if (ui.shaderProperties & textureProperty){
                        ui.needReload = true;
                    }
                }
            }

            if (ui.loaded && ui.needReload){
                destroyUI(entity, ui);
            }
            if (!ui.loadCalled){
                loadUI(entity, ui, pipelines, isText);
            }
            if (ui.needUpdateAABB || transform.needUpdate){
                ui.worldAABB = transform.modelMatrix * ui.aabb;
                ui.needUpdateAABB = false;
            }
        }else if (signature.test(cidPoints)){
            PointsComponent& points = scene->getComponent<PointsComponent>(entity);

            if (points.needUpdateTexture){
                if (!points.texture.empty()){
                    if (!(points.shaderProperties & (1 << 0))){ // not 'Tex'
                        points.needReload = true;
                    }
                }else{
                    if (points.shaderProperties & (1 << 0)){ // 'Tex'
                        points.needReload = true;
                    }
                }
            }

            if (points.transparent != points.pipelineTransparent){
                points.needReload = true;
            }

            if (points.loaded && points.needReload){
                destroyPoints(entity, points);
            }
            if (!points.loadCalled){
                loadPoints(entity, points, pipelines);
            }

            bool sortTransparentPoints = points.transparent && batchSort;

            if (batchSortChanged){
                points.needUpdate = true;
            }

            if (points.needUpdate){
                updatePoints(points, transform, mainCamera, mainCameraTransform);
            }

            // Sort/upload transparent points once here for the main camera: the buffer is
            // shared by every camera and sokol only allows one sg_update_buffer per frame
            // (VALIDATE_UPDATEBUF_ONCE), so every other camera reuses this order.
            if (sortTransparentPoints && (points.needUpdate || mainCamera.needUpdate || transform.needUpdate)){
                sortPoints(points, transform, mainCamera, mainCameraTransform);
            }

            if (points.loaded){
                points.needUpdate = false;
            }
        }else if (signature.test(cidLines)){
            LinesComponent& lines = scene->getComponent<LinesComponent>(entity);
            if (lines.loaded && lines.needReload){
                destroyLines(entity, lines);
            }
            if (!lines.loadCalled){
                loadLines(entity, lines, pipelines);
            }

        }else if (signature.test(cidLight)){
            LightComponent& light = scene->getComponent<LightComponent>(entity);

            if (mainCamera.needUpdate || transform.needUpdate || light.needUpdateShadowCamera){
                const bool shadowCameraDirty = transform.needUpdate || light.needUpdateShadowCamera ||
                    (mainCamera.needUpdate && light.type == LightType::DIRECTIONAL);
                // need to be updated ONLY for main camera
                updateLightFromScene(light, transform, mainCamera, mainCameraTransform);

                if (light.shadows && shadowCameraDirty){
                    if (light.type == LightType::POINT){
                        shadowPointAtlasContentDirty = true;
                    }else{
                        shadowAtlasContentDirty = true;
                    }
                }
                light.needUpdateShadowCamera = false;
                shadowInstanceViewsDirty = true;
            }
        }

        if (mainCamera.needUpdate || transform.needUpdate){

            // need to be updated for every camera
            if (!hasMultipleCameras){
                updateMVP(i, transform, mainCamera, mainCameraTransform);
            }

            // Select the main camera's CDLOD cut (view 0) once per frame; both the
            // shadow depth pass and the main color pass use it. Render-to-texture
            // cameras select their own views (1..N-1) during draw().
            if (signature.test(cidTerrain)){
                TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);

                updateTerrain(terrain, transform, mainCamera, mainCameraTransform, 0);
            }
            
            if (signature.test(cidSound)){
                SoundComponent& audio = scene->getComponent<SoundComponent>(entity);

                audio.needUpdate = true;
            }

        }

        if (transform.needUpdate){

            if (signature.test(cidModel)){
                ModelComponent& model = scene->getComponent<ModelComponent>(entity);

                model.inverseDerivedTransform = transform.modelMatrix.inverse();
            }

            if (signature.test(cidBone)){
                BoneComponent& bone = scene->getComponent<BoneComponent>(entity);

                if (bone.model != NULL_ENTITY && bone.index >= 0){
                    ModelComponent* model = scene->findComponent<ModelComponent>(bone.model);

                    if (model && model->skinBindings.empty()) {
                        Matrix4 skinning = model->inverseDerivedTransform * transform.modelMatrix * bone.offsetMatrix;

                        auto updateDrivenMesh = [&](MeshComponent* drivenMesh) {
                            if (!drivenMesh || !drivenMesh->bonesMatrix.validIndex(bone.index))
                                return;
                            drivenMesh->bonesMatrix[bone.index] = skinning;
                            drivenMesh->needUpdateBones = true;
                            drivenMesh->needUpdateAABB = true;
                        };

                        // Single-mesh / flattened models keep their skinned geometry on the model
                        // entity's own mesh. Multi-node skinned models split each mesh node into a
                        // child entity (each with its own MeshComponent), so the same bone must drive
                        // every child mesh that shares this skeleton.
                        // A moved bone changes the skinned bounds, so flag the driven mesh(es)
                        // for a world-AABB rebuild. Bones are processed after their mesh in this
                        // loop, so this takes effect next frame with the fresh bonesMatrix.
                        updateDrivenMesh(scene->findComponent<MeshComponent>(bone.model));

                        for (auto const& meshNode : model->meshNodesMapping){
                            updateDrivenMesh(scene->findComponent<MeshComponent>(meshNode.second));
                        }
                    }
                }
            }

        }

        transform.needUpdateChildVisibility = false;
        transform.needUpdate = false;
    }

    for (int i = 0; i < cameras->size(); i++){
        CameraComponent& camera = cameras->getComponentFromIndex(i);
        if (camera.needUpdate){
            camera.needUpdate = false;
        }
    }

    processLights(numLights, mainCamera, mainCameraTransform);
    updateClusteredLightGrid(mainCamera);
    updateShadowPageTable();
    if (hasLights2D){
        processLights2D();
    }

    // build the merged occluder segment list once per frame (world transforms are
    // final here); draw() uploads it at most once, even when the same scene is
    // drawn multiple times per frame (e.g. open as a tab AND layered as a child scene)
    if (hasShadows2D && hasShadow2DAtlas){
        occluder2DVertexCount = buildOccluder2DSegments();
        occluder2DNeedUpdateBuffer = (occluder2DVertexCount > 0);
    }else{
        occluder2DVertexCount = 0;
        occluder2DNeedUpdateBuffer = false;
    }
}

void RenderSystem::draw(){
    std::priority_queue<TransparentRenderData, std::vector<TransparentRenderData>, TransparentRenderComparison> transparentRenders;
    // drawn before the water, which covers them
    decltype(transparentRenders) behindWaterRenders;

    auto transforms = scene->getComponentArray<Transform>();
    auto cameras = scene->getComponentArray<CameraComponent>();

    frameTime = (float)Engine::getSystemTime();

    updateShadowBindings();
    updateAllTerrainRenderTextures();
    Entity mainCameraEntity = scene->getRenderCamera();
    if (CameraComponent* mainCamera = scene->findComponent<CameraComponent>(mainCameraEntity)){
        if (Transform* mainCameraTransform = scene->findComponent<Transform>(mainCameraEntity)){
            buildInstanceViews(*mainCamera, *mainCameraTransform);
        }
    }
    updateInstanceBuffers();

    // free the fixed-resolution target when the setting is turned off (the blit
    // pipeline itself is kept; it is cheap and may be re-enabled)
    if (!isFixedResolutionActive() && fixedResFramebuffer.isCreated()){
        fixedResFramebuffer.destroy();
        fixedResWidth = 0;
        fixedResHeight = 0;
    }

    //---------Depth shader----------
    if (hasShadows){
        auto lights = scene->getComponentArray<LightComponent>();
        auto meshes = scene->getComponentArray<MeshComponent>();

        // a projective slot has its own culled instance view; point faces draw everything
        auto drawShadowCasters = [&](LightComponent& light, int cameraIndex, int tile, PipelineType pipelineType, int instanceView){
            const LightCamera shadowCamera = shadowTileCamera(light, cameraIndex, tile);
            const LodView shadowLod = (instanceView >= 0) ? shadowLodView(light, cameraIndex, instanceView - 1, tile) : LodView();
            const LodView* lodView = (instanceView >= 0) ? &shadowLod : nullptr;

            // skip casters that cannot shadow the main view (not when other cameras or probes
            // reuse the maps, or a negative bias lets casters behind a receiver count)
            Plane receiverPlanes[18];
            int numReceiverPlanes = 0;
            CameraComponent* receiverCamera = scene->findComponent<CameraComponent>(mainCameraEntity);
            if (receiverCamera && light.type == LightType::DIRECTIONAL && !hasMultipleCameras && !hasReflectionProbes && light.shadowBias >= 0.0f){
                numReceiverPlanes = getShadowReceiverPlanes(*receiverCamera, light, cameraIndex, receiverPlanes);
            }

            for (int i = 0; i < meshes->size(); i++){
                MeshComponent& mesh = meshes->getComponentFromIndex(i);
                Entity entity = meshes->getEntity(i);
                Transform* transform = scene->findComponent<Transform>(entity);

                if (!transform || !transform->visible){
                    continue;
                }

                InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);

                // Culling a caster off its own AABB is the common outcome in a big scene, so the
                // terrain/tilemap lookups wait until it is known the mesh really draws.
                if (numReceiverPlanes > 0 && !transform->billboard && cullsBatch(mesh, instmesh) &&
                        !isInsidePlanes(receiverPlanes, numReceiverPlanes, mesh.worldAABB)){
                    continue;
                }

                TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
                TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);

                vs_depth_t vsDepthParams;
                if (transform->billboard && mesh.shadowsBillboard){
                    Matrix4 modelViewMatrix = shadowCamera.lightViewMatrix * transform->modelMatrix;

                    modelViewMatrix.set(0, 0, transform->worldScale.x);
                    modelViewMatrix.set(0, 1, 0.0);
                    modelViewMatrix.set(0, 2, 0.0);

                    if (!transform->cylindricalBillboard) {
                        modelViewMatrix.set(1, 0, 0.0);
                        modelViewMatrix.set(1, 1, transform->worldScale.y);
                        modelViewMatrix.set(1, 2, 0.0);
                    }

                    modelViewMatrix.set(2, 0, 0.0);
                    modelViewMatrix.set(2, 1, 0.0);
                    modelViewMatrix.set(2, 2, transform->worldScale.z);

                    const Matrix4& projection = shadowCamera.lightProjectionMatrix;
                    vsDepthParams = {modelViewMatrix, projection, projection * modelViewMatrix};
                }else{
                    const Matrix4& lightVP = shadowCamera.lightViewProjectionMatrix;
                    vsDepthParams = {transform->modelMatrix, lightVP, lightVP * transform->modelMatrix};
                }

                drawMeshDepth(
                    mesh,
                    *transform,
                    shadowCamera.nearFar.y,
                    shadowCamera.frustumPlanes,
                    vsDepthParams,
                    instmesh,
                    terrain,
                    tilemap,
                    false,
                    pipelineType,
                    instanceView,
                    lodView);
            }
        };

        // All projective slots share one pass: clear once, then viewport/scissor per slot.
        // Static scenes keep the resident atlas and skip this entire pass.
        if (shadowAtlasContentDirty){
            bool projectivePassStarted = false;
            for (size_t l = 0; l < lights->size(); l++){
            LightComponent& light = lights->getComponentFromIndex(l);
            if (light.type == LightType::POINT ||
                light.intensity <= 0 || !light.shadows ||
                light.shadowMapIndex < 0 || !hasShadowAtlas){
                continue;
            }

            int numShadowCameras =
                (light.type == LightType::DIRECTIONAL) ? (int)light.numShadowCascades : 1;
            for (int c = 0; c < numShadowCameras; c++){
                const int logicalPage = light.shadowMapIndex +
                    ((light.type == LightType::DIRECTIONAL) ? c : 0);
                for (int tile = 0; tile < SHADOW_VIRTUAL_PAGES_PER_MAP; tile++){
                    const int virtualPage = shadowVirtualPageIndex(logicalPage, tile);
                    const int slotIndex = (virtualPage >= 0 && virtualPage < MAX_SHADOW_VIRTUAL_PAGES) ?
                        shadowVirtualToPhysical[virtualPage] : -1;
                    if (slotIndex < 0 || slotIndex >= shadowAtlasUsedSlots)
                        continue;

                    if (!projectivePassStarted){
                        shadowAtlasPassRender.setClearDepth(1.0f);
                        shadowAtlasPassRender.startRenderPass(&shadowAtlasFramebuffer);
                        projectivePassStarted = true;
                    }

                    Rect slotRect = getShadowAtlasSlotRect(slotIndex);
                    shadowAtlasPassRender.applyViewport(slotRect);
                    shadowAtlasPassRender.applyScissor(slotRect);
                    passResolution = Vector2(slotRect.getWidth(), slotRect.getHeight());
                    drawShadowCasters(light, c, tile, PIP_SHADOW_DEPTH, 1 + slotIndex);
                }
            }
            }
            if (projectivePassStarted){
                shadowAtlasPassRender.endRenderPass();
            }
            shadowAtlasContentDirty = false;
        }

        // Point shadows keep the packed-color depth per-face pass (PIP_DEPTH).
        if (shadowPointAtlasContentDirty){
            bool pointAtlasSlotWritten = false;
            for (size_t l = 0; l < lights->size(); l++){
            LightComponent& light = lights->getComponentFromIndex(l);
            if (light.type != LightType::POINT ||
                light.intensity <= 0 || !light.shadows ||
                light.shadowMapIndex < 0 || !hasShadowPointAtlas){
                continue;
            }

            for (int c = 0; c < SHADOW_CUBE_FACES; c++){
                int slotIndex = light.shadowMapIndex + c;
                if (slotIndex < 0 || slotIndex >= MAX_POINT_SHADOW_ATLAS_SLOTS){
                    continue;
                }

                if (!pointAtlasSlotWritten){
                    shadowPointAtlasPassRender.setClearColor(Vector4(1.0, 1.0, 1.0, 1.0));
                }else{
                    shadowPointAtlasPassRender.setLoadActionLoad();
                }

                Rect slotRect = getShadowPointAtlasSlotRect(slotIndex);
                shadowPointAtlasPassRender.startRenderPass(&shadowPointAtlasFramebuffer);
                shadowPointAtlasPassRender.applyViewport(slotRect);
                shadowPointAtlasPassRender.applyScissor(slotRect);
                pointAtlasSlotWritten = true;

                passResolution = Vector2(slotRect.getWidth(), slotRect.getHeight());
                drawShadowCasters(light, c, -1, PIP_DEPTH, -1);
                shadowPointAtlasPassRender.endRenderPass();
            }
            }
            shadowPointAtlasContentDirty = false;
        }
    }

    //---------2D light shadow pass (1D polar rows)----------
    if (hasShadows2D && hasShadow2DAtlas){
        if (occluder2DVertexCount > 0 && (!occluder2DLoaded || occluder2DVertexCount > occluder2DBufferCapacity)){
            unsigned int capacity = std::max(occluder2DVertexCount * 2u, 256u);
            loadOccluder2DPass(capacity);
        }

        bool drawSegments = occluder2DLoaded && occluder2DVertexCount > 0;

        if (drawSegments && occluder2DNeedUpdateBuffer){
            // occluders are world-space and can move every frame: re-upload the merged
            // segment buffer (built in update(), at most once per frame — sokol allows
            // a single sg_update_buffer per buffer and frame)
            occluder2DBuffer.setData((unsigned char*)occluder2DSegments.data(), occluder2DSegments.size() * sizeof(float));
            occluder2DBuffer.getRender()->updateBuffer(occluder2DBuffer.getSize(), occluder2DBuffer.getData());
            occluder2DNeedUpdateBuffer = false;
        }

        // rows are rendered (or at least cleared) every frame even with zero
        // segments: the rows are in light-relative polar space, so stale contents
        // would make old shadows follow the light around
        auto lights2d = scene->getComponentArray<Light2DComponent>();
        bool atlasRowWritten = false;

        for (int l = 0; l < numLights2D; l++){
            Light2DComponent& light = lights2d->getComponentFromIndex(l);
            if (!light.shadows || light.shadowMapIndex < 0 || light.intensity <= 0){
                continue;
            }
            Entity entity = lights2d->getEntity(l);
            Transform* lightTransform = scene->findComponent<Transform>(entity);
            if (!lightTransform){
                continue;
            }

            // first row of the frame clears the whole atlas to white (= far/unoccluded)
            if (!atlasRowWritten){
                shadow2DAtlasPassRender.setClearColor(Vector4(1.0, 1.0, 1.0, 1.0));
            }else{
                shadow2DAtlasPassRender.setLoadActionLoad();
            }
            Rect rowRect(0.0f, (float)light.shadowMapIndex, (float)shadow2DAtlasWidth, 1.0f);
            shadow2DAtlasPassRender.startRenderPass(&shadow2DAtlasFramebuffer);
            shadow2DAtlasPassRender.applyViewport(rowRect);
            shadow2DAtlasPassRender.applyScissor(rowRect);
            atlasRowWritten = true;

            if (drawSegments && occluder2DRender.beginDraw(PIP_DEPTH)){
                vs_shadow2d_t shadow2dParams;
                shadow2dParams.lightPos_range = Vector4(
                    lightTransform->worldPosition.x, lightTransform->worldPosition.y,
                    0.0f, std::max(light.range, 0.001f));

                // three passes at NDC x offsets -2/0/+2 so segments crossing the
                // theta = +-pi seam rasterize on both ends of the row
                for (int k = -1; k <= 1; k++){
                    shadow2dParams.offset = Vector4(2.0f * (float)k, 0.0f, 0.0f, 0.0f);
                    occluder2DRender.applyUniformBlock(shadow2DSlotParams, sizeof(vs_shadow2d_t), &shadow2dParams);
                    occluder2DRender.draw(0, occluder2DVertexCount, 1);
                }
            }

            shadow2DAtlasPassRender.endRenderPass();
        }
    }

    // Runtime-captured probes share a strict budget: one cubemap face total per
    // frame. Static probes without an authored cubemap capture once; dynamic
    // probes follow their configured update policy.
    renderReflectionProbeCapture();

    // assigns each rendering camera its own terrain CDLOD view slot this frame:
    // main camera = 0 (selected in update(), also used by the shadow pass), each
    // render-to-texture camera = 1..N-1; cameras past the cap reuse the main view
    int terrainViewCounter = 0;

    // render-to-texture cameras first, so the main camera samples this frame's reflections
    std::vector<int> cameraOrder;
    for (int i = 0; i < cameras->size(); i++){
        if (cameras->getEntity(i) != mainCameraEntity){
            cameraOrder.push_back(i);
        }
    }
    if (cameras->hasEntity(mainCameraEntity)){
        cameraOrder.push_back((int)cameras->getIndex(mainCameraEntity));
    }

    const fs_lighting_t mainLighting = fs_lighting;
    for (int i : cameraOrder){
        Entity cameraEntity = cameras->getEntity(i);
        CameraComponent& camera = cameras->getComponentFromIndex(i);
        Transform* cameraTransformPtr = scene->findComponent<Transform>(cameraEntity);
        if (!cameraTransformPtr)
            continue;
        Transform& cameraTransform = *cameraTransformPtr;

        bool isMainCamera = (cameraEntity == mainCameraEntity);

        if (!isMainCamera && !camera.renderToTexture){
            continue; // camera is not used
        }

        fs_lighting = mainLighting;
        if (!isMainCamera){
            applyViewLights(cameraTransform.worldPosition);
            // The grid is built for the main camera. Render-to-texture/reflection
            // views keep the compatibility light selection until their own grid
            // becomes available, rather than sampling the main view's culling.
            fs_lighting.clusterDepth.w = 0.0f;
        }

        int terrainView = 0;
        if (!isMainCamera){
            terrainViewCounter++;
            terrainView = (terrainViewCounter < MAX_TERRAIN_VIEWS) ? terrainViewCounter : 0;
        }

        depthPrePassRendered = false;
        currentSceneDepthTexture = NULL;
        currentSceneCopy = NULL;

        // Screen-space reflections (main camera only). The G-buffer geometry pass runs
        // FIRST so SSAO can share its depth (a single geometry pre-pass feeds both
        // effects). Further below the opaque color pass is redirected into an offscreen
        // buffer, then renderSSR() marches the G-buffer and composites reflections to the
        // real destination: a framebuffer (editor / render-to-texture / fixed resolution)
        // or the swapchain in exported builds (see updateSwapchainRedirect).
        // Fixed game resolution (main scene main camera only): the color pass is
        // redirected into fixedResFramebuffer at the scene's fixed size, and
        // renderFixedResolutionBlit() upscales it to the view rect afterwards.
        bool useFixedRes = false;
        if (isMainCamera && !camera.renderToTexture && isFixedResolutionActive()){
            loadBlit();
            if (blitLoaded && ensureFixedResFramebuffer(scene->getFixedResolutionWidth(),
                    scene->getFixedResolutionHeight(), scene->getFixedResolutionFilter())){
                useFixedRes = true;
            }
        }

        bool useSSR = false;
        FramebufferRender* ssrDestination = nullptr;
        if (isMainCamera && scene->isSSREnabled() && (Engine::getFramebuffer() || camera.renderToTexture || useFixedRes || swapchainRedirect)){
            loadSSR();
            unsigned int sw = (unsigned int)Engine::getViewRect().getWidth();
            unsigned int sh = (unsigned int)Engine::getViewRect().getHeight();
            if (useFixedRes){
                // effects run at the fixed resolution: SSR composites into the
                // fixed-res target, which the blit then upscales
                sw = fixedResWidth;
                sh = fixedResHeight;
            }
            if (ssrLoaded && ensureSSRFramebuffers(sw, sh) && ensureGBufferFramebuffer(sw, sh)){
                renderGBufferPass(camera);
                useSSR = true;
            }
        }

        // User post-process chain: like SSR it needs the scene color offscreen, so it
        // follows the same destination-capture rule. Main scene only: its last pass
        // writes every pixel, so a layer scene would erase the scene below it.
        bool usePostProcess = false;
        FramebufferRender* postDestination = nullptr;
        if (isMainCamera && !scene->getPostProcessPasses().empty()
                && (Engine::getMainScene() == scene || camera.renderToTexture)
                && (Engine::getFramebuffer() || camera.renderToTexture || useFixedRes || swapchainRedirect)){
            loadPostProcess();

            // only a swapchain destination ends the chain with a blit
            bool needsBlit = !(Engine::getFramebuffer() || camera.renderToTexture || useFixedRes);
            if (needsBlit){
                loadBlit();
            }

            unsigned int pw = useSSR ? ssrWidth : (unsigned int)Engine::getViewRect().getWidth();
            unsigned int ph = useSSR ? ssrHeight : (unsigned int)Engine::getViewRect().getHeight();
            if (useFixedRes){
                pw = fixedResWidth;
                ph = fixedResHeight;
            }

            if (postProcessLoaded && !postProcessPasses.empty() && (!needsBlit || blitLoaded)
                    && ensurePostProcessFramebuffers(pw, ph)){
                usePostProcess = true;

                // a pass sampling depth needs the pre-pass when neither SSR nor SSAO ran
                if (postProcessNeedsDepth && !useSSR && !scene->isSSAOEnabled()
                        && ensureDepthPrePassFramebuffer(pw, ph)){
                    renderDepthPrePass(camera);
                }
            }
        }

        // HDR chain (bloom, then tone map) reads the scene color offscreen. SSR feeds
        // this target through its dedicated HDR-safe composite framebuffer.
        bool useHDR = false;
        FramebufferRender* hdrDestination = nullptr;
        if (isMainCamera && scene->isHdrEnabled() && !useSSR &&
                (Engine::getFramebuffer() || camera.renderToTexture || useFixedRes || swapchainRedirect)){
            loadHDR();
            unsigned int hw = (unsigned int)Engine::getViewRect().getWidth();
            unsigned int hh = (unsigned int)Engine::getViewRect().getHeight();
            if (useFixedRes){
                hw = fixedResWidth;
                hh = fixedResHeight;
            }
            if (hdrLoaded && ensureSceneColorFramebuffer(hw, hh) &&
                    ensureHDRFramebuffers(hw, hh, scene->getBloomLevels())){
                useHDR = true;
            }
        }

        bool useContactShadows = false;
        bool useSSGI = false;
        bool useTAA = false;
        bool useTAAU = false;
        bool useMotionBlur = false;
        if (isMainCamera && !useSSR && (scene->isTAAEnabled() || scene->isMotionBlurEnabled()) &&
                (Engine::getFramebuffer() || camera.renderToTexture || useFixedRes || swapchainRedirect)){
            loadTemporalEffects();
            unsigned int tw = (unsigned int)Engine::getViewRect().getWidth();
            unsigned int th = (unsigned int)Engine::getViewRect().getHeight();
            if (useFixedRes){
                tw = fixedResWidth;
                th = fixedResHeight;
            }else if (camera.renderToTexture && camera.framebuffer){
                tw = camera.framebuffer->getWidth();
                th = camera.framebuffer->getHeight();
            }
            unsigned int temporalOutputWidth = (unsigned int)Engine::getViewRect().getWidth();
            unsigned int temporalOutputHeight = (unsigned int)Engine::getViewRect().getHeight();
            if (camera.renderToTexture && camera.framebuffer){
                temporalOutputWidth = camera.framebuffer->getWidth();
                temporalOutputHeight = camera.framebuffer->getHeight();
            }
            const bool wantsTAAU = useFixedRes && scene->isTAAEnabled();
            if (velocityLoaded && taaLoaded && motionBlurLoaded && ensureSceneColorFramebuffer(tw, th) &&
                    ensureTemporalFramebuffers(tw, th,
                        wantsTAAU ? temporalOutputWidth : tw,
                        wantsTAAU ? temporalOutputHeight : th, sceneColorHDR)){
                useTAAU = wantsTAAU;
                useTAA = scene->isTAAEnabled() && !useTAAU;
                useMotionBlur = scene->isMotionBlurEnabled();
            }
        }

        if (isMainCamera && scene->isContactShadowsEnabled() &&
                scene->getPostProcessPasses().empty() &&
                (Engine::getFramebuffer() || camera.renderToTexture || useFixedRes || swapchainRedirect)){
            loadContactShadows();
            loadBlit();
            unsigned int cw = (unsigned int)Engine::getViewRect().getWidth();
            unsigned int ch = (unsigned int)Engine::getViewRect().getHeight();
            if (useFixedRes){
                cw = fixedResWidth;
                ch = fixedResHeight;
            }else if (camera.renderToTexture && camera.framebuffer){
                cw = camera.framebuffer->getWidth();
                ch = camera.framebuffer->getHeight();
            }
            if (contactShadowsLoaded && blitLoaded && ensureSceneColorFramebuffer(cw, ch) &&
                    ensureContactShadowsFramebuffer(cw, ch, sceneColorHDR)){
                useContactShadows = true;
            }
        }

        if (isMainCamera && scene->isSSGIEnabled() &&
                (Engine::getFramebuffer() || camera.renderToTexture || useFixedRes || swapchainRedirect)){
            loadSSGI();
            loadBlit();
            unsigned int sw = useSSR ? ssrWidth : (unsigned int)Engine::getViewRect().getWidth();
            unsigned int sh = useSSR ? ssrHeight : (unsigned int)Engine::getViewRect().getHeight();
            if (usePostProcess){
                sw = postProcessWidth;
                sh = postProcessHeight;
            }
            if (useFixedRes){
                sw = fixedResWidth;
                sh = fixedResHeight;
            }else if (camera.renderToTexture && camera.framebuffer){
                sw = camera.framebuffer->getWidth();
                sh = camera.framebuffer->getHeight();
            }
            const bool ssgiHDRTarget = useHDR && !usePostProcess && sceneColorHDR;
            if (ssgiLoaded && blitLoaded && ensureSSGIFramebuffer(sw, sh, ssgiHDRTarget))
                useSSGI = true;
        }

        // Screen-space AO is produced once for the main camera before its color pass.
        // When SSR is active its G-buffer already holds the camera depth (color[0]), so
        // SSAO reuses it instead of running a second geometry pre-pass. Other cameras
        // sample empty white (AO = 1) so their USE_SSAO meshes are unaffected.
        currentSSAOTexture = &emptyWhite;
        if (isMainCamera && scene->isSSAOEnabled()){
            TextureRender* sharedDepth = useSSR ? &gbufferFramebuffer.getRender().getColorAttachmentTexture(0) : nullptr;
            renderSSAO(camera, sharedDepth);
        }

        // water depth from the SSR G-buffer, a pre-pass already drawn or its own pre-pass
        if (isMainCamera && hasWaterDepth){
            if (useSSR){
                currentSceneDepthTexture = &gbufferFramebuffer.getRender().getColorAttachmentTexture(0);
            }else{
                if (!depthPrePassRendered){
                    unsigned int dw = (unsigned int)Engine::getViewRect().getWidth();
                    unsigned int dh = (unsigned int)Engine::getViewRect().getHeight();
                    if (useFixedRes){
                        dw = fixedResWidth;
                        dh = fixedResHeight;
                    }else if (camera.renderToTexture){
                        dw = camera.framebuffer->getWidth();
                        dh = camera.framebuffer->getHeight();
                    }
                    if (ensureDepthPrePassFramebuffer(dw, dh)){
                        renderDepthPrePass(camera);
                    }
                }
                if (depthPrePassRendered){
                    currentSceneDepthTexture = &ssaoDepthFramebuffer.getRender().getColorTexture();
                }
            }
        }

        // Depth of field (main camera only). It needs the scene color in an offscreen
        // buffer and a depth source, and it is decided after SSAO and the water pre-pass
        // so it can reuse the depth they already drew instead of paying for a third one.
        // SSR is excluded: its composite writes the destination, not the scene color.
        bool useDOF = false;
        TextureRender* dofDepthTexture = nullptr;
        TextureRender* temporalDepthTexture = nullptr;
        TextureRender* contactDepthTexture = nullptr;
        TextureRender* ssgiDepthTexture = nullptr;
        if (isMainCamera && useContactShadows && !useSSR && !depthPrePassRendered){
            unsigned int cw = (unsigned int)Engine::getViewRect().getWidth();
            unsigned int ch = (unsigned int)Engine::getViewRect().getHeight();
            if (useFixedRes){
                cw = fixedResWidth;
                ch = fixedResHeight;
            }else if (camera.renderToTexture && camera.framebuffer){
                cw = camera.framebuffer->getWidth();
                ch = camera.framebuffer->getHeight();
            }
            if (ensureDepthPrePassFramebuffer(cw, ch))
                renderDepthPrePass(camera);
        }
        if (isMainCamera && useContactShadows && useSSR)
            contactDepthTexture = &gbufferFramebuffer.getRender().getColorAttachmentTexture(0);
        else if (isMainCamera && useContactShadows && depthPrePassRendered)
            contactDepthTexture = &ssaoDepthFramebuffer.getRender().getColorTexture();
        if (isMainCamera && useSSGI && !useSSR && !depthPrePassRendered){
            unsigned int sw = (unsigned int)Engine::getViewRect().getWidth();
            unsigned int sh = (unsigned int)Engine::getViewRect().getHeight();
            if (usePostProcess){
                sw = postProcessWidth;
                sh = postProcessHeight;
            }else if (useFixedRes){
                sw = fixedResWidth;
                sh = fixedResHeight;
            }else if (camera.renderToTexture && camera.framebuffer){
                sw = camera.framebuffer->getWidth();
                sh = camera.framebuffer->getHeight();
            }
            if (ensureDepthPrePassFramebuffer(sw, sh))
                renderDepthPrePass(camera);
        }
        if (isMainCamera && useSSGI){
            if (useSSR)
                ssgiDepthTexture = &gbufferFramebuffer.getRender().getColorAttachmentTexture(0);
            else if (depthPrePassRendered)
                ssgiDepthTexture = &ssaoDepthFramebuffer.getRender().getColorTexture();
        }
        if (isMainCamera && (useTAA || useTAAU || useMotionBlur) && !depthPrePassRendered){
            unsigned int tw = (unsigned int)Engine::getViewRect().getWidth();
            unsigned int th = (unsigned int)Engine::getViewRect().getHeight();
            if (useFixedRes){
                tw = fixedResWidth;
                th = fixedResHeight;
            }else if (camera.renderToTexture && camera.framebuffer){
                tw = camera.framebuffer->getWidth();
                th = camera.framebuffer->getHeight();
            }
            if (ensureDepthPrePassFramebuffer(tw, th))
                renderDepthPrePass(camera);
        }
        if (isMainCamera && (useTAA || useTAAU || useMotionBlur) && depthPrePassRendered)
            temporalDepthTexture = &ssaoDepthFramebuffer.getRender().getColorTexture();
        if (isMainCamera && scene->isDOFEnabled() && !useSSR &&
                (Engine::getFramebuffer() || camera.renderToTexture || useFixedRes || swapchainRedirect)){
            unsigned int dw = (unsigned int)Engine::getViewRect().getWidth();
            unsigned int dh = (unsigned int)Engine::getViewRect().getHeight();
            if (useFixedRes){
                dw = fixedResWidth;
                dh = fixedResHeight;
            }else if (camera.renderToTexture && camera.framebuffer){
                dw = camera.framebuffer->getWidth();
                dh = camera.framebuffer->getHeight();
            }else if (Engine::getFramebuffer()){
                dw = (unsigned int)Engine::getFramebuffer()->getWidth();
                dh = (unsigned int)Engine::getFramebuffer()->getHeight();
            }

            loadDOF();
            if (dofLoaded && ensureSceneColorFramebuffer(dw, dh) && ensureDOFFramebuffers(dw, dh)){
                if (depthPrePassRendered){
                    dofDepthTexture = &ssaoDepthFramebuffer.getRender().getColorTexture();
                    useDOF = ssaoDepthFramebuffer.isCreated();
                }else if (ensureDepthPrePassFramebuffer(dw, dh)){
                    renderDepthPrePass(camera);
                    dofDepthTexture = &ssaoDepthFramebuffer.getRender().getColorTexture();
                    useDOF = depthPrePassRendered;
                }
            }
        }

        // Atmosphere (main camera only) reads the same two offscreen buffers, so it is decided
        // after DOF and reuses the depth DOF may just have forced.
        bool useAtmosphere = false;
        TextureRender* atmosphereDepthTexture = dofDepthTexture;
        if (isMainCamera && !useSSR && atmosphereRequested() &&
                (Engine::getFramebuffer() || camera.renderToTexture || useFixedRes || swapchainRedirect)){
            unsigned int aw = (unsigned int)Engine::getViewRect().getWidth();
            unsigned int ah = (unsigned int)Engine::getViewRect().getHeight();
            if (useFixedRes){
                aw = fixedResWidth;
                ah = fixedResHeight;
            }else if (camera.renderToTexture && camera.framebuffer){
                aw = camera.framebuffer->getWidth();
                ah = camera.framebuffer->getHeight();
            }else if (Engine::getFramebuffer()){
                aw = (unsigned int)Engine::getFramebuffer()->getWidth();
                ah = (unsigned int)Engine::getFramebuffer()->getHeight();
            }

            loadAtmosphere();
            loadBlit();
            if (atmosphereLoaded && blitLoaded && ensureSceneColorFramebuffer(aw, ah)
                    && ensureAtmosphereFramebuffer(aw, ah)){
                if (!atmosphereDepthTexture && ensureDepthPrePassFramebuffer(aw, ah)){
                    renderDepthPrePass(camera);
                    if (depthPrePassRendered)
                        atmosphereDepthTexture = &ssaoDepthFramebuffer.getRender().getColorTexture();
                }
                useAtmosphere = atmosphereDepthTexture != nullptr;
            }
        }

        // Decals share the packed opaque depth with the other screen-space passes.
        // They are deliberately scheduled before post-processing so their albedo is
        // graded, bloomed and tone-mapped together with the receiver.
        bool useDecals = false;
        TextureRender* decalDepthTexture = nullptr;
        auto decals = scene->getComponentArray<DecalComponent>();
        if (isMainCamera && decals && decals->size() > 0 &&
                (Engine::getFramebuffer() || camera.renderToTexture || useFixedRes || swapchainRedirect)){
            unsigned int dw = (unsigned int)Engine::getViewRect().getWidth();
            unsigned int dh = (unsigned int)Engine::getViewRect().getHeight();
            if (useSSR){
                dw = ssrWidth;
                dh = ssrHeight;
            }else if (usePostProcess){
                dw = postProcessWidth;
                dh = postProcessHeight;
            }else if (useFixedRes){
                dw = fixedResWidth;
                dh = fixedResHeight;
            }else if (camera.renderToTexture && camera.framebuffer){
                dw = camera.framebuffer->getWidth();
                dh = camera.framebuffer->getHeight();
            }else if (Engine::getFramebuffer()){
                dw = (unsigned int)Engine::getFramebuffer()->getWidth();
                dh = (unsigned int)Engine::getFramebuffer()->getHeight();
            }

            if (!useSSR && !depthPrePassRendered && ensureDepthPrePassFramebuffer(dw, dh))
                renderDepthPrePass(camera);
            if (useSSR)
                decalDepthTexture = &gbufferFramebuffer.getRender().getColorAttachmentTexture(0);
            else if (depthPrePassRendered)
                decalDepthTexture = &ssaoDepthFramebuffer.getRender().getColorTexture();

            bool decalColorReady = false;
            if (useSSR)
                decalColorReady = sceneColorFramebuffer.isCreated();
            else if (usePostProcess)
                decalColorReady = postProcessFramebuffer[0].isCreated();
            else if (useFixedRes)
                decalColorReady = fixedResFramebuffer.isCreated();
            else
                decalColorReady = ensureSceneColorFramebuffer(dw, dh);

            loadDecals(sceneColorHDR && !usePostProcess && !useFixedRes);
            useDecals = decalColorReady && decalsLoaded && decalDepthTexture != nullptr;
        }

        // the waters this pass draws: transparencies across them go before them, the rest after
        std::vector<WaterRenderData> passWaters;
        std::vector<TransparentRenderData> afterWaterRenders; // in submission order
        bool refractingWater = false;
        auto waters = scene->getComponentArray<WaterComponent>();
        for (int w = 0; w < waters->size(); w++){
            Entity waterEntity = waters->getEntity(w);
            WaterComponent& water = waters->getComponentFromIndex(w);
            Transform* waterTransform = scene->findComponent<Transform>(waterEntity);
            // a water cannot sample its own reflection while rendering it
            if (!water.loaded || water.needReload || !waterTransform || !waterTransform->visible ||
                    getMirrorCamera(waterEntity) == cameraEntity || !isInsideCamera(camera, water.worldAABB))
                continue;

            const Vector3& eye = cameraTransform.worldPosition;
            const bool eyeAbove = eye.y > waterTransform->worldPosition.y + getWaterSurfaceOffset(water, eye.x, eye.z).y;
            passWaters.push_back({waterEntity, &water, waterTransform, eyeAbove});
            refractingWater = refractingWater || water.refraction;
        }

        // the water the main camera is inside of, even with its surface out of view
        WaterComponent* underwater = (isMainCamera && !disableUnderwater) ? findUnderwater(cameraTransform.worldPosition) : NULL;
        currentUnderwater = (underwater && loadUnderwater(*underwater)) ? underwater : NULL;

        // the swapchain redirect of a water copying the scene, when no other effect took it
        const bool useSceneTarget = isMainCamera && swapchainRedirect && !useSSR && !usePostProcess
                && !camera.renderToTexture && sceneColorFramebuffer.isCreated();

        // the HDR chain reads the scene color, so it needs the same redirect the
        // swapchain blit uses - but it presents the finished image itself
        const bool colorToSceneTarget = useSceneTarget || useHDR || useContactShadows || useSSGI || useTAA || useTAAU || useMotionBlur || useDecals;

        // FXAA is the last stage of the frame: the pass that would have presented is
        // redirected into fxaaFramebuffer and only the anti-aliased result reaches the real
        // destination. That needs the finished image in a texture, so it is available whenever
        // an offscreen stage already ran - not when the color pass draws straight into the
        // headless capture framebuffer or a render-to-texture camera with nothing after it.
        bool useFXAA = false;
        FramebufferRender* fxaaSource = nullptr;
        if (isMainCamera && scene->isFXAAEnabled() &&
                (useSSR || usePostProcess || useHDR || useContactShadows || useSSGI || useTAA || useTAAU || useMotionBlur || useFixedRes || useSceneTarget || useDOF || useAtmosphere || useDecals)){
            unsigned int fw = (unsigned int)Engine::getViewRect().getWidth();
            unsigned int fh = (unsigned int)Engine::getViewRect().getHeight();
            if (camera.renderToTexture && camera.framebuffer){
                fw = camera.framebuffer->getWidth();
                fh = camera.framebuffer->getHeight();
            }else if (Engine::getFramebuffer()){
                fw = (unsigned int)Engine::getFramebuffer()->getWidth();
                fh = (unsigned int)Engine::getFramebuffer()->getHeight();
            }
            loadFXAA();
            useFXAA = fxaaLoaded && ensureFXAAFramebuffer(fw, fh);
            if (useFXAA){
                // where the anti-aliased image goes when no offscreen stage captured one
                if (camera.renderToTexture && camera.framebuffer)
                    fxaaSource = &camera.framebuffer->getRender();
                else if (Engine::getFramebuffer())
                    fxaaSource = &Engine::getFramebuffer()->getRender();
            }
        }

        // whether this camera's color pass targets an offscreen framebuffer
        // (selects PIP_RTT pipelines and flipped rendering on GL)
        bool offscreenTarget = camera.renderToTexture || Engine::getFramebuffer() || useFixedRes || useSSR || usePostProcess || colorToSceneTarget;
        PipelineType colorPip = colorPipeline(camera, offscreenTarget);
        bool distanceSort = sortsByDistance(camera);

        // the main camera copies its scene before the water: refraction and the underwater fade
        // sample the copy, and SSR composites into it first so the reflections stay under the water
        bool copyScene = isMainCamera && offscreenTarget && (refractingWater || currentUnderwater || (useSSR && !passWaters.empty()));
        if (copyScene){
            loadBlit();
            copyScene = blitLoaded;
        }
        // the pass after the copy continues on this depth
        camera.render.setStoreDepth(copyScene);

        if (Engine::getMainScene() == scene || camera.renderToTexture){
            camera.render.setClearColor(scene->getBackgroundColor());
        } else {
            // When drawing a scene as an Engine layer, don't clear the color buffer.
            // Otherwise a scene that was previously the main scene would keep clearing
            // and hide the main scene when reused as a layer.
            camera.render.setLoadActionLoad();
        }

        // the space the UI scissor lives in
        Rect colorPassViewport;
        // the offscreen framebuffer the pass draws into, null for the swapchain
        Framebuffer* colorTarget = nullptr;

        if (useSSR || usePostProcess || useHDR || useContactShadows || useSSGI || useTAA || useTAAU || useMotionBlur || useDecals){
            // capture the real destination for the composite / post-process chain, then
            // redirect the scene into the offscreen color buffer
            FramebufferRender* destination = nullptr;
            if (camera.renderToTexture){
                if (!camera.framebuffer->isCreated()){
                    camera.framebuffer->create();
                }
                destination = &camera.framebuffer->getRender();
            }else if (useFixedRes){
                destination = &fixedResFramebuffer.getRender();
            }else if (Engine::getFramebuffer()){
                if (!Engine::getFramebuffer()->isCreated()){
                    Engine::getFramebuffer()->create();
                }
                destination = &Engine::getFramebuffer()->getRender();
            }

            // FXAA presents after the chain, so the chain leaves its finished image in
            // fxaaFramebuffer and only the anti-aliased result reaches the destination. With a
            // fixed resolution the upscale is the last stage instead, and it is what runs into
            // the AA target - so the chain keeps writing the fixed-size buffer.
            if (useFXAA && !useFixedRes){
                fxaaSource = destination;
                destination = &fxaaFramebuffer.getRender();
            }

            // the chain samples its input, so whatever runs before it writes the first
            // ping-pong buffer and the chain writes the real destination
            if (usePostProcess){
                postDestination = destination;
                ssrDestination = &postProcessFramebuffer[0].getRender();
            }else{
                ssrDestination = destination;
            }

            // the tone map ends the chain at the real destination, so the post-process
            // chain hands it the scene color instead of presenting it
            if (useHDR){
                if (usePostProcess){
                    postDestination = &sceneColorFramebuffer.getRender();
                }
                hdrDestination = destination;
            }

            if (useSSR){
                colorPassViewport = Rect(0, 0, (float)ssrWidth, (float)ssrHeight);
                colorTarget = &sceneColorFramebuffer;
            }else if (usePostProcess){
                colorPassViewport = Rect(0, 0, (float)postProcessWidth, (float)postProcessHeight);
                colorTarget = &postProcessFramebuffer[0];
            }else if (useFixedRes && useDecals){
                colorPassViewport = Rect(0, 0, (float)fixedResWidth, (float)fixedResHeight);
                colorTarget = &fixedResFramebuffer;
            }else{
                colorPassViewport = Rect(0, 0, (float)sceneColorFramebuffer.getWidth(), (float)sceneColorFramebuffer.getHeight());
                colorTarget = &sceneColorFramebuffer;
            }
            camera.render.startRenderPass(&colorTarget->getRender());
            camera.render.applyViewport(colorPassViewport);
        }else if (useFixedRes){
            // full offscreen target; letterbox/upscale happens in the blit pass
            colorPassViewport = Rect(0, 0, (float)fixedResWidth, (float)fixedResHeight);
            colorTarget = &fixedResFramebuffer;
            camera.render.startRenderPass(&fixedResFramebuffer.getRender());
        }else if (!camera.renderToTexture){
            colorPassViewport = Engine::getViewRect();
            if (Engine::getFramebuffer()){
                if (!Engine::getFramebuffer()->isCreated()){
                    Engine::getFramebuffer()->create();
                }
                colorTarget = Engine::getFramebuffer();
                camera.render.startRenderPass(&Engine::getFramebuffer()->getRender());
            }else if (useSceneTarget){
                // presented by a blit once the pass is done
                colorPassViewport = Rect(0, 0, (float)sceneColorFramebuffer.getWidth(), (float)sceneColorFramebuffer.getHeight());
                colorTarget = &sceneColorFramebuffer;
                camera.render.startRenderPass(&sceneColorFramebuffer.getRender());
            }else{
                camera.render.startRenderPass();
            }
            camera.render.applyViewport(colorPassViewport);
        }else{
            if (!camera.framebuffer->isCreated()){
                camera.framebuffer->create();
            }
            colorPassViewport = Rect(0, 0, (float)camera.framebuffer->getWidth(), (float)camera.framebuffer->getHeight());
            colorTarget = camera.framebuffer;
            camera.render.startRenderPass(&camera.framebuffer->getRender());
        }

        passResolution = Vector2(colorPassViewport.getWidth(), colorPassViewport.getHeight());


        //---------Draw opaque meshes and UI----------
        bool hasActiveScissor = false;

        // only the main camera has a culled instance view and detail levels
        const int instanceView = isMainCamera ? 0 : -1;
        const LodView* lodView = isMainCamera ? &mainLodView : nullptr;

        const bool depthPrepass = isMainCamera && camera.depthTest && scene->isDepthPrepassEnabled();

        // a 3D pass draws opaque meshes nearest first so early depth rejects what nearer
        // surfaces cover; a 2D pass keeps the transform order, which is its layering, and
        // only lists them for the depth prepass
        std::vector<OpaqueRenderData> opaqueRenders;
        if (distanceSort || depthPrepass){
            // Worst case is one entry per drawable, and without this the list reallocs and
            // copies itself ~10 times over in a 1000-entity scene.
            opaqueRenders.reserve(transforms->size());

            const ComponentId meshId = scene->getComponentId<MeshComponent>();
            const ComponentId cameraId = scene->getComponentId<CameraComponent>();

            for (int i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                Entity entity = transforms->getEntity(i);
                Signature signature = scene->getSignature(entity);

                if (signature.test(cameraId)){
                    continue;
                }

                // The signature bit and the array entry are kept together, so asking the array
                // for the mesh tests the bit and hands back the pointer in one lookup.
                MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
                if (!mesh || !signature.test(meshId)){
                    continue;
                }

                if (hasMultipleCameras || hasReflectionProbes){
                    updateMVP(i, transform, camera, cameraTransform);
                }

                MeshComponent& m = *mesh;
                if (!transform.visible || samplesCameraTarget(camera, m)){
                    continue;
                }

                InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
                TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
                if (terrain && terrainView != 0){
                    updateTerrain(*terrain, transform, camera, cameraTransform, terrainView);
                }
                TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);

                if (!m.transparent){
                    const float distance = m.worldAABB.isNull() ? 0.0f : m.worldAABB.squaredDistance(cameraTransform.worldPosition);
                    opaqueRenders.push_back({entity, mesh, instmesh, terrain, tilemap, &transform, distance});
                }else if (distanceSort){
                    auto& renders = isBehindWater(passWaters, m.worldAABB) ? behindWaterRenders : transparentRenders;
                    renders.push({TransparentRenderType::MESH, entity, mesh, nullptr, instmesh, terrain, tilemap, &transform, transform.distanceToCamera});
                }
            }

            if (distanceSort){
                std::sort(opaqueRenders.begin(), opaqueRenders.end(), [](const OpaqueRenderData& a, const OpaqueRenderData& b){
                    return a.distanceToCamera < b.distanceToCamera;
                });
            }
        }

        // the same draws, depth only, with the color pass's own MVP; a color fork
        // without a depth fork is left out since its vertices may not match
        if (depthPrepass){
            const PipelineType prepassPip = offscreenTarget ? PIP_ZPREPASS_RTT : PIP_ZPREPASS;
            const bool sceneColorFork = !scene->getDefaultMeshShader().empty();
            Matrix4 renderVP = camera.viewProjectionMatrix;
            if (isRenderingFlipped(camera)){
                renderVP = Matrix4::scaleMatrix(Vector3(1, -1, 1)) * renderVP;
            }
            for (const OpaqueRenderData& opaque : opaqueRenders){
                if ((sceneColorFork || !opaque.mesh->customShader.empty()) && opaque.mesh->customDepthShader.empty())
                    continue;
                vs_depth_t params = {opaque.transform->modelMatrix, renderVP, opaque.transform->modelViewProjectionMatrix};
                drawMeshDepth(*opaque.mesh, *opaque.transform, camera.effectiveFarClip, camera.frustumPlanes, params, opaque.instmesh, opaque.terrain, opaque.tilemap, true, prepassPip, instanceView, lodView);
            }
        }

        //---------Draw sky----------
        auto skys = scene->getComponentArray<SkyComponent>();
        if (skys->size() > 0){
            SkyComponent& sky = skys->getComponentFromIndex(0);
            Entity entity = skys->getEntity(0);
            if (sky.visible){
                if (hasMultipleCameras || hasReflectionProbes){
                    updateSkyViewProjection(sky, camera);
                }

                drawSky(sky, colorPip);
            }
        }

        if (distanceSort){
            for (const OpaqueRenderData& opaque : opaqueRenders){
                drawMesh(opaque.entity, *opaque.mesh, *opaque.transform, camera, cameraTransform, colorPip, opaque.instmesh, opaque.terrain, opaque.tilemap, terrainView, instanceView, lodView);
            }
        }

        // The id of each type this scan branches on, read once instead of once per entity:
        // every one of them is a guarded static plus a vector subscript.
        const ComponentId cidCamera = scene->getComponentId<CameraComponent>();
        const ComponentId cidMesh = scene->getComponentId<MeshComponent>();
        const ComponentId cidUILayout = scene->getComponentId<UILayoutComponent>();
        const ComponentId cidImage = scene->getComponentId<ImageComponent>();
        const ComponentId cidUI = scene->getComponentId<UIComponent>();
        const ComponentId cidText = scene->getComponentId<TextComponent>();
        const ComponentId cidPoints = scene->getComponentId<PointsComponent>();
        const ComponentId cidLines = scene->getComponentId<LinesComponent>();

        for (int i = 0; i < transforms->size(); i++){
            Transform& transform = transforms->getComponentFromIndex(i);
            Entity entity = transforms->getEntity(i);
            Signature signature = scene->getSignature(entity);

            if (signature.test(cidCamera)){
                continue;
            }

            if (distanceSort && signature.test(cidMesh)){
                continue; // drawn above
            }

            if (hasMultipleCameras || hasReflectionProbes){
                updateMVP(i, transform, camera, cameraTransform);
            }

            // apply scissor on UI
            if (signature.test(cidUILayout)){
                UILayoutComponent& layout = scene->getComponent<UILayoutComponent>(entity);

                Rect parentScissor;
                bool parentScissorActive = false;

                if (transform.parent != NULL_ENTITY){
                    Signature parentSignature = scene->getSignature(transform.parent);
                    if (parentSignature.test(cidUILayout)){
                        UILayoutComponent& parentLayout = scene->getComponent<UILayoutComponent>(transform.parent);

                        parentScissorActive = parentLayout.scissorActive;
                        parentScissor = parentLayout.scissor;
                    }
                }

                // recomputed every pass, so children never get a stale rect
                layout.scissorActive = false;

                bool inheritsScissor = parentScissorActive && !layout.ignoreScissor;
                bool clippedAway = false;

                if (inheritsScissor){
                    layout.scissor = parentScissor;
                    layout.scissorActive = true;

                    if (parentScissor.getWidth() <= 0 || parentScissor.getHeight() <= 0){
                        // sokol widens a scissor to 1x1 on Metal, Vulkan and WebGPU, so an
                        // empty rect would leak a pixel: skip the draw instead
                        clippedAway = true;
                    }else{
                        camera.render.applyScissor(parentScissor);

                        hasActiveScissor = true;
                    }
                }

                if (signature.test(cidImage)){
                    ImageComponent& img = scene->getComponent<ImageComponent>(entity);

                    layout.scissor = getScissorRect(layout, img, transform, colorPassViewport);
                    // applied or not: a clipped away element still passes the empty rect down
                    if (inheritsScissor){
                        layout.scissor = intersectScissor(layout.scissor, parentScissor);
                    }
                    layout.scissorActive = true;
                }

                if (clippedAway)
                    continue;
            }

            if (signature.test(cidMesh)){
                MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

                if (transform.visible && !samplesCameraTarget(camera, mesh)){

                    // Per-camera transparent-instance sorting is intentionally NOT done
                    // here: the instance buffer is shared across all cameras and sokol
                    // only allows one sg_update_buffer per buffer per frame
                    // (VALIDATE_UPDATEBUF_ONCE). Re-sorting per camera re-flags
                    // needUpdateBuffer and uploads the buffer again, which panics the
                    // validation layer when a render-to-texture camera is present. Every
                    // camera reuses the main camera's ordering computed in update().
                    InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);

                    // Per-view CDLOD: the main camera's selection (view 0) is done in
                    // update(); each render-to-texture camera (mirror/RTT) selects its
                    // own node cut here into its own buffer, so the reflection shows
                    // complete, correctly-tessellated terrain.
                    TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
                    if (terrain && terrainView != 0){
                        updateTerrain(*terrain, transform, camera, cameraTransform, terrainView);
                    }

                    // chunk bounds are view-independent, so every camera reuses the
                    // ones refreshed in update()
                    TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);

                    // a transparency on the eye's side of a water waits for it
                    if (mesh.transparent && !passWaters.empty() && !isBehindWater(passWaters, mesh.worldAABB)){
                        afterWaterRenders.push_back({TransparentRenderType::MESH, entity, &mesh, nullptr, instmesh, terrain, tilemap, &transform, transform.distanceToCamera});
                    }else{
                        drawMesh(entity, mesh, transform, camera, cameraTransform, colorPip, instmesh, terrain, tilemap, terrainView, instanceView, lodView);
                    }
                }

            }else if (signature.test(cidUI)){
                UIComponent& ui = scene->getComponent<UIComponent>(entity);

                bool isText = false;
                if (signature.test(cidText)){
                    isText = true;
                }
                if (transform.visible && !samplesCameraTarget(camera, ui.texture))
                    drawUI(ui, transform, colorPip);

            }else if (signature.test(cidPoints)){
                PointsComponent& points = scene->getComponent<PointsComponent>(entity);

                // Per-camera sorting is intentionally NOT done here: the points buffer is
                // shared across all cameras and sokol only allows one sg_update_buffer per
                // buffer per frame (VALIDATE_UPDATEBUF_ONCE). Every camera reuses the main
                // camera's ordering computed in update().
                if (transform.visible && !samplesCameraTarget(camera, points.texture)){
                    if (!points.transparent){
                        drawPoints(points, transform, camera, cameraTransform, colorPip);
                    }else{
                        const bool behindWater = !passWaters.empty() && isBehindWater(passWaters, transform.modelMatrix * points.aabb);
                        const TransparentRenderData renderData = {TransparentRenderType::POINTS, entity, nullptr, &points, nullptr, nullptr, nullptr, &transform, transform.distanceToCamera};
                        if (distanceSort){
                            auto& renders = behindWater ? behindWaterRenders : transparentRenders;
                            renders.push(renderData);
                        }else if (!behindWater && !passWaters.empty()){
                            afterWaterRenders.push_back(renderData);
                        }else{
                            drawPoints(points, transform, camera, cameraTransform, colorPip);
                        }
                    }
                }

            }else if (signature.test(cidLines)){
                LinesComponent& lines = scene->getComponent<LinesComponent>(entity);

                if (transform.visible)
                    drawLines(lines, transform, cameraTransform, colorPip);

            }

            if (hasActiveScissor){
                // screen pixels would overflow an offscreen SSR / post-process attachment
                camera.render.applyScissor(colorPassViewport);
                hasActiveScissor = false;
            }
        }

        auto drawTransparent = [&](const TransparentRenderData& renderData){
            if (renderData.type == TransparentRenderType::MESH){
                drawMesh(renderData.entity, *renderData.mesh, *renderData.transform, camera, cameraTransform, colorPip, renderData.instmesh, renderData.terrain, renderData.tilemap, terrainView, instanceView, lodView);
            }else if (renderData.type == TransparentRenderType::POINTS){
                drawPoints(*renderData.points, *renderData.transform, camera, cameraTransform, colorPip);
            }
        };

        // the water writes depth, so the transparencies it covers go first
        while (!behindWaterRenders.empty()){
            drawTransparent(behindWaterRenders.top());
            behindWaterRenders.pop();
        }

        // the scene copy: an SSR composite or the underwater fade replace the pass content,
        // a plain copy keeps it
        CameraRender* passRender = &camera.render;
        if (copyScene){
            // ended first: creating the copy framebuffer runs a clear pass, and passes can't nest
            camera.render.endRenderPass();
            const bool copied = ensureSceneCopyFramebuffer(colorTarget->getWidth(), colorTarget->getHeight());
            if (copied){
                if (useSSR){
                    renderSSR(camera, &sceneCopyFramebuffer.getRender());
                }else{
                    renderSceneCopy(colorTarget);
                }

                // view rects are centered, so the gap above the view matches the one below
                const float copyWidth = (float)colorTarget->getWidth();
                const float copyHeight = (float)colorTarget->getHeight();
                currentSceneCopy = &sceneCopyFramebuffer.getRender().getColorTexture();
                currentSceneCopyRect = Vector4(colorPassViewport.getX() / copyWidth, colorPassViewport.getY() / copyHeight,
                        colorPassViewport.getWidth() / copyWidth, colorPassViewport.getHeight() / copyHeight);
            }

            passRender = &resumePassRender;
            // kept for another copy between overlapping waters
            resumePassRender.setStoreDepth(passWaters.size() > 1);
            resumePassRender.startRenderPass(&colorTarget->getRender());
            resumePassRender.applyViewport(colorPassViewport);
            if (copied){
                if (currentUnderwater){
                    drawUnderwater(*currentUnderwater, camera, cameraTransform);
                }else if (useSSR){
                    drawBlit(&sceneCopyFramebuffer.getRender().getColorTexture(), PIP_RTT_NODEPTH, false);
                }
            }
        }

        //---------Draw water----------
        // farther first, so a refracting water sees the ones behind it in a new copy
        const float eyeY = cameraTransform.worldPosition.y;
        std::sort(passWaters.begin(), passWaters.end(), [eyeY](const WaterRenderData& a, const WaterRenderData& b){
            return std::abs(eyeY - a.transform->worldPosition.y) > std::abs(eyeY - b.transform->worldPosition.y);
        });
        bool copyOutdated = false;
        for (const WaterRenderData& waterData : passWaters){
            if (copyOutdated && waterData.water->refraction){
                resumePassRender.endRenderPass();
                renderSceneCopy(colorTarget);
                resumePassRender.startRenderPass(&colorTarget->getRender());
                resumePassRender.applyViewport(colorPassViewport);
                copyOutdated = false;
            }
            if (drawWater(waterData.entity, *waterData.water, *waterData.transform, camera, cameraTransform, colorPip, isMainCamera) && currentSceneCopy){
                copyOutdated = true;
            }
        }

        //---------Draw transparent renderers----------
        for (const TransparentRenderData& renderData : afterWaterRenders){
            drawTransparent(renderData);
        }
        while (!transparentRenders.empty()){
            drawTransparent(transparentRenders.top());
            transparentRenders.pop();
        }

        passRender->endRenderPass();

        if (useDecals && decalDepthTexture){
            const double started = Engine::getSystemTime();
            renderDecals(camera, colorTarget, decalDepthTexture);
            Engine::addSystemStats("decals", (Engine::getSystemTime() - started) * 1000.0, 2);
        }

        if (useSSGI && ssgiDepthTexture){
            const double started = Engine::getSystemTime();
            renderSSGI(camera, colorTarget, ssgiDepthTexture);
            Engine::addSystemStats("ssgi", (Engine::getSystemTime() - started) * 1000.0, 2);
        }

        // DOF: blur the offscreen scene color and put it back, so the stages below - the
        // scene-target blit, the post-process chain, the tone map, the upscale and FXAA -
        // all read the blurred image without knowing this ran.
        if (useDOF){
            const double started = Engine::getSystemTime();
            renderDOF(camera, colorTarget, dofDepthTexture);
            Engine::addSystemStats("dof", (Engine::getSystemTime() - started) * 1000.0, 2);
        }

        // Atmosphere: fog the whole frame along the view ray, then light shafts. It runs
        // with DOF because both are already finished, and before everything that follows
        // because its in-scatter has to be tone mapped with the scene it lights.
        if (useAtmosphere){
            const double started = Engine::getSystemTime();
            renderAtmosphere(camera, cameraTransform, colorTarget, atmosphereDepthTexture);
            Engine::addSystemStats("atmosphere", (Engine::getSystemTime() - started) * 1000.0, 2);
        }

        if (useContactShadows && contactDepthTexture){
            const double started = Engine::getSystemTime();
            renderContactShadows(camera, cameraTransform, colorTarget, contactDepthTexture);
            Engine::addSystemStats("contactShadows", (Engine::getSystemTime() - started) * 1000.0, 2);
        }

        if (useTAA && temporalDepthTexture){
            const double started = Engine::getSystemTime();
            renderTAA(camera, colorTarget, temporalDepthTexture);
            Engine::addSystemStats("taa", (Engine::getSystemTime() - started) * 1000.0, 2);
        }else if (useMotionBlur && temporalDepthTexture){
            // Motion blur still needs the velocity pass when TAA is disabled.
            renderVelocity(camera, temporalDepthTexture);
        }
        if (useMotionBlur && temporalDepthTexture){
            const double started = Engine::getSystemTime();
            renderMotionBlur(colorTarget, temporalDepthTexture);
            Engine::addSystemStats("motionBlur", (Engine::getSystemTime() - started) * 1000.0, 2);
        }

        // SSR: march the offscreen scene color and composite reflections into the
        // real destination (the swapchain or the captured framebuffer). After a scene
        // copy it was composited already, and only the finished pass is left to present.
        if (useSSR){
            if (currentSceneCopy){
                const Rect viewport = Engine::getViewRect();
                renderBlit(&sceneColorFramebuffer.getRender().getColorTexture(), ssrDestination, ssrDestination ? nullptr : &viewport);
            }else{
                renderSSR(camera, ssrDestination, useHDR && !usePostProcess);
            }
        }

        if (useSceneTarget && !useHDR){
            if (useFXAA){
                // FXAA reads the whole target, so the scene moves into it 1:1
                renderBlit(&sceneColorFramebuffer.getRender().getColorTexture(), &fxaaFramebuffer.getRender(), nullptr);
            }else{
                const Rect viewport = Engine::getViewRect();
                renderBlit(&sceneColorFramebuffer.getRender().getColorTexture(), nullptr, &viewport);
            }
        }

        if ((useTAA || useMotionBlur) && !useSceneTarget && !useSSR && !usePostProcess && !useHDR && !useFixedRes){
            FramebufferRender* temporalDestination = nullptr;
            if (camera.renderToTexture && camera.framebuffer)
                temporalDestination = &camera.framebuffer->getRender();
            else if (Engine::getFramebuffer())
                temporalDestination = &Engine::getFramebuffer()->getRender();
            if (useFXAA)
                temporalDestination = &fxaaFramebuffer.getRender();
            if (temporalDestination){
                renderBlit(&sceneColorFramebuffer.getRender().getColorTexture(), temporalDestination, nullptr);
            }else{
                const Rect viewport = Engine::getViewRect();
                renderBlit(&sceneColorFramebuffer.getRender().getColorTexture(), nullptr, &viewport);
            }
        }

        // buffer 0 now holds the scene color (or the SSR composite)
        if (usePostProcess){
            renderPostProcess(postDestination);
        }

        // bloom over the scene color, then the tone map to the real destination
        if (useHDR){
            renderHDR(hdrDestination);
        }

        // TAAU reconstructs the native view from the finished fixed-resolution image. It
        // deliberately runs after post-processing and tone mapping so the history stores the
        // exact image the player sees, while velocity and depth remain on the input grid.
        if (useTAAU && temporalDepthTexture){
            const double started = Engine::getSystemTime();
            Framebuffer* temporalSource = (usePostProcess || useHDR)
                ? &fixedResFramebuffer : &sceneColorFramebuffer;
            renderTAA(camera, temporalSource, temporalDepthTexture, false);
            Engine::addSystemStats("taau", (Engine::getSystemTime() - started) * 1000.0, 2);
        }

        // Fixed game resolution: upscale the offscreen scene color to the view
        // rect of the real destination. Layer scenes draw after this (load
        // action), so overlaid UI scenes stay at native resolution.
        if (useFixedRes){
            if (useTAAU && taaHistoryValid){
                FramebufferRender* taaDestination = nullptr;
                if (useFXAA){
                    taaDestination = &fxaaFramebuffer.getRender();
                }else if (Engine::getFramebuffer()){
                    if (!Engine::getFramebuffer()->isCreated())
                        Engine::getFramebuffer()->create();
                    taaDestination = &Engine::getFramebuffer()->getRender();
                }

                TextureRender* history =
                    &taaHistoryFramebuffers[taaHistoryIndex].getRender().getColorTexture();
                if (taaDestination){
                    renderBlit(history, taaDestination, nullptr);
                }else{
                    const Rect viewport = Engine::getViewRect();
                    renderBlit(history, nullptr, &viewport);
                }
            }else{
                renderFixedResolutionBlit(useFXAA ? &fxaaFramebuffer.getRender() : nullptr);
            }
        }

        // Anti-aliasing last: it has to see the image at the size it is presented at,
        // and its own output is what the viewer gets.
        if (useFXAA){
            renderFXAA(fxaaSource);
        }

        if (isMainCamera){
            previousViewProjection = camera.viewProjectionMatrix;
            previousViewProjectionValid = true;
        }

    }
    fs_lighting = mainLighting;

    // waves move after drawing, so Water::getHeight in the next update matches the next draw
    advanceWaters();

    //---------Missing some shaders----------
    if (ShaderPool::getMissingShaders().size() > 0){
        const std::string shaderSpecs = ShaderPool::getMissingShadersDisplayList();
        std::string shaderCliArgs = ShaderPool::getMissingShadersCliArgs();
        const std::string shaderOutputDir = System::instance().getShaderPath();
        const std::string shaderBackend = ShaderPool::getSuggestedCliBackend();

        if (shaderCliArgs.empty()) {
            shaderCliArgs = " --shader \"<shader-spec>\"";
        }

        const std::string shaderCommand = "quantum-editor shaders --out \"" + shaderOutputDir + "\" --backend " + shaderBackend + shaderCliArgs;

        Log::verbose(
            "\n"
            "-------------------\n"
            "Quantum is missing precompiled shaders in:\n"
            "%s\n"
            "\n"
            "Missing shaders:\n"
            "%s\n"
            "\n"
            "Bundle precompiled shaders for graphic backend: %s\n"
            "To generate them manually with Quantum Editor:\n"
            "\n"
            "> %s\n"
            "\n"
            "Current runtime shader format: %s\n"
            "-------------------"
            , shaderOutputDir.c_str(), shaderSpecs.c_str(), shaderBackend.c_str(), shaderCommand.c_str(), ShaderPool::getShaderLangStr().c_str());
        exit(1);
    }
}

void RenderSystem::onComponentAdded(Entity entity, ComponentId componentId) {
    if (componentId == scene->getComponentId<LightComponent>()) {
        needReloadMeshes();
    } else if (componentId == scene->getComponentId<Light2DComponent>()) {
        // shader variant change (USE_LIGHT2D)
        needReloadMeshes();
    } else if (componentId == scene->getComponentId<MirrorComponent>()) {
        // a mirror enables the inverted-culling pipeline on meshes; reload so it bakes
        needReloadMeshes();
    } else if (componentId == scene->getComponentId<ReflectionProbeComponent>()) {
        hasReflectionProbes = true;
        needReloadMeshes();
    }
}

void RenderSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
    if (componentId == scene->getComponentId<LightComponent>()) {
        LightComponent& light = scene->getComponent<LightComponent>(entity);
        destroyLight(light);
        needReloadMeshes();
    } else if (componentId == scene->getComponentId<Light2DComponent>()) {
        needReloadMeshes();
    } else if (componentId == scene->getComponentId<MeshComponent>()) {
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        destroyMesh(entity, mesh);
    } else if (componentId == scene->getComponentId<UIComponent>()) {
        UIComponent& ui = scene->getComponent<UIComponent>(entity);
        destroyUI(entity, ui);
    } else if (componentId == scene->getComponentId<PointsComponent>()) {
        PointsComponent& points = scene->getComponent<PointsComponent>(entity);
        destroyPoints(entity, points);
    } else if (componentId == scene->getComponentId<LinesComponent>()) {
        LinesComponent& lines = scene->getComponent<LinesComponent>(entity);
        destroyLines(entity, lines);
    } else if (componentId == scene->getComponentId<SkyComponent>()) {
        SkyComponent& sky = scene->getComponent<SkyComponent>(entity);
        destroySky(entity, sky);
    } else if (componentId == scene->getComponentId<CameraComponent>()) {
        CameraComponent& camera = scene->getComponent<CameraComponent>(entity);
        destroyCamera(camera, true);
    } else if (componentId == scene->getComponentId<MirrorComponent>()) {
        destroyMirrorCamera(entity);

        // reload meshes so the surface drops the projective-mirror shader variant and
        // other meshes drop the now-unused inverted-culling pipeline
        needReloadMeshes();
    } else if (componentId == scene->getComponentId<ReflectionProbeComponent>()) {
        ReflectionProbeComponent& probe = scene->getComponent<ReflectionProbeComponent>(entity);
        destroyReflectionProbe(entity, probe);
        hasReflectionProbes = scene->getComponentArray<ReflectionProbeComponent>()->size() > 1;
        needReloadMeshes();
    } else if (componentId == scene->getComponentId<WaterComponent>()) {
        WaterComponent& water = scene->getComponent<WaterComponent>(entity);
        destroyWater(entity, water);
        // a mirror on the same entity recreates its camera next frame
        destroyMirrorCamera(entity);
    }
}
