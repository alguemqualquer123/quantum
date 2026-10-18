#version 450

// Motion Blur: per-pixel directional blur using velocity buffer
// Tile-based optimization for better performance

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_velocityTexture;
uniform sampler u_velocity_smp;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_motionBlurParams {
    vec4 params; // x = intensity, y = maxSamples, z = velocityClamp, w = depthThreshold
} motionBlurParams;

// Decode motion vector
vec2 decodeMotion(vec2 encoded){
    return (encoded - 0.5) * 2.0;
}

void main(){
    float intensity = motionBlurParams.params.x;
    float maxSamples = motionBlurParams.params.y;
    float velocityClamp = motionBlurParams.params.z;
    float depthThreshold = motionBlurParams.params.w;

    // Sample motion vector
    vec2 motionVector = decodeMotion(texture(sampler2D(u_velocityTexture, u_velocity_smp), v_texcoord).rg);

    // Clamp velocity
    float velLength = length(motionVector);
    if (velLength > velocityClamp){
        motionVector = motionVector / velLength * velocityClamp;
    }

    // Scale by intensity
    motionVector *= intensity;

    // Sample center color
    vec3 color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;

    // Depth-aware sampling (skip blur on distant objects)
    float centerDepth = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord).r;
    float depthFactor = smoothstep(0.0, depthThreshold, 1.0 - centerDepth);

    // Apply motion blur with multiple samples
    float totalWeight = 1.0;
    vec3 accumColor = color;

    for (float i = 1.0; i <= maxSamples; i += 1.0){
        float t = i / maxSamples;
        vec2 offset = motionVector * t;

        // Sample along motion vector
        vec2 sampleUV = v_texcoord + offset;
        vec3 sampleColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), sampleUV).rgb;

        // Weight decreases towards the end
        float weight = 1.0 - t;
        accumColor += sampleColor * weight;
        totalWeight += weight;
    }

    // Normalize
    accumColor /= totalWeight;

    // Blend based on depth (distant objects get less blur)
    vec3 result = mix(color, accumColor, depthFactor);

    frag_color = vec4(result, 1.0);
}
