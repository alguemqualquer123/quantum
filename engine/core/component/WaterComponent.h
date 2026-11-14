// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_COMPONENT_H
#define WATER_COMPONENT_H

#include "buffer/InterleavedBuffer.h"
#include "buffer/IndexBuffer.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "render/ObjectRender.h"
#include "render/ShaderRender.h"
#include "render/TextureRender.h"
#include "shader/ShaderUniforms.h"
#include "texture/Texture.h"
#include "util/Color.h"
#include <memory>
#include <string>
#include <vector>

#define WATER_WAVE_COUNT 4
#define MAX_WATER_SUBDIVISIONS 512
// cells per axis of the simulation field: the CPU steps it every fixed update, so this
// stays far below MAX_WATER_SUBDIVISIONS, which only bounds the drawn grid
#define MAX_WATER_SIM 256
#define MIN_WATER_SIM 8
// the field is sampled with LINEAR filtering, so the drawn grid can be finer than it
#define WATER_DIRS 4

namespace quantum{

    // Water surface at the entity height, with Gerstner waves in world space so the
    // transform never stretches them
    struct QUANTUM_API WaterComponent{
        bool loaded = false;
        bool loadCalled = false;

        InterleavedBuffer buffer;
        IndexBuffer indices;
        unsigned int indexCount = 0;
        // size and cells the grid was built with
        Vector2 gridSize = Vector2(0.0f, 0.0f);
        unsigned int gridCells = 0;
        AABB aabb;
        AABB worldAABB;

        ObjectRender render;
        std::shared_ptr<ShaderRender> shader;
        uint32_t shaderProperties = 0;
        // optional user-forked shader: project-relative base path (no extension),
        // empty = built-in. customShaderId is the resolved id for ShaderPool keys.
        std::string customShader;
        uint16_t customShaderId = 0;
        int slotVSParams = -1;
        int slotVSSimParams = -1;
        int slotFSParams = -1;
        unsigned int fsParamsSize = 0; // as the shader declares it, shorter in forks of older versions
        int slotFSLighting = -1;
        int slotFSFog = -1;
        int slotVSShadows = -1;
        int slotFSShadows = -1;
        int slotFSShadowProjection = -1;
        int slotFSPointShadows = -1;
        // values of the fork's custom uniform blocks, by member name
        ShaderUniformValues shaderUniforms;
        bool needUpdateShaderUniforms = false;
        CustomUniformBlock customVSParams;
        CustomUniformBlock customFSParams;

        // the fullscreen fade seen from inside, loaded with the water while underwater is on
        bool underwaterLoaded = false;
        ObjectRender underwaterRender;
        std::shared_ptr<ShaderRender> underwaterShader;
        std::string customUnderwaterShader; // fork of underwater.frag, built-in when empty
        uint16_t customUnderwaterShaderId = 0;
        int slotUnderwaterParams = -1;
        CustomUniformBlock customUnderwaterParams;

        Vector2 size = Vector2(50.0f, 50.0f);
        unsigned int subdivisions = 128;

        Vector3 shallowColor = Color::sRGBToLinear(Vector3(0.20f, 0.62f, 0.62f)); // linear color
        Vector3 deepColor = Color::sRGBToLinear(Vector3(0.02f, 0.13f, 0.22f)); // linear color
        float depthFade = 3.0f; // depth where the deep color takes over

        float waveHeight = 0.2f;
        float waveLength = 6.0f; // of the longest wave
        float waveSpeed = 1.0f;
        float waveDirection = 0.0f; // around Y, 0 = +X
        float waveSteepness = 0.6f; // 0 = round, 1 = sharp crests

        // How much of this sea the scene's wind raises. 0 keeps the five numbers above exactly as
        // authored. It has no effect at all while the scene's wind is off, which is why its default
        // is 1: a scene that turns the wind on wants the water to answer it.
        float windInfluence = 1.0f;
        // Metres of open water the wind blows over before it reaches this water, which is what
        // decides how big a sea the same breeze can raise. 0 = the water's own extent along the
        // wind, so a canal ripples and an ocean swells without anyone doing arithmetic.
        float fetch = 0.0f;

        Texture normalTexture; // built-in ripples when empty
        float normalScale = 4.0f; // world size of one tile
        float normalStrength = 0.4f;
        float rippleSpeed = 0.3f;

        float reflectivity = 1.0f;
        float specularIntensity = 1.0f;
        float roughness = 0.06f;

        Vector3 foamColor = Vector3(1.0f, 1.0f, 1.0f); // linear color
        float shoreFoam = 0.4f; // depth covered by shore foam
        float crestFoam = 0.15f;

        bool receiveShadows = true;
        bool depthEffects = true; // shore foam and depth tint from the scene depth
        bool planarReflection = false;
        float reflectionDistortion = 0.04f;
        bool refraction = true; // the scene behind is seen through, bent and absorbed
        float refractionDistortion = 0.03f;
        float caustics = 0.5f; // sunlight patterns on the floor seen through, 0 = off
        float causticsScale = 2.0f; // world size of one pattern cell
        bool underwater = true; // a camera inside sees the scene fade into the water
        bool buoyancy = true; // floats the dynamic bodies that have Body3D buoyancy
        float buoyancyDepth = 0.0f; // how far below the surface they still float, 0 = no bottom

        // A depth per cell instead of one level plane: the water runs downhill, fills the
        // low spots, is raised by rain and dries out. Stepped by WaterSystem.
        bool simulate = false;
        unsigned int simResolution = 128; // cells per axis, MIN_WATER_SIM..MAX_WATER_SIM
        float simDepth = 0.3f; // starting depth over the floor, 0 = starts dry
        float simFlow = 0.45f; // how fast the volume moves sideways, 0 = still water
        float simRain = 0.0f; // metres of water added over the area each second
        float simEvaporation = 0.0f; // metres taken back each second, so a puddle the rain left dries out
        float simDryDepth = 0.002f; // under this the cell holds no water: no surface, no float
        bool simEdgeDrain = false; // water reaching the border leaves instead of pooling there
        bool simRipples = true; // rain drops and impacts disturb the surface
        float simRippleSpeed = 7.0f; // metres per second a ripple travels
        float simRippleDamping = 1.8f; // how fast a ripple dies out
        float simRippleStrength = 0.06f; // how far a rain drop pushes the surface down

        // The field, in entity-local space and owned by WaterSystem. Floor and depth are the
        // slow layer that moves the volume; the ripple is the fast layer that only disturbs
        // the surface; the flux is what each cell wall let through during the last step.
        std::vector<float> simFloor; // ground height under each cell
        std::vector<float> simWater; // metres of water standing in each cell
        std::vector<float> simRipple;
        std::vector<float> simRippleVel;
        std::vector<float> simFlux; // cell * WATER_DIRS
        std::vector<float> simVelocity; // cell * 2, flow speed along X and Z
        // one RGBA32F texel per cell: r = depth, g = floor, b = ripple, a = flow speed
        std::vector<float> simTexels;
        unsigned int simCells = 0;
        bool simFloorLoaded = false; // the ground under the area was sampled
        bool simDirty = true; // the field changed since the texels were uploaded
        float simLevel = 0.0f; // world Y of the entity the floor was baked against
        Vector2 simOrigin; // world XZ of the field's lower bound
        Vector2 simBaked; // world XZ of the field the floor was baked for
        Vector2 simCell = Vector2(1.0f, 1.0f); // world metres per cell, x along X and y along Z
        float simFloorMin = 0.0f; // field extremes, which grow the bounds
        float simFloorMax = 0.0f;
        float simWaterMax = 0.0f;
        TextureRender simTexture; // created and uploaded by RenderSystem
        unsigned int simTextureCells = 0; // cells the texture was made with, so a new resolution rebuilds it

        // advanced by RenderSystem::advanceWaters() at the end of each frame
        float wavePhase[WATER_WAVE_COUNT] = {0.0f, 0.0f, 0.0f, 0.0f};
        Vector2 rippleOffset[2] = {Vector2(0.0f, 0.0f), Vector2(0.0f, 0.0f)};
        float time = 0.0f;
        float pendingTime = 0.0f; // update time not applied yet

        // The sea the wind is raising here, and the wind raising it. State that is integrated, not a
        // multiplier read off the scene: a swell grown under a blow keeps rolling through the lull
        // and flattens slowly, which is the difference between weather and a blinking amplitude.
        // Never persisted - it is the weather of this run, not something an author drew.
        float seaHeight = 0.0f; // metres, the wind sea's own height before windInfluence mixes it in
        float seaLength = 0.0f; // metres, its crest-to-crest distance
        float seaAngle = 0.0f;  // radians around Y, downwind, the convention of waveDirection
        float seaWind = 0.0f;   // metres per second over the water, smoothed, what whitecaps read
        float seaMix = 0.0f;    // windInfluence, smoothed, and 0 while the wind is off

        Matrix4 reflectionViewProjection;
        std::shared_ptr<TextureRender> defaultNormalMap;

        bool needUpdateTexture = false;
        bool needReload = false;
    };

}

#endif //WATER_COMPONENT_H
