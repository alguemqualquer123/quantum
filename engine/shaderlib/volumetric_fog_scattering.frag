#version 450

// Volumetric Fog Scattering: accumulates scattering along view rays
// Performs front-to-back integration of the fog volume

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_fogInjectionTexture;
uniform sampler u_fogInjection_smp;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_fogScatteringParams {
    vec4 params; // x = sliceCount, y = sliceDistribution, z = temporalAccumulation, w = time
} fogScatteringParams;

// Reconstruct view-space depth from froxel slice
float sliceDepth(float slice, float sliceCount, float nearPlane, float farPlane){
    float z = slice / sliceCount;
    // Non-linear distribution (more slices near camera)
    z = pow(z, 3.0); // cube distribution for better near-field quality
    return mix(nearPlane, farPlane, z);
}

void main(){
    float sliceCount = fogScatteringParams.params.x;
    float sliceDistribution = fogScatteringParams.params.y;
    float temporalAccumulation = fogScatteringParams.params.z;

    // Sample current slice
    vec4 currentSlice = texture(sampler2D(u_fogInjectionTexture, u_fogInjection_smp), v_texcoord);

    // Accumulate from front to back
    vec3 accumulatedScattering = vec3(0.0);
    float accumulatedTransmission = 1.0;

    for (float i = 0.0; i < sliceCount; i += 1.0){
        float sliceUv = (i + 0.5) / sliceCount;
        vec4 sliceData = texture(sampler2D(u_fogInjectionTexture, u_fogInjection_smp), vec2(v_texcoord.x, sliceUv));

        vec3 scattering = sliceData.rgb;
        float extinction = sliceData.a;

        // Transmission through this slice
        float sliceTransmission = exp(-extinction);

        // Accumulate scattering
        accumulatedScattering += scattering * accumulatedTransmission * (1.0 - sliceTransmission) / max(extinction, 0.0001);

        // Update transmission
        accumulatedTransmission *= sliceTransmission;

        // Early exit when transmission is negligible
        if (accumulatedTransmission < 0.001){
            break;
        }
    }

    // Temporal accumulation
    vec3 prevAccumulated = texture(sampler2D(u_fogInjectionTexture, u_fogInjection_smp), v_texcoord).rgb;
    vec3 finalScattering = mix(prevAccumulated, accumulatedScattering, 0.1); // temporal blend

    frag_color = vec4(finalScattering, accumulatedTransmission);
}
