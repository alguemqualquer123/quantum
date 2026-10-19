#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D u_sceneColorTexture;

layout(set = 1, binding = 0) uniform fs_bloom_threshold_params {
    vec4 params; // x = threshold, y = soft knee
} u_fs_bloom_threshold;

void main(){
    vec3 color = texture(u_sceneColorTexture, uv).rgb;

    // Luminance-based threshold with soft knee
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float threshold = u_fs_bloom_threshold.params.x;
    float knee = u_fs_bloom_threshold.params.y * threshold;
    float soft = luminance - threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.00001);
    float contribution = max(soft, luminance - threshold) / max(luminance, 0.00001);

    fragColor = vec4(color * contribution, 1.0);
}
