#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D u_sceneColorTexture;

layout(set = 1, binding = 0) uniform fs_tonemap_params {
    vec4 params; // x = exposure, y = tonemap mode, z = auto exposure
} u_fs_tonemap;

// ACES fitted curve (Stephen Hill)
vec3 ACESFilm(vec3 x){
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Reinhard tone mapping
vec3 Reinhard(vec3 x){
    return x / (1.0 + x);
}

// Reinhard Extended
vec3 ReinhardExtended(vec3 x, float maxWhite){
    vec3 numerator = x * (1.0 + x / (maxWhite * maxWhite));
    return numerator / (1.0 + x);
}

// Uncharted 2 curve helper
vec3 Uncharted2Partial(vec3 x){
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

// Uncharted 2 tone mapping
vec3 Uncharted2(vec3 x){
    float exposureBias = 2.0;
    vec3 curr = Uncharted2Partial(x * exposureBias);
    vec3 whiteScale = vec3(1.0) / Uncharted2Partial(vec3(11.2));
    return curr * whiteScale;
}

// AgX base (simplified approximation)
vec3 AgX(vec3 x){
    const mat3 agxTransform = mat3(
        0.842479062253094, 0.0423526992193578, 0.0423526992193578,
        0.0784335999999992, 0.878468636469772, 0.0784335999999992,
        0.0282895236084510, 0.0282895236084510, 0.878468636469772
    );
    vec3 logColor = log2(agxTransform * x);
    vec3 slideColor = (logColor + 2.0) / 6.0; // remap [-2,4] to [0,1]
    slideColor = clamp(slideColor, 0.0, 1.0);
    // EOTF approximation (sRGB curve)
    vec3 srgb = pow(slideColor, vec3(2.2));
    return srgb;
}

void main(){
    vec3 color = texture(u_sceneColorTexture, uv).rgb;
    float exposure = u_fs_tonemap.params.x;
    int mode = int(u_fs_tonemap.params.y);

    // Apply exposure
    color *= exposure;

    // Tone mapping
    vec3 mapped;
    if (mode == 0){
        mapped = ACESFilm(color);
    } else if (mode == 1){
        mapped = Reinhard(color);
    } else if (mode == 2){
        mapped = ReinhardExtended(color, 4.0);
    } else if (mode == 3){
        mapped = Uncharted2(color);
    } else {
        mapped = AgX(color);
    }

    // sRGB gamma correction
    mapped = pow(mapped, vec3(1.0 / 2.2));

    fragColor = vec4(mapped, 1.0);
}
