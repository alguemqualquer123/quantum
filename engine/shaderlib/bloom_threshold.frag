#version 450

// Bright pass of the bloom chain: keeps only the pixels above a luminance threshold.
// The soft knee avoids lights popping in and out as the camera moves, which is what
// makes the difference between a bloom that reads as light and one that reads as a
// white overlay. The scene target is sRGB-encoded unless the HDR mesh variant is
// compiled in, so the decode flag below puts the whole chain into linear light.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform u_fs_bloomThresholdParams {
    vec4 params; // x = threshold, y = soft knee (0 = hard), z = 1 when the source is sRGB
} bloomThreshold;

#include "includes/srgb.glsl"

void main(){
    vec3 color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;
    if (bloomThreshold.params.z > 0.5)
        color = sRGBToLinear(color);

    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float threshold = bloomThreshold.params.x;
    float knee = bloomThreshold.params.y * threshold;
    float soft = clamp(luminance - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.00001);
    float contribution = max(soft, luminance - threshold) / max(luminance, 0.00001);

    frag_color = vec4(color * contribution, 1.0);
}
