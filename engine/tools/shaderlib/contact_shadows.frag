#version 450

// Contact Shadows: screen-space short-range shadows
// Ray marches in depth buffer for near-contact detail

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform texture2D u_normalTexture;
uniform sampler u_normal_smp;

uniform u_fs_contactShadowsParams {
    mat4 invViewProjection;
    vec4 params; // x = rayLength, y = numSteps, z = thickness, w = intensity
} contactShadowsParams;

// Linearize depth
float linearizeDepth(float depth){
    float near = 0.1;
    float far = 1000.0;
    return (2.0 * near * far) / (far + near - depth * (far - near));
}

// Reconstruct world position from depth
vec3 reconstructWorldPos(vec2 uv, float depth){
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 worldPos = contactShadowsParams.invViewProjection * clipPos;
    return worldPos.xyz / worldPos.w;
}

void main(){
    float rayLength = contactShadowsParams.params.x;
    float numSteps = contactShadowsParams.params.y;
    float thickness = contactShadowsParams.params.z;
    float intensity = contactShadowsParams.params.w;

    // Sample depth and normal
    float centerDepth = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord).r;
    vec3 normal = texture(sampler2D(u_normalTexture, u_normal_smp), v_texcoord).rgb * 2.0 - 1.0;

    // Reconstruct world position
    vec3 worldPos = reconstructWorldPos(v_texcoord, centerDepth);

    // Ray march in world space along light direction (simplified: use normal as direction)
    vec3 rayDir = normalize(normal) * 0.01; // Small steps along normal

    float shadow = 0.0;
    vec3 currentPos = worldPos;

    for (float i = 0.0; i < numSteps; i += 1.0){
        currentPos += rayDir * (rayLength / numSteps);

        // Project back to screen space
        vec4 clipPos = vec4(currentPos, 1.0);
        vec2 sampleUV = (clipPos.xy / clipPos.w) * 0.5 + 0.5;

        // Sample depth at this position
        float sampleDepth = texture(sampler2D(u_depthTexture, u_depth_smp), sampleUV).r;
        float sampleLinearDepth = linearizeDepth(sampleDepth);

        // Check if occluded
        float currentLinearDepth = linearizeDepth(centerDepth);
        if (sampleLinearDepth < currentLinearDepth - thickness){
            shadow += intensity;
        }
    }

    shadow = min(shadow, 1.0);

    // Apply shadow to scene color
    vec3 color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;
    color *= (1.0 - shadow);

    frag_color = vec4(color, shadow);
}
