// Based on https://github.com/KhronosGroup/glTF-Sample-Viewer shaders

//
// Fresnel
//
// http://graphicrants.blogspot.com/2013/08/specular-brdf-reference.html
// https://github.com/wdas/brdf/tree/master/src/brdfs
// https://google.github.io/filament/Filament.md.html
//

// The following equation models the Fresnel reflectance term of the spec equation (aka F())
// Implementation of fresnel from [4], Equation 15
vec3 F_Schlick(vec3 f0, vec3 f90, float VdotH){
    return f0 + (f90 - f0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

// Smith Joint GGX
// Note: Vis = G / (4 * NdotL * NdotV)
// see Eric Heitz. 2014. Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs. Journal of Computer Graphics Techniques, 3
// see Real-Time Rendering. Page 331 to 336.
// see https://google.github.io/filament/Filament.md.html#materialsystem/specularbrdf/geometricshadowing(specularg)
float V_GGX(float NdotL, float NdotV, float alphaRoughness){
    NdotL = clamp(NdotL, 0.0, 1.0);
    NdotV = clamp(NdotV, 0.0, 1.0);
    alphaRoughness = clamp(alphaRoughness, 0.001, 1.0);
    float alphaRoughnessSq = alphaRoughness * alphaRoughness;

    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - alphaRoughnessSq) + alphaRoughnessSq);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - alphaRoughnessSq) + alphaRoughnessSq);

    float GGX = GGXV + GGXL;
    return 0.5 / max(GGX, 1e-5);
}

// The following equation(s) model the distribution of microfacet normals across the area being drawn (aka D())
// Implementation from "Average Irregularity Representation of a Roughened Surface for Ray Reflection" by T. S. Trowbridge, and K. P. Reitz
// Follows the distribution function recommended in the SIGGRAPH 2013 course notes from EPIC Games [1], Equation 3.
float D_GGX(float NdotH, float alphaRoughness){
    NdotH = clamp(NdotH, 0.0, 1.0);
    alphaRoughness = clamp(alphaRoughness, 0.001, 1.0);
    float alphaRoughnessSq = alphaRoughness * alphaRoughness;
    float f = (NdotH * NdotH) * (alphaRoughnessSq - 1.0) + 1.0;
    return alphaRoughnessSq / (M_PI * max(f * f, 1e-5));
}

float lambdaSheenNumericHelper(float x, float alphaG){
    float oneMinusAlphaSq = (1.0 - alphaG) * (1.0 - alphaG);
    float a = mix(21.5473, 25.3245, oneMinusAlphaSq);
    float b = mix(3.82987, 3.32435, oneMinusAlphaSq);
    float c = mix(0.19823, 0.16801, oneMinusAlphaSq);
    float d = mix(-1.97760, -1.27393, oneMinusAlphaSq);
    float e = mix(-4.32054, -4.85967, oneMinusAlphaSq);
    return a / (1.0 + b * pow(x, c)) + d * x + e;
}

float lambdaSheen(float cosTheta, float alphaG){
    if(abs(cosTheta) < 0.5)
    {
        return exp(lambdaSheenNumericHelper(cosTheta, alphaG));
    }
    else
    {
        return exp(2.0 * lambdaSheenNumericHelper(0.5, alphaG) - lambdaSheenNumericHelper(1.0 - cosTheta, alphaG));
    }
}

float V_Sheen(float NdotL, float NdotV, float sheenRoughness){
    sheenRoughness = max(sheenRoughness, 0.000001); //clamp (0,1]
    float alphaG = sheenRoughness * sheenRoughness;

    return clamp(1.0 / ((1.0 + lambdaSheen(NdotV, alphaG) + lambdaSheen(NdotL, alphaG)) *
        (4.0 * NdotV * NdotL)), 0.0, 1.0);
}

//Sheen implementation-------------------------------------------------------------------------------------
// See  https://github.com/sebavan/glTF/tree/KHR_materials_sheen/extensions/2.0/Khronos/KHR_materials_sheen

// Estevez and Kulla http://www.aconty.com/pdf/s2017_pbs_imageworks_sheen.pdf
float D_Charlie(float sheenRoughness, float NdotH){
    sheenRoughness = max(sheenRoughness, 0.000001); //clamp (0,1]
    float alphaG = sheenRoughness * sheenRoughness;
    float invR = 1.0 / alphaG;
    float cos2h = NdotH * NdotH;
    float sin2h = 1.0 - cos2h;
    return (2.0 + invR) * pow(sin2h, invR * 0.5) / (2.0 * M_PI);
}

//https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#acknowledgments AppendixB
vec3 BRDF_lambertian(vec3 f0, vec3 f90, vec3 diffuseColor, float VdotH){
    // see https://seblagarde.wordpress.com/2012/01/08/pi-or-not-to-pi-in-game-lighting-equation/
    return (1.0 - F_Schlick(f0, f90, VdotH)) * (diffuseColor / M_PI);
}

//  https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#acknowledgments AppendixB
vec3 BRDF_specularGGX(vec3 f0, vec3 f90, float alphaRoughness, float VdotH, float NdotL, float NdotV, float NdotH){
    vec3 F = F_Schlick(f0, f90, VdotH);
    float Vis = V_GGX(NdotL, NdotV, alphaRoughness);
    float D = D_GGX(NdotH, alphaRoughness);

    return F * Vis * D;
}

// Anisotropic GGX for brushed metals and satin materials. The tangent frame is
// rotated by the authored angle before stretching the distribution along its axes.
float D_GGXAnisotropic(vec3 n, vec3 h, vec3 t, vec3 b, float alphaX, float alphaY){
    float NdotH = clampedDot(n, h);
    float HdotT = dot(h, t);
    float HdotB = dot(h, b);
    float alphaX2 = max(alphaX * alphaX, 0.000001);
    float alphaY2 = max(alphaY * alphaY, 0.000001);
    float denominator = (HdotT * HdotT) / alphaX2 +
                        (HdotB * HdotB) / alphaY2 + NdotH * NdotH;
    return 1.0 / max(M_PI * alphaX * alphaY * denominator * denominator, 0.000001);
}

vec3 BRDF_specularGGXAnisotropic(vec3 f0, vec3 f90, float alphaRoughness,
                                 float anisotropy, float rotation,
                                 vec3 n, vec3 t, vec3 b, vec3 h,
                                 float VdotH, float NdotL, float NdotV){
    float stretch = clamp(anisotropy, -0.95, 0.95);
    float alphaX = alphaRoughness * sqrt(1.0 + stretch);
    float alphaY = alphaRoughness / sqrt(1.0 + stretch);
    float c = cos(rotation);
    float s = sin(rotation);
    vec3 rotatedT = normalize(t * c + b * s);
    vec3 rotatedB = normalize(cross(n, rotatedT));
    vec3 F = F_Schlick(f0, f90, VdotH);
    float Vis = V_GGX(NdotL, NdotV, alphaRoughness);
    float D = D_GGXAnisotropic(n, h, rotatedT, rotatedB, alphaX, alphaY);
    return F * Vis * D;
}

// f_sheen
vec3 BRDF_specularSheen(vec3 sheenColor, float sheenRoughness, float NdotL, float NdotV, float NdotH){
    float sheenDistribution = D_Charlie(sheenRoughness, NdotH);
    float sheenVisibility = V_Sheen(NdotL, NdotV, sheenRoughness);
    return sheenColor * sheenDistribution * sheenVisibility;
}
