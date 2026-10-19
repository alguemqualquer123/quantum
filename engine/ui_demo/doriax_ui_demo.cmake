# (c) Eduardo Doria
# SPDX-License-Identifier: MIT
#
# doriax-ui-demo: development-only executable for the retained-mode UI module
# (engine/core/ui/). Include this file from the top-level CMakeLists.txt AFTER:
#   - add_subdirectory(${DORIAX_ROOT}) so the `doriax` link target exists
#     (root CMakeLists.txt:463),
#   - on Windows, include(${DORIAX_ROOT}/platform/win/WindowsManifest.cmake).
#
# It mirrors tools/engine/doriax_engine_exe.cmake (copied, not refactored):
# the platform entry-loop sources are not part of the doriax library, so this
# target compiles its own copy and engine/ui_demo/main.cpp supplies main().

set(DORIAX_UI_DEMO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine/ui_demo)

# ---------------------------------------------------------------------------
# Platform sources (same lists as the doriax-engine runner)
# ---------------------------------------------------------------------------
set(DORIAX_UI_DEMO_SOURCES ${DORIAX_UI_DEMO_DIR}/main.cpp)
set(DORIAX_UI_DEMO_LIBS Threads::Threads)

if(WIN32)
    # engine/CMakeLists.txt:326-359
    set(DORIAX_UI_DEMO_PLATFORM_ROOT ${DORIAX_ROOT}/platform/win)

    list(APPEND DORIAX_UI_DEMO_SOURCES
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/DoriaxWin.cpp
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/WindowWin.cpp
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/SystemWin.cpp
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/WinInputRouter.cpp
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/GamepadWin.cpp
    )

    list(APPEND DORIAX_UI_DEMO_LIBS
        user32
        gdi32
        dwmapi
        shell32
    )

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        # engine/CMakeLists.txt:343-350
        list(APPEND DORIAX_UI_DEMO_SOURCES
            ${DORIAX_ROOT}/platform/common/VulkanContext.cpp
        )
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND DORIAX_UI_DEMO_LIBS Vulkan::Vulkan)
    else()
        list(APPEND DORIAX_UI_DEMO_LIBS opengl32)
    endif()
elseif(APPLE)
    # engine/CMakeLists.txt:496-544 (non-Xcode macOS path: bare executable,
    # platform/mac backend; the root editor build is always metal on mac)
    enable_language(OBJCXX)
    set(DORIAX_UI_DEMO_PLATFORM_ROOT ${DORIAX_ROOT}/platform/mac)

    list(APPEND DORIAX_UI_DEMO_SOURCES
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/DoriaxMac.mm
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/WindowMac.mm
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/SystemMac.mm
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/MacInputRouter.mm
        ${DORIAX_ROOT}/platform/apple/DoriaxGameController.mm
    )

    if(GRAPHIC_BACKEND STREQUAL "glcore")
        list(APPEND DORIAX_UI_DEMO_SOURCES ${DORIAX_UI_DEMO_PLATFORM_ROOT}/MacViewGL.mm)
        list(APPEND DORIAX_UI_DEMO_LIBS "-framework OpenGL")
    elseif(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND DORIAX_UI_DEMO_SOURCES
            ${DORIAX_UI_DEMO_PLATFORM_ROOT}/MacViewVulkan.mm
            ${DORIAX_ROOT}/platform/common/VulkanContext.cpp
        )
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND DORIAX_UI_DEMO_LIBS Vulkan::Vulkan)
    else()
        list(APPEND DORIAX_UI_DEMO_SOURCES ${DORIAX_UI_DEMO_PLATFORM_ROOT}/MacViewMetal.mm)
        list(APPEND DORIAX_UI_DEMO_LIBS "-framework Metal" "-framework MetalKit")
    endif()

    list(APPEND DORIAX_UI_DEMO_LIBS
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework GameController"
    )

    set(DORIAX_UI_DEMO_OBJCXX_SOURCES)
    foreach(DORIAX_UI_DEMO_SRC ${DORIAX_UI_DEMO_SOURCES})
        if(DORIAX_UI_DEMO_SRC MATCHES "\\.mm$")
            list(APPEND DORIAX_UI_DEMO_OBJCXX_SOURCES ${DORIAX_UI_DEMO_SRC})
        endif()
    endforeach()
    if(DORIAX_UI_DEMO_OBJCXX_SOURCES)
        set_source_files_properties(
            ${DORIAX_UI_DEMO_OBJCXX_SOURCES}
            PROPERTIES COMPILE_FLAGS "-fobjc-arc"
        )
    endif()
elseif(UNIX)
    # engine/CMakeLists.txt:368-422
    set(DORIAX_UI_DEMO_PLATFORM_ROOT ${DORIAX_ROOT}/platform/linux)

    list(APPEND DORIAX_UI_DEMO_SOURCES
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/DoriaxLinux.cpp
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/WindowLinux.cpp
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/SystemLinux.cpp
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/LinuxInputRouter.cpp
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/GamepadLinux.cpp
        ${DORIAX_UI_DEMO_PLATFORM_ROOT}/GamepadDB.cpp
    )

    if(NOT X11_FOUND)
        find_package(X11 REQUIRED)
    endif()

    list(APPEND DORIAX_UI_DEMO_LIBS X11::X11 dl m)

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND DORIAX_UI_DEMO_SOURCES
            ${DORIAX_ROOT}/platform/common/VulkanContext.cpp
        )
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND DORIAX_UI_DEMO_LIBS Vulkan::Vulkan)
    else()
        list(APPEND DORIAX_UI_DEMO_LIBS GL)
    endif()
endif()

# ---------------------------------------------------------------------------
# Target configuration (mirrors configure_doriax_engine_target)
# ---------------------------------------------------------------------------
function(configure_doriax_ui_demo_target target_name)
    target_include_directories(${target_name} PRIVATE
        ${DORIAX_UI_DEMO_DIR}
    )
    if(WIN32)
        target_include_directories(${target_name} PRIVATE
            ${DORIAX_ROOT}/platform/win
        )
    elseif(APPLE)
        target_include_directories(${target_name} PRIVATE
            ${DORIAX_ROOT}/platform/mac
            ${DORIAX_ROOT}/platform/apple
        )
    elseif(UNIX)
        target_include_directories(${target_name} PRIVATE
            ${DORIAX_ROOT}/platform/linux
        )
    endif()

    # DoriaxWin.cpp reads its window defaults from compile definitions
    # (engine/platform/win/DoriaxWin.cpp:23-38); DEFAULT_WINDOW_* have no
    # #ifndef fallback in that file, so they must be set here.
    #
    # The runtime loads compiled shaders from .sdat files under
    # DORIAX_SHADER_PATH (engine/platform/win/SystemWin.cpp:133-139); exported
    # projects bake the editor's user cache dir at generation time
    # (editor/Generator.cpp:414, editor/App.cpp:2731). The demo mirrors that so
    # it can run without a project's assets/shaders folder.
    if(WIN32)
        set(DORIAX_UI_DEMO_SHADER_CACHE "$ENV{LOCALAPPDATA}/doriax/shaders/v23")
    elseif(APPLE)
        set(DORIAX_UI_DEMO_SHADER_CACHE "$ENV{HOME}/Library/Caches/doriax/shaders/v23")
    elseif(DEFINED ENV{XDG_CACHE_HOME})
        set(DORIAX_UI_DEMO_SHADER_CACHE "$ENV{XDG_CACHE_HOME}/doriax/shaders/v23")
    else()
        set(DORIAX_UI_DEMO_SHADER_CACHE "$ENV{HOME}/.cache/doriax/shaders/v23")
    endif()
    file(TO_CMAKE_PATH "${DORIAX_UI_DEMO_SHADER_CACHE}" DORIAX_UI_DEMO_SHADER_CACHE)

    target_compile_definitions(${target_name} PRIVATE
        DEFAULT_WINDOW_WIDTH=1280
        DEFAULT_WINDOW_HEIGHT=720
        DORIAX_WINDOW_TITLE="Doriax UI Demo"
        DORIAX_SHADER_PATH="${DORIAX_UI_DEMO_SHADER_CACHE}"
    )
    if(UNIX AND NOT APPLE)
        target_compile_definitions(${target_name} PRIVATE DORIAX_LINUX)
    endif()

    target_link_libraries(
        ${target_name}

        PRIVATE

        doriax
        ${DORIAX_UI_DEMO_LIBS}
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
        # Same flat output dir as the doriax dll (engine/CMakeLists.txt:906-916),
        # so the exe finds doriax.dll next to it at run time.
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    if(WIN32)
        # The GUI subsystem enters at WinMain while the platform backend's
        # entry point is main() (engine/CMakeLists.txt:317-324).
        set_target_properties(${target_name} PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
        if(COMMAND doriax_add_windows_manifest)
            doriax_add_windows_manifest(${target_name})
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

    add_dependencies(${target_name} doriax)
endfunction()

# No install() rule on purpose: development-only target.
add_executable(
    doriax-ui-demo

    ${PLATFORM_EXEC_FLAGS}

    ${DORIAX_UI_DEMO_SOURCES}
)

configure_doriax_ui_demo_target(doriax-ui-demo)
