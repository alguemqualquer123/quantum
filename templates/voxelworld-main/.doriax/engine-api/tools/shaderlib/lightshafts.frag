#version 450

// Light Shafts (God Rays): radial blur from light source
// Simulates volumetric lighting through atmosphere

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_lightShaftsParams {
    vec4 lightScreenPos; // xy = screen-space light position, z = intensity, w = decay
    vec4 params; // x = density, y = weight, z = exposure, w = illuminationDecay
} lightShaftsParams;

// Radial blur from light source
vec3 radialBlur(vec2 uv, vec2 lightPos, float density, float weight, float exposure, float illuminationDecay){
    vec2 texCoord = uv;
    vec2 deltaTextCoord = texCoord - lightPos;
    deltaTextCoord *= 1.0 / float(16) * density; // 16 samples

    vec3 color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), texCoord).rgb;
    float illuminationDecayFactor = illuminationDecay;

    for (int i = 0; i < 16; i++){
        texCoord -= deltaTextCoord;
        vec3 sampleColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), texCoord).rgb;

        // Apply decay and weight
        sampleColor *= illuminationDecayFactor * weight;
        color += sampleColor;

        illuminationDecayFactor *= illuminationDecay;
    }

    return color * exposure;
}

void main(){
    float density = lightShaftsParams.params.x;
    float weight = lightShaftsParams.params.y;
    float exposure = lightShaftsParams.params.z;
    float illuminationDecay = lightShaftsParams.params.w;

    vec2 lightPos = lightShaftsParams.lightScreenPos.xy;
    float lightIntensity = lightShaftsParams.lightScreenPos.z;
    float lightDecay = lightShaftsParams.lightScreenPos.w;

    // Sample scene color
    vec3 sceneColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;

    // Apply radial blur
    vec3 lightShafts = radialBlur(v_texcoord, lightPos, density, weight, exposure, illuminationDecay);

    // Modulate by light intensity and decay
    lightShafts *= lightIntensity * lightDecay;

    // Composite light shafts with scene
    vec3 result = sceneColor + lightShafts;

    // Screen-space fade (fade at edges)
    float edgeFade = smoothstep(0.0, 0.3, v_texcoord.x) * smoothstep(1.0, 0.7, v_texcoord.x);
    edgeFade *= smoothstep(0.0, 0.3, v_texcoord.y) * smoothstep(1.0, 0.7, v_texcoord.y);
    result *= edgeFade;

    frag_color = vec4(result, 1.0);
}
