#version 450

// FXAA (Fast Approximate Anti-Aliasing): spatial AA fallback
// Based on FXAA 3.11 by Timothy Lottes (NVIDIA)
//
// Runs on the finished image, after the tone map, so it sees display-encoded pixels and no
// linear-space assumption is made here. The taps are in texel units of this target, which is
// presented at the display resolution: anti-aliasing a fixed-resolution frame has to happen
// after the upscale or the edges it blurs are the upscale's own.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform u_fs_fxaaParams {
    vec4 params; // x = 1/width, y = 1/height, z = blend of the AA result [0..1], w = flip source Y
} fxaaParams;

// FXAA quality settings (from FXAA 3.11)
#define FXAA_REDUCE_MIN   (1.0/128.0)
#define FXAA_REDUCE_MUL   (1.0/8.0)
#define FXAA_SPAN_MAX     8.0

vec3 fxaa(vec2 uv, vec2 rcpFrame){
    // Sample 3x3 neighborhood
    vec3 rgbNW = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv + vec2(-1.0, -1.0) * rcpFrame).rgb;
    vec3 rgbNE = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv + vec2( 1.0, -1.0) * rcpFrame).rgb;
    vec3 rgbSW = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv + vec2(-1.0,  1.0) * rcpFrame).rgb;
    vec3 rgbSE = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv + vec2( 1.0,  1.0) * rcpFrame).rgb;
    vec3 rgbM  = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv).rgb;

    // Luminance for edge detection
    const vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM  = dot(rgbM, luma);

    // Find edge direction
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * FXAA_REDUCE_MUL, FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(FXAA_SPAN_MAX), max(vec2(-FXAA_SPAN_MAX), dir * rcpDirMin)) * rcpFrame;

    vec3 rgbA = 0.5 * (
        texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv + dir * (1.0/3.0 - 0.5)).rgb +
        texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv + dir * (2.0/3.0 - 0.5)).rgb);

    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv + dir * -0.5).rgb +
        texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv + dir *  0.5).rgb);

    // Keep the sharper result when the along-edge sample leaves the local luminance range
    float lumaB = dot(rgbB, luma);
    if (lumaB < lumaMin || lumaB > lumaMax){
        return rgbA;
    }
    return rgbB;
}

void main(){
    vec2 uv = v_texcoord;
    if (fxaaParams.params.w > 0.5){
        uv.y = 1.0 - uv.y;
    }

    vec3 aa = fxaa(uv, fxaaParams.params.xy);
    // Intensity 1 is the full effect; below it the pass fades toward its own input, so the
    // sharpness loss can be traded against the jaggies per scene instead of being a toggle.
    frag_color = vec4(mix(texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv).rgb, aa,
                          clamp(fxaaParams.params.z, 0.0, 1.0)), 1.0);
}
