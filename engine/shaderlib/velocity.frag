#version 450

// Velocity Buffer: generates per-pixel motion vectors for temporal effects
// Used by TAA, motion blur, and temporal reprojection

in vec4 v_position;
in vec2 v_texcoord;

out vec4 frag_color;

uniform u_fs_velocityParams {
    mat4 prevViewProjection;
    mat4 currentViewProjection;
    vec4 params; // x = velocityScale, y = velocityClamp, z = unused, w = unused
} velocityParams;

void main(){
    // Current frame clip-space position
    vec4 currentClip = v_position;

    // Previous frame world-space position (reproject using inverse current VP)
    vec4 worldPos = inverse(velocityParams.currentViewProjection) * currentClip;
    vec4 prevClip = velocityParams.prevViewProjection * worldPos;

    // Convert to screen-space motion vectors
    vec2 currentNDC = currentClip.xy / currentClip.w;
    vec2 prevNDC = prevClip.xy / prevClip.w;

    // Motion vector (difference in NDC space)
    vec2 velocity = (currentNDC - prevNDC) * 0.5;

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
