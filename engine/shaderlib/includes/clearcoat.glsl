// Clearcoat BRDF implementation
// Based on KHR_materials_clearcoat extension
// https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_clearcoat

// Clearcoat normal mapping
NormalInfo getClearcoatNormalInfo(NormalInfo baseNormalInfo, vec3 clearcoatNormalTS){
    NormalInfo info = baseNormalInfo;

    // Apply clearcoat normal map if present
    #ifdef HAS_CLEARCOAT_NORMAL_MAP
        vec3 clearcoatNormal = clearcoatNormalTS * 2.0 - vec3(1.0);
        clearcoatNormal = normalize(mat3(info.t, info.b, info.ng) * clearcoatNormal);
        info.n = normalize(mix(info.n, clearcoatNormal, 0.5));
    #endif

    return info;
}

// Clearcoat Fresnel (using IOR 1.5 = 0.04 at normal incidence)
vec3 clearcoatFresnel(float clearcoatThickness, float VdotH){
    // Clearcoat uses a fixed IOR of 1.5 (f0 = 0.04)
    float f0 = 0.04;
    float f90 = 1.0;
    float clearcoatFresnel = f0 + (f90 - f0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
    return vec3(clearcoatFresnel * clearcoatThickness);
}

// Clearcoat BRDF evaluation
// Returns: vec4(clearcoatContribution, clearcoatRoughness, unused, unused)
vec4 evaluateClearcoat(
    float clearcoatFactor,
    float clearcoatRoughness,
    float clearcoatF0,
    float NdotH,
    float NdotL,
    float NdotV,
    float VdotH
){
    if (clearcoatFactor <= 0.001){
        return vec4(0.0);
    }

    float alphaRoughness = clearcoatRoughness * clearcoatRoughness;

    // GGX distribution for clearcoat
    float D = D_GGX(NdotH, alphaRoughness);

    // Smith visibility (no fresnel, clearcoat has fixed f0)
    float Vis = V_GGX(NdotL, NdotV, alphaRoughness);

    // Fresnel (fixed f0 = 0.04 for dielectric)
    float F = clearcoatF0 + (1.0 - clearcoatF0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);

    // Clearcoat contribution
    float clearcoat = clearcoatFactor * D * Vis * F;

    return vec4(clearcoat, 0.0, 0.0, 0.0);
}

// Energy conservation for clearcoat
// The base layer loses energy when clearcoat is applied
vec3 applyClearcoatEnergyConservation(vec3 baseColor, float clearcoatContribution, float clearcoatF0){
    // Energy lost by base layer due to clearcoat reflection
    float energyFactor = 1.0 - clearcoatContribution * (1.0 - clearcoatF0);
    return baseColor * energyFactor;
}
