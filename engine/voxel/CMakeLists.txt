cmake_minimum_required(VERSION 3.20)
project(VoxelModule LANGUAGES CXX)

add_library(voxel STATIC
    Chunk.cpp
    Mesher.cpp
    Generator.cpp
    World.cpp
)

target_include_directories(voxel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(voxel PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
