// Based on https://github.com/KhronosGroup/glTF-Sample-Viewer shaders

#include "includes/srgb.glsl"

float clampedDot(vec3 x, vec3 y){
    return clamp(dot(x, y), 0.0, 1.0);
}

vec4 getVertexColor(){
   vec4 color = vec4(1.0, 1.0, 1.0, 1.0);

    #ifdef HAS_VERTEX_COLOR_VEC3
        color.rgb = v_color.rgb;
    #endif
    #if defined(HAS_VERTEX_COLOR_VEC4) || defined(HAS_INSTANCING)
        color = v_color;
    #endif

   return color;
}

// Per-texture UV set selection (glTF texCoord). When a second UV set is present
// the renderer supplies u_fs_texCoordSets telling each texture which set to sample
// (0 = v_uv1, 1 = v_uv2). Without a second set everything samples v_uv1.
// A parallaxed material slides every map by the same amount: the offset is computed
// once from the height field and added here, so base color, roughness, occlusion,
// emissive and the normal map all stay registered against each other instead of
// drifting apart as the view angle changes.
#ifdef HAS_HEIGHT
    vec2 heightOffset(vec2 uv)        { return uv + heightUVOffset; }
#else
    vec2 heightOffset(vec2 uv)        { return uv; }
#endif

#ifdef HAS_UV_SET2
    uniform u_fs_texCoordSets {
        ivec4 set0; // x=baseColor, y=metallicRoughness, z=occlusion, w=emissive
        ivec4 set1; // x=normal, y=height
    } texCoordSets;

    // set1.w is always 0; reading it keeps set1 active when no normal map uses it
    vec2 selectUV(int set)        { return (set + texCoordSets.set1.w == 1) ? v_uv2 : v_uv1; }
    vec2 getBaseColorUV()         { return heightOffset(selectUV(texCoordSets.set0.x)); }
    vec2 getMetallicRoughnessUV() { return heightOffset(selectUV(texCoordSets.set0.y)); }
    vec2 getOcclusionUV()         { return heightOffset(selectUV(texCoordSets.set0.z)); }
    vec2 getEmissiveUV()          { return heightOffset(selectUV(texCoordSets.set0.w)); }
    vec2 getNormalUV()            { return heightOffset(selectUV(texCoordSets.set1.x)); }
    // the height field itself is read un-offset: it is what the offset is derived from
    vec2 getHeightUV()            { return selectUV(texCoordSets.set1.y); }
#elif defined(HAS_UV_SET1)
    vec2 getBaseColorUV()         { return heightOffset(v_uv1); }
    vec2 getMetallicRoughnessUV() { return heightOffset(v_uv1); }
    vec2 getOcclusionUV()         { return heightOffset(v_uv1); }
    vec2 getEmissiveUV()          { return heightOffset(v_uv1); }
    vec2 getNormalUV()            { return heightOffset(v_uv1); }
    vec2 getHeightUV()            { return v_uv1; }
#endif

vec4 getBaseColor(){
    vec4 baseColor = pbrParams.baseColorFactor;
    #if defined(HAS_UV_SET1) || defined(HAS_UV_SET2)
        baseColor *= sRGBToLinear(texture(sampler2D(u_baseColorTexture, u_baseColor_smp), getBaseColorUV()));
    #endif
    return baseColor * getVertexColor();
}
#ifndef MATERIAL_UNLIT
    MaterialInfo getMetallicRoughnessInfo(MaterialInfo info, float f0_ior){
        info.metallic = pbrParams.metallicFactor;
        info.perceptualRoughness = pbrParams.roughnessFactor;
        // Roughness is stored in the 'g' channel, metallic is stored in the 'b' channel.
        // This layout intentionally reserves the 'r' channel for (optional) occlusion map data
        #if defined(HAS_UV_SET1) || defined(HAS_UV_SET2)
            vec4 mrSample = texture(sampler2D(u_metallicRoughnessTexture, u_metallicRoughness_smp), getMetallicRoughnessUV());
            info.perceptualRoughness *= mrSample.g;
            info.metallic *= mrSample.b;
        #endif
        // Achromatic f0 based on IOR.
        vec3 f0 = vec3(f0_ior);
        info.albedoColor = mix(info.baseColor.rgb * (vec3(1.0) - f0),  vec3(0), info.metallic);
        info.f0 = mix(f0, info.baseColor.rgb, info.metallic);
        return info;
    }

    #if defined(HAS_UV_SET1) || defined(HAS_UV_SET2)
        vec4 getOcclusionTexture(){
            return texture(sampler2D(u_occlusionTexture, u_occlusion_smp), getOcclusionUV());
        }

        vec4 getEmissiveTexture(){
            return texture(sampler2D(u_emissiveTexture, u_emissive_smp), getEmissiveUV());
        }
    #endif
#endif

#ifndef MATERIAL_UNLIT
// Get the un-perturbed frame. A backface reports a flipped basis, because that is how
// the old single flip reached the shading normal too - and a parallax ray has to march
// into the surface it is looking at, not out of it.
TangentBasis getTangentBasis(){
    vec3 t, b, ng;
    // Compute geometrical TBN:
    #ifdef HAS_TANGENTS
        // Trivial TBN computation, present as vertex attribute.
        // Normalize eigenvectors as matrix is linearly interpolated.
        t = normalize(v_tbn[0]);
        b = normalize(v_tbn[1]);
        ng = normalize(v_tbn[2]);
    #else
        // Normals are either present as vertex attributes or approximated.
        #ifdef HAS_NORMALS
            ng = normalize(v_normal);
        #else
            ng = normalize(cross(dFdx(v_position), dFdy(v_position)));
        #endif
        #if defined(HAS_UV_SET1) || defined(HAS_UV_SET2)
            // getNormalUV carries the parallax offset, so the derivatives of the frame
            // are taken from the same UV the shading will sample
            vec2 normalUV = getNormalUV();
            vec3 uv_dx = dFdx(vec3(normalUV, 0.0));
            vec3 uv_dy = dFdy(vec3(normalUV, 0.0));
            vec3 t_ = (uv_dy.t * dFdx(v_position) - uv_dx.t * dFdy(v_position)) /
                (uv_dx.s * uv_dy.t - uv_dy.s * uv_dx.t);

            t = normalize(t_ - ng * dot(ng, t_));
            b = cross(ng, t);
        #endif
    #endif

    if (!gl_FrontFacing) {
        ng = -ng;
        t = -t;
        b = -b;
    }

    TangentBasis basis;
    basis.ng = ng;
    basis.t = t;
    basis.b = b;
    return basis;
}

// Get normal, tangent and bitangent vectors.
NormalInfo getNormalInfo(){
    TangentBasis basis = getTangentBasis();
    vec3 n;

    // Compute pertubed normals:
    #ifdef HAS_NORMAL_MAP
        n = texture(sampler2D(u_normalTexture, u_normal_smp), getNormalUV()).rgb * 2.0 - vec3(1.0);
        n *= vec3(normalScale, normalScale, 1.0);
        n = mat3(basis.t, basis.b, basis.ng) * normalize(n);
        // Double-sided glTF materials shade the reverse side with reversed normals.
        // Face culling already removes backfaces for single-sided materials.
        if (!gl_FrontFacing) {
            n = -n;
        }
    #else
        n = basis.ng;
    #endif

    n = normalize(n);
    NormalInfo info;
    info.ng = basis.ng;
    info.t = basis.t;
    info.b = basis.b;
    info.n = n;
    return info;
}
#endif
