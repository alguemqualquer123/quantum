// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include "Scene.h"
#include "SceneSettings.h"
#include "util/Color.h"

namespace quantum::editor{
    enum UpdateFlags : uint64_t {
        UpdateFlags_None                = 0,
        UpdateFlags_Transform           = 1 << 0,
        UpdateFlags_Camera              = 1 << 1,
        UpdateFlags_Scene_Mesh_Reload   = 1 << 2,
        UpdateFlags_Mesh_Reload         = 1 << 3,
        UpdateFlags_Mesh_Texture        = 1 << 4,
        UpdateFlags_UI_Reload           = 1 << 5,
        UpdateFlags_UI_Texture          = 1 << 6,
        UpdateFlags_Image_Patches       = 1 << 7,
        UpdateFlags_Layout_Sizes        = 1 << 8,
        UpdateFlags_Layout_Anchors      = 1 << 9,
        UpdateFlags_Sprite              = 1 << 10,
        UpdateFlags_LightShadowMap      = 1 << 11,
        UpdateFlags_LightShadowCamera   = 1 << 12,
        UpdateFlags_Sky_Texture         = 1 << 13,
        UpdateFlags_Sky                 = 1 << 14,
        UpdateFlags_Text                = 1 << 15,
        UpdateFlags_Text_Atlas          = 1 << 16,
        UpdateFlags_Body2D              = 1 << 17,
        UpdateFlags_Body3D              = 1 << 18,
        UpdateFlags_Joint2D             = 1 << 19,
        UpdateFlags_Joint3D             = 1 << 20,
        UpdateFlags_Model               = 1 << 21,
        UpdateFlags_Tilemap             = 1 << 22,
        UpdateFlags_Instanced_Mesh      = 1 << 23,
        UpdateFlags_Points              = 1 << 24,
        UpdateFlags_Terrain             = 1 << 25,
        UpdateFlags_Terrain_Texture     = 1 << 26,
        UpdateFlags_Sound               = 1 << 27,
        UpdateFlags_Lines               = 1 << 28,
        UpdateFlags_Lines_Reload        = 1 << 29,
        UpdateFlags_Points_Texture      = 1 << 30,
        // Generic "reload the renderable's shader" flag, used by the per-component
        // customShader property of Mesh/UI/Points/Lines/Sky.
        UpdateFlags_Shader_Reload       = (uint64_t)1 << 31,
        UpdateFlags_Reflection_Probe_Capture  = (uint64_t)1 << 32,
        UpdateFlags_Reflection_Probe_Sampling = (uint64_t)1 << 33,
        // for changes outside the probe itself, like a mesh leaving the capture pass
        UpdateFlags_Reflection_Probe_Recapture_All = (uint64_t)1 << 34,
        UpdateFlags_Terrain_Foliage     = (uint64_t)1 << 35,
        // shaderUniforms of Mesh/UI/Points/Lines/Sky/Water (no shader reload)
        UpdateFlags_Shader_Uniforms     = (uint64_t)1 << 36,
        // water grid size or subdivisions
        UpdateFlags_Water               = (uint64_t)1 << 37,
        UpdateFlags_Water_Texture       = (uint64_t)1 << 38,
        // handlers that touch the whole scene, must be masked out when applying all flags
        UpdateFlags_SceneWide = UpdateFlags_Scene_Mesh_Reload | UpdateFlags_Reflection_Probe_Recapture_All
    };

    // the order of components here affects properties window
    enum class ComponentType : int {
        Transform,
        ScriptComponent,
        MeshComponent,
        LinesComponent,
        LightComponent,
        TerrainComponent,
        PointsComponent,
        SkyComponent,
        ModelComponent,
        UIComponent,
        UILayoutComponent,
        ActionComponent,
        TimedActionComponent,
        AlphaActionComponent,
        AnimationComponent,
        SoundComponent,
        Body2DComponent,
        Body3DComponent,
        BoneComponent,
        BundleComponent,
        ButtonComponent,
        CameraComponent,
        ColorActionComponent,
        FogComponent,
        ImageComponent,
        InstancedMeshComponent,
        Joint2DComponent,
        Joint3DComponent,
        KeyframeTracksComponent,
        MeshPolygonComponent,
        MorphTracksComponent,
        PanelComponent,
        ParticlesComponent,
        PolygonComponent,
        PositionActionComponent,
        ProgressbarComponent,
        RotateTracksComponent,
        RotationActionComponent,
        ScaleActionComponent,
        ScaleTracksComponent,
        ScrollbarComponent,
        SpriteAnimationComponent,
        SpriteComponent,
        TextComponent,
        TextEditComponent,
        TilemapComponent,
        TranslateTracksComponent,
        UIContainerComponent,
        MirrorComponent,
        Light2DComponent,
        Occluder2DComponent,
        // Keep new component types at the end: their ordinal is used as the bit
        // position in EntityBundle's in-memory component override mask.
        ReflectionProbeComponent,
        WaterComponent,
        WaterExclusionComponent,
        AutoColliderComponent,
        AnimatorControllerComponent,
        NetworkComponent,
        DecalComponent
    };

    enum class PropertyType{
        Bool,
        String,
        Float,
        Double,
        Vector2,
        Vector3,
        Vector4,
        Quat,
        Int,
        UInt,
        Material,
        Texture,
        Font,
        Enum,
        Ease,
        Custom,
        Entity,
        EntityReference
    };

    struct PropertyData{
        PropertyType type;
        uint64_t updateFlags;
        void* def;
        void* ref;
    };

    class Catalog{
    private:

        static std::string removeComponentSuffix(std::string str);

    public:
        Catalog();

        static std::string getComponentName(ComponentType component, bool removeSuffix = false);
        static ComponentId getComponentId(const EntityRegistry* registry, ComponentType compType);
        static ComponentType getComponentType(const std::string& componentName);
        static Signature componentTypeToSignature(const EntityRegistry* registry, ComponentType compType);
        static Signature componentMaskToSignature(const EntityRegistry* registry, uint64_t mask);

        static PropertyType scriptPropertyTypeToPropertyType(ScriptPropertyType scriptType);

        static std::map<std::string, PropertyData> getProperties(ComponentType component, void* compRef);

        static std::vector<ComponentType> findComponents(EntityRegistry* registry, Entity entity);
        static std::map<std::string, PropertyData> findEntityProperties(EntityRegistry* registry, Entity entity, ComponentType component);

        static uint64_t getChangedUpdateFlags(ComponentType compType, void* oldComp, void* newComp);
        static uint64_t getComponentStructuralUpdateFlags(ComponentType compType);

        static void updateEntity(EntityRegistry* registry, Entity entity, uint64_t updateFlags);

        static void copyComponent(EntityRegistry* sourceRegistry, Entity sourceEntity,
                                EntityRegistry* targetRegistry, Entity targetEntity,
                                ComponentType compType);
        static void copyPropertyValue(EntityRegistry* sourceRegistry, Entity sourceEntity, 
                                    EntityRegistry* targetRegistry, Entity targetEntity, 
                                    ComponentType compType, const std::string& property);

        static PropertyData getProperty(EntityRegistry* registry, Entity entity, ComponentType component, std::string propertyName);

        // The same lookup without the not-found log, for callers a miss is expected on
        static PropertyData findProperty(EntityRegistry* registry, Entity entity, ComponentType component, std::string propertyName);

        // Submesh::overrideFields of a "submeshes[N].<field>" property, null for anything else,
        // with propertyFields set to the bits that property occupies. A command ORs them in after
        // writing the value, and puts the whole mask back on undo.
        static uint32_t* getSubmeshOverrideMask(EntityRegistry* registry, Entity entity, ComponentType component, const std::string& propertyName, uint32_t& propertyFields);

        // custom uniform blocks of a Mesh/UI/Points/Lines/Sky/Water component's loaded shader, vertex first
        static std::vector<const CustomUniformBlock*> getShaderUniformBlocks(EntityRegistry* registry, Entity entity, ComponentType component);
        // declarations the engine cannot upload right: mixed int/float blocks, one name with two types
        static std::vector<std::string> getShaderUniformWarnings(const std::vector<const CustomUniformBlock*>& blocks);
        // the component's custom shader (own or scene default) failed and the built-in is rendering
        static bool isCustomShaderBuildFailed(Scene* scene, Entity entity, ComponentType component);
        // same for a mesh's depth fork
        static bool isCustomDepthShaderBuildFailed(Scene* scene, Entity entity);
        static bool isCustomUnderwaterShaderBuildFailed(Scene* scene, Entity entity);
        // scene property of a default shader's uniform values ("default_mesh_shader_uniforms"...)
        static const char* getDefaultShaderUniformsProperty(ShaderType type);
        static bool getDefaultShaderUniformsType(const std::string& propertyName, ShaderType& type);

        template<typename T>
        static T* getPropertyRef(EntityRegistry* registry, Entity entity, ComponentType component, std::string propertyName){
            PropertyData property = Catalog::getProperty(registry, entity, component, propertyName);
            if (property.ref){
                return static_cast<T*>(property.ref);
            }

            return nullptr;
        }

        template<typename T>
        static T getSceneProperty(Scene* scene, const std::string& propertyName) {
            if (!scene) {
                // Return default values if scene is null
                if constexpr (std::is_same_v<T, Vector4>) return Vector4(0.0, 0.0, 0.0, 1.0);
                if constexpr (std::is_same_v<T, Vector3>) return Vector3(1.0, 1.0, 1.0);
                if constexpr (std::is_same_v<T, Vector2>) return Vector2(0.0, 0.0);
                if constexpr (std::is_same_v<T, bool>) return false;
                if constexpr (std::is_same_v<T, float>) return 0.0f;
                if constexpr (std::is_same_v<T, int>) return 0;
                if constexpr (std::is_same_v<T, LightState>) return LightState::AUTO;
                if constexpr (std::is_same_v<T, ShadowQuality>) return ShadowQuality::LOW;
                if constexpr (std::is_same_v<T, TextureFilter>) return TextureFilter::NEAREST;
                if constexpr (std::is_same_v<T, std::string>) return std::string();
                if constexpr (std::is_same_v<T, ShaderUniformValues>) return ShaderUniformValues();
                // Add other types as needed
            }

            if (propertyName == "background_color") {
                if constexpr (std::is_same_v<T, Vector4>) {
                    return scene->getBackgroundColor();
                }
            }
            else if (propertyName == "shadows_quality") {
                if constexpr (std::is_same_v<T, ShadowQuality>) {
                    return scene->getShadowQuality();
                }
            }
            else if (propertyName == "global_illumination_color") {
                if constexpr (std::is_same_v<T, Vector3>) {
                    return scene->getGlobalIlluminationColor();
                }
            }
            else if (propertyName == "global_illumination_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getGlobalIlluminationIntensity();
                }
            }
            else if (propertyName == "ambient_light_2d_color") {
                if constexpr (std::is_same_v<T, Vector3>) {
                    return scene->getAmbientLight2DColor();
                }
            }
            else if (propertyName == "ambient_light_2d_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getAmbientLight2DIntensity();
                }
            }
            else if (propertyName == "shadows_2d_quality") {
                if constexpr (std::is_same_v<T, ShadowQuality>) {
                    return scene->getShadow2DQuality();
                }
            }
            else if (propertyName == "physics_gravity_2d") {
                if constexpr (std::is_same_v<T, Vector2>) {
                    return scene->getGravity2D();
                }
            }
            else if (propertyName == "physics_gravity_3d") {
                if constexpr (std::is_same_v<T, Vector3>) {
                    return scene->getGravity3D();
                }
            }
            else if (propertyName == "light_state") {
                if constexpr (std::is_same_v<T, LightState>) {
                    return scene->getLightState();
                }
            }
            else if (propertyName == "mesh_lod_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isMeshLodEnabled();
                }
            }
            else if (propertyName == "mesh_lod_threshold") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getMeshLodThreshold();
                }
            }
            else if (propertyName == "depth_prepass_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isDepthPrepassEnabled();
                }
            }
            else if (propertyName == "render_distance") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getRenderDistance();
                }
            }
            else if (propertyName == "wind_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isWindEnabled();
                }
            }
            else if (propertyName == "wind_speed") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getWindSpeed();
                }
            }
            else if (propertyName == "wind_direction") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getWindDirection();
                }
            }
            else if (propertyName == "wind_gustiness") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getWindGustiness();
                }
            }
            else if (propertyName == "wind_change") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getWindChange();
                }
            }
            else if (propertyName == "ssao_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isSSAOEnabled();
                }
            }
            else if (propertyName == "ssao_radius") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSAORadius();
                }
            }
            else if (propertyName == "ssao_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSAOIntensity();
                }
            }
            else if (propertyName == "ssao_bias") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSAOBias();
                }
            }
            else if (propertyName == "ssao_debug") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isSSAODebug();
                }
            }
            else if (propertyName == "contact_shadows_enabled") {
                if constexpr (std::is_same_v<T, bool>) return scene->isContactShadowsEnabled();
            }
            else if (propertyName == "contact_shadow_length") {
                if constexpr (std::is_same_v<T, float>) return scene->getContactShadowLength();
            }
            else if (propertyName == "contact_shadow_steps") {
                if constexpr (std::is_same_v<T, int>) return scene->getContactShadowSteps();
            }
            else if (propertyName == "contact_shadow_thickness") {
                if constexpr (std::is_same_v<T, float>) return scene->getContactShadowThickness();
            }
            else if (propertyName == "contact_shadow_intensity") {
                if constexpr (std::is_same_v<T, float>) return scene->getContactShadowIntensity();
            }
            else if (propertyName == "ssgi_enabled") {
                if constexpr (std::is_same_v<T, bool>) return scene->isSSGIEnabled();
            }
            else if (propertyName == "ssgi_radius") {
                if constexpr (std::is_same_v<T, float>) return scene->getSSGIRadius();
            }
            else if (propertyName == "ssgi_intensity") {
                if constexpr (std::is_same_v<T, float>) return scene->getSSGIIntensity();
            }
            else if (propertyName == "ssgi_steps") {
                if constexpr (std::is_same_v<T, int>) return scene->getSSGISteps();
            }
            else if (propertyName == "ssgi_thickness") {
                if constexpr (std::is_same_v<T, float>) return scene->getSSGIThickness();
            }
            else if (propertyName == "dof_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isDOFEnabled();
                }
            }
            else if (propertyName == "dof_focus_distance") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getDOFFocusDistance();
                }
            }
            else if (propertyName == "dof_aperture") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getDOFAperture();
                }
            }
            else if (propertyName == "dof_focus_range") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getDOFFocusRange();
                }
            }
            else if (propertyName == "dof_max_blur") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getDOFMaxBlur();
                }
            }
            else if (propertyName == "ssr_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isSSREnabled();
                }
            }
            else if (propertyName == "ssr_max_distance") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSRMaxDistance();
                }
            }
            else if (propertyName == "ssr_thickness") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSRThickness();
                }
            }
            else if (propertyName == "ssr_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSRIntensity();
                }
            }
            else if (propertyName == "ssr_blur") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getSSRBlur();
                }
            }
            else if (propertyName == "ssr_max_steps") {
                if constexpr (std::is_same_v<T, int>) {
                    return scene->getSSRMaxSteps();
                }
            }
            else if (propertyName == "ssr_debug_mode") {
                if constexpr (std::is_same_v<T, int>) {
                    return scene->getSSRDebugMode();
                }
            }
            else if (propertyName == "taa_enabled") {
                if constexpr (std::is_same_v<T, bool>) return scene->isTAAEnabled();
            }
            else if (propertyName == "taa_blend") {
                if constexpr (std::is_same_v<T, float>) return scene->getTAABlend();
            }
            else if (propertyName == "taa_velocity_scale") {
                if constexpr (std::is_same_v<T, float>) return scene->getTAAVelocityScale();
            }
            else if (propertyName == "taa_velocity_clamp") {
                if constexpr (std::is_same_v<T, float>) return scene->getTAAVelocityClamp();
            }
            else if (propertyName == "motion_blur_enabled") {
                if constexpr (std::is_same_v<T, bool>) return scene->isMotionBlurEnabled();
            }
            else if (propertyName == "motion_blur_intensity") {
                if constexpr (std::is_same_v<T, float>) return scene->getMotionBlurIntensity();
            }
            else if (propertyName == "motion_blur_samples") {
                if constexpr (std::is_same_v<T, int>) return scene->getMotionBlurSamples();
            }
            else if (propertyName == "motion_blur_velocity_clamp") {
                if constexpr (std::is_same_v<T, float>) return scene->getMotionBlurVelocityClamp();
            }
            else if (propertyName == "motion_blur_depth_threshold") {
                if constexpr (std::is_same_v<T, float>) return scene->getMotionBlurDepthThreshold();
            }
            else if (propertyName == "fixed_resolution_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isFixedResolutionEnabled();
                }
            }
            else if (propertyName == "fixed_resolution_width") {
                if constexpr (std::is_same_v<T, int>) {
                    return (int)scene->getFixedResolutionWidth();
                }
            }
            else if (propertyName == "fixed_resolution_height") {
                if constexpr (std::is_same_v<T, int>) {
                    return (int)scene->getFixedResolutionHeight();
                }
            }
            else if (propertyName == "fixed_resolution_filter") {
                if constexpr (std::is_same_v<T, TextureFilter>) {
                    return scene->getFixedResolutionFilter();
                }
            }
            else if (propertyName == "bloom_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isBloomEnabled();
                }
            }
            else if (propertyName == "bloom_threshold") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getBloomThreshold();
                }
            }
            else if (propertyName == "bloom_soft_knee") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getBloomSoftKnee();
                }
            }
            else if (propertyName == "bloom_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getBloomIntensity();
                }
            }
            else if (propertyName == "bloom_dirt") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getBloomDirtIntensity();
                }
            }
            else if (propertyName == "bloom_mip_levels") {
                if constexpr (std::is_same_v<T, int>) {
                    return scene->getBloomLevels();
                }
            }
            else if (propertyName == "tonemap_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isTonemapEnabled();
                }
            }
            else if (propertyName == "tonemap_mode") {
                if constexpr (std::is_same_v<T, int>) {
                    return scene->getTonemapMode();
                }
            }
            else if (propertyName == "exposure") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getExposure();
                }
            }
            else if (propertyName == "auto_exposure") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isAutoExposureEnabled();
                }
            }
            else if (propertyName == "auto_exposure_speed") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getAutoExposureSpeed();
                }
            }
            else if (propertyName == "auto_exposure_min_ev") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getAutoExposureMinEV();
                }
            }
            else if (propertyName == "auto_exposure_max_ev") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getAutoExposureMaxEV();
                }
            }
            else if (propertyName == "exposure_compensation") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getExposureCompensation();
                }
            }
            else if (propertyName == "color_grading_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isColorGradingEnabled();
                }
            }
            else if (propertyName == "color_grading_saturation") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getColorGradingSaturation();
                }
            }
            else if (propertyName == "color_grading_contrast") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getColorGradingContrast();
                }
            }
            else if (propertyName == "color_grading_brightness") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getColorGradingBrightness();
                }
            }
            else if (propertyName == "color_grading_hue_shift") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getColorGradingHueShift();
                }
            }
            else if (propertyName == "color_grading_temperature") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getColorGradingTemperature();
                }
            }
            else if (propertyName == "color_grading_tint") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getColorGradingTint();
                }
            }
            else if (propertyName == "color_grading_lut") {
                if constexpr (std::is_same_v<T, std::string>) return scene->getColorGradingLUTPath();
            }
            else if (propertyName == "fxaa_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    return scene->isFXAAEnabled();
                }
            }
            else if (propertyName == "fxaa_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    return scene->getFXAAIntensity();
                }
            }
            else if (propertyName == "default_mesh_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    return scene->getDefaultMeshShader();
                }
            }
            else if (propertyName == "default_ui_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    return scene->getDefaultUIShader();
                }
            }
            else if (propertyName == "default_sky_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    return scene->getDefaultSkyShader();
                }
            }
            else if (propertyName == "default_points_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    return scene->getDefaultPointsShader();
                }
            }
            else if (propertyName == "default_lines_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    return scene->getDefaultLinesShader();
                }
            }
            else if (propertyName == "default_water_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    return scene->getDefaultWaterShader();
                }
            }
            else if (propertyName == "post_process") {
                if constexpr (std::is_same_v<T, std::vector<PostProcessPass>>) {
                    return scene->getPostProcessPasses();
                }
            }

            if constexpr (std::is_same_v<T, ShaderUniformValues>) {
                ShaderType type;
                if (getDefaultShaderUniformsType(propertyName, type))
                    return scene->getDefaultShaderUniforms(type);
            }

            // Return default value if property not found
            if constexpr (std::is_same_v<T, Vector4>) return Vector4(0.0, 0.0, 0.0, 1.0);
            if constexpr (std::is_same_v<T, Vector3>) return Vector3(1.0, 1.0, 1.0);
            if constexpr (std::is_same_v<T, Vector2>) return Vector2(0.0, 0.0);
            if constexpr (std::is_same_v<T, bool>) return false;
            if constexpr (std::is_same_v<T, float>) return 0.0f;
            if constexpr (std::is_same_v<T, int>) return 0;
            if constexpr (std::is_same_v<T, LightState>) return LightState::AUTO;
                if constexpr (std::is_same_v<T, ShadowQuality>) return ShadowQuality::LOW;
            if constexpr (std::is_same_v<T, TextureFilter>) return TextureFilter::NEAREST;
            if constexpr (std::is_same_v<T, std::string>) return std::string();
            if constexpr (std::is_same_v<T, std::vector<PostProcessPass>>) return std::vector<PostProcessPass>();
            if constexpr (std::is_same_v<T, ShaderUniformValues>) return ShaderUniformValues();
            // Add other types as needed
        }

        // Factory default for a scene property, used by the Properties panel's "reset to
        // default" arrow. Values come straight from a default-constructed SceneSettings — the
        // engine's single source of truth for scene defaults (Scene embeds one) — so they can
        // never drift from what a new scene gets. SceneSettings is plain data (no physics
        // runtime), so this is cheap; no caching needed. Values are returned in the same space
        // getSceneProperty exposes (GI/ambient colors are converted from stored linear to sRGB).
        template<typename T>
        static T getScenePropertyDefault(const std::string& propertyName) {
            static const SceneSettings d;

            if (propertyName == "background_color") {
                if constexpr (std::is_same_v<T, Vector4>) return d.backgroundColor;
            }
            else if (propertyName == "shadows_quality") {
                if constexpr (std::is_same_v<T, ShadowQuality>) return d.shadowQuality;
            }
            else if (propertyName == "global_illumination_color") {
                if constexpr (std::is_same_v<T, Vector3>) return Color::linearTosRGB(d.globalIllumColor);
            }
            else if (propertyName == "global_illumination_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.globalIllumIntensity;
            }
            else if (propertyName == "ambient_light_2d_color") {
                if constexpr (std::is_same_v<T, Vector3>) return Color::linearTosRGB(d.ambientLight2DColor);
            }
            else if (propertyName == "ambient_light_2d_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.ambientLight2DIntensity;
            }
            else if (propertyName == "shadows_2d_quality") {
                if constexpr (std::is_same_v<T, ShadowQuality>) return d.shadow2DQuality;
            }
            else if (propertyName == "physics_gravity_2d") {
                if constexpr (std::is_same_v<T, Vector2>) return d.gravity2D;
            }
            else if (propertyName == "physics_gravity_3d") {
                if constexpr (std::is_same_v<T, Vector3>) return d.gravity3D;
            }
            else if (propertyName == "light_state") {
                if constexpr (std::is_same_v<T, LightState>) return d.lightState;
            }
            else if (propertyName == "mesh_lod_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.meshLodEnabled;
            }
            else if (propertyName == "mesh_lod_threshold") {
                if constexpr (std::is_same_v<T, float>) return d.meshLodThreshold;
            }
            else if (propertyName == "depth_prepass_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.depthPrepassEnabled;
            }
            else if (propertyName == "render_distance") {
                if constexpr (std::is_same_v<T, float>) return d.renderDistance;
            }
            else if (propertyName == "wind_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.windEnabled;
            }
            else if (propertyName == "wind_speed") {
                if constexpr (std::is_same_v<T, float>) return d.windSpeed;
            }
            else if (propertyName == "wind_direction") {
                if constexpr (std::is_same_v<T, float>) return d.windDirection;
            }
            else if (propertyName == "wind_gustiness") {
                if constexpr (std::is_same_v<T, float>) return d.windGustiness;
            }
            else if (propertyName == "wind_change") {
                if constexpr (std::is_same_v<T, float>) return d.windChange;
            }
            else if (propertyName == "ssao_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.ssaoEnabled;
            }
            else if (propertyName == "ssao_radius") {
                if constexpr (std::is_same_v<T, float>) return d.ssaoRadius;
            }
            else if (propertyName == "ssao_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.ssaoIntensity;
            }
            else if (propertyName == "ssao_bias") {
                if constexpr (std::is_same_v<T, float>) return d.ssaoBias;
            }
            else if (propertyName == "ssao_debug") {
                if constexpr (std::is_same_v<T, bool>) return d.ssaoDebug;
            }
            else if (propertyName == "contact_shadows_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.contactShadowsEnabled;
            }
            else if (propertyName == "contact_shadow_length") {
                if constexpr (std::is_same_v<T, float>) return d.contactShadowLength;
            }
            else if (propertyName == "contact_shadow_steps") {
                if constexpr (std::is_same_v<T, int>) return d.contactShadowSteps;
            }
            else if (propertyName == "contact_shadow_thickness") {
                if constexpr (std::is_same_v<T, float>) return d.contactShadowThickness;
            }
            else if (propertyName == "contact_shadow_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.contactShadowIntensity;
            }
            else if (propertyName == "ssgi_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.ssgiEnabled;
            }
            else if (propertyName == "ssgi_radius") {
                if constexpr (std::is_same_v<T, float>) return d.ssgiRadius;
            }
            else if (propertyName == "ssgi_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.ssgiIntensity;
            }
            else if (propertyName == "ssgi_steps") {
                if constexpr (std::is_same_v<T, int>) return d.ssgiSteps;
            }
            else if (propertyName == "ssgi_thickness") {
                if constexpr (std::is_same_v<T, float>) return d.ssgiThickness;
            }
            else if (propertyName == "dof_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.dofEnabled;
            }
            else if (propertyName == "dof_focus_distance") {
                if constexpr (std::is_same_v<T, float>) return d.dofFocusDistance;
            }
            else if (propertyName == "dof_aperture") {
                if constexpr (std::is_same_v<T, float>) return d.dofAperture;
            }
            else if (propertyName == "dof_focus_range") {
                if constexpr (std::is_same_v<T, float>) return d.dofFocusRange;
            }
            else if (propertyName == "dof_max_blur") {
                if constexpr (std::is_same_v<T, float>) return d.dofMaxBlur;
            }
            else if (propertyName == "ssr_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.ssrEnabled;
            }
            else if (propertyName == "ssr_max_distance") {
                if constexpr (std::is_same_v<T, float>) return d.ssrMaxDistance;
            }
            else if (propertyName == "ssr_thickness") {
                if constexpr (std::is_same_v<T, float>) return d.ssrThickness;
            }
            else if (propertyName == "ssr_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.ssrIntensity;
            }
            else if (propertyName == "ssr_blur") {
                if constexpr (std::is_same_v<T, float>) return d.ssrBlur;
            }
            else if (propertyName == "ssr_max_steps") {
                if constexpr (std::is_same_v<T, int>) return d.ssrMaxSteps;
            }
            else if (propertyName == "ssr_debug_mode") {
                if constexpr (std::is_same_v<T, int>) return d.ssrDebugMode;
            }
            else if (propertyName == "taa_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.taaEnabled;
            }
            else if (propertyName == "taa_blend") {
                if constexpr (std::is_same_v<T, float>) return d.taaBlend;
            }
            else if (propertyName == "taa_velocity_scale") {
                if constexpr (std::is_same_v<T, float>) return d.taaVelocityScale;
            }
            else if (propertyName == "taa_velocity_clamp") {
                if constexpr (std::is_same_v<T, float>) return d.taaVelocityClamp;
            }
            else if (propertyName == "motion_blur_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.motionBlurEnabled;
            }
            else if (propertyName == "motion_blur_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.motionBlurIntensity;
            }
            else if (propertyName == "motion_blur_samples") {
                if constexpr (std::is_same_v<T, int>) return d.motionBlurSamples;
            }
            else if (propertyName == "motion_blur_velocity_clamp") {
                if constexpr (std::is_same_v<T, float>) return d.motionBlurVelocityClamp;
            }
            else if (propertyName == "motion_blur_depth_threshold") {
                if constexpr (std::is_same_v<T, float>) return d.motionBlurDepthThreshold;
            }
            else if (propertyName == "fixed_resolution_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.fixedResolutionEnabled;
            }
            else if (propertyName == "fixed_resolution_width") {
                if constexpr (std::is_same_v<T, int>) return (int)d.fixedResolutionWidth;
            }
            else if (propertyName == "fixed_resolution_height") {
                if constexpr (std::is_same_v<T, int>) return (int)d.fixedResolutionHeight;
            }
            else if (propertyName == "fixed_resolution_filter") {
                if constexpr (std::is_same_v<T, TextureFilter>) return d.fixedResolutionFilter;
            }
            else if (propertyName == "bloom_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.bloomEnabled;
            }
            else if (propertyName == "bloom_threshold") {
                if constexpr (std::is_same_v<T, float>) return d.bloomThreshold;
            }
            else if (propertyName == "bloom_soft_knee") {
                if constexpr (std::is_same_v<T, float>) return d.bloomSoftKnee;
            }
            else if (propertyName == "bloom_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.bloomIntensity;
            }
            else if (propertyName == "bloom_dirt") {
                if constexpr (std::is_same_v<T, float>) return d.bloomDirtIntensity;
            }
            else if (propertyName == "bloom_mip_levels") {
                if constexpr (std::is_same_v<T, int>) return d.bloomMipLevels;
            }
            else if (propertyName == "tonemap_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.tonemapEnabled;
            }
            else if (propertyName == "tonemap_mode") {
                if constexpr (std::is_same_v<T, int>) return d.tonemapMode;
            }
            else if (propertyName == "exposure") {
                if constexpr (std::is_same_v<T, float>) return d.exposure;
            }
            else if (propertyName == "auto_exposure") {
                if constexpr (std::is_same_v<T, bool>) return d.autoExposure;
            }
            else if (propertyName == "auto_exposure_speed") {
                if constexpr (std::is_same_v<T, float>) return d.autoExposureSpeed;
            }
            else if (propertyName == "auto_exposure_min_ev") {
                if constexpr (std::is_same_v<T, float>) return d.autoExposureMinEV;
            }
            else if (propertyName == "auto_exposure_max_ev") {
                if constexpr (std::is_same_v<T, float>) return d.autoExposureMaxEV;
            }
            else if (propertyName == "exposure_compensation") {
                if constexpr (std::is_same_v<T, float>) return d.exposureCompensation;
            }
            else if (propertyName == "color_grading_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.colorGradingEnabled;
            }
            else if (propertyName == "color_grading_saturation") {
                if constexpr (std::is_same_v<T, float>) return d.colorGradingSaturation;
            }
            else if (propertyName == "color_grading_contrast") {
                if constexpr (std::is_same_v<T, float>) return d.colorGradingContrast;
            }
            else if (propertyName == "color_grading_brightness") {
                if constexpr (std::is_same_v<T, float>) return d.colorGradingBrightness;
            }
            else if (propertyName == "color_grading_hue_shift") {
                if constexpr (std::is_same_v<T, float>) return d.colorGradingHueShift;
            }
            else if (propertyName == "color_grading_temperature") {
                if constexpr (std::is_same_v<T, float>) return d.colorGradingTemperature;
            }
            else if (propertyName == "color_grading_tint") {
                if constexpr (std::is_same_v<T, float>) return d.colorGradingTint;
            }
            else if (propertyName == "color_grading_lut") {
                if constexpr (std::is_same_v<T, std::string>) return d.colorGradingLUTPath;
            }
            else if (propertyName == "fxaa_enabled") {
                if constexpr (std::is_same_v<T, bool>) return d.fxaaEnabled;
            }
            else if (propertyName == "fxaa_intensity") {
                if constexpr (std::is_same_v<T, float>) return d.fxaaIntensity;
            }
            else if (propertyName == "default_mesh_shader") {
                if constexpr (std::is_same_v<T, std::string>) return d.defaultMeshShader;
            }
            else if (propertyName == "default_ui_shader") {
                if constexpr (std::is_same_v<T, std::string>) return d.defaultUIShader;
            }
            else if (propertyName == "default_sky_shader") {
                if constexpr (std::is_same_v<T, std::string>) return d.defaultSkyShader;
            }
            else if (propertyName == "default_points_shader") {
                if constexpr (std::is_same_v<T, std::string>) return d.defaultPointsShader;
            }
            else if (propertyName == "default_lines_shader") {
                if constexpr (std::is_same_v<T, std::string>) return d.defaultLinesShader;
            }
            else if (propertyName == "default_water_shader") {
                if constexpr (std::is_same_v<T, std::string>) return d.defaultWaterShader;
            }
            else if (propertyName == "post_process") {
                if constexpr (std::is_same_v<T, std::vector<PostProcessPass>>) return d.postProcess;
            }

            // Unknown property (or type mismatch): fall back to the type-generic default.
            return getSceneProperty<T>(nullptr, propertyName);
        }

        template<typename T>
        static void setSceneProperty(Scene* scene, const std::string& propertyName, const T& value) {
            if (!scene) return;

            if (propertyName == "background_color") {
                if constexpr (std::is_same_v<T, Vector4>) {
                    scene->setBackgroundColor(value);
                }
            }
            else if (propertyName == "shadows_quality") {
                if constexpr (std::is_same_v<T, ShadowQuality>) {
                    scene->setShadowQuality(value);
                }
            }
            else if (propertyName == "global_illumination_color") {
                if constexpr (std::is_same_v<T, Vector3>) {
                    scene->setGlobalIllumination(value);
                }
            }
            else if (propertyName == "global_illumination_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setGlobalIllumination(value);
                }
            }
            else if (propertyName == "ambient_light_2d_color") {
                if constexpr (std::is_same_v<T, Vector3>) {
                    scene->setAmbientLight2D(value);
                }
            }
            else if (propertyName == "ambient_light_2d_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setAmbientLight2D(value);
                }
            }
            else if (propertyName == "shadows_2d_quality") {
                if constexpr (std::is_same_v<T, ShadowQuality>) {
                    scene->setShadow2DQuality(value);
                }
            }
            else if (propertyName == "physics_gravity_2d") {
                if constexpr (std::is_same_v<T, Vector2>) {
                    scene->setGravity2D(value);
                }
            }
            else if (propertyName == "physics_gravity_3d") {
                if constexpr (std::is_same_v<T, Vector3>) {
                    scene->setGravity3D(value);
                }
            }
            else if (propertyName == "light_state") {
                if constexpr (std::is_same_v<T, LightState>) {
                    scene->setLightState(value);
                }
            }
            else if (propertyName == "mesh_lod_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setMeshLodEnabled(value);
                }
            }
            else if (propertyName == "mesh_lod_threshold") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setMeshLodThreshold(value);
                }
            }
            else if (propertyName == "depth_prepass_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setDepthPrepassEnabled(value);
                }
            }
            else if (propertyName == "render_distance") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setRenderDistance(value);
                }
            }
            else if (propertyName == "wind_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setWindEnabled(value);
                }
            }
            else if (propertyName == "wind_speed") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setWindSpeed(value);
                }
            }
            else if (propertyName == "wind_direction") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setWindDirection(value);
                }
            }
            else if (propertyName == "wind_gustiness") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setWindGustiness(value);
                }
            }
            else if (propertyName == "wind_change") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setWindChange(value);
                }
            }
            else if (propertyName == "ssao_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setSSAOEnabled(value);
                }
            }
            else if (propertyName == "ssao_radius") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSAORadius(value);
                }
            }
            else if (propertyName == "ssao_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSAOIntensity(value);
                }
            }
            else if (propertyName == "ssao_bias") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSAOBias(value);
                }
            }
            else if (propertyName == "ssao_debug") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setSSAODebug(value);
                }
            }
            else if (propertyName == "contact_shadows_enabled") {
                if constexpr (std::is_same_v<T, bool>) scene->setContactShadowsEnabled(value);
            }
            else if (propertyName == "contact_shadow_length") {
                if constexpr (std::is_same_v<T, float>) scene->setContactShadowLength(value);
            }
            else if (propertyName == "contact_shadow_steps") {
                if constexpr (std::is_same_v<T, int>) scene->setContactShadowSteps(value);
            }
            else if (propertyName == "contact_shadow_thickness") {
                if constexpr (std::is_same_v<T, float>) scene->setContactShadowThickness(value);
            }
            else if (propertyName == "contact_shadow_intensity") {
                if constexpr (std::is_same_v<T, float>) scene->setContactShadowIntensity(value);
            }
            else if (propertyName == "ssgi_enabled") {
                if constexpr (std::is_same_v<T, bool>) scene->setSSGIEnabled(value);
            }
            else if (propertyName == "ssgi_radius") {
                if constexpr (std::is_same_v<T, float>) scene->setSSGIRadius(value);
            }
            else if (propertyName == "ssgi_intensity") {
                if constexpr (std::is_same_v<T, float>) scene->setSSGIIntensity(value);
            }
            else if (propertyName == "ssgi_steps") {
                if constexpr (std::is_same_v<T, int>) scene->setSSGISteps(value);
            }
            else if (propertyName == "ssgi_thickness") {
                if constexpr (std::is_same_v<T, float>) scene->setSSGIThickness(value);
            }
            else if (propertyName == "dof_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setDOFEnabled(value);
                }
            }
            else if (propertyName == "dof_focus_distance") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setDOFFocusDistance(value);
                }
            }
            else if (propertyName == "dof_aperture") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setDOFAperture(value);
                }
            }
            else if (propertyName == "dof_focus_range") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setDOFFocusRange(value);
                }
            }
            else if (propertyName == "dof_max_blur") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setDOFMaxBlur(value);
                }
            }
            else if (propertyName == "ssr_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setSSREnabled(value);
                }
            }
            else if (propertyName == "ssr_max_distance") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSRMaxDistance(value);
                }
            }
            else if (propertyName == "ssr_thickness") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSRThickness(value);
                }
            }
            else if (propertyName == "ssr_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSRIntensity(value);
                }
            }
            else if (propertyName == "ssr_blur") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setSSRBlur(value);
                }
            }
            else if (propertyName == "ssr_max_steps") {
                if constexpr (std::is_same_v<T, int>) {
                    scene->setSSRMaxSteps(value);
                }
            }
            else if (propertyName == "ssr_debug_mode") {
                if constexpr (std::is_same_v<T, int>) {
                    scene->setSSRDebugMode(value);
                }
            }
            else if (propertyName == "taa_enabled") {
                if constexpr (std::is_same_v<T, bool>) scene->setTAAEnabled(value);
            }
            else if (propertyName == "taa_blend") {
                if constexpr (std::is_same_v<T, float>) scene->setTAABlend(value);
            }
            else if (propertyName == "taa_velocity_scale") {
                if constexpr (std::is_same_v<T, float>) scene->setTAAVelocityScale(value);
            }
            else if (propertyName == "taa_velocity_clamp") {
                if constexpr (std::is_same_v<T, float>) scene->setTAAVelocityClamp(value);
            }
            else if (propertyName == "motion_blur_enabled") {
                if constexpr (std::is_same_v<T, bool>) scene->setMotionBlurEnabled(value);
            }
            else if (propertyName == "motion_blur_intensity") {
                if constexpr (std::is_same_v<T, float>) scene->setMotionBlurIntensity(value);
            }
            else if (propertyName == "motion_blur_samples") {
                if constexpr (std::is_same_v<T, int>) scene->setMotionBlurSamples(value);
            }
            else if (propertyName == "motion_blur_velocity_clamp") {
                if constexpr (std::is_same_v<T, float>) scene->setMotionBlurVelocityClamp(value);
            }
            else if (propertyName == "motion_blur_depth_threshold") {
                if constexpr (std::is_same_v<T, float>) scene->setMotionBlurDepthThreshold(value);
            }
            else if (propertyName == "fixed_resolution_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setFixedResolutionEnabled(value);
                }
            }
            else if (propertyName == "fixed_resolution_width") {
                if constexpr (std::is_same_v<T, int>) {
                    scene->setFixedResolutionWidth((unsigned int)std::max(1, (int)value));
                }
            }
            else if (propertyName == "fixed_resolution_height") {
                if constexpr (std::is_same_v<T, int>) {
                    scene->setFixedResolutionHeight((unsigned int)std::max(1, (int)value));
                }
            }
            else if (propertyName == "fixed_resolution_filter") {
                if constexpr (std::is_same_v<T, TextureFilter>) {
                    scene->setFixedResolutionFilter(value);
                }
            }
            else if (propertyName == "bloom_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setBloomEnabled(value);
                }
            }
            else if (propertyName == "bloom_threshold") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setBloomThreshold(value);
                }
            }
            else if (propertyName == "bloom_soft_knee") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setBloomSoftKnee(value);
                }
            }
            else if (propertyName == "bloom_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setBloomIntensity(value);
                }
            }
            else if (propertyName == "bloom_dirt") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setBloomDirtIntensity(value);
                }
            }
            else if (propertyName == "bloom_mip_levels") {
                if constexpr (std::is_same_v<T, int>) {
                    scene->setBloomLevels(value);
                }
            }
            else if (propertyName == "tonemap_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setTonemapEnabled(value);
                }
            }
            else if (propertyName == "tonemap_mode") {
                if constexpr (std::is_same_v<T, int>) {
                    scene->setTonemapMode(value);
                }
            }
            else if (propertyName == "exposure") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setExposure(value);
                }
            }
            else if (propertyName == "auto_exposure") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setAutoExposureEnabled(value);
                }
            }
            else if (propertyName == "auto_exposure_speed") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setAutoExposureSpeed(value);
                }
            }
            else if (propertyName == "auto_exposure_min_ev") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setAutoExposureMinEV(value);
                }
            }
            else if (propertyName == "auto_exposure_max_ev") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setAutoExposureMaxEV(value);
                }
            }
            else if (propertyName == "exposure_compensation") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setExposureCompensation(value);
                }
            }
            else if (propertyName == "color_grading_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setColorGradingEnabled(value);
                }
            }
            else if (propertyName == "color_grading_saturation") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setColorGradingSaturation(value);
                }
            }
            else if (propertyName == "color_grading_contrast") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setColorGradingContrast(value);
                }
            }
            else if (propertyName == "color_grading_brightness") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setColorGradingBrightness(value);
                }
            }
            else if (propertyName == "color_grading_hue_shift") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setColorGradingHueShift(value);
                }
            }
            else if (propertyName == "color_grading_temperature") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setColorGradingTemperature(value);
                }
            }
            else if (propertyName == "color_grading_tint") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setColorGradingTint(value);
                }
            }
            else if (propertyName == "color_grading_lut") {
                if constexpr (std::is_same_v<T, std::string>) scene->setColorGradingLUTPath(value);
            }
            else if (propertyName == "fxaa_enabled") {
                if constexpr (std::is_same_v<T, bool>) {
                    scene->setFXAAEnabled(value);
                }
            }
            else if (propertyName == "fxaa_intensity") {
                if constexpr (std::is_same_v<T, float>) {
                    scene->setFXAAIntensity(value);
                }
            }
            else if (propertyName == "default_mesh_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    scene->setDefaultMeshShader(value);
                }
            }
            else if (propertyName == "default_ui_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    scene->setDefaultUIShader(value);
                }
            }
            else if (propertyName == "default_sky_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    scene->setDefaultSkyShader(value);
                }
            }
            else if (propertyName == "default_points_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    scene->setDefaultPointsShader(value);
                }
            }
            else if (propertyName == "default_lines_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    scene->setDefaultLinesShader(value);
                }
            }
            else if (propertyName == "default_water_shader") {
                if constexpr (std::is_same_v<T, std::string>) {
                    scene->setDefaultWaterShader(value);
                }
            }
            else if (propertyName == "post_process") {
                if constexpr (std::is_same_v<T, std::vector<PostProcessPass>>) {
                    scene->setPostProcessPasses(value);
                }
            }

            if constexpr (std::is_same_v<T, ShaderUniformValues>) {
                ShaderType type;
                if (getDefaultShaderUniformsType(propertyName, type))
                    scene->setDefaultShaderUniforms(type, value);
            }
        }
    };

}
