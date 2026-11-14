#version 450

// Probe fork: identical vertex side to the built-in sky, which the engine fills by the
// u_vs_skyParams block name (RenderSystem applies vpMatrix there). Renaming or dropping
// that block leaves the sky drawn with a garbage transform instead of failing to load.
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
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
