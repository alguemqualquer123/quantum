#version 450

// Temporal Anti-Aliasing (TAA): reduces aliasing by accumulating frames
// Based on UE5's TAA with neighborhood clamping and variance clipping

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture; // current frame
uniform sampler u_sceneColor_smp;

uniform texture2D u_depthTexture; // depth buffer
uniform sampler u_depth_smp;

uniform texture2D u_velocityTexture; // motion vectors
uniform sampler u_velocity_smp;

uniform texture2D u_prevSceneColorTexture; // previous frame
uniform sampler u_prevSceneColor_smp;

uniform u_fs_taaParams {
    vec4 params0; // x = blendFactor, y = velocityScale, z = viewWidth, w = viewHeight
    vec4 params1; // x = currentJitterX, y = currentJitterY, z = prevJitterX, w = prevJitterY
} taaParams;

// Decode motion vector from [0,1] to [-1,1]
vec2 decodeMotion(vec2 encoded){
    return (encoded - 0.5) * 2.0;
}

// Luminance
float getLuminance(vec3 color){
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// Neighborhood clamping (min/max AABB)
void getNeighborhoodClamp(vec3 samples[9], out vec3 minColor, out vec3 maxColor, out vec3 avgColor){
    minColor = samples[0];
    maxColor = samples[0];
    avgColor = samples[0];

    for (int i = 1; i < 9; i++){
        minColor = min(minColor, samples[i]);
        maxColor = max(maxColor, samples[i]);
        avgColor += samples[i];
    }
    avgColor /= 9.0;
}

// Variance clipping (reduces ghosting)
vec3 varianceClamp(vec3 history, vec3 minColor, vec3 maxColor, float varianceClipScale){
    vec3 avg = (minColor + maxColor) * 0.5;
    vec3 halfExtent = (maxColor - minColor) * 0.5;

    vec3 variance = (history - avg) * varianceClipScale;
    vec3 r = max(halfExtent, abs(variance));

    return clamp(history, avg - r, avg + r);
}

// Sample 3x3 neighborhood
vec3 sampleNeighborhood(sampler2D tex, vec2 uv, vec2 texelSize){
    vec3 samples[9];

    samples[0] = texture(sampler2D(tex, uv), uv).rgb;
    samples[1] = texture(sampler2D(tex, uv), uv + vec2(-texelSize.x, 0.0)).rgb;
    samples[2] = texture(sampler2D(tex, uv), uv + vec2(texelSize.x, 0.0)).rgb;
    samples[3] = texture(sampler2D(tex, uv), uv + vec2(0.0, -texelSize.y)).rgb;
    samples[4] = texture(sampler2D(tex, uv), uv + vec2(0.0, texelSize.y)).rgb;
    samples[5] = texture(sampler2D(tex, uv), uv + vec2(-texelSize.x, -texelSize.y)).rgb;
    samples[6] = texture(sampler2D(tex, uv), uv + vec2(texelSize.x, -texelSize.y)).rgb;
    samples[7] = texture(sampler2D(tex, uv), uv + vec2(-texelSize.x, texelSize.y)).rgb;
    samples[8] = texture(sampler2D(tex, uv), uv + vec2(texelSize.x, texelSize.y)).rgb;

    return samples;
}

void main(){
    vec2 viewSize = taaParams.params0.zw;
    vec2 texelSize = 1.0 / viewSize;
    float blendFactor = taaParams.params0.x;

    // Current frame sample
    vec3 currentColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;

    // Motion vector
    vec2 motionVector = decodeMotion(texture(sampler2D(u_velocityTexture, u_velocity_smp), v_texcoord).rg);

    // Re-project to previous frame UV
    vec2 prevUV = v_texcoord - motionVector;

    // Sample previous frame
    vec3 prevColor = texture(sampler2D(u_prevSceneColorTexture, u_prevSceneColor_smp), prevUV).rgb;

    // Neighborhood clamping to reduce ghosting
    vec3 samples[9];
    samples[0] = currentColor;
    samples[1] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord + vec2(-texelSize.x, 0.0)).rgb;
    samples[2] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord + vec2(texelSize.x, 0.0)).rgb;
    samples[3] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord + vec2(0.0, -texelSize.y)).rgb;
    samples[4] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord + vec2(0.0, texelSize.y)).rgb;
    samples[5] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord + vec2(-texelSize.x, -texelSize.y)).rgb;
    samples[6] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord + vec2(texelSize.x, -texelSize.y)).rgb;
    samples[7] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord + vec2(-texelSize.x, texelSize.y)).rgb;
    samples[8] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord + vec2(texelSize.x, texelSize.y)).rgb;

    vec3 minColor, maxColor, avgColor;
    getNeighborhoodClamp(samples, minColor, maxColor, avgColor);

    // Variance clipping for anti-ghosting
    float varianceClipScale = 0.75;
    prevColor = varianceClamp(prevColor, minColor, maxColor, varianceClipScale);

    // Adaptive blend factor based on motion and luminance
    float motionLength = length(motionVector * viewSize);
    float motionAdaptive = 1.0 - min(motionLength / 10.0, 1.0);

    // Use the blend factor with motion adaptation
    float finalBlend = mix(blendFactor, 1.0, motionAdaptive);

    // Blend current and history
    vec3 result = mix(currentColor, prevColor, finalBlend);

    frag_color = vec4(result, 1.0);
}
