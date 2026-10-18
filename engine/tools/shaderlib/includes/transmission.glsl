// Transmission BRDF implementation
// Based on KHR_materials_transmission extension
// https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_transmission

// Transmission factor calculation
// Returns the fraction of light that passes through the surface
float getTransmissionFactor(float transmissionFactor, vec2 transmissionUV, texture2D transmissionTex, sampler transmissionSmp){
    float transmission = transmissionFactor;

    #ifdef HAS_TRANSMISSION_TEXTURE
        transmission *= texture(sampler2D(transmissionTex, transmissionSmp), transmissionUV).r;
    #endif

    return clamp(transmission, 0.0, 1.0);
}

// Screen-space refraction
// Finds the refracted color by tracing through the depth buffer
vec3 getScreenSpaceRefraction(
    vec3 viewDir,
    vec3 normal,
    float ior,
    vec2 screenUV,
    vec2 texelSize,
    texture2D depthTex,
    sampler depthSmp,
    texture2D sceneColorTex,
    sampler sceneColorSmp
){
    // Calculate refraction vector
    float eta = 1.0 / ior;
    vec3 refractDir = refract(viewDir, normal, eta);

    // Project refraction to screen space
    vec2 offset = refractDir.xy * texelSize * 50.0; // Scale for visible effect

    // Trace along refraction direction in screen space
    vec2 sampleUV = screenUV + offset;

    // Clamp to screen bounds
    sampleUV = clamp(sampleUV, texelSize, 1.0 - texelSize);

    // Sample scene color at refracted position
    vec3 refractedColor = texture(sampler2D(sceneColorTex, sceneColorSmp), sampleUV).rgb;

    return refractedColor;
}

// Volume attenuation (for subsurface scattering approximation)
// Applies exponential absorption based on surface thickness
vec3 volumeAttenuation(
    float distance,
    vec3 attenuationColor,
    float attenuationDistance
){
    if (attenuationDistance <= 0.001){
        return vec3(1.0);
    }

    // Beer-Lambert law: I = I0 * exp(-attenuation * distance)
    vec3 attenuation = -log(attenuationColor) / attenuationDistance;
    return exp(-attenuation * distance);
}

// Combined transmission + volume attenuation
vec3 evaluateTransmission(
    float transmissionFactor,
    vec3 viewDir,
    vec3 normal,
    float ior,
    vec2 screenUV,
    vec2 texelSize,
    float surfaceThickness,
    vec3 attenuationColor,
    float attenuationDistance,
    texture2D depthTex,
    sampler depthSmp,
    texture2D sceneColorTex,
    sampler sceneColorSmp
){
    float transmission = transmissionFactor;
    if (transmission <= 0.001){
        return vec3(0.0);
    }

    // Get screen-space refraction
    vec3 refractedColor = getScreenSpaceRefraction(
        viewDir, normal, ior, screenUV, texelSize,
        depthTex, depthSmp, sceneColorTex, sceneColorSmp
    );

    // Apply volume attenuation
    vec3 attenuation = volumeAttenuation(surfaceThickness, attenuationColor, attenuationDistance);

    return refractedColor * attenuation * transmission;
}
