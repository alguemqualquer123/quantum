# (c) Eduardo Doria
# SPDX-License-Identifier: MIT
#
# Slate module: retained-mode UI inspired by Unreal Slate/UMG/CommonUI.
# Included from the top-level CMakeLists.txt AFTER add_subdirectory(${DORIAX_ROOT})
# so the `doriax` link target exists.
#
# Everything lives outside the engine library on purpose: engine/core/ui is being
# written concurrently, and engine/CMakeLists.txt is a hand-maintained list. Here
# the module is a static lib (doriax-slate), a development executable
# (doriax-slate-demo) and a GPU-free test executable (doriax-slate-tests).

set(DORIAX_SLATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine/slate)
set(DORIAX_SLATE_DEMO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine/slate_demo)

# ---------------------------------------------------------------------------
# doriax-slate: the module itself
# ---------------------------------------------------------------------------
set(DORIAX_SLATE_SOURCES
    ${DORIAX_SLATE_DIR}/Slate.cpp
    ${DORIAX_SLATE_DIR}/core/SlateTypes.cpp
    ${DORIAX_SLATE_DIR}/core/Widget.cpp
    ${DORIAX_SLATE_DIR}/core/StyleSet.cpp
    ${DORIAX_SLATE_DIR}/core/PaintList.cpp
    ${DORIAX_SLATE_DIR}/core/Geometry.cpp
    ${DORIAX_SLATE_DIR}/core/TextRenderer.cpp
    ${DORIAX_SLATE_DIR}/layout/FlexEngine.cpp
    ${DORIAX_SLATE_DIR}/layout/Containers.cpp
    ${DORIAX_SLATE_DIR}/widgets/Label.cpp
    ${DORIAX_SLATE_DIR}/widgets/Image.cpp
    ${DORIAX_SLATE_DIR}/widgets/Button.cpp
    ${DORIAX_SLATE_DIR}/widgets/ProgressBar.cpp
    ${DORIAX_SLATE_DIR}/render/sokol/SokolSlateRenderer.cpp
)

add_library(doriax-slate STATIC ${DORIAX_SLATE_SOURCES})

target_include_directories(doriax-slate PUBLIC
    #so consumers write #include "slate/core/Widget.h"
    ${DORIAX_ROOT}
    ${DORIAX_SLATE_DIR}
)

target_link_libraries(doriax-slate PUBLIC doriax yoga)

if(MSVC)
    target_compile_options(doriax-slate PRIVATE /utf-8 /bigobj)
endif()

set_target_properties(doriax-slate PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# ---------------------------------------------------------------------------
# doriax-slate-demo: gallery executable (platform sources copied from
# tools/engine/doriax_engine_exe.cmake, because the entry loop is not in the
# doriax library)
# ---------------------------------------------------------------------------
set(DORIAX_SLATE_DEMO_SOURCES ${DORIAX_SLATE_DEMO_DIR}/main.cpp)
set(DORIAX_SLATE_DEMO_LIBS Threads::Threads)

if(WIN32)
    set(DORIAX_SLATE_DEMO_PLATFORM_ROOT ${DORIAX_ROOT}/platform/win)
    list(APPEND DORIAX_SLATE_DEMO_SOURCES
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/DoriaxWin.cpp
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/WindowWin.cpp
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/SystemWin.cpp
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/WinInputRouter.cpp
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/GamepadWin.cpp
    )
    list(APPEND DORIAX_SLATE_DEMO_LIBS user32 gdi32 dwmapi shell32)

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND DORIAX_SLATE_DEMO_SOURCES ${DORIAX_ROOT}/platform/common/VulkanContext.cpp)
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND DORIAX_SLATE_DEMO_LIBS Vulkan::Vulkan)
    else()
        list(APPEND DORIAX_SLATE_DEMO_LIBS opengl32)
    endif()
elseif(APPLE)
    enable_language(OBJCXX)
    set(DORIAX_SLATE_DEMO_PLATFORM_ROOT ${DORIAX_ROOT}/platform/mac)
    list(APPEND DORIAX_SLATE_DEMO_SOURCES
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/DoriaxMac.mm
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/WindowMac.mm
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/SystemMac.mm
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/MacInputRouter.mm
        ${DORIAX_ROOT}/platform/apple/DoriaxGameController.mm
    )
    if(GRAPHIC_BACKEND STREQUAL "glcore")
        list(APPEND DORIAX_SLATE_DEMO_SOURCES ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/MacViewGL.mm)
        list(APPEND DORIAX_SLATE_DEMO_LIBS "-framework OpenGL")
    elseif(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND DORIAX_SLATE_DEMO_SOURCES
            ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/MacViewVulkan.mm
            ${DORIAX_ROOT}/platform/common/VulkanContext.cpp
        )
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND DORIAX_SLATE_DEMO_LIBS Vulkan::Vulkan)
    else()
        list(APPEND DORIAX_SLATE_DEMO_SOURCES ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/MacViewMetal.mm)
        list(APPEND DORIAX_SLATE_DEMO_LIBS "-framework Metal" "-framework MetalKit")
    endif()
    list(APPEND DORIAX_SLATE_DEMO_LIBS
        "-framework Cocoa" "-framework QuartzCore" "-framework GameController"
    )

    set(DORIAX_SLATE_DEMO_OBJCXX_SOURCES)
    foreach(DORIAX_SLATE_DEMO_SRC ${DORIAX_SLATE_DEMO_SOURCES})
        if(DORIAX_SLATE_DEMO_SRC MATCHES "\\.mm$")
            list(APPEND DORIAX_SLATE_DEMO_OBJCXX_SOURCES ${DORIAX_SLATE_DEMO_SRC})
        endif()
    endforeach()
    if(DORIAX_SLATE_DEMO_OBJCXX_SOURCES)
        set_source_files_properties(${DORIAX_SLATE_DEMO_OBJCXX_SOURCES}
            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()
elseif(UNIX)
    set(DORIAX_SLATE_DEMO_PLATFORM_ROOT ${DORIAX_ROOT}/platform/linux)
    list(APPEND DORIAX_SLATE_DEMO_SOURCES
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/DoriaxLinux.cpp
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/WindowLinux.cpp
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/SystemLinux.cpp
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/LinuxInputRouter.cpp
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/GamepadLinux.cpp
        ${DORIAX_SLATE_DEMO_PLATFORM_ROOT}/GamepadDB.cpp
    )
    if(NOT X11_FOUND)
        find_package(X11 REQUIRED)
    endif()
    list(APPEND DORIAX_SLATE_DEMO_LIBS X11::X11 dl m)

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND DORIAX_SLATE_DEMO_SOURCES ${DORIAX_ROOT}/platform/common/VulkanContext.cpp)
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND DORIAX_SLATE_DEMO_LIBS Vulkan::Vulkan)
    else()
        list(APPEND DORIAX_SLATE_DEMO_LIBS GL)
    endif()
endif()

add_executable(doriax-slate-demo ${PLATFORM_EXEC_FLAGS} ${DORIAX_SLATE_DEMO_SOURCES})

# DoriaxWin.cpp reads window defaults from compile definitions
# (engine/platform/win/DoriaxWin.cpp:23-38), and the runtime loads compiled
# shaders from .sdat under DORIAX_SHADER_PATH (engine/platform/win/SystemWin.cpp:133-139),
# the same cache an exported project bakes at generation time.
if(WIN32)
    set(DORIAX_SLATE_SHADER_CACHE "$ENV{LOCALAPPDATA}/doriax/shaders/v23")
elseif(APPLE)
    set(DORIAX_SLATE_SHADER_CACHE "$ENV{HOME}/Library/Caches/doriax/shaders/v23")
elseif(DEFINED ENV{XDG_CACHE_HOME})
    set(DORIAX_SLATE_SHADER_CACHE "$ENV{XDG_CACHE_HOME}/doriax/shaders/v23")
else()
    set(DORIAX_SLATE_SHADER_CACHE "$ENV{HOME}/.cache/doriax/shaders/v23")
endif()
file(TO_CMAKE_PATH "${DORIAX_SLATE_SHADER_CACHE}" DORIAX_SLATE_SHADER_CACHE)

target_compile_definitions(doriax-slate-demo PRIVATE
    DEFAULT_WINDOW_WIDTH=1280
    DEFAULT_WINDOW_HEIGHT=720
    DORIAX_WINDOW_TITLE="Doriax Slate UI Demo"
    DORIAX_SHADER_PATH="${DORIAX_SLATE_SHADER_CACHE}"
)
if(UNIX AND NOT APPLE)
    target_compile_definitions(doriax-slate-demo PRIVATE DORIAX_LINUX)
endif()

target_link_libraries(doriax-slate-demo PRIVATE doriax-slate ${DORIAX_SLATE_DEMO_LIBS})

if(MSVC)
    target_compile_options(doriax-slate-demo PRIVATE /utf-8 /bigobj)
    #GUI subsystem enters at WinMain while the platform backend's entry is main()
    target_link_options(doriax-slate-demo PRIVATE /ENTRY:mainCRTStartup)
endif()

set_target_properties(doriax-slate-demo PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    #flat with doriax.dll so the exe finds it at run time
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(WIN32)
    if(COMMAND doriax_add_windows_manifest)
        doriax_add_windows_manifest(doriax-slate-demo)
    endif()
elseif(APPLE)
    set_target_properties(doriax-slate-demo PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set_target_properties(doriax-slate-demo PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
endif()

# ---------------------------------------------------------------------------
# doriax-slate-tests: GPU-free doctest binary, never built by default
# ---------------------------------------------------------------------------
option(DORIAX_SLATE_BUILD_TESTS "Build the doriax-slate unit tests" ON)
if(DORIAX_SLATE_BUILD_TESTS)
    add_executable(doriax-slate-tests EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/slate/slate_layout_tests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/slate/slate_paint_tests.cpp
    )
    target_include_directories(doriax-slate-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/doctest
    )
    target_link_libraries(doriax-slate-tests PRIVATE doriax-slate)
    if(MSVC)
        target_compile_options(doriax-slate-tests PRIVATE /utf-8)
    endif()
    set_target_properties(doriax-slate-tests PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()
