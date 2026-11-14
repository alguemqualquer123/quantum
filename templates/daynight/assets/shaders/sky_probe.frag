#version 450

out vec4 frag_color;

in vec3 uv;

uniform textureCube u_skyTexture;
uniform sampler u_sky_smp;

uniform u_fs_customParams {
    vec4 probeHigh;
    vec4 probeLow;
    vec4 resolution;
    float time;
} cp;

void main(){
    vec3 dir = normalize(uv);
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 colour = mix(cp.probeLow.xyz, cp.probeHigh.xyz, t);
    // A band that only exists if the engine-written "time" reached the block.
    colour += 0.12 * vec3(sin(dir.y * 14.0 + cp.time), 0.0, cos(dir.y * 14.0 - cp.time));
    frag_color = vec4(colour, 1.0);
}
