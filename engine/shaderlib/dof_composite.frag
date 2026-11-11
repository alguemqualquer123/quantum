#version 450

// DOF composite: mixes the sharp scene color with the twice-blurred copy by the circle of
// confusion, and is the stage that presents the frame when nothing runs after it. Like
// fxaa.frag it only flips its lookups when it draws into the swapchain on GL - the three
// inputs are all offscreen textures in scene-color space.
//
// The vignette that used to live here is not depth of field and has its own pass type
// (ShaderType::VIGNETTE); it was dropped rather than smuggled in behind a DOF setting.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_dofTexture; // the vertically blurred image
uniform sampler u_dof_smp;

uniform texture2D u_depthTexture; // the CoC texture
uniform sampler u_depth_smp;

uniform u_fs_dofCompositeParams {
    vec4 params; // x = flip for a swapchain draw, y = intensity, z = far-field bias, w = unused
} dofComposite;

void main(){
    vec2 uv = v_texcoord;
    if (dofComposite.params.x > 0.5) {
        uv.y = 1.0 - uv.y;
    }

    vec3 sharpColor = textureLod(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv, 0.0).rgb;
    vec3 blurColor = textureLod(sampler2D(u_dofTexture, u_dof_smp), uv, 0.0).rgb;
    float coc = textureLod(sampler2D(u_depthTexture, u_depth_smp), uv, 0.0).r;

    // The blur reaches full strength one max-CoC away from focus; the far-field bias lifts
    // the near side of that curve so background blur shows up before the eye distance is
    // at its cap (a flat mix makes distant backgrounds look untouched at small apertures).
    float blend = clamp(coc + coc * coc * dofComposite.params.z, 0.0, 1.0) * dofComposite.params.y;

    frag_color = vec4(mix(sharpColor, blurColor, blend), 1.0);
}
