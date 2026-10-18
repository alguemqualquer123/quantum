#version 450

// SSR Hi-Z: screen-space reflections with hierarchical Z acceleration
// Much faster than basic SSR due to Hi-Z traversal

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform texture2D u_hiZTexture;
uniform sampler u_hiZ_smp;

uniform texture2D u_normalTexture;
uniform sampler u_normal_smp;

uniform u_fs_ssrHiZParams {
    mat4 projection;
    mat4 invProjection;
    vec4 params; // x = maxDistance, y = thickness, z = intensity, w = maxMipLevel
} ssrHiZParams;

// Reconstruct view-space position from depth
vec3 reconstructViewPos(vec2 uv, float depth){
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 viewPos = ssrHiZParams.invProjection * clipPos;
    return viewPos.xyz / viewPos.w;
}

// Reconstruct view-space normal from depth
vec3 reconstructNormal(vec2 uv, float depth){
    vec2 texelSize = 1.0 / textureSize(u_depthTexture, 0);

    float depthL = texture(sampler2D(u_depthTexture, u_depth_smp), uv - vec2(texelSize.x, 0.0)).r;
    float depthR = texture(sampler2D(u_depthTexture, u_depth_smp), uv + vec2(texelSize.x, 0.0)).r;
    float depthU = texture(sampler2D(u_depthTexture, u_depth_smp), uv - vec2(0.0, texelSize.y)).r;
    float depthD = texture(sampler2D(u_depthTexture, u_depth_smp), uv + vec2(0.0, texelSize.y)).r;

    vec3 posC = reconstructViewPos(uv, depth);
    vec3 posL = reconstructViewPos(uv - vec2(texelSize.x, 0.0), depthL);
    vec3 posR = reconstructViewPos(uv + vec2(texelSize.x, 0.0), depthR);
    vec3 posU = reconstructViewPos(uv - vec2(0.0, texelSize.y), depthU);
    vec3 posD = reconstructViewPos(uv + vec2(0.0, texelSize.y), depthD);

    vec3 dX = posR - posL;
    vec3 dY = posD - posU;
    return normalize(cross(dX, dY));
}

// Sample Hi-Z at specific mip level
float sampleHiZ(vec2 uv, float mipLevel){
    return textureLod(sampler2D(u_hiZTexture, u_hiZ_smp), uv, mipLevel).r;
}

// Hierarchical-Z ray marching
vec3 hiZRayMarch(vec3 viewPos, vec3 viewDir, float maxDistance, float thickness, float maxMipLevel){
    // Project ray into screen space
    vec4 startClip = ssrHiZParams.projection * vec4(viewPos, 1.0);
    vec4 endClip = ssrHiZParams.projection * vec4(viewPos + viewDir * maxDistance, 1.0);

    vec2 startUV = startClip.xy / startClip.w * 0.5 + 0.5;
    vec2 endUV = endClip.xy / endClip.w * 0.5 + 0.5;

    vec2 rayDir = endUV - startUV;
    float rayLength = length(rayDir);
    rayDir /= rayLength;

    // Start at highest mip level
    float currentMip = maxMipLevel;
    vec2 currentUV = startUV;
    float stepSize = rayLength / 32.0; // Initial step size

    for (int i = 0; i < 32; i++){
        // Sample depth at current mip level
        float sceneDepth = sampleHiZ(currentUV, currentMip);
        float rayDepth = texture(sampler2D(u_depthTexture, u_depth_smp), currentUV).r;

        // Check intersection
        if (rayDepth < sceneDepth - thickness){
            // Hit found
            if (currentMip > 0.0){
                // Descend to finer mip level
                currentMip -= 1.0;
                stepSize *= 0.5;
            } else {
                // Full resolution hit
                vec3 hitPos = reconstructViewPos(currentUV, rayDepth);
                return hitPos;
            }
        } else {
            // Miss, advance along ray
            currentUV += rayDir * stepSize;
            stepSize *= 1.0; // Can adjust step size

            // Check bounds
            if (currentUV.x < 0.0 || currentUV.x > 1.0 || currentUV.y < 0.0 || currentUV.y > 1.0){
                break;
            }
        }
    }

    return vec3(0.0); // No hit
}

void main(){
    float maxDistance = ssrHiZParams.params.x;
    float thickness = ssrHiZParams.params.y;
    float intensity = ssrHiZParams.params.z;
    float maxMipLevel = ssrHiZParams.params.w;

    // Sample depth and normal
    float depth = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord).r;
    vec3 normal = reconstructNormal(v_texcoord, depth);

    // Reconstruct view position
    vec3 viewPos = reconstructViewPos(v_texcoord, depth);

    // Calculate reflection direction
    vec3 viewDir = normalize(viewPos);
    vec3 reflectDir = reflect(viewDir, normal);

    // Ray march
    vec3 hitPos = hiZRayMarch(viewPos, reflectDir, maxDistance, thickness, maxMipLevel);

    if (hitPos != vec3(0.0)){
        // Project hit position to screen space
        vec4 hitClip = ssrHiZParams.projection * vec4(hitPos, 1.0);
        vec2 hitUV = hitClip.xy / hitClip.w * 0.5 + 0.5;

        // Sample scene color at hit point
        vec3 reflectionColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), hitUV).rgb;

        // Fade based on distance from center
        float fade = 1.0 - length(hitUV - v_texcoord);

        frag_color = vec4(reflectionColor * intensity * fade, fade);
    } else {
        frag_color = vec4(0.0);
    }
}
