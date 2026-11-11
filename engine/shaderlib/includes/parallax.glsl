// Parallax occlusion mapping for a material height map.
// https://learnopengl.com/Advanced-Lighting/Parallax-Mapping
//
// A height map is authored white = the surface stands up, black = it is recessed, so
// the march compares the *inverted* sample against the depth it has already walked.
//
// The sampling helpers here address u_heightTexture / u_height_smp directly, the same
// way every other include in this folder reaches the uniforms its shader declared:
// GLSL does not pass a combined image sampler as a function argument.

float getHeightSample(vec2 texCoords){
    return texture(sampler2D(u_heightTexture, u_height_smp), texCoords).r;
}

// texCoords is the un-offset UV the height field is read from, and viewDirTangent
// points from this fragment toward the eye in tangent space (x = tangent,
// y = bitangent, z = normal). viewDirTangent.z must not approach 0: the offset is
// proportional to 1/z, so a ray lying in the surface plane would run off the tile.
vec2 parallaxOcclusionMapping(vec2 texCoords, vec3 viewDirTangent, vec4 heightParams){
    const float minViewZ = 0.05;

    float heightScale = heightParams.x;
    float minLayers = max(heightParams.y, 1.0);
    float maxLayers = max(heightParams.z, minLayers);

    // A head-on view barely needs any steps; the grazing pixels that expose the
    // relief are the ones that get the layer budget.
    float numLayers = mix(maxLayers, minLayers, abs(viewDirTangent.z));
    float layerDepth = 1.0 / numLayers;

    // The slide is proportional to 1/z, so an unclamped grazing ray travels several
    // tiles across and shows its neighbour's colour instead of its own relief. One
    // heightScale of UV travel is exactly the ray running along the full height of
    // the bump, which is the most any surface point can be displaced.
    vec2 slide = viewDirTangent.xy / max(viewDirTangent.z, minViewZ);
    float slideLength = length(slide);
    if (slideLength > 1.0){
        slide /= slideLength;
    }
    vec2 deltaTexCoords = slide * heightScale / numLayers;

    vec2 currentTexCoords = texCoords;
    float currentLayer = 0.0;
    float currentDepth = 1.0 - getHeightSample(currentTexCoords);

    // Terminates in at most numLayers steps: currentLayer passes 1.0 exactly there.
    while (currentDepth > currentLayer && currentLayer < 1.0){
        currentTexCoords -= deltaTexCoords;
        currentDepth = 1.0 - getHeightSample(currentTexCoords);
        currentLayer += layerDepth;
    }

    // Refine between the last two layers instead of snapping to one of them, which is
    // what would otherwise turn the step count into visible banding.
    vec2 previousTexCoords = currentTexCoords + deltaTexCoords;
    float afterDepth = currentDepth - currentLayer;
    float beforeDepth = (1.0 - getHeightSample(previousTexCoords)) - (currentLayer - layerDepth);

    float weight = afterDepth / (afterDepth - beforeDepth);
    return previousTexCoords * weight + currentTexCoords * (1.0 - weight);
}

// Single-sample version: no relief at steep angles, but no loop either. Kept for
// forks that need the cheaper approximation.
vec2 parallaxMapping(vec2 texCoords, vec3 viewDirTangent, float heightScale){
    float height = getHeightSample(texCoords);
    return texCoords - viewDirTangent.xy / max(viewDirTangent.z, 0.05) * (height * heightScale);
}
