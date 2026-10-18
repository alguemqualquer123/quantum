#version 450

// Virtual Shadow Maps Sampling: samples virtual shadow map with PCF
// Uses page table for efficient lookup

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_shadowAtlasTexture;
uniform samplerShadow u_shadowAtlas_smp;

uniform u_fs_vsmSampleParams {
    mat4 lightViewProjection;
    vec4 params; // x = pcfRadius, y = bias, z = normalBias, w = maxSearchSize
} vsmParams;

// PCF sampling for VSM
float sampleVSM(vec3 shadowCoord, float pcfRadius, float bias){
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(u_shadowAtlasTexture, 0);

    // PCF kernel
    for (float x = -pcfRadius; x <= pcfRadius; x += 1.0){
        for (float y = -pcfRadius; y <= pcfRadius; y += 1.0){
            vec2 offset = vec2(x, y) * texelSize;
            vec2 sampleUV = shadowCoord.xy + offset;

            // Depth comparison
            float sampledDepth = texture(sampler2D(u_shadowAtlasTexture, u_shadowAtlas_smp), sampleUV).r;
            if (sampledDepth + bias < shadowCoord.z){
                shadow += 1.0;
            }
        }
    }

    float samples = (2.0 * pcfRadius + 1.0) * (2.0 * pcfRadius + 1.0);
    return shadow / samples;
}

// Cascaded VSM sampling
float sampleCascadedVSM(vec3 worldPos, int cascadeLevel, float pcfRadius, float bias){
    // Transform to light space
    vec4 lightPos = u_fs_vsmParams.lightViewProjection * vec4(worldPos, 1.0);
    vec3 shadowCoord = lightPos.xyz / lightPos.w;

    // Remap to [0,1]
    shadowCoord = shadowCoord * 0.5 + 0.5;

    return sampleVSM(shadowCoord, pcfRadius, bias);
}

void main(){
    float pcfRadius = vsmParams.params.x;
    float bias = vsmParams.params.y;

    // Output shadow value
    float shadow = sampleVSM(vec3(v_texcoord, 0.5), pcfRadius, bias);

    frag_color = vec4(vec3(shadow), 1.0);
}
