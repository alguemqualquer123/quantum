// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#version 450

// Screen-space projected decal. The depth buffer reconstructs the receiver
// position, while inverseDecalModel tests and unwraps the local projection box.
in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;
uniform texture2D u_decalTexture;
uniform sampler u_decalTexture_smp;

uniform u_fs_decalParams {
    mat4 invViewProjection;
    mat4 inverseDecalModel;
    vec4 colorOpacity;
    vec4 params; // x = edge fade, y = depth UV orientation
} decal;

#include "includes/depth_util.glsl"

void main(){
    // The depth test below keeps the fullscreen projection limited to receiver geometry.
    vec2 depthUV = v_texcoord;
    if (decal.params.y > 0.5) depthUV.y = 1.0 - depthUV.y;
    float depth01 = decodeDepth(texture(sampler2D(u_depthTexture, u_depth_smp), depthUV));
    if (depth01 >= 0.9999) discard;

    vec3 ndc = vec3(depthUV.x * 2.0 - 1.0, depthUVToNDCY(depthUV.y), depth01 * 2.0 - 1.0);
    vec4 worldPosition = decal.invViewProjection * vec4(ndc, 1.0);
    if (worldPosition.w <= 0.0001) discard;
    worldPosition /= worldPosition.w;

    vec3 local = (decal.inverseDecalModel * worldPosition).xyz;
    vec3 edge = vec3(0.5) - abs(local);
    if (edge.x < 0.0 || edge.y < 0.0 || edge.z < 0.0) discard;

    float fadeWidth = max(decal.params.x, 0.0001);
    float edgeFade = smoothstep(0.0, fadeWidth, min(edge.x, min(edge.y, edge.z)));
    vec2 uv = local.xz + 0.5;
    vec4 source = texture(sampler2D(u_decalTexture, u_decalTexture_smp), uv);
    float alpha = source.a * decal.colorOpacity.a * edgeFade;
    if (alpha <= 0.001) discard;
    frag_color = vec4(source.rgb * decal.colorOpacity.rgb, alpha);
}
