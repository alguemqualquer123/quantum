#version 450

// Film Grain: adds noise for cinematic look
// Temporal noise varies per frame for realistic grain

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform u_fs_filmGrainParams {
    vec4 params; // x = intensity, y = size, z = speed, w = time
} filmGrainParams;

// Pseudo-random noise function
float random(vec2 co){
    return fract(sin(dot(co.xy, vec2(12.9898, 78.233))) * 43758.5453);
}

void main(){
    float intensity = filmGrainParams.params.x;
    float size = filmGrainParams.params.y;
    float speed = filmGrainParams.params.z;
    float time = filmGrainParams.params.w;

    // Generate time-varying noise
    vec2 noiseCoord = v_texcoord * size + time * speed;
    float noise = random(noiseCoord);

    // Remap noise to [-1, 1]
    noise = noise * 2.0 - 1.0;

    // Apply grain
    vec3 color = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;
    color += noise * intensity;

    // Clamp to valid range
    color = max(color, vec3(0.0));

    frag_color = vec4(color, 1.0);
}
