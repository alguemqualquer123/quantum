#version 450

// DOF circle of confusion: one tap of the shared depth pre-pass becomes a normalized
// blur radius, written in R (G holds the eye distance for debugging). The value is in
// [0,1] of screen height, so the blur pass converts it to texels with its own size.
//
// Orientation: this pass writes in scene-color space, while the depth target keeps the
// pre-pass's native orientation, so on GL the depth lookup needs the Y flip (params1.x),
// exactly like ssr.frag's misc.z.
//
// Distance: the pre-pass packs 0.5*z/w + 0.5 taken before the API clip-space fixup, so
// [-1,1] always matches invProjection. The eye position is recovered from that, and the
// radial length is what the focus plane compares against (a flat z would blur the corners
// of a wall unevenly).

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_dofCocParams {
    mat4 invProjection;
    vec4 params0; // x = focusDistance, y = aperture, z = focusRange, w = maxBlur
    vec4 params1; // x = flip depth<->color Y, yz = unused, w = unused
} dofCoc;

#include "includes/depth_util.glsl"

void main(){
    vec2 duv = vec2(v_texcoord.x, dofCoc.params1.x > 0.5 ? 1.0 - v_texcoord.y : v_texcoord.y);
    float depth01 = decodeDepth(textureLod(sampler2D(u_depthTexture, u_depth_smp), duv, 0.0));

    vec3 ndc = vec3(duv.x * 2.0 - 1.0, depthUVToNDCY(duv.y), depth01 * 2.0 - 1.0);
    vec4 view = dofCoc.invProjection * vec4(ndc, 1.0);

    // Nothing in front of the near plane: leave it sharp instead of dividing by a zero w.
    if (view.w <= 0.0) {
        frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float eyeDistance = length(view.xyz / view.w);

    float focusDistance = max(dofCoc.params0.x, 0.001);
    float aperture = dofCoc.params0.y;
    float focusRange = max(dofCoc.params0.z, 0.0);
    float maxBlur = clamp(dofCoc.params0.w, 0.0, 1.0);

    // Out-of-focus amount grows with the distance from the in-focus band, measured against
    // the focus distance itself so the same setting behaves at 2 m and at 200 m. An
    // aperture of 1 is the neutral reference; larger apertures reach maxBlur sooner.
    float spread = max(abs(eyeDistance - focusDistance) - focusRange, 0.0);
    float coc = clamp((spread / focusDistance) * (aperture * 0.5), 0.0, 1.0) * maxBlur;

    frag_color = vec4(coc, eyeDistance * 0.01, 0.0, 1.0);
}
