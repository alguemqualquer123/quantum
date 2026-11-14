# (c) SR VINIX
# SPDX-License-Identifier: MIT
#
# quantum-ui-demo: development-only executable for the retained-mode UI module
# (engine/core/ui/). Include this file from the top-level CMakeLists.txt AFTER:
#   - add_subdirectory(${QUANTUM_ROOT}) so the `quantum` link target exists
#     (root CMakeLists.txt:463),
#   - on Windows, include(${QUANTUM_ROOT}/platform/win/WindowsManifest.cmake).
#
# It mirrors tools/engine/quantum_engine_exe.cmake (copied, not refactored):
# the platform entry-loop sources are not part of the quantum library, so this
# target compiles its own copy and engine/ui_demo/main.cpp supplies main().

set(QUANTUM_UI_DEMO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine/ui_demo)

# ---------------------------------------------------------------------------
# Platform sources (same lists as the quantum-engine runner)
# ---------------------------------------------------------------------------
set(QUANTUM_UI_DEMO_SOURCES ${QUANTUM_UI_DEMO_DIR}/main.cpp)
set(QUANTUM_UI_DEMO_LIBS Threads::Threads)

if(WIN32)
    # engine/CMakeLists.txt:326-359
    set(QUANTUM_UI_DEMO_PLATFORM_ROOT ${QUANTUM_ROOT}/platform/win)

    list(APPEND QUANTUM_UI_DEMO_SOURCES
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/QuantumWin.cpp
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/WindowWin.cpp
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/SystemWin.cpp
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/WinInputRouter.cpp
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/GamepadWin.cpp
    )

    list(APPEND QUANTUM_UI_DEMO_LIBS
        user32
        gdi32
        dwmapi
        shell32
    )

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        # engine/CMakeLists.txt:343-350
        list(APPEND QUANTUM_UI_DEMO_SOURCES
            ${QUANTUM_ROOT}/platform/common/VulkanContext.cpp
        )
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND QUANTUM_UI_DEMO_LIBS Vulkan::Vulkan)
    else()
        list(APPEND QUANTUM_UI_DEMO_LIBS opengl32)
    endif()
elseif(APPLE)
    # engine/CMakeLists.txt:496-544 (non-Xcode macOS path: bare executable,
    # platform/mac backend; the root editor build is always metal on mac)
    enable_language(OBJCXX)
    set(QUANTUM_UI_DEMO_PLATFORM_ROOT ${QUANTUM_ROOT}/platform/mac)

    list(APPEND QUANTUM_UI_DEMO_SOURCES
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/QuantumMac.mm
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/WindowMac.mm
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/SystemMac.mm
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/MacInputRouter.mm
        ${QUANTUM_ROOT}/platform/apple/QuantumGameController.mm
    )

    if(GRAPHIC_BACKEND STREQUAL "glcore")
        list(APPEND QUANTUM_UI_DEMO_SOURCES ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/MacViewGL.mm)
        list(APPEND QUANTUM_UI_DEMO_LIBS "-framework OpenGL")
    elseif(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND QUANTUM_UI_DEMO_SOURCES
            ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/MacViewVulkan.mm
            ${QUANTUM_ROOT}/platform/common/VulkanContext.cpp
        )
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND QUANTUM_UI_DEMO_LIBS Vulkan::Vulkan)
    else()
        list(APPEND QUANTUM_UI_DEMO_SOURCES ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/MacViewMetal.mm)
        list(APPEND QUANTUM_UI_DEMO_LIBS "-framework Metal" "-framework MetalKit")
    endif()

    list(APPEND QUANTUM_UI_DEMO_LIBS
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework GameController"
    )

    set(QUANTUM_UI_DEMO_OBJCXX_SOURCES)
    foreach(QUANTUM_UI_DEMO_SRC ${QUANTUM_UI_DEMO_SOURCES})
        if(QUANTUM_UI_DEMO_SRC MATCHES "\\.mm$")
            list(APPEND QUANTUM_UI_DEMO_OBJCXX_SOURCES ${QUANTUM_UI_DEMO_SRC})
        endif()
    endforeach()
    if(QUANTUM_UI_DEMO_OBJCXX_SOURCES)
        set_source_files_properties(
            ${QUANTUM_UI_DEMO_OBJCXX_SOURCES}
            PROPERTIES COMPILE_FLAGS "-fobjc-arc"
        )
    endif()
elseif(UNIX)
    # engine/CMakeLists.txt:368-422
    set(QUANTUM_UI_DEMO_PLATFORM_ROOT ${QUANTUM_ROOT}/platform/linux)

    list(APPEND QUANTUM_UI_DEMO_SOURCES
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/QuantumLinux.cpp
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/WindowLinux.cpp
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/SystemLinux.cpp
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/LinuxInputRouter.cpp
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/GamepadLinux.cpp
        ${QUANTUM_UI_DEMO_PLATFORM_ROOT}/GamepadDB.cpp
    )

    if(NOT X11_FOUND)
        find_package(X11 REQUIRED)
    endif()

    list(APPEND QUANTUM_UI_DEMO_LIBS X11::X11 dl m)

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND QUANTUM_UI_DEMO_SOURCES
            ${QUANTUM_ROOT}/platform/common/VulkanContext.cpp
        )
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND QUANTUM_UI_DEMO_LIBS Vulkan::Vulkan)
    else()
        list(APPEND QUANTUM_UI_DEMO_LIBS GL)
    endif()
endif()

# ---------------------------------------------------------------------------
# Target configuration (mirrors configure_quantum_engine_target)
# ---------------------------------------------------------------------------
function(configure_quantum_ui_demo_target target_name)
    target_include_directories(${target_name} PRIVATE
        ${QUANTUM_UI_DEMO_DIR}
    )
    if(WIN32)
        target_include_directories(${target_name} PRIVATE
            ${QUANTUM_ROOT}/platform/win
        )
    elseif(APPLE)
        target_include_directories(${target_name} PRIVATE
            ${QUANTUM_ROOT}/platform/mac
            ${QUANTUM_ROOT}/platform/apple
        )
    elseif(UNIX)
        target_include_directories(${target_name} PRIVATE
            ${QUANTUM_ROOT}/platform/linux
        )
    endif()

    # QuantumWin.cpp reads its window defaults from compile definitions
    # (engine/platform/win/QuantumWin.cpp:23-38); DEFAULT_WINDOW_* have no
    # #ifndef fallback in that file, so they must be set here.
    #
    # The runtime loads compiled shaders from .sdat files under
    # QUANTUM_SHADER_PATH (engine/platform/win/SystemWin.cpp:133-139); exported
    # projects bake the editor's user cache dir at generation time
    # (editor/Generator.cpp:414, editor/App.cpp:2731). The demo mirrors that so
    # it can run without a project's assets/shaders folder.
    if(WIN32)
        set(QUANTUM_UI_DEMO_SHADER_CACHE "$ENV{LOCALAPPDATA}/quantum/shaders/v23")
    elseif(APPLE)
        set(QUANTUM_UI_DEMO_SHADER_CACHE "$ENV{HOME}/Library/Caches/quantum/shaders/v23")
    elseif(DEFINED ENV{XDG_CACHE_HOME})
        set(QUANTUM_UI_DEMO_SHADER_CACHE "$ENV{XDG_CACHE_HOME}/quantum/shaders/v23")
    else()
        set(QUANTUM_UI_DEMO_SHADER_CACHE "$ENV{HOME}/.cache/quantum/shaders/v23")
    endif()
    file(TO_CMAKE_PATH "${QUANTUM_UI_DEMO_SHADER_CACHE}" QUANTUM_UI_DEMO_SHADER_CACHE)

    target_compile_definitions(${target_name} PRIVATE
        DEFAULT_WINDOW_WIDTH=1280
        DEFAULT_WINDOW_HEIGHT=720
        QUANTUM_WINDOW_TITLE="Quantum UI Demo"
        QUANTUM_SHADER_PATH="${QUANTUM_UI_DEMO_SHADER_CACHE}"
    )
    if(UNIX AND NOT APPLE)
        target_compile_definitions(${target_name} PRIVATE QUANTUM_LINUX)
    endif()

    target_link_libraries(
        ${target_name}

        PRIVATE

        quantum
        ${QUANTUM_UI_DEMO_LIBS}
    )

    if(MSVC)
        # engine/CMakeLists.txt:1004-1011: generated/embedded literals are
        # UTF-8 without BOM.
        target_compile_options(${target_name} PRIVATE /utf-8 /bigobj)
    endif()

    set_target_properties(
        ${target_name}

        PROPERTIES

        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        # Same flat output dir as the quantum dll (engine/CMakeLists.txt:906-916),
        # so the exe finds quantum.dll next to it at run time.
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    if(WIN32)
        # The GUI subsystem enters at WinMain while the platform backend's
        # entry point is main() (engine/CMakeLists.txt:317-324).
        set_target_properties(${target_name} PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
        if(COMMAND quantum_add_windows_manifest)
            quantum_add_windows_manifest(${target_name})
        endif()
    elseif(APPLE)
        set_target_properties(${target_name} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@loader_path"
        )
    elseif(UNIX)
        set_target_properties(${target_name} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN"
        )
    endif()

    add_dependencies(${target_name} quantum)
endfunction()

# No install() rule on purpose: development-only target.
add_executable(
    quantum-ui-demo

    ${PLATFORM_EXEC_FLAGS}

    ${QUANTUM_UI_DEMO_SOURCES}
)

configure_quantum_ui_demo_target(quantum-ui-demo)
