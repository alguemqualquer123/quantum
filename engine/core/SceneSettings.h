// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCENESETTINGS_H
#define SCENESETTINGS_H

#include "render/Render.h"
#include "shader/ShaderUniforms.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include <string>
#include <vector>

namespace doriax{

    enum class LightState {
        OFF,
        ON,
        AUTO
    };

    // PCF filter quality of shadow edges, uniform-driven (no shader rebuild).
    // 3D shadows (shadowQuality): NONE = 1 tap, LOW = 3x3, MEDIUM = 5x5, HIGH = 7x7.
    // 2D shadows (shadow2DQuality): NONE = 1 tap, LOW = 5, MEDIUM = 9, HIGH = 13 taps
    // along the 1D polar map, smoothing the penumbra set by Light2D shadowSoftness.
    enum class ShadowQuality {
        NONE,   // 1 tap, no filtering (hard edges)
        LOW,
        MEDIUM,
        HIGH
    };

    // One user post-process pass: a forked fullscreen shader run after the scene color.
    // Uniforms are keyed by the name reflected from the shader's u_fs_postParams block,
    // so a renamed or removed member just drops and an unset one stays zero.
    struct PostProcessPass {
        std::string shader; // project-relative fork base, empty = built-in passthrough
        bool enabled = true;
        ShaderUniformValues uniforms;

        // applied by Scene::setPostProcessPasses (Scene::setPostProcessUniform edits a live pass)
        void setUniform(const std::string& name, const Vector4& value) { ShaderUniforms::set(uniforms, name, value); }
        void setUniform(const std::string& name, const Vector3& value) { setUniform(name, Vector4(value.x, value.y, value.z, 0.0f)); }
        void setUniform(const std::string& name, const Vector2& value) { setUniform(name, Vector4(value.x, value.y, 0.0f, 0.0f)); }
        void setUniform(const std::string& name, float value) { setUniform(name, Vector4(value, 0.0f, 0.0f, 0.0f)); }
        Vector4 getUniform(const std::string& name) const { return ShaderUniforms::get(uniforms, name); }
        bool removeUniform(const std::string& name) { return ShaderUniforms::remove(uniforms, name); }
    };

    // Plain-data scene configuration whose member initializers are the engine's factory
    // defaults. This is the single source of truth for scene-setting defaults: Scene embeds
    // one and delegates its getters/setters to it (keeping side effects and color-space
    // conversions in Scene), and the editor reads a default-constructed SceneSettings to show
    // and reset "default" values without building a full Scene (which owns a physics runtime).
    struct SceneSettings {
        Vector4 backgroundColor = Vector4(0.0, 0.0, 0.0, 1.0); // sRGB
        ShadowQuality shadowQuality = ShadowQuality::LOW;

        // off builds no levels at all; a detail level is drawn once its error projects below this many pixels
        bool meshLodEnabled = false;
        float meshLodThreshold = 1.0f;

        // depth-only pass before the opaque draws; pays off with heavy overdraw
        bool depthPrepassEnabled = false;

        LightState lightState = LightState::AUTO;
        Vector3 globalIllumColor = Vector3(1.0, 1.0, 1.0);    // stored linear
        float globalIllumIntensity = 0.1f;

        // default 1.0 keeps 2D scenes visually unchanged until the user dims it
        Vector3 ambientLight2DColor = Vector3(1.0, 1.0, 1.0); // stored linear
        float ambientLight2DIntensity = 1.0f;
        ShadowQuality shadow2DQuality = ShadowQuality::LOW;

        bool ssaoEnabled = false;
        float ssaoRadius = 0.5f;
        float ssaoIntensity = 1.0f;
        float ssaoBias = 0.025f;
        bool ssaoDebug = false;

        bool ssrEnabled = false;
        float ssrMaxDistance = 8.0f;   // max ray length in view-space units
        float ssrThickness = 0.5f;     // depth-compare tolerance (view-space units)
        int ssrMaxSteps = 48;          // linear march sample count
        float ssrIntensity = 1.0f;     // reflection strength multiplier
        float ssrBlur = 0.0f;          // glossy blur amount [0..1] (0 = sharp/mirror)
        int ssrDebugMode = 0;          // 0=off,1=reflection,2=normal,3=roughness,4=metallic,5=albedo,6=IBL specular

        // HDR Bloom
        bool bloomEnabled = false;
        float bloomThreshold = 1.0f;     // luminance threshold for bright pixel extraction
        float bloomIntensity = 1.0f;     // bloom strength multiplier
        float bloomSoftKnee = 0.5f;      // soft knee for threshold (0=hard, 1=smooth)
        int bloomMipLevels = 5;          // number of downsample/upsample mip levels (1-5)
        float bloomDirtIntensity = 0.0f; // lens dirt intensity (0 = no dirt)

        // Tone mapping
        bool tonemapEnabled = false;
        int tonemapMode = 0;             // 0=ACES, 1=Reinhard, 2=Reinhard Extended, 3=Uncharted2, 4=AgX
        float exposure = 1.0f;           // manual exposure (when auto-exposure off)
        bool autoExposure = false;       // use luminance histogram for auto exposure
        float autoExposureSpeed = 1.0f;  // adaptation speed
        float autoExposureMinEV = -2.0f; // minimum exposure value
        float autoExposureMaxEV = 8.0f;  // maximum exposure value

        // Color Grading
        bool colorGradingEnabled = false;
        float colorGradingSaturation = 1.0f;  // 0=desaturated, 1=normal, 2=oversaturated
        float colorGradingContrast = 1.0f;    // 0=flat, 1=normal, 2=high contrast
        float colorGradingBrightness = 0.0f;  // -1=dark, 0=normal, 1=bright
        float colorGradingHueShift = 0.0f;    // hue rotation in degrees [-180, 180]
        float colorGradingTemperature = 0.0f; // -1=cool (blue), 0=neutral, 1=warm (orange)
        float colorGradingTint = 0.0f;        // -1=green, 0=neutral, 1=magenta
        std::string colorGradingLUTPath;      // path to 3D LUT texture (empty = use built-in adjustments)

        // Initial physics gravity. The live value is owned by PhysicsSystem (it must be pushed
        // to the Box2D/Jolt worlds on change); this is the default it initializes from.
        Vector2 gravity2D = Vector2(0.0f, -9.81f);
        Vector3 gravity3D = Vector3(0.0f, -9.81f, 0.0f);

        // fixed game resolution: when enabled and this scene is the Engine main scene, the main
        // camera renders into an offscreen framebuffer of this size, upscaled to the view rect
        // by RenderSystem
        bool fixedResolutionEnabled = false;
        unsigned int fixedResolutionWidth = 640;
        unsigned int fixedResolutionHeight = 360;
        TextureFilter fixedResolutionFilter = TextureFilter::NEAREST;

        // scene-wide custom shaders, used when a component's customShader is empty
        // (project-relative base or "a.vert|b.frag"; empty = engine built-in)
        std::string defaultMeshShader;
        std::string defaultUIShader;
        std::string defaultSkyShader;
        std::string defaultPointsShader;
        std::string defaultLinesShader;
        std::string defaultWaterShader;

        // custom block values of the default shaders, under the inheriting components' own
        ShaderUniformValues defaultMeshShaderUniforms;
        ShaderUniformValues defaultUIShaderUniforms;
        ShaderUniformValues defaultSkyShaderUniforms;
        ShaderUniformValues defaultPointsShaderUniforms;
        ShaderUniformValues defaultLinesShaderUniforms;
        ShaderUniformValues defaultWaterShaderUniforms;

        // ordered user post-process chain (main camera only), applied after SSR
        std::vector<PostProcessPass> postProcess;
    };

}

#endif //SCENESETTINGS_H
