cmake_minimum_required(VERSION 3.22)
project(uilab_embedded CXX)

# Stands in for one of the user's own projects: it brings its own Dear ImGui and
# GLFW targets and pulls the lab in with add_subdirectory().
find_package(OpenGL REQUIRED)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/imgui")
add_library(host_imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp")
target_include_directories(host_imgui SYSTEM PUBLIC
    "${IMGUI_DIR}" "${IMGUI_DIR}/backends" "${IMGUI_DIR}/misc/cpp")
target_compile_definitions(host_imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/glfw" "${CMAKE_BINARY_DIR}/glfw")
target_link_libraries(host_imgui PUBLIC glfw)

add_library(imgui::imgui ALIAS host_imgui)

set(UILAB_PROVIDE_THIRD_PARTY OFF CACHE BOOL "" FORCE)
set(UILAB_IMGUI_TARGET "imgui::imgui" CACHE STRING "" FORCE)
set(UILAB_GLFW_TARGET "glfw" CACHE STRING "" FORCE)
set(UILAB_BUILD_APP ON CACHE BOOL "" FORCE)
set(UILAB_EXPORT_TARGETS OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_BINARY_DIR}/uilab")
