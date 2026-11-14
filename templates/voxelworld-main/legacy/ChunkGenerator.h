#pragma once

#include "ScriptBase.h"
#include "Engine.h"
#include "ScriptProperty.h"
#include "Vector3.h"
#include <vector>
#include <cstdint>

// Block types for our simple Minecraft-style world
enum class BlockType : uint8_t {
    AIR = 0,
    GRASS,
    DIRT,
    STONE,
    SAND,
    WATER,
    WOOD,
    LEAVES
};

// Chunk dimensions (16x16x16 like Minecraft)
static const int CHUNK_SIZE = 16;

class ChunkGenerator : public quantum::ScriptBase {
public:
    DPROPERTY("Seed")
    int seed = 12345;

    DPROPERTY("Chunk Scale")
    float chunkScale = 1.0f;

    DPROPERTY("Generate on Start")
    bool generateOnStart = true;

    ChunkGenerator(quantum::Scene* scene, quantum::Entity entity);
    ~ChunkGenerator();

    void onUpdate();

private:
    // 3D block data storage [x][y][z]
    BlockType blocks[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];

    // Simple Perlin-like noise (value noise with interpolation)
    float hash(float x, float y, float z);
    float smoothNoise(float x, float y, float z);
    float interpolatedNoise(float x, float y, float z);
    float perlinNoise(float x, float y, float z, int octaves = 4);

    // Terrain generation
    void generateTerrain();

    // Block queries
    BlockType getBlock(int x, int y, int z) const;
    bool isBlockSolid(int x, int y, int z) const;

    // Mesh building - only adds faces where block touches air
    void buildMesh();

    // Face vertex data
    void addFace(float x, float y, float z, int face, BlockType blockType);

    // Block colors
    void getBlockColor(BlockType type, float& r, float& g, float& b);

    bool meshBuilt = false;
    int currentVertexCount = 0;
    int currentIndexCount = 0;
};
