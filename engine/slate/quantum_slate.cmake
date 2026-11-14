# (c) SR VINIX
# SPDX-License-Identifier: MIT
#
# Slate module: retained-mode UI inspired by Unreal Slate/UMG/CommonUI.
# Included from the top-level CMakeLists.txt AFTER add_subdirectory(${QUANTUM_ROOT})
# so the `quantum` link target exists.
#
# Everything lives outside the engine library on purpose: engine/core/ui is being
# written concurrently, and engine/CMakeLists.txt is a hand-maintained list. Here
# the module is a static lib (quantum-slate), a development executable
# (quantum-slate-demo) and a GPU-free test executable (quantum-slate-tests).

set(QUANTUM_SLATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine/slate)
set(QUANTUM_SLATE_DEMO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine/slate_demo)

# ---------------------------------------------------------------------------
# quantum-slate: the module itself
# ---------------------------------------------------------------------------
set(QUANTUM_SLATE_SOURCES
    ${QUANTUM_SLATE_DIR}/Slate.cpp
    ${QUANTUM_SLATE_DIR}/core/SlateTypes.cpp
    ${QUANTUM_SLATE_DIR}/core/Widget.cpp
    ${QUANTUM_SLATE_DIR}/core/EventRouter.cpp
    ${QUANTUM_SLATE_DIR}/core/StyleSet.cpp
    ${QUANTUM_SLATE_DIR}/core/USS.cpp
    ${QUANTUM_SLATE_DIR}/core/UXML.cpp
    ${QUANTUM_SLATE_DIR}/core/PaintList.cpp
    ${QUANTUM_SLATE_DIR}/core/Geometry.cpp
    ${QUANTUM_SLATE_DIR}/core/TextRenderer.cpp
    ${QUANTUM_SLATE_DIR}/layout/FlexEngine.cpp
    ${QUANTUM_SLATE_DIR}/layout/Containers.cpp
    ${QUANTUM_SLATE_DIR}/widgets/Label.cpp
    ${QUANTUM_SLATE_DIR}/widgets/Image.cpp
    ${QUANTUM_SLATE_DIR}/widgets/Button.cpp
    ${QUANTUM_SLATE_DIR}/widgets/CheckBox.cpp
    ${QUANTUM_SLATE_DIR}/widgets/RadioButton.cpp
    ${QUANTUM_SLATE_DIR}/widgets/Slider.cpp
    ${QUANTUM_SLATE_DIR}/widgets/ProgressBar.cpp
    ${QUANTUM_SLATE_DIR}/render/sokol/SokolSlateRenderer.cpp
)

add_library(quantum-slate STATIC ${QUANTUM_SLATE_SOURCES})

target_include_directories(quantum-slate PUBLIC
    #so consumers write #include "slate/core/Widget.h"
    ${QUANTUM_ROOT}
    ${QUANTUM_SLATE_DIR}
    ${QUANTUM_ROOT}/libs/tinyxml2
)

target_link_libraries(quantum-slate PUBLIC quantum yoga)

if(MSVC)
    target_compile_options(quantum-slate PRIVATE /utf-8 /bigobj)
endif()

set_target_properties(quantum-slate PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# ---------------------------------------------------------------------------
# quantum-slate-demo: gallery executable (platform sources copied from
# tools/engine/quantum_engine_exe.cmake, because the entry loop is not in the
# quantum library)
# ---------------------------------------------------------------------------
set(QUANTUM_SLATE_DEMO_SOURCES ${QUANTUM_SLATE_DEMO_DIR}/main.cpp)
set(QUANTUM_SLATE_DEMO_LIBS Threads::Threads)

if(WIN32)
    set(QUANTUM_SLATE_DEMO_PLATFORM_ROOT ${QUANTUM_ROOT}/platform/win)
    list(APPEND QUANTUM_SLATE_DEMO_SOURCES
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/QuantumWin.cpp
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/WindowWin.cpp
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/SystemWin.cpp
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/WinInputRouter.cpp
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/GamepadWin.cpp
    )
    list(APPEND QUANTUM_SLATE_DEMO_LIBS user32 gdi32 dwmapi shell32)

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND QUANTUM_SLATE_DEMO_SOURCES ${QUANTUM_ROOT}/platform/common/VulkanContext.cpp)
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND QUANTUM_SLATE_DEMO_LIBS Vulkan::Vulkan)
    else()
        list(APPEND QUANTUM_SLATE_DEMO_LIBS opengl32)
    endif()
elseif(APPLE)
    enable_language(OBJCXX)
    set(QUANTUM_SLATE_DEMO_PLATFORM_ROOT ${QUANTUM_ROOT}/platform/mac)
    list(APPEND QUANTUM_SLATE_DEMO_SOURCES
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/QuantumMac.mm
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/WindowMac.mm
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/SystemMac.mm
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/MacInputRouter.mm
        ${QUANTUM_ROOT}/platform/apple/QuantumGameController.mm
    )
    if(GRAPHIC_BACKEND STREQUAL "glcore")
        list(APPEND QUANTUM_SLATE_DEMO_SOURCES ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/MacViewGL.mm)
        list(APPEND QUANTUM_SLATE_DEMO_LIBS "-framework OpenGL")
    elseif(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND QUANTUM_SLATE_DEMO_SOURCES
            ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/MacViewVulkan.mm
            ${QUANTUM_ROOT}/platform/common/VulkanContext.cpp
        )
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND QUANTUM_SLATE_DEMO_LIBS Vulkan::Vulkan)
    else()
        list(APPEND QUANTUM_SLATE_DEMO_SOURCES ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/MacViewMetal.mm)
        list(APPEND QUANTUM_SLATE_DEMO_LIBS "-framework Metal" "-framework MetalKit")
    endif()
    list(APPEND QUANTUM_SLATE_DEMO_LIBS
        "-framework Cocoa" "-framework QuartzCore" "-framework GameController"
    )

    set(QUANTUM_SLATE_DEMO_OBJCXX_SOURCES)
    foreach(QUANTUM_SLATE_DEMO_SRC ${QUANTUM_SLATE_DEMO_SOURCES})
        if(QUANTUM_SLATE_DEMO_SRC MATCHES "\\.mm$")
            list(APPEND QUANTUM_SLATE_DEMO_OBJCXX_SOURCES ${QUANTUM_SLATE_DEMO_SRC})
        endif()
    endforeach()
    if(QUANTUM_SLATE_DEMO_OBJCXX_SOURCES)
        set_source_files_properties(${QUANTUM_SLATE_DEMO_OBJCXX_SOURCES}
            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()
elseif(UNIX)
    set(QUANTUM_SLATE_DEMO_PLATFORM_ROOT ${QUANTUM_ROOT}/platform/linux)
    list(APPEND QUANTUM_SLATE_DEMO_SOURCES
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/QuantumLinux.cpp
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/WindowLinux.cpp
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/SystemLinux.cpp
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/LinuxInputRouter.cpp
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/GamepadLinux.cpp
        ${QUANTUM_SLATE_DEMO_PLATFORM_ROOT}/GamepadDB.cpp
    )
    if(NOT X11_FOUND)
        find_package(X11 REQUIRED)
    endif()
    list(APPEND QUANTUM_SLATE_DEMO_LIBS X11::X11 dl m)

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND QUANTUM_SLATE_DEMO_SOURCES ${QUANTUM_ROOT}/platform/common/VulkanContext.cpp)
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND QUANTUM_SLATE_DEMO_LIBS Vulkan::Vulkan)
    else()
        list(APPEND QUANTUM_SLATE_DEMO_LIBS GL)
    endif()
endif()

# EXCLUDE_FROM_ALL: the gallery is a scratch window for the Slate module, so a full
# build of the product must not spend time on it or stop on it. Build it by name when
# working on Slate (cmake --build <dir> --target quantum-slate-demo).
add_executable(quantum-slate-demo EXCLUDE_FROM_ALL ${PLATFORM_EXEC_FLAGS} ${QUANTUM_SLATE_DEMO_SOURCES})

# QuantumWin.cpp reads window defaults from compile definitions
# (engine/platform/win/QuantumWin.cpp:23-38), and the runtime loads compiled
# shaders from .sdat under QUANTUM_SHADER_PATH (engine/platform/win/SystemWin.cpp:133-139),
# the same cache an exported project bakes at generation time.
if(WIN32)
    set(QUANTUM_SLATE_SHADER_CACHE "$ENV{LOCALAPPDATA}/quantum/shaders/v23")
elseif(APPLE)
    set(QUANTUM_SLATE_SHADER_CACHE "$ENV{HOME}/Library/Caches/quantum/shaders/v23")
elseif(DEFINED ENV{XDG_CACHE_HOME})
    set(QUANTUM_SLATE_SHADER_CACHE "$ENV{XDG_CACHE_HOME}/quantum/shaders/v23")
else()
    set(QUANTUM_SLATE_SHADER_CACHE "$ENV{HOME}/.cache/quantum/shaders/v23")
endif()
file(TO_CMAKE_PATH "${QUANTUM_SLATE_SHADER_CACHE}" QUANTUM_SLATE_SHADER_CACHE)

target_compile_definitions(quantum-slate-demo PRIVATE
    DEFAULT_WINDOW_WIDTH=1280
    DEFAULT_WINDOW_HEIGHT=720
    QUANTUM_WINDOW_TITLE="Quantum Slate UI Demo"
    QUANTUM_SHADER_PATH="${QUANTUM_SLATE_SHADER_CACHE}"
)
if(UNIX AND NOT APPLE)
    target_compile_definitions(quantum-slate-demo PRIVATE QUANTUM_LINUX)
endif()

target_link_libraries(quantum-slate-demo PRIVATE quantum-slate ${QUANTUM_SLATE_DEMO_LIBS})

if(MSVC)
    target_compile_options(quantum-slate-demo PRIVATE /utf-8 /bigobj)
    #GUI subsystem enters at WinMain while the platform backend's entry is main()
    target_link_options(quantum-slate-demo PRIVATE /ENTRY:mainCRTStartup)
endif()

set_target_properties(quantum-slate-demo PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    #flat with quantum.dll so the exe finds it at run time
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(WIN32)
    if(COMMAND quantum_add_windows_manifest)
        quantum_add_windows_manifest(quantum-slate-demo)
    endif()
elseif(APPLE)
    set_target_properties(quantum-slate-demo PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set_target_properties(quantum-slate-demo PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
endif()

# ---------------------------------------------------------------------------
# quantum-slate-tests: GPU-free doctest binary, never built by default
# ---------------------------------------------------------------------------
option(QUANTUM_SLATE_BUILD_TESTS "Build the quantum-slate unit tests" ON)
if(QUANTUM_SLATE_BUILD_TESTS)
    add_executable(quantum-slate-tests EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/slate/slate_layout_tests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/slate/slate_paint_tests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/slate/slate_event_tests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/slate/slate_uss_tests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/slate/slate_uxml_tests.cpp
    )
    target_include_directories(quantum-slate-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/doctest
    )
    target_link_libraries(quantum-slate-tests PRIVATE quantum-slate)
    if(MSVC)
        target_compile_options(quantum-slate-tests PRIVATE /utf-8)
    endif()
    set_target_properties(quantum-slate-tests PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()
