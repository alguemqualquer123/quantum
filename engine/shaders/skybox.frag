#version 330 core
in vec3 TexCoords;
out vec4 FragColor;

uniform samplerCube skyboxDay;
uniform samplerCube skyboxNight;
uniform float timeOfDay; // 0.0 = midnight, 12.0 = noon, 24.0 = next midnight

vec3 blendCubemap()
{
    // Normalize time to [0,24)
    float t = mod(timeOfDay, 24.0);
    // Compute blend factor: 0 = night, 1 = day
    float factor = smoothstep(6.0, 18.0, t); // sunrise at 6h, sunset at 18h
    vec4 dayColor = texture(skyboxDay, TexCoords);
    vec4 nightColor = texture(skyboxNight, TexCoords);
    return mix(nightColor.rgb, dayColor.rgb, factor);
}

void main()
{
    vec3 color = blendCubemap();
    FragColor = vec4(color, 1.0);
}
