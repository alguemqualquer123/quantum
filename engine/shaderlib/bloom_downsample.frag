#version 450

// Halves the bloom chain with a 9-tap Gaussian. Each level reads the level above it,
// so texelSize is the size of the SOURCE, not of the destination.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_bloomTexture0;
uniform sampler u_bloom0_smp;

uniform u_fs_bloomDownsampleParams {
    vec4 texelSize; // 1.0 / source size in .xy
} bloomDownsample;

void main(){
    vec2 ts = bloomDownsample.texelSize.xy;
    vec3 color = texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord).rgb * 4.0;

    color += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2(-1.0, 0.0) * ts).rgb * 2.0;
    color += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2( 1.0, 0.0) * ts).rgb * 2.0;
    color += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2( 0.0,-1.0) * ts).rgb * 2.0;
    color += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2( 0.0, 1.0) * ts).rgb * 2.0;

    color += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2(-1.0,-1.0) * ts).rgb;
    color += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2( 1.0,-1.0) * ts).rgb;
    color += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2(-1.0, 1.0) * ts).rgb;
    color += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2( 1.0, 1.0) * ts).rgb;

    // 4 + 2*4 + 1*4 = 16
    frag_color = vec4(color / 16.0, 1.0);
}
