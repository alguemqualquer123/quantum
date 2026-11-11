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
    vec4 params0; // x = blendFactor, y = velocityScale, z = outputWidth, w = outputHeight
    vec4 params1; // x = inputWidth, y = inputHeight, z = currentJitterX, w = currentJitterY
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

void main(){
    vec2 inputSize = max(taaParams.params1.xy, vec2(1.0));
    vec2 texelSize = 1.0 / inputSize;
    float blendFactor = taaParams.params0.x;
    vec2 inputUV = clamp(v_texcoord, vec2(0.0), vec2(1.0));

    // The current frame, depth and velocity are sampled on the lower-resolution input grid;
    // the history target is the native output grid. This is the TAAU path when the two sizes
    // differ, and the same equations reduce to regular TAA when they match.
    vec3 currentColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), inputUV).rgb;
    float currentDepth = texture(sampler2D(u_depthTexture, u_depth_smp), inputUV).r;

    // Motion vector
    vec2 motionVector = decodeMotion(texture(sampler2D(u_velocityTexture, u_velocity_smp), inputUV).rg);

    // Re-project to previous frame UV
    vec2 prevUV = clamp(v_texcoord - motionVector * taaParams.params0.y, vec2(0.0), vec2(1.0));

    // Sample previous frame
    vec3 prevColor = texture(sampler2D(u_prevSceneColorTexture, u_prevSceneColor_smp), prevUV).rgb;

    // Neighborhood clamping to reduce ghosting
    vec3 samples[9];
    samples[0] = currentColor;
    samples[1] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), clamp(inputUV + vec2(-texelSize.x, 0.0), vec2(0.0), vec2(1.0))).rgb;
    samples[2] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), clamp(inputUV + vec2(texelSize.x, 0.0), vec2(0.0), vec2(1.0))).rgb;
    samples[3] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), clamp(inputUV + vec2(0.0, -texelSize.y), vec2(0.0), vec2(1.0))).rgb;
    samples[4] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), clamp(inputUV + vec2(0.0, texelSize.y), vec2(0.0), vec2(1.0))).rgb;
    samples[5] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), clamp(inputUV + vec2(-texelSize.x, -texelSize.y), vec2(0.0), vec2(1.0))).rgb;
    samples[6] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), clamp(inputUV + vec2(texelSize.x, -texelSize.y), vec2(0.0), vec2(1.0))).rgb;
    samples[7] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), clamp(inputUV + vec2(-texelSize.x, texelSize.y), vec2(0.0), vec2(1.0))).rgb;
    samples[8] = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), clamp(inputUV + vec2(texelSize.x, texelSize.y), vec2(0.0), vec2(1.0))).rgb;

    vec3 minColor, maxColor, avgColor;
    getNeighborhoodClamp(samples, minColor, maxColor, avgColor);

    // Variance clipping for anti-ghosting
    float varianceClipScale = 0.75;
    prevColor = varianceClamp(prevColor, minColor, maxColor, varianceClipScale);

    // Adaptive blend factor based on motion and luminance
    float motionLength = length(motionVector * inputSize);
    float motionAdaptive = 1.0 - min(motionLength / 10.0, 1.0);

    // Use the blend factor with motion adaptation
    float finalBlend = mix(blendFactor, 1.0, motionAdaptive);
    if (currentDepth >= 0.999999)
        finalBlend = 0.0;

    // Blend current and history
    vec3 result = mix(currentColor, prevColor, finalBlend);

    frag_color = vec4(result, 1.0);
}
