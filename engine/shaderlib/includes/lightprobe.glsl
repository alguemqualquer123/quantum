// Light Probe Grid: world-space SH probes for diffuse GI
// Simplified version of Lumen Radiance Cache

// Spherical Harmonics L2 (9 coefficients per channel)
struct SHL2 {
    vec3 coefficients[9];
};

// Evaluate SH at direction
vec3 evaluateSH(SHL2 sh, vec3 direction){
    // SH basis functions (L0, L1, L2)
    float x = direction.x;
    float y = direction.y;
    float z = direction.z;

    // L0 (constant)
    vec3 result = sh.coefficients[0] * 0.282095;

    // L1 (linear)
    result += sh.coefficients[1] * 0.488603 * y;
    result += sh.coefficients[2] * 0.488603 * z;
    result += sh.coefficients[3] * 0.488603 * x;

    // L2 (quadratic)
    result += sh.coefficients[4] * 1.092548 * x * y;
    result += sh.coefficients[5] * 1.092548 * y * z;
    result += sh.coefficients[6] * 0.315392 * (3.0 * z * z - 1.0);
    result += sh.coefficients[7] * 1.092548 * x * z;
    result += sh.coefficients[8] * 0.546274 * (x * x - y * y);

    return max(result, vec3(0.0));
}

// Convert cubemap face to SH (simplified - use 6 samples)
SHL2 cubemapToSH(samplerCube cubemap, sampler smp){
    SHL2 sh;
    for (int i = 0; i < 9; i++){
        sh.coefficients[i] = vec3(0.0);
    }

    // Sample 6 directions (face centers)
    vec3 directions[6] = vec3[](
        vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0),
        vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
        vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0)
    );

    float weights[6] = float[](0.166667, 0.166667, 0.166667, 0.166667, 0.166667, 0.166667);

    for (int face = 0; face < 6; face++){
        vec3 dir = directions[face];
        vec3 color = texture(cubemap, dir).rgb * weights[face];

        // Project into SH basis
        float x = dir.x;
        float y = dir.y;
        float z = dir.z;

        sh.coefficients[0] += color * 0.282095;
        sh.coefficients[1] += color * 0.488603 * y;
        sh.coefficients[2] += color * 0.488603 * z;
        sh.coefficients[3] += color * 0.488603 * x;
        sh.coefficients[4] += color * 1.092548 * x * y;
        sh.coefficients[5] += color * 1.092548 * y * z;
        sh.coefficients[6] += color * 0.315392 * (3.0 * z * z - 1.0);
        sh.coefficients[7] += color * 1.092548 * x * z;
        sh.coefficients[8] += color * 0.546274 * (x * x - y * y);
    }

    return sh;
}

// Trilinear interpolation between SH probes
SHL2 interpolateSH(SHL2 sh000, SHL2 sh001, SHL2 sh010, SHL2 sh011,
                   SHL2 sh100, SHL2 sh101, SHL2 sh110, SHL2 sh111,
                   float fx, float fy, float fz){
    SHL2 result;
    for (int i = 0; i < 9; i++){
        vec3 c00 = mix(sh000.coefficients[i], sh100.coefficients[i], fx);
        vec3 c01 = mix(sh001.coefficients[i], sh101.coefficients[i], fx);
        vec3 c10 = mix(sh010.coefficients[i], sh110.coefficients[i], fx);
        vec3 c11 = mix(sh011.coefficients[i], sh111.coefficients[i], fx);

        vec3 c0 = mix(c00, c10, fy);
        vec3 c1 = mix(c01, c11, fy);

        result.coefficients[i] = mix(c0, c1, fz);
    }
    return result;
}

// Sample light probe grid at world position
vec3 sampleLightProbeGrid(vec3 worldPos, vec3 normal, vec3 gridOrigin, vec3 gridSpacing, ivec3 gridSize,
                          sampler2D probeData, sampler probeSmp){
    // Convert world position to grid coordinates
    vec3 gridPos = (worldPos - gridOrigin) / gridSpacing;
    vec3 clampedPos = clamp(gridPos, vec3(0.0), vec3(gridSize) - vec3(1.0));

    // Get integer grid coordinates
    ivec3 baseCoord = ivec3(floor(clampedPos));
    ivec3 nextCoord = ivec3(min(baseCoord + 1, gridSize - 1));

    // Fractional part for interpolation
    vec3 frac = clampedPos - floor(clampedPos);

    // Sample 8 neighboring probes (trilinear)
    // Each probe stores 9 SH coefficients = 9 texels in a 1D texture
    // This is a simplified version - in practice you'd use a 3D texture or structured buffer

    // For now, return a simple ambient color based on position
    vec3 ambient = vec3(0.1, 0.1, 0.1); // default ambient

    return ambient;
}

// Compute indirect lighting from SH probe
vec3 computeIndirectLighting(vec3 worldPos, vec3 normal, SHL2 sh){
    // Cosine-weighted SH evaluation for diffuse
    return evaluateSH(sh, normal);
}
