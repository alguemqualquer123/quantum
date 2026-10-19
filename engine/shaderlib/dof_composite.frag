#version 450

// DOF Composite: combines sharp and blurred layers based on CoC
// Applies the depth of field effect to the scene

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_blurTexture;
uniform sampler u_blur_smp;

uniform texture2D u_depthTexture; // CoC texture
uniform sampler u_depth_smp;

uniform u_fs_dofCompositeParams {
    vec4 params; // x = blendSharpness, y = vignetteIntensity, z = vignetteFalloff, w = unused
} dofCompositeParams;

void main(){
    float sharpness = dofCompositeParams.params.x;

    // Sample sharp scene
    vec3 sharpColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;

    // Sample blurred scene
    vec3 blurColor = texture(sampler2D(u_blurTexture, u_blur_smp), v_texcoord).rgb;

    // Sample CoC
    float coc = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord).r;

    // Blend based on CoC with sharpness control
    float blendFactor = smoothstep(0.0, 1.0, coc);

    // Apply sharpness to near field
    if (coc > 0.0){
        blendFactor = max(blendFactor, sharpness * coc);
    }

    vec3 result = mix(sharpColor, blurColor, blendFactor);

    // Optional vignette for DOF
    float vignetteIntensity = dofCompositeParams.params.y;
    float vignetteFalloff = dofCompositeParams.params.z;
    if (vignetteIntensity > 0.0){
        vec2 uv = v_texcoord * 2.0 - 1.0;
        float vignette = 1.0 - dot(uv, uv) * vignetteIntensity;
        vignette = smoothstep(0.0, vignetteFalloff, vignette);
        result *= vignette;
    }

    frag_color = vec4(result, 1.0);
}
