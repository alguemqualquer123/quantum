#version 450

// Maps the HDR scene color (and the bloom added on top of it) into the 8-bit
// swapchain. Runs last of the HDR chain, so the curves below see linear values and
// the sRGB encode is done here rather than by the framebuffer.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

// Written by the 1x1 adaptation pass. Only read when grade2.w says auto exposure is on, but it is
// always bound so the pipeline never sees an unattached sampler.
uniform texture2D u_exposureTexture;
uniform sampler u_exposure_smp;

// A strip LUT is uploaded as a texture2DArray: each array layer is one blue
// slice of the conventional size*size by size strip layout.
uniform texture2DArray u_lut3DTexture;
uniform sampler u_lut3DTexture_smp;

uniform u_fs_tonemapParams {
    vec4 params; // x = exposure, y = mode, z = white point, w = flip source Y (GL swapchain)
    vec4 grade;  // x = saturation, y = contrast, z = brightness, w = hue shift in degrees
    vec4 grade2; // x = temperature, y = tint, z = 1 when grading is on, w = 1 when exposure comes from u_exposureTexture
    vec4 lut;    // x = enabled, y = LUT edge length
} tonemap;

#include "includes/srgb.glsl"

// ACES fitted curve (Stephen Hill)
vec3 ACESFilm(vec3 x){
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 Reinhard(vec3 x){
    return x / (1.0 + x);
}

vec3 ReinhardExtended(vec3 x, float maxWhite){
    return (x * (1.0 + x / (maxWhite * maxWhite))) / (1.0 + x);
}

vec3 Uncharted2Partial(vec3 x){
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 Uncharted2(vec3 x){
    vec3 curr = Uncharted2Partial(x * 2.0);
    vec3 whiteScale = vec3(1.0) / Uncharted2Partial(vec3(11.2));
    return curr * whiteScale;
}

// AgX display transform. The contrast approximation is the same smooth S-curve
// used by the reference implementation, but kept in this shader's linear-light
// input/output convention so it remains compatible with the existing grade/LUT path.
vec3 agxContrastApprox(vec3 x){
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

vec3 AgX(vec3 x){
    const mat3 agxIn = mat3(
        0.842479062253094, 0.0423526992193578, 0.0423526992193578,
        0.0784335999999992, 0.878468636469772, 0.0784335999999992,
        0.0282895236084510, 0.0282895236084510, 0.878468636469772
    );
    const mat3 agxOut = mat3(
         1.19687900512017, -0.0528968517574562, -0.0528968517574562,
        -0.0980208811401368,  1.15190312990417, -0.0980208811401368,
        -0.0990297440797205, -0.0990297440797205,  1.15755838811751
    );
    vec3 logColor = log2(max(agxIn * max(x, vec3(0.0)), vec3(1e-6)));
    vec3 encoded = clamp((logColor + 10.0) / 16.0, 0.0, 1.0);
    return clamp(agxOut * agxContrastApprox(encoded), 0.0, 1.0);
}

// ITU-R BT.709 luminance, the same weighting the adaptation pass measures with
float getLuminance(vec3 color){
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// Rotation about the luma axis (Graphics Gems). Built column-major on purpose: GLSL takes the
// vec3 arguments as columns, and the classic table above is written row-wise.
vec3 hueRotate(vec3 color, float degrees){
    float rad = radians(degrees);
    float cs = cos(rad);
    float sn = sin(rad);
    mat3 m = mat3(
        vec3(0.299 + 0.701 * cs + 0.167 * sn, 0.299 - 0.299 * cs - 0.328 * sn, 0.299 - 0.300 * cs + 1.250 * sn),
        vec3(0.587 - 0.587 * cs + 0.330 * sn, 0.587 + 0.413 * cs + 0.035 * sn, 0.587 - 0.586 * cs - 1.050 * sn),
        vec3(0.114 - 0.114 * cs - 0.497 * sn, 0.114 - 0.114 * cs + 0.292 * sn, 0.114 + 0.886 * cs - 0.203 * sn)
    );
    return m * color;
}

// White balance. Warm pushes red up and blue down, cool the reverse, tint moves the green channel
// against magenta; both are gains around 1.0 so a neutral scene is exactly unchanged.
vec3 whiteBalance(vec3 color, float temperature, float tint){
    float warm = max(temperature, 0.0);
    float cool = max(-temperature, 0.0);
    color *= vec3(1.0 + 0.30 * warm - 0.30 * cool,
                  1.0 - 0.06 * abs(temperature),
                  1.0 - 0.30 * warm + 0.30 * cool);
    color.g *= 1.0 - 0.20 * tint;
    return color;
}

void main(){
    vec2 uv = v_texcoord;
    if (tonemap.params.w > 0.5) {
        uv.y = 1.0 - uv.y;
    }

    int mode = int(tonemap.params.y);
    vec3 color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv).rgb;

    float exposureValue = tonemap.params.x;
    if (tonemap.grade2.w > 0.5){
        float adapted = texture(sampler2D(u_exposureTexture, u_exposure_smp), vec2(0.5)).x;
        if (adapted > 0.0)
            exposureValue = adapted;
    }
    color *= exposureValue;

    vec3 mapped;
    if (mode < 0){
        // The chain also carries grading and the adapted exposure, so it runs with the curve off.
        // A scene that did not ask for a tone map keeps its authored response.
        mapped = clamp(color, 0.0, 1.0);
    } else if (mode == 0){
        mapped = ACESFilm(color);
    } else if (mode == 1){
        mapped = Reinhard(color);
    } else if (mode == 2){
        mapped = ReinhardExtended(color, max(tonemap.params.z, 1.0));
    } else if (mode == 3){
        mapped = Uncharted2(color);
    } else {
        mapped = AgX(color);
    }

    if (tonemap.grade2.z > 0.5){
        // Grading runs after the curve but before the encode, in the display-referred linear
        // range: contrast and saturation have to see values a viewer's eye reads as proportional,
        // and doing it after the sRGB multiply would make a mid-grey pivot land in the wrong place.
        mapped = whiteBalance(mapped, tonemap.grade2.x, tonemap.grade2.y);
        mapped = hueRotate(mapped, tonemap.grade.w);
        mapped *= exp2(tonemap.grade.z);
        mapped = (mapped - 0.5) * tonemap.grade.y + 0.5;
        float luma = getLuminance(mapped);
        mapped = mix(vec3(luma), mapped, tonemap.grade.x);
    }

    if (tonemap.lut.x > 0.5){
        int lutSize = int(tonemap.lut.y + 0.5);
        if (lutSize >= 2){
            vec3 coordinate = clamp(mapped, vec3(0.0), vec3(1.0)) * float(lutSize - 1);
            float lowSlice = floor(coordinate.z);
            float highSlice = min(lowSlice + 1.0, float(lutSize - 1));
            float sliceBlend = fract(coordinate.z);
            vec2 lutUV = (coordinate.xy + vec2(0.5)) / float(lutSize);
            vec3 low = texture(sampler2DArray(u_lut3DTexture, u_lut3DTexture_smp), vec3(lutUV, lowSlice)).rgb;
            vec3 high = texture(sampler2DArray(u_lut3DTexture, u_lut3DTexture_smp), vec3(lutUV, highSlice)).rgb;
            mapped = mix(low, high, sliceBlend);
        }
    }

    frag_color = vec4(linearTosRGB(clamp(mapped, 0.0, 1.0)), 1.0);
}
