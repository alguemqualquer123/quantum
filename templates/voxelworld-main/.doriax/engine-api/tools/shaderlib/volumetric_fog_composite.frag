#version 450

// Volumetric Fog Composite: composites fog into the scene
// Combines accumulated fog with scene color

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_fogScatteringTexture;
uniform sampler u_fogScattering_smp;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_fogCompositeParams {
    vec4 params; // x = fogIntensity, y = heightFogIntensity, z = volumetricFogIntensity, w = unused
} fogCompositeParams;

void main(){
    float fogIntensity = fogCompositeParams.params.x;
    float heightFogIntensity = fogCompositeParams.params.y;
    float volumetricFogIntensity = fogCompositeParams.params.z;

    // Sample scene color
    vec3 sceneColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;

    // Sample accumulated fog
    vec4 fogData = texture(sampler2D(u_fogScatteringTexture, u_fogScattering_smp), v_texcoord);
    vec3 fogScattering = fogData.rgb;
    float fogTransmission = fogData.a;

    // Sample depth for distance-based fog
    float depth = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord).r;

    // Composite fog with scene
    vec3 result = sceneColor * fogTransmission + fogScattering * volumetricFogIntensity;

    // Apply fog intensity
    result = mix(sceneColor, result, fogIntensity);

    frag_color = vec4(result, 1.0);
}
