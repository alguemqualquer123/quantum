cmake_minimum_required(VERSION 3.20)
project(VoxelModule LANGUAGES CXX)

add_library(voxel STATIC
    Chunk.cpp
    Generator.cpp
    World.cpp
)

target_include_directories(voxel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
