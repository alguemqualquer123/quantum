#include "ChunkGenerator.h"
#include "MeshComponent.h"
#include <cmath>
#include <cstring>

using namespace quantum;

ChunkGenerator::ChunkGenerator(Scene* scene, Entity entity): ScriptBase(scene, entity) {
    memset(blocks, 0, sizeof(blocks));
    REGISTER_ENGINE_EVENT(onUpdate);
}

ChunkGenerator::~ChunkGenerator() {
    UNREGISTER_ENGINE_EVENT(onUpdate);
}

// ===================== Noise Functions =====================

float ChunkGenerator::hash(float x, float y, float z) {
    int ix = (int)floor(x);
    int iy = (int)floor(y);
    int iz = (int)floor(z);
    
    // Simple hash using prime numbers
    int h = seed;
    h ^= ix * 374761393;
    h ^= iy * 668265263;
    h ^= iz * 1274126177;
    h = (h ^ (h >> 13)) * 1274126177;
    h = h ^ (h >> 16);
    
    return (float)(h & 0xFFFF) / (float)0xFFFF;
}

float ChunkGenerator::smoothNoise(float x, float y, float z) {
    float ix = floor(x);
    float iy = floor(y);
    float iz = floor(z);
    float fx = x - ix;
    float fy = y - iy;
    float fz = z - iz;
    
    // Smoothstep
    float ux = fx * fx * (3.0f - 2.0f * fx);
    float uy = fy * fy * (3.0f - 2.0f * fy);
    float uz = fz * fz * (3.0f - 2.0f * fz);
    
    float v000 = hash(ix,     iy,     iz);
    float v100 = hash(ix + 1, iy,     iz);
    float v010 = hash(ix,     iy + 1, iz);
    float v110 = hash(ix + 1, iy + 1, iz);
    float v001 = hash(ix,     iy,     iz + 1);
    float v101 = hash(ix + 1, iy,     iz + 1);
    float v011 = hash(ix,     iy + 1, iz + 1);
    float v111 = hash(ix + 1, iy + 1, iz + 1);
    
    float v00 = v000 + (v100 - v000) * ux;
    float v10 = v010 + (v110 - v010) * ux;
    float v01 = v001 + (v101 - v001) * ux;
    float v11 = v011 + (v111 - v011) * ux;
    
    float v0 = v00 + (v10 - v00) * uy;
    float v1 = v01 + (v11 - v01) * uy;
    
    return v0 + (v1 - v0) * uz;
}

float ChunkGenerator::interpolatedNoise(float x, float y, float z) {
    return smoothNoise(x, y, z);
}

float ChunkGenerator::perlinNoise(float x, float y, float z, int octaves) {
    float total = 0.0f;
    float frequency = 1.0f;
    float amplitude = 1.0f;
    float maxValue = 0.0f;
    
    for (int i = 0; i < octaves; i++) {
        total += interpolatedNoise(x * frequency, y * frequency, z * frequency) * amplitude;
        maxValue += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    
    return total / maxValue;
}

// ===================== Terrain Generation =====================

void ChunkGenerator::generateTerrain() {
    float scale = 0.05f;  // Controls terrain roughness
    
    for (int x = 0; x < CHUNK_SIZE; x++) {
        for (int z = 0; z < CHUNK_SIZE; z++) {
            // Generate height using noise
            float nx = (float)x * scale;
            float nz = (float)z * scale;
            
            float heightNoise = perlinNoise(nx, 0.0f, nz, 4);
            int height = (int)(heightNoise * 8.0f) + 4;  // Height between 4 and 12
            
            // Clamp to chunk size
            if (height > CHUNK_SIZE) height = CHUNK_SIZE;
            
            for (int y = 0; y < CHUNK_SIZE; y++) {
                if (y == 0) {
                    // Bottom layer is always stone
                    blocks[x][y][z] = BlockType::STONE;
                }
                else if (y < height - 3) {
                    // Deep underground is stone
                    blocks[x][y][z] = BlockType::STONE;
                }
                else if (y < height) {
                    // Middle layers are dirt
                    blocks[x][y][z] = BlockType::DIRT;
                }
                else if (y == height) {
                    // Top layer is grass
                    blocks[x][y][z] = BlockType::GRASS;
                }
                else if (y <= 5 && y > height) {
                    // Water level at y=5
                    blocks[x][y][z] = BlockType::WATER;
                }
                else {
                    // Air above everything
                    blocks[x][y][z] = BlockType::AIR;
                }
            }
        }
    }
}

// ===================== Block Queries =====================

BlockType ChunkGenerator::getBlock(int x, int y, int z) const {
    if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE) {
        return BlockType::AIR;  // Treat out-of-bounds as air
    }
    return blocks[x][y][z];
}

bool ChunkGenerator::isBlockSolid(int x, int y, int z) const {
    BlockType type = getBlock(x, y, z);
    return type != BlockType::AIR && type != BlockType::WATER;
}

// ===================== Colors =====================

void ChunkGenerator::getBlockColor(BlockType type, float& r, float& g, float& b) {
    switch (type) {
        case BlockType::GRASS:
            r = 0.30f; g = 0.70f; b = 0.20f;
            break;
        case BlockType::DIRT:
            r = 0.55f; g = 0.35f; b = 0.20f;
            break;
        case BlockType::STONE:
            r = 0.50f; g = 0.50f; b = 0.50f;
            break;
        case BlockType::SAND:
            r = 0.85f; g = 0.80f; b = 0.55f;
            break;
        case BlockType::WATER:
            r = 0.20f; g = 0.40f; b = 0.80f;
            break;
        case BlockType::WOOD:
            r = 0.45f; g = 0.30f; b = 0.15f;
            break;
        case BlockType::LEAVES:
            r = 0.15f; g = 0.55f; b = 0.10f;
            break;
        default:
            r = 1.0f; g = 1.0f; b = 1.0f;
            break;
    }
}

// ===================== Face Generation =====================

// Face directions: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z
// Each face has 4 vertices forming a quad

void ChunkGenerator::addFace(float x, float y, float z, int face, BlockType blockType) {
    MeshComponent& mesh = getScene()->getComponent<MeshComponent>(getEntity());
    
    // Get block color with slight face-dependent shading
    float r, g, b;
    getBlockColor(blockType, r, g, b);
    
    // Apply simple directional lighting to faces
    float shade = 1.0f;
    switch (face) {
        case 0: shade = 0.85f; break;  // +X
        case 1: shade = 0.85f; break;  // -X
        case 2: shade = 1.00f; break;  // +Y (top - brightest)
        case 3: shade = 0.50f; break;  // -Y (bottom - darkest)
        case 4: shade = 0.70f; break;  // +Z
        case 5: shade = 0.70f; break;  // -Z
    }
    
    r *= shade;
    g *= shade;
    b *= shade;
    
    // 4 vertices per face, each with position(3) + normal(3) + color(4) = 10 floats
    Vector3 positions[4];
    Vector3 normals[4];
    
    switch (face) {
        case 0: // +X
            positions[0] = Vector3(x + 1, y,     z);
            positions[1] = Vector3(x + 1, y + 1, z);
            positions[2] = Vector3(x + 1, y + 1, z + 1);
            positions[3] = Vector3(x + 1, y,     z + 1);
            normals[0] = normals[1] = normals[2] = normals[3] = Vector3(1, 0, 0);
            break;
        case 1: // -X
            positions[0] = Vector3(x, y,     z + 1);
            positions[1] = Vector3(x, y + 1, z + 1);
            positions[2] = Vector3(x, y + 1, z);
            positions[3] = Vector3(x, y,     z);
            normals[0] = normals[1] = normals[2] = normals[3] = Vector3(-1, 0, 0);
            break;
        case 2: // +Y
            positions[0] = Vector3(x,     y + 1, z);
            positions[1] = Vector3(x + 1, y + 1, z);
            positions[2] = Vector3(x + 1, y + 1, z + 1);
            positions[3] = Vector3(x,     y + 1, z + 1);
            normals[0] = normals[1] = normals[2] = normals[3] = Vector3(0, 1, 0);
            break;
        case 3: // -Y
            positions[0] = Vector3(x,     y, z + 1);
            positions[1] = Vector3(x + 1, y, z + 1);
            positions[2] = Vector3(x + 1, y, z);
            positions[3] = Vector3(x,     y, z);
            normals[0] = normals[1] = normals[2] = normals[3] = Vector3(0, -1, 0);
            break;
        case 4: // +Z
            positions[0] = Vector3(x + 1, y,     z + 1);
            positions[1] = Vector3(x + 1, y + 1, z + 1);
            positions[2] = Vector3(x,     y + 1, z + 1);
            positions[3] = Vector3(x,     y,     z + 1);
            normals[0] = normals[1] = normals[2] = normals[3] = Vector3(0, 0, 1);
            break;
        case 5: // -Z
            positions[0] = Vector3(x,     y,     z);
            positions[1] = Vector3(x,     y + 1, z);
            positions[2] = Vector3(x + 1, y + 1, z);
            positions[3] = Vector3(x + 1, y,     z);
            normals[0] = normals[1] = normals[2] = normals[3] = Vector3(0, 0, -1);
            break;
    }
    
    // Add 4 vertices to buffer
    unsigned int baseVertex = currentVertexCount;
    for (int i = 0; i < 4; i++) {
        mesh.buffer.addVector3(AttributeType::POSITION, positions[i]);
        mesh.buffer.addVector3(AttributeType::NORMAL, normals[i]);
        mesh.buffer.addVector4(AttributeType::COLOR, Vector4(r, g, b, 1.0f));
        currentVertexCount++;
    }
    
    // Add 2 triangles (6 indices)
    mesh.indices.addUInt16(AttributeType::INDEX, baseVertex + 0);
    mesh.indices.addUInt16(AttributeType::INDEX, baseVertex + 1);
    mesh.indices.addUInt16(AttributeType::INDEX, baseVertex + 2);
    mesh.indices.addUInt16(AttributeType::INDEX, baseVertex + 0);
    mesh.indices.addUInt16(AttributeType::INDEX, baseVertex + 2);
    mesh.indices.addUInt16(AttributeType::INDEX, baseVertex + 3);
    currentIndexCount += 6;
}

// ===================== Mesh Building =====================

void ChunkGenerator::buildMesh() {
    MeshComponent& mesh = getScene()->getComponent<MeshComponent>(getEntity());
    
    // Clear existing mesh data
    mesh.buffer.clearAll();
    mesh.indices.clearAll();
    currentVertexCount = 0;
    currentIndexCount = 0;
    
    // Setup buffer attributes
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);
    
    mesh.indices.createIndexAttribute();
    
    // Iterate through all blocks and add faces where block touches air
    for (int x = 0; x < CHUNK_SIZE; x++) {
        for (int y = 0; y < CHUNK_SIZE; y++) {
            for (int z = 0; z < CHUNK_SIZE; z++) {
                BlockType block = getBlock(x, y, z);
                
                // Skip air blocks
                if (block == BlockType::AIR) continue;
                
                // Check each face - only add if neighbor is air
                // +X face
                if (!isBlockSolid(x + 1, y, z)) {
                    addFace((float)x, (float)y, (float)z, 0, block);
                }
                // -X face
                if (!isBlockSolid(x - 1, y, z)) {
                    addFace((float)x, (float)y, (float)z, 1, block);
                }
                // +Y face
                if (!isBlockSolid(x, y + 1, z)) {
                    addFace((float)x, (float)y, (float)z, 2, block);
                }
                // -Y face
                if (!isBlockSolid(x, y - 1, z)) {
                    addFace((float)x, (float)y, (float)z, 3, block);
                }
                // +Z face
                if (!isBlockSolid(x, y, z + 1)) {
                    addFace((float)x, (float)y, (float)z, 4, block);
                }
                // -Z face
                if (!isBlockSolid(x, y, z - 1)) {
                    addFace((float)x, (float)y, (float)z, 5, block);
                }
            }
        }
    }
    
    // Configure mesh properties
    mesh.numSubmeshes = 1;
    mesh.submeshes[0].faceCulling = true;
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.submeshes[0].vertexCount = currentVertexCount;
    
    // Update mesh buffers
    mesh.buffer.setCount(currentVertexCount);
    mesh.buffer.setStride(10 * sizeof(float));  // 10 floats per vertex
    mesh.indices.setStride(2);  // 16-bit indices
    mesh.indices.setCount(currentIndexCount);
    mesh.vertexCount = currentVertexCount;
    
    // Set mesh flags
    mesh.castShadows = true;
    mesh.receiveShadows = true;
    mesh.receiveLights = true;
    mesh.needReload = true;
}

// ===================== Main Update =====================

void ChunkGenerator::onUpdate() {
    if (generateOnStart && !meshBuilt) {
        // Generate terrain noise
        generateTerrain();
        
        // Build optimized mesh (only visible faces)
        buildMesh();
        
        meshBuilt = true;
    }
}
