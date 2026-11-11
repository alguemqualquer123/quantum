// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include <iostream>
#include <filesystem>
#include <array>
#include <thread>
#include <chrono>
#include "../voxel/Mesher.h"
#include "../voxel/World.h"

int main() {
    World world(1234);
    world.update(0, 0, 1);
    auto &chunk = world.getChunk(0,0);
    const bool safeBounds = chunk.get(-1, 0, 0) == 0 && chunk.get(Chunk::SIZE, 0, 0) == 0;
    const bool generated = chunk.nonAirCount() > 0 && chunk.get(0, 0, 0) != 0;
    Generator biomeProbe(1234);
    const Generator::Biome biomeAtOrigin = biomeProbe.getBiome(0, 0);
    const bool biomeDeterministic = biomeAtOrigin == biomeProbe.getBiome(0, 0);
    std::array<bool, 4> observedBiomes{};
    for (int x = 0; x < 64; ++x) {
        for (int z = 0; z < 64; ++z) {
            observedBiomes[static_cast<size_t>(biomeProbe.getBiome(x, z))] = true;
        }
    }
    const size_t biomeCount = static_cast<size_t>(observedBiomes[0])
        + static_cast<size_t>(observedBiomes[1])
        + static_cast<size_t>(observedBiomes[2])
        + static_cast<size_t>(observedBiomes[3]);
    const bool biomes = biomeDeterministic && biomeCount >= 3;
    world.setBlock(8, 8, 8, 3);
    chunk.rebuildLighting();
    const bool torchLighting = chunk.getBlockLight(8, 8, 8) == 14
        && chunk.getBlockLight(9, 8, 8) == 13
        && chunk.getSunlight(0, Chunk::SIZE - 1, 0) == 15;
    world.setBlock(15, 8, 8, 3);
    world.rebuildLighting();
    const bool boundaryLighting = world.getChunk(1, 0).getBlockLight(0, 8, 8) == 13;
    VoxelMeshData mesh;
    VoxelMesher::Neighbors neighbors = {
        &world.getChunk(1, 0), nullptr, nullptr, nullptr, nullptr, nullptr
    };
    VoxelMesher::build(world.getChunk(0, 0), neighbors, mesh);
    VoxelMeshData greedyMesh;
    VoxelMesher::buildGreedy(world.getChunk(0, 0), neighbors, greedyMesh);
    VoxelMeshData lodMesh;
    VoxelMesher::buildLOD(world.getChunk(0, 0), neighbors, 1, lodMesh);
    const bool meshed = !mesh.vertices.empty() && mesh.indices.size() % 6 == 0;
    const bool internalFacesCulled = mesh.faceCount() < chunk.nonAirCount() * 6;
    const bool greedyMeshed = greedyMesh.faceCount() > 0
        && greedyMesh.faceCount() <= mesh.faceCount();
    const bool lodMeshed = lodMesh.faceCount() > 0 && lodMesh.faceCount() < mesh.faceCount();
    const bool lodTransitionSkirts = lodMesh.transitionSkirtFaces > 0;
    bool meshCarriesLight = false;
    for (const VoxelMeshVertex& vertex : mesh.vertices) {
        if (vertex.block == 3 && vertex.light >= 13) {
            meshCarriesLight = true;
            break;
        }
    }
    World physicsWorld(5678);
    Chunk::BlockData physicsData{};
    Chunk& physicsChunk = physicsWorld.getChunk(0, 0);
    physicsChunk.assign(physicsData);
    physicsChunk.set(4, 8, 4, 1);
    physicsWorld.rebuildLighting();
    const VoxelRayHit rayHit = physicsWorld.raycast(4.5f, 15.5f, 4.5f,
        0.0f, -1.0f, 0.0f, 20.0f);
    const bool voxelPhysics = rayHit.hit && rayHit.block == 1
        && rayHit.blockX == 4 && rayHit.blockY == 8 && rayHit.blockZ == 4
        && rayHit.normalY == 1;
    const std::filesystem::path region = std::filesystem::temp_directory_path() / "quantum_voxel_smoke.qvx";
    const size_t nonAir = chunk.nonAirCount();
    const bool saved = world.saveRegion(region.string());
    const bool replaced = saved && world.saveRegion(region.string());
    const uintmax_t regionBytes = saved ? std::filesystem::file_size(region) : 0;
    const bool compressed = regionBytes < 9u * Chunk::SIZE * Chunk::SIZE * Chunk::SIZE;
    world.update(8, 8, 0);
    const bool streamed = world.chunkCount() == 1;
    World asyncWorld(2468);
    const size_t asyncInitial = asyncWorld.updateAsync(0, 0, 1, 2);
    const size_t asyncInitialPending = asyncWorld.pendingChunkCount();
    for (int attempt = 0; attempt < 200 && asyncWorld.chunkCount() < 9; ++attempt) {
        asyncWorld.updateAsync(0, 0, 1, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool asyncLoaded = asyncInitial == 0 && asyncInitialPending == 2
        && asyncWorld.chunkCount() == 9
        && asyncWorld.pendingChunkCount() == 0;
    asyncWorld.updateAsync(8, 8, 0, 1);
    for (int attempt = 0; attempt < 200 && asyncWorld.chunkCount() != 1; ++attempt) {
        asyncWorld.updateAsync(8, 8, 0, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool asyncWindow = asyncWorld.chunkCount() == 1 && asyncWorld.pendingChunkCount() == 0;
    World restored(9999);
    const bool loaded = replaced && restored.loadRegion(region.string());
    const bool restoredData = loaded && restored.chunkCount() == 9
        && restored.getChunk(0, 0).nonAirCount() == nonAir;
    const bool restoredLighting = loaded && restored.getChunk(0, 0).getBlockLight(9, 8, 8) == 13;
    const bool restoredBoundaryLighting = loaded && restored.getChunk(1, 0).getBlockLight(0, 8, 8) == 13;
    World incremental(4321);
    incremental.update(0, 0, 1);
    const std::filesystem::path incrementalRegion = std::filesystem::temp_directory_path() / "quantum_voxel_incremental.qvx";
    const bool incrementalBase = incremental.saveRegionIncremental(incrementalRegion.string());
    const uint8_t originalIncrementalBlock = incremental.getBlock(-1, 3, -1);
    const uint8_t editedIncrementalBlock = originalIncrementalBlock == 0 ? 7 : 0;
    const bool incrementalEdit = incremental.setBlock(-1, 3, -1, editedIncrementalBlock);
    const size_t dirtyChunks = incremental.dirtyChunkCount();
    // Editing a corner voxel dirties the owning chunk and the two loaded edge
    // neighbors so their border lighting and meshes can be rebuilt together.
    const bool incrementalSaved = incrementalEdit && dirtyChunks == 3
        && incremental.saveRegionIncremental(incrementalRegion.string());
    World incrementalRestored(9999);
    const bool incrementalLoaded = incrementalSaved && incrementalRestored.loadRegion(incrementalRegion.string())
        && incrementalRestored.getBlock(-1, 3, -1) == editedIncrementalBlock;
    std::error_code cleanupError;
    std::filesystem::remove(region, cleanupError);
    std::filesystem::remove(incrementalRegion, cleanupError);
    std::cout << "voxel-demo: chunks=" << world.chunkCount()
              << " nonAir=" << nonAir
              << " safeBounds=" << (safeBounds ? "yes" : "no")
              << " generated=" << (generated ? "yes" : "no")
              << " biomes=" << (biomes ? "yes" : "no")
              << " biomeCount=" << biomeCount
              << " lighting=" << (torchLighting && boundaryLighting && restoredLighting && restoredBoundaryLighting ? "yes" : "no")
              << " mesher=" << (meshed && internalFacesCulled && meshCarriesLight ? "yes" : "no")
              << " faces=" << mesh.faceCount()
              << " greedyFaces=" << greedyMesh.faceCount()
              << " lodFaces=" << lodMesh.faceCount()
              << " lodSkirts=" << lodMesh.transitionSkirtFaces
              << " raycast=" << (voxelPhysics ? "yes" : "no")
              << " streamed=" << (streamed ? "yes" : "no")
              << " async=" << (asyncLoaded && asyncWindow ? "yes" : "no")
              << " asyncInitial=" << asyncInitial
              << " asyncInitialPending=" << asyncInitialPending
              << " asyncChunks=" << asyncWorld.chunkCount()
              << " asyncPending=" << asyncWorld.pendingChunkCount()
              << " region=" << (restoredData ? "ok" : "failed")
              << " incremental=" << (incrementalLoaded && incrementalBase ? "ok" : "failed")
              << " atomicReplace=" << (replaced ? "yes" : "no")
              << " compressed=" << (compressed ? "yes" : "no") << std::endl;
    return safeBounds && generated && biomes && streamed && asyncLoaded && asyncWindow
        && restoredData && compressed && replaced
        && incrementalLoaded && incrementalBase
        && torchLighting && boundaryLighting && restoredLighting && restoredBoundaryLighting
        && meshed && internalFacesCulled && meshCarriesLight && greedyMeshed && lodMeshed
        && lodTransitionSkirts && voxelPhysics ? 0 : 1;
}
