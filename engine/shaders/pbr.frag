#define PI 3.14159265359`n#version 330 core
layout(location = 0) out vec4 FragColor;

in vec3 Normal;
in vec3 WorldPos;

struct Light {
    vec3 position;
    vec3 color;
    float intensity;
};

uniform Light lights[4];
uniform vec3 viewPos;

// Material parameters (simple PBR placeholder)
uniform vec3 albedo;
uniform float metallic;
uniform float roughness;
uniform float ao;

// Helper: normal distribution function (GGX)
float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom);
}

// Helper: geometry term (Smith)
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;
    float denom = NdotV * (1.0 - k) + k;
    return NdotV / denom;
}
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

// Helper: fresnel (Schlick)
vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

void main()
{
    vec3 N = normalize(Normal);
    vec3 V = normalize(viewPos - WorldPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 Lo = vec3(0.0);
    for(int i = 0; i < 4; ++i)
    {
        vec3 L = normalize(lights[i].position - WorldPos);
        vec3 H = normalize(V + L);
        float distance = length(lights[i].position - WorldPos);
        float attenuation = 1.0 / (distance * distance);
        vec3 radiance = lights[i].color * lights[i].intensity * attenuation;

        // Cook-Torrance BRDF
        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);
        vec3 F    = fresnelSchlick(max(dot(H, V), F0);

        vec3 numerator    = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001;
        vec3 specular = numerator / denominator;

        // kS is specular, kD is diffuse (energy conservation)
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;

        float NdotL = max(dot(N, L), 0.0);
        Lo += (kD * albedo / PI + specular) * radiance * NdotL;
    }
    // Ambient term (IBL placeholder)
    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 color = ambient + Lo;
    // HDR tonemap
    color = color / (color + vec3(1.0));
    // Gamma correction
    color = pow(color, vec3(1.0/2.2));
    FragColor = vec4(color, 1.0);
}



