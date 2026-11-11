// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef SCENE_H
#define SCENE_H

#include "Entity.h"
#include "SubSystem.h"
#include "EntityRegistry.h"
#include "Export.h"
#include "SceneSettings.h"
#include "render/Render.h"
#include "math/Vector2.h"
#include "InputActionMap.h"
#include <string>
#include <vector>


namespace quantum{

    class Camera;

    // LightState and ShadowQuality live in SceneSettings.h (they are scene-setting types).

    enum class UIEventState {
        NOT_SET,
        ENABLED,
        DISABLED
    };

    class QUANTUM_API Scene : public EntityRegistry {
    private:

        Entity camera;
        Entity defaultCamera;
        Entity viewCamera;

        // All persistent scene configuration lives here (defaults from SceneSettings). The
        // public getters/setters below delegate to it, keeping the side effects (shader
        // reloads) and color-space conversions that can't live in a plain-data struct.
        // Gravity's live value stays in PhysicsSystem; settings.gravity* is only the default.
        SceneSettings settings;

        // Runtime-only render-distance cap, never serialized: the editor drives it from its own
        // viewport settings so a designer can preview a shorter view without touching the value
        // the game ships with. 0 = no cap.
        float renderDistanceOverride = 0.0f;

        UIEventState uiEventState;

        // One action map belongs to the scene so gameplay and Lua observe the same
        // edge-triggered state during a frame.
        InputActionMap inputActions;

        std::vector<std::pair<std::string, std::shared_ptr<SubSystem>>> systems;

        void init();
        Entity createDefaultCamera();

    protected:

        void onComponentAdded(Entity entity, ComponentId componentId) override {
            for (auto const& pair : systems) {
                pair.second->onComponentAdded(entity, componentId);
            }
        }

        void onComponentRemoved(Entity entity, ComponentId componentId) override {
            for (auto const& pair : systems) {
                pair.second->onComponentRemoved(entity, componentId);
            }
        }

        void onEntityDestroyed(Entity entity, Signature signature) override {
            if (entity == viewCamera) {
                viewCamera = NULL_ENTITY;
            }
            for (auto const& pair : systems) {
                for (ComponentId componentId = 0; componentId < signature.size(); ++componentId) {
                    if (signature.test(componentId)) {
                        pair.second->onComponentRemoved(entity, componentId);
                    }
                }
            }
        }

    public:

        Scene();
        Scene(EntityPool defaultPool);
        virtual ~Scene();

        void load();
        void destroy();
        void draw();
        void update(double dt);
        void fixedUpdate(double dt);

        InputActionMap& getInputActionMap() { return inputActions; }
        const InputActionMap& getInputActionMap() const { return inputActions; }

        void removeSubscriptionsByTag(const std::string& substring);

        // Stops every subsystem from ticking while the scene keeps receiving events:
        // the engine loop and the scripts still run, so a paused world can be watched
        // and stepped out of. Scripts use it through the scene.pause / scene.resume
        // graph nodes and the Lua equivalent.
        void setSystemsPaused(bool paused);

        void updateCameraSize();

        void setCamera(Camera* camera);
        void setCamera(Entity camera);
        Entity getCamera() const;

        // draws from this camera while getCamera() keeps the game one
        void setViewCamera(Entity camera);
        Entity getRenderCamera() const;

        void setBackgroundColor(Vector4 color);
        void setBackgroundColor(float red, float green, float blue);
        void setBackgroundColor(float red, float green, float blue, float alpha);
        Vector4 getBackgroundColor() const;

        void setShadowQuality(ShadowQuality quality);
        ShadowQuality getShadowQuality() const;

        void setMeshLodEnabled(bool enabled);
        bool isMeshLodEnabled() const;

        void setMeshLodThreshold(float pixels);
        float getMeshLodThreshold() const;

        void setDepthPrepassEnabled(bool enabled);
        bool isDepthPrepassEnabled() const;

        // How far the scene is drawn in metres; 0 = unlimited. The value caps the perspective
        // projection, so the frustum far plane that culls moves in with it.
        void setRenderDistance(float metres);
        float getRenderDistance() const;

        // Non-zero override that wins over the authored value for as long as it lasts (the
        // editor viewport). getEffectiveRenderDistance is what the renderer reads.
        void setRenderDistanceOverride(float metres);
        float getRenderDistanceOverride() const;
        float getEffectiveRenderDistance() const;

        // The air movement the water reads: metres per second, and an angle in the project's
        // unit like a Water's waveDirection. Wind::sample turns these into the wind blowing at
        // one point and instant.
        void setWindEnabled(bool enabled);
        bool isWindEnabled() const;

        void setWindSpeed(float metresPerSecond);
        float getWindSpeed() const;

        void setWindDirection(float angle);
        float getWindDirection() const;

        void setWindGustiness(float gustiness);
        float getWindGustiness() const;

        void setWindChange(float cyclesPerSecond);
        float getWindChange() const;

        void setSSAOEnabled(bool ssaoEnabled);
        bool isSSAOEnabled() const;

        void setSSAORadius(float radius);
        float getSSAORadius() const;

        void setSSAOIntensity(float intensity);
        float getSSAOIntensity() const;

        void setSSAOBias(float bias);
        float getSSAOBias() const;

        void setSSAODebug(bool debug);
        bool isSSAODebug() const;

        void setContactShadowsEnabled(bool enabled);
        bool isContactShadowsEnabled() const;

        void setContactShadowLength(float length);
        float getContactShadowLength() const;

        void setContactShadowSteps(int steps);
        int getContactShadowSteps() const;

        void setContactShadowThickness(float thickness);
        float getContactShadowThickness() const;

        void setContactShadowIntensity(float intensity);
        float getContactShadowIntensity() const;

        void setSSGIEnabled(bool enabled);
        bool isSSGIEnabled() const;

        void setSSGIRadius(float radius);
        float getSSGIRadius() const;

        void setSSGIIntensity(float intensity);
        float getSSGIIntensity() const;

        void setSSGISteps(int steps);
        int getSSGISteps() const;

        void setSSGIThickness(float thickness);
        float getSSGIThickness() const;

        void setDOFEnabled(bool dofEnabled);
        bool isDOFEnabled() const;

        void setDOFFocusDistance(float distance);
        float getDOFFocusDistance() const;

        void setDOFAperture(float aperture);
        float getDOFAperture() const;

        void setDOFFocusRange(float range);
        float getDOFFocusRange() const;

        void setDOFMaxBlur(float maxBlur);
        float getDOFMaxBlur() const;

        void setSSREnabled(bool ssrEnabled);
        bool isSSREnabled() const;

        void setSSRMaxDistance(float maxDistance);
        float getSSRMaxDistance() const;

        void setSSRThickness(float thickness);
        float getSSRThickness() const;

        void setSSRMaxSteps(int maxSteps);
        int getSSRMaxSteps() const;

        void setSSRIntensity(float intensity);
        float getSSRIntensity() const;

        void setSSRBlur(float blur);
        float getSSRBlur() const;

        void setSSRDebugMode(int mode);
        int getSSRDebugMode() const;

        void setTAAEnabled(bool enabled);
        bool isTAAEnabled() const;
        void setTAABlend(float blend);
        float getTAABlend() const;
        void setTAAVelocityScale(float scale);
        float getTAAVelocityScale() const;
        void setTAAVelocityClamp(float clamp);
        float getTAAVelocityClamp() const;
        void setMotionBlurEnabled(bool enabled);
        bool isMotionBlurEnabled() const;
        void setMotionBlurIntensity(float intensity);
        float getMotionBlurIntensity() const;
        void setMotionBlurSamples(int samples);
        int getMotionBlurSamples() const;
        void setMotionBlurVelocityClamp(float clamp);
        float getMotionBlurVelocityClamp() const;
        void setMotionBlurDepthThreshold(float threshold);
        float getMotionBlurDepthThreshold() const;

        void setFixedResolutionEnabled(bool fixedResolutionEnabled);
        bool isFixedResolutionEnabled() const;

        void setFixedResolutionWidth(unsigned int width);
        unsigned int getFixedResolutionWidth() const;

        void setFixedResolutionHeight(unsigned int height);
        unsigned int getFixedResolutionHeight() const;

        void setFixedResolutionSize(unsigned int width, unsigned int height);

        void setFixedResolutionFilter(TextureFilter filter);
        TextureFilter getFixedResolutionFilter() const;

        // HDR pipeline: these run the bloom / tone map chain. The chain itself works in
        // half float, but the scene target is still the 0..1 sRGB one until an HDR mesh
        // variant exists, so the passes decode on entry and the tone map re-encodes.
        void setBloomEnabled(bool enabled);
        bool isBloomEnabled() const;

        void setBloomThreshold(float threshold);
        float getBloomThreshold() const;

        void setBloomSoftKnee(float knee);
        float getBloomSoftKnee() const;

        void setBloomIntensity(float intensity);
        float getBloomIntensity() const;

        void setBloomDirtIntensity(float dirt);
        float getBloomDirtIntensity() const;

        void setBloomLevels(int levels);
        int getBloomLevels() const;

        void setTonemapEnabled(bool enabled);
        bool isTonemapEnabled() const;

        void setTonemapMode(int mode);
        int getTonemapMode() const;

        void setExposure(float exposure);
        float getExposure() const;

        // Eye adaptation. The EV pair is the range the meter may pick, in exposure values, so it
        // reaches the pass as 2^ev multipliers.
        void setAutoExposureEnabled(bool enabled);
        bool isAutoExposureEnabled() const;

        void setAutoExposureSpeed(float speed);
        float getAutoExposureSpeed() const;

        void setAutoExposureMinEV(float ev);
        float getAutoExposureMinEV() const;

        void setAutoExposureMaxEV(float ev);
        float getAutoExposureMaxEV() const;

        void setExposureCompensation(float ev);
        float getExposureCompensation() const;

        // Display-referred grading, applied by the tone map after its curve.
        void setColorGradingEnabled(bool enabled);
        bool isColorGradingEnabled() const;

        void setColorGradingSaturation(float saturation);
        float getColorGradingSaturation() const;

        void setColorGradingContrast(float contrast);
        float getColorGradingContrast() const;

        void setColorGradingBrightness(float brightness);
        float getColorGradingBrightness() const;

        void setColorGradingHueShift(float degrees);
        float getColorGradingHueShift() const;

        void setColorGradingTemperature(float temperature);
        float getColorGradingTemperature() const;

        void setColorGradingTint(float tint);
        float getColorGradingTint() const;

        // A strip LUT is uploaded as a texture2DArray: each square slice is one
        // blue coordinate, while red and green occupy the slice's XY plane.
        void setColorGradingLUTPath(const std::string& path);
        const std::string& getColorGradingLUTPath() const;

        // FXAA over the finished image. Independent of isHdrEnabled(): a scene with no bloom
        // and no tone map still wants its edges smoothed.
        void setFXAAEnabled(bool enabled);
        bool isFXAAEnabled() const;

        void setFXAAIntensity(float intensity);
        float getFXAAIntensity() const;

        // Whether the frame has to go through the HDR chain at all.
        bool isHdrEnabled() const;

        void setLightState(LightState state);
        LightState getLightState() const;

        void setGlobalIllumination(float intensity, Vector3 color);
        void setGlobalIllumination(float intensity);
        void setGlobalIllumination(Vector3 color);

        float getGlobalIlluminationIntensity() const;
        Vector3 getGlobalIlluminationColor() const;
        Vector3 getGlobalIlluminationColorLinear() const;

        void setAmbientLight2D(float intensity, Vector3 color);
        void setAmbientLight2D(float intensity);
        void setAmbientLight2D(Vector3 color);

        float getAmbientLight2DIntensity() const;
        Vector3 getAmbientLight2DColor() const;
        Vector3 getAmbientLight2DColorLinear() const;

        void setShadow2DQuality(ShadowQuality quality);
        ShadowQuality getShadow2DQuality() const;

        // convenience forwarders to PhysicsSystem gravity
        Vector2 getGravity2D() const;
        void setGravity2D(Vector2 gravity);
        void setGravity2D(float x, float y);

        Vector3 getGravity3D() const;
        void setGravity3D(Vector3 gravity);
        void setGravity3D(float x, float y, float z);

        void setDefaultMeshShader(const std::string& path);
        const std::string& getDefaultMeshShader() const;

        void setDefaultUIShader(const std::string& path);
        const std::string& getDefaultUIShader() const;

        void setDefaultSkyShader(const std::string& path);
        const std::string& getDefaultSkyShader() const;

        void setDefaultPointsShader(const std::string& path);
        const std::string& getDefaultPointsShader() const;

        void setDefaultLinesShader(const std::string& path);
        const std::string& getDefaultLinesShader() const;

        void setDefaultWaterShader(const std::string& path);
        const std::string& getDefaultWaterShader() const;

        void setDefaultCustomShader(ShaderType type, const std::string& path);
        const std::string& getDefaultCustomShader(ShaderType type) const;

        // custom block values of a default shader, under those of the components inheriting it
        void setDefaultShaderUniforms(ShaderType type, const ShaderUniformValues& values);
        const ShaderUniformValues& getDefaultShaderUniforms(ShaderType type) const;
        void setDefaultShaderUniform(ShaderType type, const std::string& name, const Vector4& value);
        void setDefaultShaderUniform(ShaderType type, const std::string& name, const Vector3& value);
        void setDefaultShaderUniform(ShaderType type, const std::string& name, const Vector2& value);
        void setDefaultShaderUniform(ShaderType type, const std::string& name, float value);
        Vector4 getDefaultShaderUniform(ShaderType type, const std::string& name) const;
        bool removeDefaultShaderUniform(ShaderType type, const std::string& name);
        // the values a component with this customShader inherits, null when it has its own fork
        const ShaderUniformValues* getInheritedShaderUniforms(ShaderType type, const std::string& customShader) const;

        // ordered user post-process chain; setting it rebuilds the RenderSystem passes
        // unless only uniform values changed
        const std::vector<PostProcessPass>& getPostProcessPasses() const;
        void setPostProcessPasses(const std::vector<PostProcessPass>& passes);

        // one u_fs_postParams member of a pass; rewrites the value without rebuilding the chain
        void setPostProcessUniform(unsigned int index, const std::string& name, const Vector4& value);
        void setPostProcessUniform(unsigned int index, const std::string& name, const Vector3& value);
        void setPostProcessUniform(unsigned int index, const std::string& name, const Vector2& value);
        void setPostProcessUniform(unsigned int index, const std::string& name, float value);
        Vector4 getPostProcessUniform(unsigned int index, const std::string& name) const;
        bool removePostProcessUniform(unsigned int index, const std::string& name);
        void setPostProcessPassEnabled(unsigned int index, bool enabled);
        bool isPostProcessPassEnabled(unsigned int index) const;

        bool canReceiveUIEvents();

        UIEventState getEnableUIEvents() const;
        void setEnableUIEvents(UIEventState enableUIEvents);
        void enableUIEvents();

        bool isEnableUIEvents() const;
        void setEnableUIEvents(bool enableUIEvents);

        // System methods

        template<typename T>
        std::shared_ptr<T> registerSystem(){
            const char* typeName = typeid(T).name();

            auto it = std::find_if(systems.begin(), systems.end(), [&](const auto& pair) { return pair.first == typeName; });

            // Scene bootstrap can be reached more than once by headless tools while a project is
            // being inspected and regenerated. Reuse the existing system instead of aborting the
            // process; systems are scene-owned singletons and duplicate registration cannot add
            // useful state.
            if (it != systems.end()) {
                return std::dynamic_pointer_cast<T>(it->second);
            }

            auto system = std::make_shared<T>(this);
            systems.push_back(std::make_pair(typeName, system));
            return system;
        }

        template<typename T>
        std::shared_ptr<T> getSystem(){
            const char* typeName = typeid(T).name();

            auto it = std::find_if(systems.begin(), systems.end(), [&](const auto& pair) { return pair.first == typeName; });

            assert(it != systems.end() && "System not found.");

            return std::dynamic_pointer_cast<T>(it->second);
        }
    };

}

#endif //SCENE_H
