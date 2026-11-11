#version 450

// Generates motion vectors from the packed camera depth pre-pass. Keeping this fullscreen
// means every opaque material shares the same reprojection path.

in vec2 v_texcoord;

out vec4 frag_color;

uniform u_fs_velocityParams {
    mat4 prevViewProjection;
    mat4 invCurrentViewProjection;
    vec4 params; // x = velocityScale, y = velocityClamp, z = unused, w = unused
} velocityParams;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

#include "includes/depth_util.glsl"

void main(){
    float depth01 = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord));
    vec3 ndc = vec3(v_texcoord.x * 2.0 - 1.0, depthUVToNDCY(v_texcoord.y), depth01 * 2.0 - 1.0);
    vec4 currentClip = vec4(ndc, 1.0);

    // Previous frame world-space position (reproject using the current inverse VP)
    vec4 worldPos = velocityParams.invCurrentViewProjection * currentClip;
    worldPos /= max(abs(worldPos.w), 1e-6);
    vec4 prevClip = velocityParams.prevViewProjection * worldPos;

    // Convert to screen-space motion vectors
    vec2 currentNDC = currentClip.xy / currentClip.w;
    vec2 prevNDC = prevClip.xy / prevClip.w;

    // Motion vector (difference in NDC space)
    vec2 velocity = (currentNDC - prevNDC) * 0.5;

    if (depth01 >= 1.0){
        frag_color = vec4(0.5, 0.5, 0.0, 1.0);
        return;
    }

    // Scale and clamp
    float scale = velocityParams.params.x;
    float clampVal = velocityParams.params.y;
    velocity *= scale;

    // Clamp maximum velocity (prevents artifacts from fast camera movement)
    float velLength = length(velocity);
    if (velLength > clampVal){
        velocity = velocity / velLength * clampVal;
    }

    // Encode motion vector into RG channels (128 = 0 motion)
    // This allows negative motion vectors to be stored in unsigned formats
    frag_color = vec4(velocity * 0.5 + 0.5, 0.0, 1.0);
}
