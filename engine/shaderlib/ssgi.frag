// (c) SR VINIX
// SPDX-License-Identifier: MIT

#version 450

// Screen-space diffuse bounce lighting. This is intentionally conservative: it
// gathers only visible radiance from the current frame, so it degrades to the
// normal scene color when a bounce leaves the screen.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;
uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_ssgiParams {
    mat4 projection;
    mat4 invProjection;
    vec4 params0; // x = radius, y = ray count, z = intensity, w = thickness
    vec4 params1; // xy = 1/depth size, z = frame rotation, w = unused
} ssgi;

#include "includes/depth_util.glsl"

vec3 reconstructViewPos(vec2 uv, float depth01){
    vec3 ndc = vec3(uv.x * 2.0 - 1.0, depthUVToNDCY(uv.y), depth01 * 2.0 - 1.0);
    vec4 view = ssgi.invProjection * vec4(ndc, 1.0);
    return view.xyz / view.w;
}

float depthAt(vec2 uv){
    return decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), uv));
}

vec3 viewNormal(vec2 uv, vec3 center){
    vec2 texel = ssgi.params1.xy;
    float dl = depthAt(uv - vec2(texel.x, 0.0));
    float dr = depthAt(uv + vec2(texel.x, 0.0));
    float dd = depthAt(uv - vec2(0.0, texel.y));
    float du = depthAt(uv + vec2(0.0, texel.y));
    vec3 left = reconstructViewPos(uv - vec2(texel.x, 0.0), dl);
    vec3 right = reconstructViewPos(uv + vec2(texel.x, 0.0), dr);
    vec3 down = reconstructViewPos(uv - vec2(0.0, texel.y), dd);
    vec3 up = reconstructViewPos(uv + vec2(0.0, texel.y), du);
    vec3 dx = abs(right.z - center.z) < abs(center.z - left.z) ? right - center : center - left;
    vec3 dy = abs(up.z - center.z) < abs(center.z - down.z) ? up - center : center - down;
    vec3 normal = normalize(cross(dx, dy));
    if (dot(normal, center) > 0.0) normal = -normal;
    return normal;
}

vec2 projectToUV(vec3 position){
    vec4 clip = ssgi.projection * vec4(position, 1.0);
    if (clip.w <= 0.0001) return vec2(-2.0);
    vec2 ndc = clip.xy / clip.w;
    return vec2(ndc.x * 0.5 + 0.5, ndcYToDepthUV(ndc.y));
}

vec2 rotate2D(vec2 value, float angle){
    float c = cos(angle);
    float s = sin(angle);
    return vec2(c * value.x - s * value.y, s * value.x + c * value.y);
}

void main(){
    float depth01 = depthAt(v_texcoord);
    vec3 source = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;
    if (depth01 >= 0.9999){
        frag_color = vec4(source, 1.0);
        return;
    }

    vec3 position = reconstructViewPos(v_texcoord, depth01);
    vec3 normal = viewNormal(v_texcoord, position);
    vec3 tangentSeed = normalize(vec3(0.7, 0.35, 0.6));
    vec3 tangent = normalize(tangentSeed - normal * dot(tangentSeed, normal));
    vec3 bitangent = normalize(cross(normal, tangent));
    int rayCount = clamp(int(ssgi.params0.y), 1, 8);
    int stepCount = clamp(int(ssgi.params0.y * 2.0), 4, 32);
    float radius = max(ssgi.params0.x, 0.01);
    float thickness = max(ssgi.params0.w, 0.001);
    float angle = ssgi.params1.z + dot(v_texcoord, vec2(41.7, 17.3));

    vec3 radiance = vec3(0.0);
    float hitWeight = 0.0;
    for (int ray = 0; ray < 8; ++ray){
        if (ray >= rayCount) break;
        float rayAngle = angle + float(ray) * 2.39996323;
        vec2 disk = rotate2D(vec2(cos(rayAngle), sin(rayAngle)), angle * 0.37);
        float z = 0.35 + 0.65 * (float(ray) + 0.5) / float(rayCount);
        vec3 rayDirection = normalize(tangent * disk.x * sqrt(max(0.0, 1.0 - z * z)) +
                                      bitangent * disk.y * sqrt(max(0.0, 1.0 - z * z)) + normal * z);

        for (int step = 1; step <= 32; ++step){
            if (step > stepCount) break;
            float distance = radius * float(step) / float(stepCount);
            vec3 rayPosition = position + rayDirection * distance;
            vec2 sampleUV = projectToUV(rayPosition);
            if (sampleUV.x < 0.001 || sampleUV.x > 0.999 || sampleUV.y < 0.001 || sampleUV.y > 0.999)
                break;

            float sampleDepth = depthAt(sampleUV);
            if (sampleDepth >= 0.9999) continue;
            vec3 surfacePosition = reconstructViewPos(sampleUV, sampleDepth);
            float depthDelta = surfacePosition.z - rayPosition.z;
            if (depthDelta > -thickness && depthDelta < radius * 0.25){
                float distanceWeight = 1.0 - distance / radius;
                float facingWeight = max(dot(normal, normalize(surfacePosition - position)), 0.0);
                float weight = distanceWeight * facingWeight;
                radiance += texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), sampleUV).rgb * weight;
                hitWeight += weight;
                break;
            }
        }
    }

    if (hitWeight > 0.001){
        vec3 indirect = radiance / hitWeight;
        float visibility = clamp(hitWeight / float(rayCount), 0.0, 1.0);
        source += indirect * (ssgi.params0.z * 0.35 * visibility);
    }
    frag_color = vec4(source, 1.0);
}
