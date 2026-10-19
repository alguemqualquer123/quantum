#version 450

// Vignette: darkens edges of the screen for focus
// Simulates camera lens falloff

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform u_fs_vignetteParams {
    vec4 params; // x = intensity, y = smoothness, z = centerX, w = centerY
} vignetteParams;

void main(){
    float intensity = vignetteParams.params.x;
    float smoothness = vignetteParams.params.y;
    vec2 center = vignetteParams.params.zw;

    vec2 uv = v_texcoord * 2.0 - 1.0;
    vec2 offset = uv - center;
    float dist = length(offset);

    // Smooth vignette falloff
    float vignette = 1.0 - smoothstep(1.0 - intensity - smoothness, 1.0, dist);

    vec3 color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;
    color *= vignette;

    frag_color = vec4(color, 1.0);
}
