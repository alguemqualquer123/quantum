#version 450

// Adds the finished bloom chain back onto the scene color. The result is written to the
// HDR target, so it stays in linear light: the tone map that follows owns the encode to
// the display, which is what lets a lamp keep its energy instead of clipping to white
// before the glow is even extracted.
//
// The glow is multiplied by a lens-dirt mask before it is added. A real matte box is not
// clean: dust and grease carry the out-of-focus light into smudges that stay fixed in
// screen space while the scene moves under them, and that is most of what reads as "shot
// through a lens". No dirt texture ships with the engine, so the mask is generated here.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;
uniform texture2D u_bloomTexture0;
uniform sampler u_bloom0_smp;

uniform u_fs_bloomCompositeParams {
    vec4 params; // x = intensity, y = 1 when the scene source is sRGB, z = lens dirt (0 = clean lens)
} bloomComposite;

#include "includes/srgb.glsl"

float dirtHash(vec2 p){
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float dirtNoise(vec2 p){
    vec2 cell = floor(p);
    vec2 f = fract(p);
    // Smoothstep on the fraction, not f*f: the smudge edges want the soft shoulder, otherwise
    // the mask reads as a grid of cells once the bloom is bright enough to see it.
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = dirtHash(cell);
    float b = dirtHash(cell + vec2(1.0, 0.0));
    float c = dirtHash(cell + vec2(0.0, 1.0));
    float d = dirtHash(cell + vec2(1.0, 1.0));

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Three octaves stretched 3x along Y: a smear a lens element leaves runs mostly radially
// downward, and an isotropic noise looks like TV static rather than a dirty filter.
float dirtMask(vec2 uv){
    float m = dirtNoise(uv * vec2(9.0, 3.0)) * 0.55;
    m += dirtNoise(uv * vec2(21.0, 7.0) + 4.3) * 0.30;
    m += dirtNoise(uv * vec2(47.0, 17.0) + 9.1) * 0.15;

    // Vignette the mask: dirt sits at the edges of the aperture, and the centre of the frame is
    // where the glass is clear.
    float edge = smoothstep(0.15, 0.85, length(uv - 0.5) * 1.42);

    return clamp(m * (0.25 + 0.75 * edge), 0.0, 1.0);
}

void main(){
    vec3 scene = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;
    if (bloomComposite.params.y > 0.5)
        scene = sRGBToLinear(scene);

    vec3 bloom = texture(sampler2D(u_bloomTexture0, u_bloom0_smp), v_texcoord).rgb;
    float dirt = bloomComposite.params.z;

    if (dirt > 0.0)
        // Kept as a floor under the mask so a bright light never vanishes from the glow entirely.
        bloom *= mix(vec3(1.0), max(vec3(dirtMask(v_texcoord)), vec3(0.12)), dirt);

    frag_color = vec4(scene + bloom * bloomComposite.params.x, 1.0);
}
