cmake_minimum_required(VERSION 3.20)
project(VoxelDemo LANGUAGES CXX)

add_executable(voxel_demo main.cpp)

target_link_libraries(voxel_demo PRIVATE voxel)

target_include_directories(voxel_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../voxel)
