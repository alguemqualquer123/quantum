// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef FOG_COMPONENT_H
#define FOG_COMPONENT_H

#include "util/Color.h"
#include "math/Vector2.h"

namespace quantum{

    enum class FogType{
        LINEAR,
		EXPONENTIAL,
		EXPONENTIALSQUARED
    };

    struct QUANTUM_API FogComponent{
        FogType type = FogType::LINEAR;
		Vector3 color = Color::sRGBToLinear(Vector3(0.8f, 0.8f, 0.8f)); // linear color
		float density = 0.01f; // for exponential fog
		float linearStart = 10;
		float linearEnd = 60;

        // Height fog and light shafts are not this per-pixel fog: they run as one
        // fullscreen pass over the HDR scene color, so they fog everything the same way
        // - the skybox's neighbours included - and their in-scatter can carry the sun.
        // Both default off so a scene that only wants the classic fog draws exactly what
        // it drew before.
        bool heightFog = false;
        float fogHeightFalloff = 8.0f;   // scale height: density falls by 1/e every this many metres
        float fogGroundHeight = 0.0f;    // world Y the height is measured from
        float fogSunAmount = 0.65f;      // in-scatter lit by the sun, against the flat ambient colour
        float fogFarRange = 1200.0f;     // the analytic integral saturates here, so distant haze stops deepening
        float fogScatteringG = 0.6f;     // Henyey-Greenstein anisotropy: 0 is even, +1 scatters only forward
        Vector3 fogSunColor = Color::sRGBToLinear(Vector3(1.0f, 0.86f, 0.66f));
        bool fogNoiseEnabled = false;
        float fogNoiseScale = 0.008f;
        float fogNoiseStrength = 0.25f;
        float fogNoiseSpeed = 0.2f;
        Vector2 fogNoiseWind = Vector2(1.0f, 0.2f);

        // Optional ray-marched aerial perspective. The analytic integral remains the
        // default for legacy scenes; this mode trades a bounded number of samples for
        // denser near-ground light scattering and temporal dithering.
        bool volumetricFog = false;
        unsigned int volumetricFogSteps = 32;
        float volumetricFogIntensity = 1.0f;

        bool godRays = false;
        float rayIntensity = 0.85f;
        float rayThreshold = 1.0f;   // bright-pass cut in linear HDR; 1.0 is the sun disk
        float raySoftness = 1.5f;    // metres of depth an occluder edge feathers over
        float rayDecay = 0.955f;     // per-sample extinction along the march toward the sun
        float rayDensity = 0.65f;    // fraction of the pixel->sun ray that is marched

        // Procedural cloud volume. The atmosphere pass ray-marches this layer between the
        // camera and the reconstructed scene depth; it is deliberately opt-in so legacy
        // scenes keep the exact analytic fog cost and appearance.
        bool cloudsEnabled = false;
        float cloudBaseHeight = 120.0f;
        float cloudThickness = 90.0f;
        float cloudCoverage = 0.45f;
        float cloudDensity = 1.0f;
        float cloudScale = 0.004f;
        float cloudSpeed = 4.0f;
        float cloudSunAmount = 1.0f;
        Vector2 cloudWind = Vector2(1.0f, 0.25f);
        Vector3 cloudColor = Color::sRGBToLinear(Vector3(0.86f, 0.9f, 1.0f));
    };
    
}

#endif //FOG_COMPONENT_H
