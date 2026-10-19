#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D u_bloomTexture;

layout(set = 1, binding = 0) uniform fs_bloom_downsample_params {
    vec4 texelSize; // 1.0/textureSize in .xy
} u_fs_bloom_downsample;

// 13-tap Gaussian downsample filter
void main(){
    vec2 texelSize = u_fs_bloom_downsample.texelSize.xy;
    vec3 color = vec3(0.0);

    // Center tap
    color += texture(u_bloomTexture, uv).rgb * 4.0;

    // Inner 4 taps (cardinal)
    color += texture(u_bloomTexture, uv + vec2(-1.0, 0.0) * texelSize).rgb * 2.0;
    color += texture(u_bloomTexture, uv + vec2( 1.0, 0.0) * texelSize).rgb * 2.0;
    color += texture(u_bloomTexture, uv + vec2( 0.0,-1.0) * texelSize).rgb * 2.0;
    color += texture(u_bloomTexture, uv + vec2( 0.0, 1.0) * texelSize).rgb * 2.0;

    // Outer 4 taps (diagonal)
    color += texture(u_bloomTexture, uv + vec2(-1.0,-1.0) * texelSize).rgb;
    color += texture(u_bloomTexture, uv + vec2( 1.0,-1.0) * texelSize).rgb;
    color += texture(u_bloomTexture, uv + vec2(-1.0, 1.0) * texelSize).rgb;
    color += texture(u_bloomTexture, uv + vec2( 1.0, 1.0) * texelSize).rgb;

    // Normalize: 4 + 2*4 + 1*4 = 16
    fragColor = vec4(color / 16.0, 1.0);
}
