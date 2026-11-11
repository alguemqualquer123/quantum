#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D u_sceneColorTexture;
layout(set = 0, binding = 1) uniform sampler3D u_lut3DTexture;

layout(set = 1, binding = 0) uniform fs_colorgrading_params {
    vec4 params;  // x = saturation, y = contrast, z = brightness, w = hue shift
    vec4 params2; // x = temperature, y = tint, z = has LUT
} u_fs_colorgrading;

// RGB to HSV
vec3 rgb2hsv(vec3 c){
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// HSV to RGB
vec3 hsv2rgb(vec3 c){
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main(){
    vec3 color = texture(u_sceneColorTexture, uv).rgb;

    float saturation = u_fs_colorgrading.params.x;
    float contrast = u_fs_colorgrading.params.y;
    float brightness = u_fs_colorgrading.params.z;
    float hueShift = u_fs_colorgrading.params.w;
    float temperature = u_fs_colorgrading.params2.x;
    float tint = u_fs_colorgrading.params2.y;
    float hasLUT = u_fs_colorgrading.params2.z;

    // Apply temperature (warm/cool shift)
    color.r += temperature * 0.1;
    color.b -= temperature * 0.1;

    // Apply tint (green/magenta shift)
    color.g += tint * 0.1;

    // Apply brightness
    color += brightness;

    // Apply contrast (around midpoint 0.5)
    color = (color - 0.5) * contrast + 0.5;

    // Apply hue shift and saturation via HSV
    vec3 hsv = rgb2hsv(color);
    hsv.x = fract(hsv.x + hueShift / 360.0);
    hsv.y *= saturation;
    color = hsv2rgb(hsv);

    // 3D LUT color grading (if available)
    if (hasLUT > 0.5){
        vec3 lutCoord = clamp(color, vec3(0.0), vec3(1.0));
        // Remap to LUT texel center (assuming 64x64 or 32x32 LUT)
        lutCoord = lutCoord * 0.5 + 0.5; // scale from [-1,1] range doesn't apply; we're in [0,1]
        vec3 lutColor = texture(u_lut3DTexture, lutCoord).rgb;
        color = mix(color, lutColor, 1.0); // full replacement when LUT is provided
    }

    color = clamp(color, vec3(0.0), vec3(1.0));
    fragColor = vec4(color, 1.0);
}
