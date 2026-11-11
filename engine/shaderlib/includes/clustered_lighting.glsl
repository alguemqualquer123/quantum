// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

// The storage buffer is present only on backends with fragment-stage SSBO support.
// GLES3 receives the same shader without USE_CLUSTERED_LIGHTS and stays on the
// compact MAX_LIGHTS compatibility path.
#ifdef USE_CLUSTERED_LIGHTS
readonly buffer sbo_lightGrid {
    uint lightIndices[];
};

uint clusteredLightCountAtFragment(){
    if (lighting.clusterDepth.w < 0.5)
        return 0u;

    ivec2 tile = ivec2(gl_FragCoord.xy / lighting.clusterInfo.xx);
    int gridWidth = int(lighting.clusterInfo.y);
    int gridHeight = int(lighting.clusterInfo.z);
    int depthSlices = int(lighting.clusterInfo.w);
    if (lighting.viewportInfo.w > 0.5)
        tile.y = gridHeight - 1 - tile.y;
    tile = clamp(tile, ivec2(0), ivec2(gridWidth - 1, gridHeight - 1));

    float ndcDepth = gl_FragCoord.z * 2.0 - 1.0;
    float nearDepth = lighting.clusterDepth.x;
    float farDepth = lighting.clusterDepth.y;
    float viewDepth = (2.0 * nearDepth * farDepth) /
        max(farDepth + nearDepth - ndcDepth * (farDepth - nearDepth), 0.0001);
    int depthSlice = int(floor(log2(max(viewDepth, nearDepth) / nearDepth) /
        max(lighting.clusterDepth.z, 0.0001) * float(depthSlices)));
    depthSlice = clamp(depthSlice, 0, depthSlices - 1);

    uint stride = uint(MAX_CLUSTER_LIGHTS_PER_TILE + 1);
    uint cluster = (uint(depthSlice * gridHeight + tile.y) * uint(gridWidth) + uint(tile.x));
    return min(lightIndices[cluster * stride], uint(MAX_CLUSTER_LIGHTS_PER_TILE));
}

vec4 clusteredLightDebugColor(){
    if (lighting.clusterDebug.x < 0.5 || lighting.clusterDepth.w < 0.5)
        return vec4(0.0);

    float occupancy = float(clusteredLightCountAtFragment()) /
        float(MAX_CLUSTER_LIGHTS_PER_TILE);
    vec3 heat = mix(vec3(0.05, 0.2, 1.0), vec3(1.0, 0.08, 0.02),
                    smoothstep(0.0, 1.0, occupancy));
    vec2 tileUV = fract(gl_FragCoord.xy / lighting.clusterInfo.xx);
    float border = 1.0 - smoothstep(0.0, 0.045,
        min(min(tileUV.x, tileUV.y), min(1.0 - tileUV.x, 1.0 - tileUV.y)));
    heat = mix(heat, vec3(1.0), border * 0.75);
    return vec4(heat, 0.68);
}

bool clusteredLightEnabled(int lightIndex){
    if (lighting.clusterDepth.w < 0.5)
        return true;

    ivec2 tile = ivec2(gl_FragCoord.xy / lighting.clusterInfo.xx);
    int gridWidth = int(lighting.clusterInfo.y);
    int gridHeight = int(lighting.clusterInfo.z);
    int depthSlices = int(lighting.clusterInfo.w);
    if (lighting.viewportInfo.w > 0.5)
        tile.y = gridHeight - 1 - tile.y;
    tile = clamp(tile, ivec2(0), ivec2(gridWidth - 1, gridHeight - 1));

    float ndcDepth = gl_FragCoord.z * 2.0 - 1.0;
    float nearDepth = lighting.clusterDepth.x;
    float farDepth = lighting.clusterDepth.y;
    float viewDepth = (2.0 * nearDepth * farDepth) /
        max(farDepth + nearDepth - ndcDepth * (farDepth - nearDepth), 0.0001);
    int depthSlice = int(floor(log2(max(viewDepth, nearDepth) / nearDepth) /
        max(lighting.clusterDepth.z, 0.0001) * float(depthSlices)));
    depthSlice = clamp(depthSlice, 0, depthSlices - 1);

    uint stride = uint(MAX_CLUSTER_LIGHTS_PER_TILE + 1);
    uint cluster = (uint(depthSlice * gridHeight + tile.y) * uint(gridWidth) + uint(tile.x));
    uint base = cluster * stride;
    uint count = min(lightIndices[base], uint(MAX_CLUSTER_LIGHTS_PER_TILE));
    for (uint i = 0u; i < count; i++){
        if (lightIndices[base + 1u + i] == uint(lightIndex))
            return true;
    }
    return false;
}
#else
bool clusteredLightEnabled(int lightIndex){
    return true;
}

vec4 clusteredLightDebugColor(){
    return vec4(0.0);
}
#endif
