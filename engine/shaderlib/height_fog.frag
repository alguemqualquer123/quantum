#version 450

// Height Fog: exponential height-based atmospheric fog
// Simple analytical fog with height falloff

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_heightFogParams {
    mat4 invViewProjection;
    vec4 params0; // x = fogDensity, y = fogHeightFalloff, z = fogHeightOffset, w = fogDensityOffset
    vec4 params1; // x = fogStartDistance, y = fogMaxDistance, z = fogIntensity, w = unused
    vec4 fogColor; // rgb = fog color (linear), w = ground level
} heightFogParams;

// Reconstruct world position from depth
vec3 reconstructWorldPos(vec2 uv, float depth){
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 worldPos = heightFogParams.invViewProjection * clipPos;
    return worldPos.xyz / worldPos.w;
}

// Exponential height fog calculation
float getHeightFog(vec3 worldPos, float viewHeight, float fogDensity, float fogHeightFalloff, float fogHeightOffset, float fogDensityOffset){
    float height = worldPos.y - heightFogParams.fogColor.w; // relative to ground

    // Height falloff
    float heightFactor = exp(-max(height + fogHeightOffset, 0.0) * fogHeightFalloff);

    // View height density (thicker near ground)
    float viewHeightFactor = exp(-max(viewHeight + fogDensityOffset, 0.0) * fogHeightFalloff * 0.5);

    return fogDensity * heightFactor * viewHeightFactor;
}

// Distance-based fog attenuation
float getDistanceFog(float distance, float startDistance, float maxDistance){
    if (distance < startDistance){
        return 0.0;
    }

    float t = (distance - startDistance) / (maxDistance - startDistance);
    return 1.0 - exp(-t * 2.0); // smooth transition
}

void main(){
    float fogDensity = heightFogParams.params0.x;
    float fogHeightFalloff = heightFogParams.params0.y;
    float fogHeightOffset = heightFogParams.params0.z;
    float fogDensityOffset = heightFogParams.params0.w;

    float fogStartDistance = heightFogParams.params1.x;
    float fogMaxDistance = heightFogParams.params1.y;
    float fogIntensity = heightFogParams.params1.z;

    // Sample scene color
    vec3 sceneColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;

    // Sample depth
    float depth = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord).r;

    // Reconstruct world position
    vec3 worldPos = reconstructWorldPos(v_texcoord, depth);
    vec3 viewPos = (heightFogParams.invViewProjection * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    float viewHeight = -viewPos.y; // camera height above ground

    // Calculate fog density
    float fogDensityValue = getHeightFog(worldPos, viewHeight, fogDensity, fogHeightFalloff, fogHeightOffset, fogDensityOffset);

    // Distance attenuation
    float distance = length(worldPos - viewPos);
    float distanceFog = getDistanceFog(distance, fogStartDistance, fogMaxDistance);

    // Combined fog factor
    float fogFactor = 1.0 - exp(-fogDensityValue * distance * distanceFog);
    fogFactor = clamp(fogFactor, 0.0, 1.0) * fogIntensity;

    // Apply fog
    vec3 fogColor = heightFogParams.fogColor.rgb;
    vec3 result = mix(sceneColor, fogColor, fogFactor);

    frag_color = vec4(result, 1.0);
}
