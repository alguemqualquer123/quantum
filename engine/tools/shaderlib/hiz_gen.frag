#version 450

// Hi-Z Generation: creates hierarchical Z-buffer mips
// Used for accelerated screen-space ray marching (SSR)

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_hiZGenParams {
    vec4 params; // x = mipLevel, y = texelSize, z = unused, w = unused
} hiZGenParams;

void main(){
    float mipLevel = hiZGenParams.params.x;
    vec2 texelSize = hiZGenParams.params.yz;

    // 4-tap bilinear downsample for Hi-Z
    float depth0 = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord + vec2(-0.5, -0.5) * texelSize).r;
    float depth1 = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord + vec2( 0.5, -0.5) * texelSize).r;
    float depth2 = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord + vec2(-0.5,  0.5) * texelSize).r;
    float depth3 = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord + vec2( 0.5,  0.5) * texelSize).r;

    // Use max depth (nearest) for Hi-Z (conservative depth)
    float maxDepth = max(max(depth0, depth1), max(depth2, depth3));

    frag_color = vec4(maxDepth, 0.0, 0.0, 1.0);
}
