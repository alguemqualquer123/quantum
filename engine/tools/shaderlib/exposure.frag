#version 450

// Exposure: calculates scene luminance and exposure
// Two-pass approach: first computes average luminance, then applies adaptation

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_exposureParams {
    vec4 params; // x = minExposure, y = maxExposure, z = adaptationSpeed, w = exposureCompensation
} exposureParams;

uniform u_fs_exposureAdapt {
    vec4 params; // x = timeDelta, y = currentExposure, z = adaptationRate, w = unused
} exposureAdapt;

// Luminance calculation (ITU BT.709)
float getLuminance(vec3 color){
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// Simple exposure from luminance
float calculateExposure(float avgLuminance, float exposureCompensation){
    // Middle gray calibration (18% middle gray maps to exposure 1.0)
    float keyValue = 0.18;
    return keyValue / (avgLuminance + 0.001) * exposureCompensation;
}

// Progressive adaptation (temporal smoothing)
float adaptExposure(float currentExposure, float targetExposure, float adaptationSpeed, float dt){
    if (targetExposure > currentExposure){
        // Brighten faster (pupil constricts)
        return currentExposure + (targetExposure - currentExposure) * min(adaptationSpeed * 2.0 * dt, 1.0);
    } else {
        // Darken slower (pupil dilates)
        return currentExposure + (targetExposure - currentExposure) * min(adaptationSpeed * dt, 1.0);
    }
}

void main(){
    float minExposure = exposureParams.params.x;
    float maxExposure = exposureParams.params.y;
    float adaptationSpeed = exposureParams.params.z;
    float exposureCompensation = exposureParams.params.w;

    float dt = exposureAdapt.params.x;
    float currentExposure = exposureAdapt.params.y;

    // Sample scene color
    vec3 color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;

    // Calculate luminance
    float luminance = getLuminance(color);

    // Apply simple exposure calculation
    float targetExposure = calculateExposure(luminance, exposureCompensation);

    // Clamp to min/max range
    targetExposure = clamp(targetExposure, minExposure, maxExposure);

    // Temporal adaptation (smooth transition between frames)
    float adaptedExposure = adaptExposure(currentExposure, targetExposure, adaptationSpeed, dt);

    // Output: x = adapted exposure, y = current luminance, z = target exposure
    frag_color = vec4(adaptedExposure, luminance, targetExposure, 1.0);
}
