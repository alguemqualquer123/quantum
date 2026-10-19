// Parallax Occlusion Mapping (POM)
// Implements steep parallax mapping with binary search refinement
// https://learnopengl.com/Advanced-Lighting/Parallax-Mapping

// Parallax offset calculation
vec2 parallaxOcclusionMapping(
    vec2 texCoords,
    vec3 viewDirTangent,
    texture2D heightTex,
    sampler heightSmp,
    float heightScale,
    int minLayers,
    int maxLayers
){
    // Determine number of layers based on viewing angle
    float viewAngle = abs(dot(vec3(0.0, 0.0, 1.0), viewDirTangent));
    float numLayers = mix(float(maxLayers), float(minLayers), viewAngle);

    // Layer depth increment
    float layerDepth = 1.0 / numLayers;

    // Current depth from texture
    float currentDepth = texture(sampler2D(heightTex, heightSmp), texCoords).r;

    // Texture coordinate step
    vec2 deltaTexCoords = (viewDirTangent.xy / viewDirTangent.z * heightScale) / numLayers;

    // Initialize
    vec2 currentTexCoords = texCoords;
    float currentLayerDepth = 0.0;

    // Steep parallax mapping
    while (currentDepth > currentLayerDepth && currentLayerDepth < 1.0){
        currentTexCoords -= deltaTexCoords;
        currentDepth = texture(sampler2D(heightTex, heightSmp), currentTexCoords).r;
        currentLayerDepth += layerDepth;
    }

    // Get texture coordinates for interpolation
    vec2 prevTexCoords = currentTexCoords + deltaTexCoords;

    // Interpolation weight (binary search refinement)
    float afterDepth = currentDepth - currentLayerDepth;
    float beforeDepth = texture(sampler2D(heightTex, heightSmp), prevTexCoords).r
                       - (currentLayerDepth - layerDepth);

    float weight = afterDepth / (afterDepth - beforeDepth);
    vec2 finalTexCoords = prevTexCoords * weight + currentTexCoords * (1.0 - weight);

    return finalTexCoords;
}

// Simple parallax mapping (faster, less accurate)
vec2 parallaxMapping(
    vec2 texCoords,
    vec3 viewDirTangent,
    texture2D heightTex,
    sampler heightSmp,
    float heightScale
){
    float height = texture(sampler2D(heightTex, heightSmp), texCoords).r;
    vec2 p = viewDirTangent.xy / viewDirTangent.z * (height * heightScale);
    return texCoords - p;
}

// Steep parallax mapping (better quality, more layers)
vec2 steepParallaxMapping(
    vec2 texCoords,
    vec3 viewDirTangent,
    texture2D heightTex,
    sampler heightSmp,
    float heightScale,
    int numLayers
){
    float layerDepth = 1.0 / float(numLayers);
    float currentLayerDepth = 0.0;
    vec2 deltaTexCoords = (viewDirTangent.xy / viewDirTangent.z * heightScale) / float(numLayers);
    vec2 currentTexCoords = texCoords;
    float currentDepthMapValue = texture(sampler2D(heightTex, heightSmp), currentTexCoords).r;

    for (int i = 0; i < numLayers && currentLayerDepth < currentDepthMapValue; i++){
        currentTexCoords -= deltaTexCoords;
        currentDepthMapValue = texture(sampler2D(heightTex, heightSmp), currentTexCoords).r;
        currentLayerDepth += layerDepth;
    }

    return currentTexCoords;
}

// TBN matrix construction for tangent space
mat3 constructTBN(vec3 normal, vec3 tangent, vec2 texCoords, vec2 texelSize, texture2D depthTex, sampler depthSmp){
    // Perturb normal using height map gradient
    float heightL = texture(sampler2D(depthTex, depthSmp), texCoords - vec2(texelSize.x, 0.0)).r;
    float heightR = texture(sampler2D(depthTex, depthSmp), texCoords + vec2(texelSize.x, 0.0)).r;
    float heightU = texture(sampler2D(depthTex, depthSmp), texCoords - vec2(0.0, texelSize.y)).r;
    float heightD = texture(sampler2D(depthTex, depthSmp), texCoords + vec2(0.0, texelSize.y)).r;

    vec3 dX = vec3(texelSize.x, 0.0, heightR - heightL);
    vec3 dY = vec3(0.0, texelSize.y, heightD - heightU);

    vec3 perturbedNormal = normalize(cross(dX, dY));
    vec3 bitangent = cross(normal, tangent);

    return mat3(tangent, bitangent, normal);
}
