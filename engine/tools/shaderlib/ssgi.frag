#version 450

// Screen-Space Global Illumination (SSGI): simplified GI via ray marching
// Based on Lumen Screen Probe Gather (simplified for real-time)

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform texture2D u_normalTexture;
uniform sampler u_normal_smp;

uniform texture2D u_prevGIHistoryTexture;
uniform sampler u_prevGIHistory_smp;

uniform u_fs_ssgiParams {
    mat4 invViewProjection;
    vec4 params0; // x = rayLength, y = numRays, z = intensity, w = temporalBlend
    vec4 params1; // x = thickness, y = bias, z = maxDistance, w = noiseScale
} ssgiParams;

// Reconstruct view-space position
vec3 reconstructViewPos(vec2 uv, float depth){
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 viewPos = ssgiParams.invViewProjection * clipPos;
    return viewPos.xyz / viewPos.w;
}

// Pseudo-random noise
float random(vec2 co){
    return fract(sin(dot(co.xy, vec2(12.9898, 78.233))) * 43758.5453);
}

// Cosine-weighted hemisphere sampling
vec3 cosineSampleHemisphere(vec3 normal, vec2 randomVec){
    // Create orthonormal basis
    vec3 up = abs(normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);

    // Cosine-weighted distribution
    float r = sqrt(randomVec.x);
    float theta = 2.0 * M_PI * randomVec.y;
    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(max(0.0, 1.0 - x * x - y * y));

    return tangent * x + bitangent * y + normal * z;
}

// Screen-space ray marching for GI
vec3 screenSpaceRayMarch(vec3 viewPos, vec3 rayDir, float maxDistance, float thickness){
    // Project ray to screen space
    vec4 startClip = ssgiParams.invViewProjection * vec4(viewPos, 1.0);
    vec4 endClip = ssgiParams.invViewProjection * vec4(viewPos + rayDir * maxDistance, 1.0);

    vec2 startUV = startClip.xy / startClip.w * 0.5 + 0.5;
    vec2 endUV = endClip.xy / endClip.w * 0.5 + 0.5;

    vec2 rayDir2D = endUV - startUV;
    float rayLength = length(rayDir2D);
    rayDir2D /= max(rayLength, 0.001);

    // March along the ray
    float stepSize = rayLength / 16.0; // 16 steps
    vec3 hitColor = vec3(0.0);
    float hitWeight = 0.0;

    for (int i = 0; i < 16; i++){
        float t = float(i) / 16.0;
        vec2 sampleUV = startUV + rayDir2D * rayLength * t;

        // Clamp to screen bounds
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0){
            break;
        }

        // Sample depth
        float sampleDepth = texture(sampler2D(u_depthTexture, u_depth_smp), sampleUV).r;
        vec3 samplePos = reconstructViewPos(sampleUV, sampleDepth);

        // Check intersection
        float depthDiff = length(samplePos - viewPos);
        if (depthDiff < maxDistance && abs(samplePos.z - (viewPos + rayDir * depthDiff).z) < thickness){
            // Hit found - sample color
            vec3 sampleColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), sampleUV).rgb;

            // Weight by distance and angle
            float distWeight = 1.0 - (depthDiff / maxDistance);
            float angleWeight = max(dot(normalize(samplePos - viewPos), rayDir), 0.0);

            hitColor += sampleColor * distWeight * angleWeight;
            hitWeight += distWeight * angleWeight;
        }
    }

    return hitWeight > 0.0 ? hitColor / hitWeight : vec3(0.0);
}

void main(){
    float rayLength = ssgiParams.params0.x;
    float numRays = ssgiParams.params0.y;
    float intensity = ssgiParams.params0.z;
    float temporalBlend = ssgiParams.params0.w;

    float thickness = ssgiParams.params1.x;
    float bias = ssgiParams.params1.y;
    float maxDistance = ssgiParams.params1.z;

    // Sample depth and normal
    float depth = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord).r;
    vec3 normal = texture(sampler2D(u_normalTexture, u_normal_smp), v_texcoord).rgb * 2.0 - 1.0;

    // Reconstruct view position
    vec3 viewPos = reconstructViewPos(v_texcoord, depth);

    // Accumulate rays
    vec3 giColor = vec3(0.0);
    for (float i = 0.0; i < numRays; i++){
        // Random direction in hemisphere
        vec2 noise = vec2(random(v_texcoord + vec2(i * 0.1)), random(v_texcoord + vec2(i * 0.2)));
        vec3 rayDir = cosineSampleHemisphere(normal, noise);

        // Screen-space ray march
        giColor += screenSpaceRayMarch(viewPos, rayDir, rayLength, thickness);
    }
    giColor /= numRays;

    // Temporal blend
    vec3 prevGI = texture(sampler2D(u_prevGIHistoryTexture, u_prevGIHistory_smp), v_texcoord).rgb;
    vec3 finalGI = mix(prevGI, giColor, 1.0 - temporalBlend);

    // Apply intensity
    finalGI *= intensity;

    frag_color = vec4(finalGI, 1.0);
}
