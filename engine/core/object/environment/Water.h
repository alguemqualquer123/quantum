// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_H
#define WATER_H

#include "object/Object.h"
#include "component/WaterComponent.h"

namespace quantum{

    class QUANTUM_API Water: public Object{

    public:
        Water(Scene* scene);
        Water(Scene* scene, Entity entity);
        virtual ~Water();

        bool load();

        void setSize(Vector2 size);
        void setSize(const float width, const float depth);
        Vector2 getSize() const;

        void setSubdivisions(unsigned int subdivisions);
        unsigned int getSubdivisions() const;

        void setShallowColor(Vector3 color);
        void setShallowColor(const float r, const float g, const float b);
        Vector3 getShallowColor() const;

        void setDeepColor(Vector3 color);
        void setDeepColor(const float r, const float g, const float b);
        Vector3 getDeepColor() const;

        void setDepthFade(float depthFade);
        float getDepthFade() const;

        void setWaveHeight(float waveHeight);
        float getWaveHeight() const;

        void setWaveLength(float waveLength);
        float getWaveLength() const;

        void setWaveSpeed(float waveSpeed);
        float getWaveSpeed() const;

        void setWaveDirection(float waveDirection);
        float getWaveDirection() const;

        void setWaveSteepness(float waveSteepness);
        float getWaveSteepness() const;

        void setWindInfluence(float windInfluence);
        float getWindInfluence() const;

        // metres of open water the wind blows over before it reaches this one; 0 is its own extent
        void setFetch(float fetch);
        float getFetch() const;

        // What the scene's wind is raising here right now, in metres, metres and metres per second.
        // Read-only and never authored: it is weather, and a script that wants to know whether the
        // lake has finished building its swell asks these instead of guessing from the wind speed.
        float getSeaHeight() const;
        float getSeaLength() const;
        float getSeaWind() const;

        // an empty path restores the built-in ripples
        void setNormalTexture(const std::string& path);

        void setNormalScale(float normalScale);
        float getNormalScale() const;

        void setNormalStrength(float normalStrength);
        float getNormalStrength() const;

        void setRippleSpeed(float rippleSpeed);
        float getRippleSpeed() const;

        void setReflectivity(float reflectivity);
        float getReflectivity() const;

        void setSpecularIntensity(float specularIntensity);
        float getSpecularIntensity() const;

        void setRoughness(float roughness);
        float getRoughness() const;

        void setFoamColor(Vector3 color);
        void setFoamColor(const float r, const float g, const float b);
        Vector3 getFoamColor() const;

        void setShoreFoam(float shoreFoam);
        float getShoreFoam() const;

        void setCrestFoam(float crestFoam);
        float getCrestFoam() const;

        void setReceiveShadows(bool receiveShadows);
        bool isReceiveShadows() const;

        void setDepthEffects(bool depthEffects);
        bool isDepthEffects() const;

        void setPlanarReflection(bool planarReflection);
        bool isPlanarReflection() const;

        void setReflectionDistortion(float reflectionDistortion);
        float getReflectionDistortion() const;

        void setRefraction(bool refraction);
        bool isRefraction() const;

        void setRefractionDistortion(float refractionDistortion);
        float getRefractionDistortion() const;

        // needs refraction and depth effects
        void setCaustics(float caustics);
        float getCaustics() const;

        void setCausticsScale(float causticsScale);
        float getCausticsScale() const;

        void setUnderwater(bool underwater);
        bool isUnderwater() const;

        void setBuoyancy(bool buoyancy);
        bool isBuoyancy() const;

        void setBuoyancyDepth(float buoyancyDepth);
        float getBuoyancyDepth() const;

        // The water keeps a depth per cell of a grid over its area and runs it downhill, so
        // rain collects in the low points and spills out of them. Turning it on or resizing
        // the grid reloads the water.
        void setSimulate(bool simulate);
        bool isSimulated() const;

        void setSimResolution(unsigned int cells);
        unsigned int getSimResolution() const;

        // Depth every cell starts with; 0 leaves the area dry for the rain to fill
        void setSimDepth(float simDepth);
        float getSimDepth() const;

        // How fast volume crosses the cell walls; 0 keeps the water where it is
        void setSimFlow(float simFlow);
        float getSimFlow() const;

        // Metres added over the area each second, and metres taken back by the air
        void setSimRain(float simRain);
        float getSimRain() const;

        void setSimEvaporation(float simEvaporation);
        float getSimEvaporation() const;

        // Under this the cell holds no water: no surface drawn, nothing afloat
        void setSimDryDepth(float simDryDepth);
        float getSimDryDepth() const;

        // Water reaching the border leaves the field instead of pooling against it
        void setSimEdgeDrain(bool simEdgeDrain);
        bool isSimEdgeDrain() const;

        void setSimRipples(bool simRipples);
        bool isSimRipples() const;

        void setSimRippleSpeed(float simRippleSpeed);
        float getSimRippleSpeed() const;

        void setSimRippleDamping(float simRippleDamping);
        float getSimRippleDamping() const;

        void setSimRippleStrength(float simRippleStrength);
        float getSimRippleStrength() const;

        void setCustomShader(const std::string& path);
        std::string getCustomShader() const;

        void setCustomUnderwaterShader(const std::string& path);
        std::string getCustomUnderwaterShader() const;

        void setShaderUniform(const std::string& name, const Vector4& value);
        void setShaderUniform(const std::string& name, const Vector3& value);
        void setShaderUniform(const std::string& name, const Vector2& value);
        void setShaderUniform(const std::string& name, float value);
        Vector4 getShaderUniform(const std::string& name) const;
        bool removeShaderUniform(const std::string& name);

        // surface at the world point (x, z), waves included
        float getHeight(float x, float z) const;
        Vector3 getNormal(float x, float z) const;

        // Depth of the simulated column at the world point (x, z); 0 where the field is dry
        // or outside the area, which is what keeps a body from floating over an empty puddle
        float getSimWaterDepth(float x, float z) const;

        // Metres per second the water flows at the point, for a body the current carries
        Vector3 getSimVelocity(float x, float z) const;

        // Pushes the surface down in a disc: a body hitting the water, a footstep, a bucket
        // tipping over. The crater is what rings out over the field.
        void addDisturbance(const Vector3& worldPosition, float radius, float strength);

        // Pours cubicMetres into this field at the world point, and takes up to cubicMetres back
        // out of it, returning how much it actually found. The heightfield cannot fall, so these
        // are the two ends of a waterfall: drain the shelf above the lip, fill the pool below it.
        void addWater(const Vector3& worldPosition, float radius, float cubicMetres);
        float drainWater(const Vector3& worldPosition, float radius, float cubicMetres);

        // Litres standing in the field, constant between rain and evaporation
        double getSimVolume() const;
    };
}

#endif //WATER_H
