#version 450

// A fork may declare a u_vs_customParams block: its members are filled by name from the
// component's shader uniforms (Properties window, setShaderUniform). Two names are
// written by the engine instead: "time" (seconds since startup) and "resolution"
// (xy = render target size, zw = 1 / size). Do not mix int and float members: GL
// uploads the block typed after its first member.
//
// sumWaterWaves in RenderSystem.cpp repeats this wave sum for height queries, and
// WaterSystem::getSimSurface adds the field to it where the water simulates: change both.
//
//   uniform u_vs_customParams {
//       float time;
//       vec4 tint;
//   } customParams;

uniform u_vs_waterParams {
    mat4 modelMatrix;
    mat4 viewProjectionMatrix; // Y-flipped for offscreen passes on GL
    vec4 waves[4];             // xy = direction on world XZ, z = amplitude, w = wavelength
    vec4 phases;               // phase of each wave (radians)
    vec4 params;               // x = crest steepness
} waterParams;

in vec3 a_position;

// in the same order as water.frag, which matches them by location
out vec3 v_position;
out vec3 v_normal;
out float v_waveHeight;

const float TWO_PI = 6.283185307179586;

#ifdef USE_WATER_SIM
    // Depth per cell instead of one level plane, fetched per vertex like the terrain
    // heightmap. The field lies on the world ground, so a puddle stays where the water
    // left it even when the entity is moved or turned. One texel per cell: r = water
    // standing, g = ground under it, b = ripple, a = flow speed.
    uniform highp texture2D u_waterFieldTexture;
    uniform highp sampler u_waterField_smp;

    uniform u_vs_waterSimParams {
        vec4 simMap;    // xy = 1 / world metres of the field, zw = 1 / cells (one texel)
        vec4 simOrigin; // xy = world XZ of the field's lower bound
        vec4 simCell;   // xy = world metres per cell, z = depth under which a cell is dry
        vec4 simTune;   // x = share of the ground slope carried into the wave normal
    } waterSim;

    out float v_simWet;
    out float v_simFlow;

    // surface over the entity level, in world metres: ground + standing water + ripple
    float fieldHeight(vec2 uv){
        vec4 field = texture(sampler2D(u_waterFieldTexture, u_waterField_smp), uv);
        return field.g + field.r + field.b;
    }

    // the slope the water lies on, which a level plane cannot show
    vec3 fieldNormal(vec2 uv, vec3 waves){
        vec2 e = waterSim.simMap.zw;
        float dhdx = fieldHeight(uv + vec2(e.x, 0.0)) - fieldHeight(uv - vec2(e.x, 0.0));
        float dhdz = fieldHeight(uv + vec2(0.0, e.y)) - fieldHeight(uv - vec2(0.0, e.y));
        vec3 ground = normalize(vec3(-dhdx / (2.0 * waterSim.simCell.x), 1.0, -dhdz / (2.0 * waterSim.simCell.y)));
        // the bowl tilts the surface and the waves still ripple over it
        return normalize(vec3(ground.x + waves.x * waterSim.simTune.x, ground.y, ground.z + waves.z * waterSim.simTune.x));
    }
#endif

// Gerstner wave at the world point p, adding up the surface derivatives
vec3 gerstnerWave(vec4 wave, float phase, vec2 p, float steepness, inout vec3 tangent, inout vec3 binormal){
    vec2 d = wave.xy;
    float a = wave.z;
    float k = TWO_PI / wave.w;
    float f = k * dot(d, p) - phase;
    float s = sin(f);
    float c = cos(f);
    float qa = steepness * a;

    tangent += vec3(-qa * k * d.x * d.x * s, a * k * d.x * c, -qa * k * d.y * s);
    binormal += vec3(-qa * k * d.x * d.y * s, a * k * d.y * c, -qa * k * d.y * d.y * s);

    return vec3(qa * d.x * c, a * s, qa * d.y * c);
}

void main(){
    vec4 worldPos = waterParams.modelMatrix * vec4(a_position, 1.0);
    worldPos.y = waterParams.modelMatrix[3].y; // level even when the entity is tilted
    vec2 p = worldPos.xz;

    vec3 tangent = vec3(1.0, 0.0, 0.0);
    vec3 binormal = vec3(0.0, 0.0, 1.0);
    // unrolled, GL targets cannot index the phases vector
    float steepness = waterParams.params.x;
    vec3 offset = gerstnerWave(waterParams.waves[0], waterParams.phases.x, p, steepness, tangent, binormal);
    offset += gerstnerWave(waterParams.waves[1], waterParams.phases.y, p, steepness, tangent, binormal);
    offset += gerstnerWave(waterParams.waves[2], waterParams.phases.z, p, steepness, tangent, binormal);
    offset += gerstnerWave(waterParams.waves[3], waterParams.phases.w, p, steepness, tangent, binormal);

    worldPos.xyz += offset;

    vec3 normal = normalize(cross(binormal, tangent));

    #ifdef USE_WATER_SIM
        // texel centres at (i + 0.5) / cells, the same convention the ground is baked with
        vec2 uv = (p - waterSim.simOrigin.xy) * waterSim.simMap.xy;
        vec4 field = texture(sampler2D(u_waterFieldTexture, u_waterField_smp), uv);
        worldPos.y += field.g + field.r + field.b;
        normal = fieldNormal(uv, normal);
        v_simWet = field.r / max(waterSim.simCell.z, 0.0001);
        v_simFlow = field.a;
    #endif

    v_position = worldPos.xyz;
    v_normal = normal;
    v_waveHeight = offset.y;

    gl_Position = waterParams.viewProjectionMatrix * worldPos;
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
