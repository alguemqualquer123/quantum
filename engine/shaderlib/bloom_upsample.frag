#version 450

// One step of the bloom upsample chain: a tent filter over the smaller (already
// accumulated) level plus the level of its own resolution, which is what keeps the
// tight core of the glow while the wider levels add the halo. The accumulation is done
// here instead of with a blend because no offscreen additive pipeline exists.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_bloomTexture0;
uniform sampler u_bloom0_smp;
uniform texture2D u_bloomTexture1;
uniform sampler u_bloom1_smp;

uniform u_fs_bloomUpsampleParams {
    vec4 texelSize; // xy = 1 / source (smaller) size, z = weight of the same-size level
} bloomUpsample;

void main(){
    vec2 ts = bloomUpsample.texelSize.xy;
    vec3 wide = texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord).rgb * 4.0;

    wide += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2(-1.0, 0.0) * ts).rgb * 2.0;
    wide += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2( 1.0, 0.0) * ts).rgb * 2.0;
    wide += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2( 0.0,-1.0) * ts).rgb * 2.0;
    wide += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2( 0.0, 1.0) * ts).rgb * 2.0;

    wide += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2(-1.0,-1.0) * ts).rgb;
    wide += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2( 1.0,-1.0) * ts).rgb;
    wide += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2(-1.0, 1.0) * ts).rgb;
    wide += texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord + vec2( 1.0, 1.0) * ts).rgb;

    // 4 + 2*4 + 1*4 = 16
    vec3 detail = texture(sampler2D(u_bloomTexture1, u_bloom1_smp), v_texcoord).rgb;

    frag_color = vec4(wide / 16.0 + detail * bloomUpsample.texelSize.z, 1.0);
}
