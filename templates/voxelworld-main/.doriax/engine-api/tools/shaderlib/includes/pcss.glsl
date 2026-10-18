// PCSS (Percentage-Closer Soft Shadows) implementation
// Variable penumbra based on distance between blocker and receiver
// https://developer.nvidia.com/gpugems/GPUGems/gpugems_chapter_11

// Blocker search: finds average depth of shadow blockers
float findBlockerDepth(sampler2D shadowMap, vec2 shadowCoord, float searchRadius, float lightSize){
    float avgBlockerDepth = 0.0;
    int numBlockers = 0;

    for (int i = 0; i < 16; i++){
        // Poisson disk sampling
        vec2 offset = poissonDisk[i] * searchRadius;
        float sampledDepth = texture(shadowMap, shadowCoord + offset).r;

        if (sampledDepth < shadowCoord.z){
            avgBlockerDepth += sampledDepth;
            numBlockers++;
        }
    }

    if (numBlockers == 0){
        return -1.0; // No blockers found
    }

    return avgBlockerDepth / float(numBlockers);
}

// PCF with variable penumbra
float pcssFilter(sampler2D shadowMap, vec2 shadowCoord, float penumbraRadius, float bias){
    float shadow = 0.0;

    for (int i = 0; i < 16; i++){
        vec2 offset = poissonDisk[i] * penumbraRadius;
        float sampledDepth = texture(shadowMap, shadowCoord + offset).r;

        if (sampledDepth + bias < shadowCoord.z){
            shadow += 1.0;
        }
    }

    return shadow / 16.0;
}

// Main PCSS function
float pcssShadow(sampler2D shadowMap, vec3 shadowCoord, float lightSize, float nearPlane, float farPlane, float bias){
    // Step 1: Blocker search
    float searchRadius = lightSize * (shadowCoord.z - nearPlane) / farPlane;
    float avgBlockerDepth = findBlockerDepth(shadowMap, shadowCoord.xy, searchRadius, lightSize);

    // Early out if no blockers
    if (avgBlockerDepth < 0.0){
        return 0.0; // Fully lit
    }

    // Step 2: Penumbra estimation
    float penumbraWidth = (shadowCoord.z - avgBlockerDepth) / avgBlockerDepth * lightSize;
    float penumbraRadius = penumbraWidth * (shadowCoord.z - nearPlane) / farPlane;

    // Step 3: PCF filtering with variable penumbra
    return pcssFilter(shadowMap, shadowCoord, penumbraRadius, bias);
}

// Contact shadows using PCSS
float contactPCSS(sampler2D depthBuffer, vec2 screenUV, vec3 lightDir, float lightSize, float bias){
    // Reconstruct world position
    float depth = texture(depthBuffer, screenUV).r;
    vec3 worldPos = reconstructWorldPos(screenUV, depth);

    // Project along light direction
    vec3 shadowPos = worldPos + lightDir * 0.1; // Small offset

    // Project to screen space
    vec4 clipPos = projection * vec4(shadowPos, 1.0);
    vec2 shadowUV = clipPos.xy / clipPos.w * 0.5 + 0.5;

    // Sample with PCSS
    return pcssShadow(depthBuffer, vec3(shadowUV, clipPos.z / clipPos.w), lightSize, 0.1, 100.0, bias);
}
