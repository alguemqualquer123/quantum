#version 450

// Procedural day/night sky. The vertex side is the built-in sky's, unchanged: the engine applies
// the sky's view-projection matrix to the u_vs_skyParams block by name, so a fork that renames or
// drops it draws the dome with a garbage transform instead of failing to load.

in vec3 a_position;

out vec3 uv;

uniform u_vs_skyParams {
    mat4 vpMatrix;
} skyParams;

void main(){
    uv = a_position;
    vec4 pos = skyParams.vpMatrix * vec4(a_position, 1.0);
    gl_Position = pos.xyww;
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
