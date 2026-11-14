#version 450

// DOF blur: one axis of a separable Gaussian, run at half resolution over the sharp
// scene color and weighted by the circle of confusion. The CoC of the centre pixel sets
// the tap spread, so in-focus pixels keep their own value and the blur never bleeds out
// of the out-of-focus regions.
//
// Every radius here is expressed in fractions of image height, which is what the CoC
// already is: the horizontal axis just multiplies by the aspect (params.x) so a disc
// stays a disc. No texel maths, and the same setting behaves at any resolution.
//
// The tap count is a compile-time constant (DOF_BLUR_TAPS, defined by the shader build
// the way SSR_MAX_STEPS is) and the loop bound never comes from a uniform: the radius
// travels in the tap *spacing* instead, which is what a fixed-count separable blur is for.
//
// Orientation: the CoC texture is written in scene-color space (dof_coc.frag flips when it
// reads the depth pre-pass), and all three textures here are offscreen targets, so this
// pass needs no flip of its own.

#ifndef DOF_BLUR_TAPS
#define DOF_BLUR_TAPS 9
#endif

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_depthTexture; // the CoC texture
uniform sampler u_depth_smp;

uniform u_fs_dofBlurParams {
    vec4 params; // x = aspect (width/height), y = direction (0 = horizontal, 1 = vertical), z = radius cap, w = blur height in pixels
} dofBlur;

void main(){
    vec3 centerColor = textureLod(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord, 0.0).rgb;
    float centerCoC = textureLod(sampler2D(u_depthTexture, u_depth_smp), v_texcoord, 0.0).r;

    float radius = min(centerCoC, dofBlur.params.z);
    vec2 axis = dofBlur.params.y > 0.5 ? vec2(0.0, radius) : vec2(radius * dofBlur.params.x, 0.0);

    // Under half a pixel of the blur target the taps land on the centre: answer with the
    // sharp colour instead of spending the window.
    if (length(axis) * dofBlur.params.w <= 0.5) {
        frag_color = vec4(centerColor, 1.0);
        return;
    }

    int halfTaps = DOF_BLUR_TAPS / 2;
    vec3 sum = centerColor;
    float weightSum = 1.0;

    for (int i = 1; i <= halfTaps; ++i) {
        float t = float(i) / float(halfTaps);
        float weight = exp(-0.5 * (t / 0.45) * (t / 0.45));
        vec2 offset = axis * t;

        vec2 leftUV = v_texcoord - offset;
        vec2 rightUV = v_texcoord + offset;

        vec3 left = textureLod(sampler2D(u_sceneColorTexture, u_sceneColor_smp), leftUV, 0.0).rgb;
        vec3 right = textureLod(sampler2D(u_sceneColorTexture, u_sceneColor_smp), rightUV, 0.0).rgb;

        // A neighbour much sharper than the centre must not drag its detail into the blur.
        float leftCoC = textureLod(sampler2D(u_depthTexture, u_depth_smp), leftUV, 0.0).r;
        float rightCoC = textureLod(sampler2D(u_depthTexture, u_depth_smp), rightUV, 0.0).r;
        float leftGate = step(leftCoC * 0.5, centerCoC);
        float rightGate = step(rightCoC * 0.5, centerCoC);

        sum += (left * leftGate + right * rightGate) * weight;
        weightSum += (leftGate + rightGate) * weight;
    }

    frag_color = vec4(sum / weightSum, 1.0);
}
