#version 450

// Atmosphere: analytic exponential height fog plus screen-space light shafts, in one
// fullscreen pass over the HDR scene color.
//
// Why a pass and not the per-pixel fog of mesh.frag: that fog is applied per material and
// never sees the skybox, so a foggy scene keeps a clear horizon and anything that does not
// opt in stays crisp. Integrating the density along the ray gives one consistent aerial
// perspective across the whole frame, and the shafts fall out of the same march - a god ray
// is fog lit by the sun where something nearer along the ray hides that sun.
//
// The fog is not marched. For d(y) = D * exp(-(y - ground) / H) along a ray of elevation
// phi the optical depth has the closed form
//     D * H / sin(phi) * (1 - exp(-L * sin(phi) / H)) * exp(-(eyeY - ground) / H)
// which is exact for any distance and costs one exp. Only the shafts sample.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_atmosphereParams {
    mat4 invViewProjection;
    vec4 cameraPos;     // xyz world position, w = 1 whenever the pass runs
    vec4 sunDirection;  // xyz unit vector toward the sun, w = its intensity (0 hides the sun term)
    vec4 sunScreen;     // xy = sun UV in the depth target's own orientation, zw = unused
    vec4 fog0;          // x = density, y = scale height, z = ground Y, w = far range
    vec4 fog1;          // x = sun amount, y = scattering g, z = height fog on, w = depth<->color Y flip
    vec4 fogColor;      // rgb = linear ambient in-scatter
    vec4 sunColor;      // rgb = linear sun in-scatter
    vec4 fogNoise;      // x = scale, y = strength, z = speed, w = enabled
    vec4 fogWind;       // x/z = wind vector, w = animation time
    vec4 fogVolume;     // x = enabled, y = ray steps, z = intensity, w = jitter time
    vec4 rays0;         // x = intensity, y = bright-pass threshold, z = softness (m), w = per-sample decay
    vec4 rays1;         // x = marched fraction, y = shafts on, zw = unused
    vec4 clouds0;       // x = base height, y = thickness, z = coverage, w = density
    vec4 clouds1;       // x = noise scale, y = wind speed, z = sun amount, w = clouds on
    vec4 cloudWind;     // x/z = world XZ wind direction and magnitude
    vec4 cloudColor;    // rgb = linear albedo, w = animation time
} atmosphere;

#include "includes/depth_util.glsl"

// The march is fixed-length so the loop unrolls; rays1.x scales the step instead of the
// count, which is what keeps "density" a look knob rather than a quality knob.
#define RAY_SAMPLES 24
#define CLOUD_SAMPLES 16

// The depth target keeps the pre-pass's native orientation while this pass writes in
// scene-color space, so the two UVs differ by a Y flip on GL (same flag ssr.frag uses).
// The map is its own inverse, so it also converts the sun's NDC into scene-color space.
vec2 flipDepthUV(vec2 uv){
    return atmosphere.fog1.w > 0.5 ? vec2(uv.x, 1.0 - uv.y) : uv;
}

vec2 depthUVClamped(vec2 colorUV){
    return clamp(flipDepthUV(colorUV), vec2(0.0), vec2(1.0));
}

// Eye distance of whatever the depth buffer holds at this scene-color UV, plus its world
// position. Returns 0 when nothing projects in front of the eye.
float sampleEye(vec2 colorUV, out vec3 world){
    vec2 duv = depthUVClamped(colorUV);
    float depth01 = decodeDepth(textureLod(sampler2D(u_depthTexture, u_depth_smp), duv, 0.0));
    vec3 ndc = vec3(duv.x * 2.0 - 1.0, depthUVToNDCY(duv.y), depth01 * 2.0 - 1.0);
    vec4 unprojected = atmosphere.invViewProjection * vec4(ndc, 1.0);

    if (unprojected.w <= 0.0){
        world = atmosphere.cameraPos.xyz;
        return 0.0;
    }

    world = unprojected.xyz / unprojected.w;
    return length(world - atmosphere.cameraPos.xyz);
}

// Optical depth along the ray, in the [0, 20] range that still means something: exp(-20) is
// already 2e-9, and the looking-down branch would overflow a float32 exponent long before
// that, so the far range caps the path and the clamp caps the result.
float fogHash(vec2 p){
    p = fract(p * vec2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return fract((p.x + p.y) * p.x * p.y);
}

float fogNoiseValue(vec2 p){
    vec2 cell = floor(p);
    vec2 local = fract(p);
    local = local * local * (3.0 - 2.0 * local);
    float a = fogHash(cell);
    float b = fogHash(cell + vec2(1.0, 0.0));
    float c = fogHash(cell + vec2(0.0, 1.0));
    float d = fogHash(cell + vec2(1.0, 1.0));
    return mix(mix(a, b, local.x), mix(c, d, local.x), local.y);
}

float fogFBM(vec2 p){
    float value = 0.0;
    float amplitude = 0.5;
    for (int octave = 0; octave < 4; ++octave){
        value += fogNoiseValue(p) * amplitude;
        p = p * 2.02 + vec2(11.7, 7.3);
        amplitude *= 0.5;
    }
    return value / 0.9375;
}

float heightFogColumn(float eyeDistance, float sinPhi, float cameraY, vec3 samplePosition){
    float scaleHeight = max(atmosphere.fog0.y, 0.001);
    float path = min(eyeDistance, max(atmosphere.fog0.w, scaleHeight));

    float base = exp(-(cameraY - atmosphere.fog0.z) / scaleHeight);
    float column;

    if (abs(sinPhi) < 1e-4){
        column = atmosphere.fog0.x * path * base;
    }else{
        float x = clamp(path * sinPhi / scaleHeight, -40.0, 40.0);
        column = atmosphere.fog0.x * scaleHeight / sinPhi * (1.0 - exp(-x)) * base;
    }

    if (atmosphere.fogNoise.w > 0.5){
        vec2 wind = atmosphere.fogWind.xz * atmosphere.fogWind.w * atmosphere.fogNoise.z;
        vec2 noiseUV = samplePosition.xz * max(atmosphere.fogNoise.x, 0.00001) + wind;
        float variation = fogFBM(noiseUV) * 2.0 - 1.0;
        column *= 1.0 + variation * clamp(atmosphere.fogNoise.y, 0.0, 1.0);
    }

    return clamp(column, 0.0, 20.0);
}

float heightFogDensity(vec3 world){
    float scaleHeight = max(atmosphere.fog0.y, 0.001);
    float density = atmosphere.fog0.x * exp(-max(world.y - atmosphere.fog0.z, 0.0) / scaleHeight);
    if (atmosphere.fogNoise.w > 0.5){
        vec2 wind = atmosphere.fogWind.xz * atmosphere.fogWind.w * atmosphere.fogNoise.z;
        vec2 noiseUV = world.xz * max(atmosphere.fogNoise.x, 0.00001) + wind;
        float variation = fogFBM(noiseUV) * 2.0 - 1.0;
        density *= 1.0 + variation * clamp(atmosphere.fogNoise.y, 0.0, 1.0);
    }
    return max(density, 0.0);
}

// Henyey-Greenstein, divided by its own dominant-lobe peak so the whole function sits in
// [0,1]: `fogSunAmount` then stays the 0..1 look knob it is documented as instead of a
// multiplier that reaches 28 at g = 0.75 and pushes the in-scatter past the tone map. The
// peak is taken from |g| because a negative g puts the lobe behind the surface instead.
float scatteringPhase(float g, float cosTheta){
    g = clamp(g, -0.99, 0.99);
    float a = abs(g);
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    float peak = (1.0 + a) / max((1.0 - a) * (1.0 - a), 1e-4);
    return ((1.0 - g2) / pow(denom, 1.5)) / peak;
}

vec4 marchVolumetricFog(vec3 rayDirection, float distanceToScene){
    float maxDistance = min(max(distanceToScene, 0.001), max(atmosphere.fog0.w, 0.001));
    int sampleCount = clamp(int(atmosphere.fogVolume.y + 0.5), 4, 64);
    float stepLength = maxDistance / float(sampleCount);
    float jitter = fogHash(gl_FragCoord.xy + vec2(atmosphere.fogVolume.w)) - 0.5;
    float phase = scatteringPhase(atmosphere.fog1.y,
        dot(rayDirection, normalize(atmosphere.sunDirection.xyz)));
    vec3 ambient = atmosphere.fogColor.rgb * (0.65 + 0.35 * phase);
    vec3 sun = atmosphere.sunColor.rgb * atmosphere.sunDirection.w * atmosphere.fog1.x * phase;
    vec3 scattering = vec3(0.0);
    float transmittance = 1.0;

    for (int i = 0; i < 64; ++i){
        if (i >= sampleCount)
            break;
        float t = (float(i) + 0.5 + jitter) * stepLength;
        vec3 samplePosition = atmosphere.cameraPos.xyz + rayDirection * t;
        float density = heightFogDensity(samplePosition);
        float opticalDepth = density * stepLength;
        float sliceTransmittance = exp(-opticalDepth);
        float sliceAlpha = 1.0 - sliceTransmittance;

        // The ambient term provides a bounded approximation of multiple scattering;
        // direct sun scattering keeps the anisotropic lobe visible in dense haze.
        vec3 inScatter = ambient + sun;
        scattering += inScatter * sliceAlpha * transmittance;
        transmittance *= sliceTransmittance;
        if (transmittance < 0.001)
            break;
    }
    return vec4(scattering, transmittance);
}

// Only the sun and the sky around it are bright enough to make a shaft; in a linear HDR
// buffer the threshold is a plain number, and 1.0 sits right where the sky starts.
vec3 brightPass(vec3 color){
    float threshold = max(atmosphere.rays0.y, 1e-4);
    float luminance = max(max(color.r, color.g), color.b);
    return color * clamp((luminance - threshold) / threshold, 0.0, 1.0);
}

// March from the sun toward this pixel and accumulate the bright samples, killing the
// accumulation wherever a step holds geometry nearer than this pixel's own surface - that
// step is an occluder between the sun and the pixel, and the feather over `softness`
// metres is what turns a hard silhouette into a shaft with penumbral edges.
vec3 marchShafts(vec2 sunColorUV, float eyeDistance){
    vec2 step = (v_texcoord - sunColorUV) / float(RAY_SAMPLES) * atmosphere.rays1.x;
    vec2 uv = sunColorUV;

    vec3 sum = vec3(0.0);
    float decay = 1.0;
    float softness = max(atmosphere.rays0.z, 1e-3);

    for (int i = 0; i < RAY_SAMPLES; i++){
        uv += step;

        vec3 occluderPos;
        float occluderEye = sampleEye(uv, occluderPos);
        float blocked = clamp((eyeDistance - occluderEye) / softness, 0.0, 1.0);

        sum += brightPass(textureLod(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv, 0.0).rgb) * decay;

        decay *= atmosphere.rays0.w * (1.0 - blocked);
        if (decay < 1e-4)
            break;
    }

    return sum * atmosphere.rays0.x / float(RAY_SAMPLES);
}

float cloudHash(vec2 p){
    p = fract(p * vec2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return fract((p.x + p.y) * p.x * p.y);
}

float cloudNoise(vec2 p){
    vec2 cell = floor(p);
    vec2 local = fract(p);
    local = local * local * (3.0 - 2.0 * local);
    float a = cloudHash(cell);
    float b = cloudHash(cell + vec2(1.0, 0.0));
    float c = cloudHash(cell + vec2(0.0, 1.0));
    float d = cloudHash(cell + vec2(1.0, 1.0));
    return mix(mix(a, b, local.x), mix(c, d, local.x), local.y);
}

float cloudFBM(vec2 p){
    float value = 0.0;
    float amplitude = 0.5;
    for (int octave = 0; octave < 5; ++octave){
        value += cloudNoise(p) * amplitude;
        p = p * 2.03 + vec2(17.13, 9.71);
        amplitude *= 0.5;
    }
    return value;
}

float cloudLayerDensity(vec3 world){
    float base = atmosphere.clouds0.x;
    float thickness = max(atmosphere.clouds0.y, 0.001);
    float top = base + thickness;
    float lower = smoothstep(base, base + thickness * 0.12, world.y);
    float upper = 1.0 - smoothstep(base + thickness * 0.62, top, world.y);
    float vertical = lower * upper;
    if (vertical <= 0.0)
        return 0.0;

    vec2 wind = atmosphere.cloudWind.xz * atmosphere.cloudColor.w * atmosphere.clouds1.y;
    vec2 noiseUV = world.xz * max(atmosphere.clouds1.x, 0.00001) + wind;
    float noise = cloudFBM(noiseUV);
    float coverage = clamp(atmosphere.clouds0.z, 0.0, 0.99);
    float shape = smoothstep(coverage, min(coverage + 0.32, 1.0), noise);
    return shape * vertical * max(atmosphere.clouds0.w, 0.0);
}

float cloudSunVisibility(vec3 world, vec3 sunDirection){
    float visibility = 1.0;
    // A short fixed shadow ray gives cloud tops and undersides depth without requiring a
    // second 3D texture. The step length is in world metres and intentionally independent
    // of the camera ray length, so distant clouds do not become unnaturally flat.
    for (int i = 1; i <= 3; ++i){
        vec3 samplePosition = world + sunDirection * (18.0 * float(i));
        visibility -= cloudLayerDensity(samplePosition) * (0.075 / float(i));
    }
    return clamp(visibility, 0.12, 1.0);
}

vec4 marchClouds(vec3 rayDirection, float distanceToScene){
    float marchDistance = min(max(distanceToScene, 1.0), max(atmosphere.fog0.w, 1.0));
    float stepLength = marchDistance / float(CLOUD_SAMPLES);
    float transmittance = 1.0;
    vec3 accumulated = vec3(0.0);
    vec3 sunDir = normalize(atmosphere.sunDirection.xyz);
    float sunPhase = 0.5 + 0.5 * dot(rayDirection, sunDir);
    vec3 ambientLight = atmosphere.cloudColor.rgb * (0.55 + 0.45 * sunPhase);
    vec3 sunLight = atmosphere.sunColor.rgb * atmosphere.sunDirection.w * atmosphere.clouds1.z *
                    (0.35 + 0.65 * sunPhase);

    for (int i = 0; i < CLOUD_SAMPLES; ++i){
        float t = (float(i) + 0.5) * stepLength;
        vec3 samplePosition = atmosphere.cameraPos.xyz + rayDirection * t;
        float density = cloudLayerDensity(samplePosition);
        float alpha = 1.0 - exp(-density * stepLength * 0.018);
        vec3 light = ambientLight + sunLight * cloudSunVisibility(samplePosition, sunDir);
        accumulated += light * alpha * transmittance;
        transmittance *= 1.0 - alpha;
        if (transmittance < 0.002)
            break;
    }

    return vec4(accumulated, transmittance);
}

void main(){
    vec3 scene = textureLod(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord, 0.0).rgb;
    frag_color = vec4(scene, 1.0);

    vec3 world;
    float eyeDistance = sampleEye(v_texcoord, world);
    if (eyeDistance <= 0.0)
        return;

    float transmittance = 1.0;

    if (atmosphere.fog1.z > 0.5){
        vec3 viewDir = (world - atmosphere.cameraPos.xyz) / eyeDistance;
        if (atmosphere.fogVolume.x > 0.5){
            vec4 volumetric = marchVolumetricFog(viewDir, eyeDistance);
            transmittance = volumetric.a;
            frag_color.rgb = scene * transmittance + volumetric.rgb * atmosphere.fogVolume.z;
        }else{
            float sinPhi = (world.y - atmosphere.cameraPos.y) / eyeDistance;
            float column = heightFogColumn(eyeDistance, sinPhi, atmosphere.cameraPos.y, world);
            transmittance = exp(-column);
            float phase = scatteringPhase(atmosphere.fog1.y, dot(viewDir, normalize(atmosphere.sunDirection.xyz)));
            vec3 inScatter = atmosphere.fogColor.rgb
                + atmosphere.sunColor.rgb * atmosphere.sunDirection.w * phase * atmosphere.fog1.x;
            frag_color.rgb = scene * transmittance + inScatter * (1.0 - transmittance);
        }
    }

    if (atmosphere.rays1.y > 0.5){
        // The CPU clamps the sun into the frame, so an off-screen sun still lights the
        // shafts from the right side instead of marching across a clamped edge column.
        vec2 sunColorUV = flipDepthUV(clamp(vec2(atmosphere.sunScreen.x, atmosphere.sunScreen.y), vec2(0.0), vec2(1.0)));

        // Shafts are scattering too: in fog they belong to the fog (lit air you can see
        // through), and with the fog off they still add, which is the clear-air glow the
        // unwired lightshafts.frag was reaching for.
        float shaftWeight = atmosphere.fog1.z > 0.5 ? clamp(1.0 - transmittance + 0.1, 0.0, 1.0) : 1.0;
        frag_color.rgb += marchShafts(sunColorUV, eyeDistance) * shaftWeight;
    }

    if (atmosphere.clouds1.w > 0.5){
        vec3 viewDirection = normalize(world - atmosphere.cameraPos.xyz);
        vec4 cloudLight = marchClouds(viewDirection, eyeDistance);
        frag_color.rgb = frag_color.rgb * cloudLight.a + cloudLight.rgb;
    }
}
