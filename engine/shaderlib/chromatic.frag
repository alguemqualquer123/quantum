#version 450

// Chromatic Aberration: separates RGB channels radially
// Simulates lens imperfection for cinematic effect

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform u_fs_chromaticParams {
    vec4 params; // x = intensity, y = centerX, z = centerY, w = falloff
} chromaticParams;

void main(){
    float intensity = chromaticParams.params.x;
    vec2 center = chromaticParams.params.xy;
    float falloff = chromaticParams.params.z;

    // Direction from center
    vec2 dir = v_texcoord - center;
    float dist = length(dir);

    // Chromatic offset
    float offset = intensity * dist * dist * falloff;

    // Sample each channel with different offsets
    vec2 redOffset = dir * offset;
    vec2 greenOffset = vec2(0.0); // green stays at center
    vec2 blueOffset = -dir * offset;

    float r = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord + redOffset).r;
    float g = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord + greenOffset).g;
    float b = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord + blueOffset).b;

    frag_color = vec4(r, g, b, 1.0);
}
