#version 450

// FXAA (Fast Approximate Anti-Aliasing): spatial AA fallback
// Based on FXAA 3.11 by Timothy Lottes (NVIDIA)

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform u_fs_fxaaParams {
    vec4 params; // x = fxaaQualityRcpFrame.x, y = fxaaQualityRcpFrame.y, z = unused, w = unused
} fxaaParams;

// FXAA quality settings (from NVIDIA FXAA 3.11)
#define FXAA_REDUCE_MIN   (1.0/128.0)
#define FXAA_REDUCE_MUL   (1.0/8.0)
#define FXAA_SPAN_MAX     8.0

vec3 fxaa(sampler2D tex, vec2 uv, vec2 rcpFrame){
    // Sample 3x3 neighborhood
    vec3 rgbNW = texture(sampler2D(tex, uv), uv + vec2(-1.0, -1.0) * rcpFrame).rgb;
    vec3 rgbNE = texture(sampler2D(tex, uv), uv + vec2( 1.0, -1.0) * rcpFrame).rgb;
    vec3 rgbSW = texture(sampler2D(tex, uv), uv + vec2(-1.0,  1.0) * rcpFrame).rgb;
    vec3 rgbSE = texture(sampler2D(tex, uv), uv + vec2( 1.0,  1.0) * rcpFrame).rgb;
    vec3 rgbM  = texture(sampler2D(tex, uv), uv).rgb;

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

    // Sample along the edge direction
    vec3 rgbA = 0.5 * (
        texture(sampler2D(tex, uv), uv + dir * (1.0/3.0 - 0.5)).rgb +
        texture(sampler2D(tex, uv), uv + dir * (2.0/3.0 - 0.5)).rgb);

    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(sampler2D(tex, uv), uv + dir * -0.5).rgb +
        texture(sampler2D(tex, uv), uv + dir *  0.5).rgb);

    float lumaB = dot(rgbB, luma);

    // Select the better sample
    if (lumaB < lumaMin || lumaB > lumaMax){
        return rgbA;
    } else {
        return rgbB;
    }
}

void main(){
    vec2 rcpFrame = fxaaParams.params.xy;
    vec3 color = fxaa(u_sceneColor_smp, v_texcoord, rcpFrame);
    frag_color = vec4(color, 1.0);
}
