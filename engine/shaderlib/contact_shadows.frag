#version 450

// Screen-space contact shadows for the strongest directional light. The pass only marches a
// short distance from the visible surface, so it fills the small gap between a receiver and the
// first texel of the shadow atlas without replacing the atlas shadow itself.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_contactShadowsParams {
    mat4 viewProjection;
    mat4 invViewProjection;
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 params; // x = ray length, y = step count, z = thickness, w = intensity
} contactShadowsParams;

#include "includes/depth_util.glsl"

// Reconstruct world position from depth
vec3 reconstructWorldPos(vec2 uv, float depth){
    vec3 ndc = vec3(uv.x * 2.0 - 1.0, depthUVToNDCY(uv.y), depth * 2.0 - 1.0);
    vec4 worldPos = contactShadowsParams.invViewProjection * vec4(ndc, 1.0);
    return worldPos.xyz / worldPos.w;
}

void main(){
    vec3 sceneColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;
    float centerDepth = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord));
    if (centerDepth >= 0.999999 || contactShadowsParams.params.w <= 0.0){
        frag_color = vec4(sceneColor, 1.0);
        return;
    }

    float rayLength = contactShadowsParams.params.x;
    int numSteps = int(contactShadowsParams.params.y + 0.5);
    float thickness = contactShadowsParams.params.z;
    float intensity = contactShadowsParams.params.w;
    vec3 worldPos = reconstructWorldPos(v_texcoord, centerDepth);
    vec3 rayDir = normalize(contactShadowsParams.lightDirection.xyz);
    float shadow = 0.0;
    float previousVisibility = 1.0;
    for (int i = 1; i <= 64; i++){
        if (i > numSteps) break;
        float t = rayLength * (float(i) / float(max(numSteps, 1)));
        vec3 marchPos = worldPos + rayDir * t;
        vec4 clipPos = contactShadowsParams.viewProjection * vec4(marchPos, 1.0);
        if (clipPos.w <= 0.0) break;

        vec3 ndc = clipPos.xyz / clipPos.w;
        vec2 sampleUV = vec2(ndc.x * 0.5 + 0.5, ndcYToDepthUV(ndc.y));
        if (sampleUV.x <= 0.001 || sampleUV.x >= 0.999 || sampleUV.y <= 0.001 || sampleUV.y >= 0.999)
            break;

        float sampleDepth = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), sampleUV));
        vec3 blockerPos = reconstructWorldPos(sampleUV, sampleDepth);
        float cameraDistance = distance(blockerPos, contactShadowsParams.cameraPosition.xyz);
        float marchDistance = distance(marchPos, contactShadowsParams.cameraPosition.xyz);
        float occluded = smoothstep(thickness, thickness * 2.0, marchDistance - cameraDistance);
        shadow = max(shadow, occluded * previousVisibility);
        previousVisibility *= 0.92;
        if (shadow > 0.98) break;
    }

    frag_color = vec4(sceneColor * (1.0 - clamp(shadow * intensity, 0.0, 1.0)), 1.0);
}
