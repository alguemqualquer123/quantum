#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D u_sceneColorTexture;
layout(set = 0, binding = 1) uniform sampler2D u_bloomTexture;

layout(set = 1, binding = 0) uniform fs_bloom_composite_params {
    vec4 params; // x = intensity
} u_fs_bloom_composite;

void main(){
    vec3 sceneColor = texture(u_sceneColorTexture, uv).rgb;
    vec3 bloomColor = texture(u_bloomTexture, uv).rgb;
    float intensity = u_fs_bloom_composite.params.x;

    fragColor = vec4(sceneColor + bloomColor * intensity, 1.0);
}
