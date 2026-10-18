#version 450

// Volumetric Fog Injection: injects light and density into froxel volume
// Based on UE5's VolumetricFog.cpp with froxel-based approach

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_fogInjectionParams {
    mat4 invViewProjection;
    mat4 prevViewProjection;
    vec4 params0; // x = fogDensity, y = fogHeightFalloff, z = fogHeightOffset, w = maxDistance
    vec4 params1; // x = scatteringDistribution, y = extinctionScale, z = viewHeight, w = time
    vec4 lightDir; // xyz = directional light direction, w = intensity
    vec4 lightColor; // rgb = light color (linear), w = unused
    vec4 fogColor; // rgb = fog scattering color, w = ground level
} fogParams;

// Reconstruct world position from depth
vec3 reconstructWorldPos(vec2 uv, float depth){
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 worldPos = fogParams.invViewProjection * clipPos;
    return worldPos.xyz / worldPos.w;
}

// Henyey-Greenstein phase function for light scattering
float henyeyGreenstein(float cosTheta, float g){
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * M_PI * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5));
}

// Exponential height fog density
float getFogDensity(vec3 worldPos, float heightFalloff, float fogHeightOffset, float baseDensity){
    float height = worldPos.y - fogParams.fogColor.w; // relative to ground
    float heightFactor = exp(-max(height + fogHeightOffset, 0.0) * heightFalloff);
    return baseDensity * heightFactor;
}

void main(){
    float fogDensity = fogParams.params0.x;
    float fogHeightFalloff = fogParams.params0.y;
    float fogHeightOffset = fogParams.params0.z;
    float maxDistance = fogParams.params0.w;

    float scattering = fogParams.params1.x;
    float extinctionScale = fogParams.params1.y;

    // Sample depth
    float depth = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord).r;

    // Reconstruct world position
    vec3 worldPos = reconstructWorldPos(v_texcoord, depth);

    // Calculate fog properties
    float density = getFogDensity(worldPos, fogHeightFalloff, fogHeightOffset, fogDensity);

    // View direction for phase function
    vec3 viewDir = normalize(worldPos);
    float cosTheta = dot(viewDir, fogParams.lightDir.xyz);

    // Phase function (scattering distribution)
    float phase = henyeyGreenstein(cosTheta, scattering);

    // Light contribution
    vec3 lightContribution = fogParams.lightColor.rgb * fogParams.lightDir.w * phase;

    // Fog scattering coefficient
    vec3 scatteringCoeff = vec3(density) * lightContribution;

    // Extinction coefficient
    vec3 extinctionCoeff = vec3(density) * extinctionScale;

    // Output: rgb = scattering, a = extinction
    frag_color = vec4(scatteringCoeff, extinctionCoeff.r);
}
