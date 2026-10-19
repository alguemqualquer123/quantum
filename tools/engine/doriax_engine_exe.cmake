# (c) Eduardo Doria
# SPDX-License-Identifier: MIT
#
# doriax-engine: standalone game runtime executable target.
#
# Include this file from the top-level CMakeLists.txt AFTER:
#   - add_definitions()/include_directories() of the SOKOL_* backend
#     (root CMakeLists.txt:318-424),
#   - add_subdirectory(${DORIAX_ROOT}) so the `doriax` link target exists
#     (root CMakeLists.txt:431),
#   - on Windows, include(${DORIAX_ROOT}/platform/win/WindowsManifest.cmake)
#     (root CMakeLists.txt:159-161).
#
# It mirrors the root's configure_doriax_editor_target pattern
# (root CMakeLists.txt:636-707, copied, not refactored) and the engine's own
# standalone executable build (engine/CMakeLists.txt:925-1028), which compiles
# the engine/platform sources into the game exe. Those platform sources are
# NOT part of the doriax library, so this runner compiles its own copy and
# reuses the platform entry-loop (DoriaxWin / DoriaxMac / DoriaxLinux) while
# tools/engine/main.cpp supplies main().

set(DORIAX_ENGINE_RUNNER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools/engine)

# ---------------------------------------------------------------------------
# Platform sources (same lists as engine/CMakeLists.txt standalone build)
# ---------------------------------------------------------------------------
set(DORIAX_ENGINE_SOURCES ${DORIAX_ENGINE_RUNNER_DIR}/main.cpp)
set(DORIAX_ENGINE_LIBS Threads::Threads)

if(WIN32)
    # engine/CMakeLists.txt:326-359
    set(DORIAX_ENGINE_PLATFORM_ROOT ${DORIAX_ROOT}/platform/win)

    list(APPEND DORIAX_ENGINE_SOURCES
        ${DORIAX_ENGINE_PLATFORM_ROOT}/DoriaxWin.cpp
        ${DORIAX_ENGINE_PLATFORM_ROOT}/WindowWin.cpp
        ${DORIAX_ENGINE_PLATFORM_ROOT}/SystemWin.cpp
        ${DORIAX_ENGINE_PLATFORM_ROOT}/WinInputRouter.cpp
        ${DORIAX_ENGINE_PLATFORM_ROOT}/GamepadWin.cpp
    )

    list(APPEND DORIAX_ENGINE_LIBS
        user32
        gdi32
        dwmapi
        shell32
    )

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        # engine/CMakeLists.txt:343-350
        list(APPEND DORIAX_ENGINE_SOURCES
            ${DORIAX_ROOT}/platform/common/VulkanContext.cpp
        )
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND DORIAX_ENGINE_LIBS Vulkan::Vulkan)
    else()
        list(APPEND DORIAX_ENGINE_LIBS opengl32)
    endif()
elseif(APPLE)
    # engine/CMakeLists.txt:496-544 (non-Xcode macOS path: bare executable,
    # platform/mac backend; the root editor build is always metal on mac,
    # root CMakeLists.txt:68)
    enable_language(OBJCXX)
    set(DORIAX_ENGINE_PLATFORM_ROOT ${DORIAX_ROOT}/platform/mac)

    list(APPEND DORIAX_ENGINE_SOURCES
        ${DORIAX_ENGINE_PLATFORM_ROOT}/DoriaxMac.mm
        ${DORIAX_ENGINE_PLATFORM_ROOT}/WindowMac.mm
        ${DORIAX_ENGINE_PLATFORM_ROOT}/SystemMac.mm
        ${DORIAX_ENGINE_PLATFORM_ROOT}/MacInputRouter.mm
        ${DORIAX_ROOT}/platform/apple/DoriaxGameController.mm
    )

    if(GRAPHIC_BACKEND STREQUAL "glcore")
        list(APPEND DORIAX_ENGINE_SOURCES ${DORIAX_ENGINE_PLATFORM_ROOT}/MacViewGL.mm)
        list(APPEND DORIAX_ENGINE_LIBS "-framework OpenGL")
    elseif(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND DORIAX_ENGINE_SOURCES
            ${DORIAX_ENGINE_PLATFORM_ROOT}/MacViewVulkan.mm
            ${DORIAX_ROOT}/platform/common/VulkanContext.cpp
        )
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND DORIAX_ENGINE_LIBS Vulkan::Vulkan)
    else()
        list(APPEND DORIAX_ENGINE_SOURCES ${DORIAX_ENGINE_PLATFORM_ROOT}/MacViewMetal.mm)
        list(APPEND DORIAX_ENGINE_LIBS "-framework Metal" "-framework MetalKit")
    endif()

    list(APPEND DORIAX_ENGINE_LIBS
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework GameController"
    )

    set(DORIAX_ENGINE_OBJCXX_SOURCES)
    foreach(DORIAX_ENGINE_SRC ${DORIAX_ENGINE_SOURCES})
        if(DORIAX_ENGINE_SRC MATCHES "\\.mm$")
            list(APPEND DORIAX_ENGINE_OBJCXX_SOURCES ${DORIAX_ENGINE_SRC})
        endif()
    endforeach()
    if(DORIAX_ENGINE_OBJCXX_SOURCES)
        set_source_files_properties(
            ${DORIAX_ENGINE_OBJCXX_SOURCES}
            PROPERTIES COMPILE_FLAGS "-fobjc-arc"
        )
    endif()
elseif(UNIX)
    # engine/CMakeLists.txt:368-422
    set(DORIAX_ENGINE_PLATFORM_ROOT ${DORIAX_ROOT}/platform/linux)

    list(APPEND DORIAX_ENGINE_SOURCES
        ${DORIAX_ENGINE_PLATFORM_ROOT}/DoriaxLinux.cpp
        ${DORIAX_ENGINE_PLATFORM_ROOT}/WindowLinux.cpp
        ${DORIAX_ENGINE_PLATFORM_ROOT}/SystemLinux.cpp
        ${DORIAX_ENGINE_PLATFORM_ROOT}/LinuxInputRouter.cpp
        ${DORIAX_ENGINE_PLATFORM_ROOT}/GamepadLinux.cpp
        ${DORIAX_ENGINE_PLATFORM_ROOT}/GamepadDB.cpp
    )

    if(NOT X11_FOUND)
        find_package(X11 REQUIRED)
    endif()

    list(APPEND DORIAX_ENGINE_LIBS X11::X11 dl m)

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        list(APPEND DORIAX_ENGINE_SOURCES
            ${DORIAX_ROOT}/platform/common/VulkanContext.cpp
        )
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        list(APPEND DORIAX_ENGINE_LIBS Vulkan::Vulkan)
    else()
        list(APPEND DORIAX_ENGINE_LIBS GL)
    endif()
endif()

# ---------------------------------------------------------------------------
# Target configuration (mirrors configure_doriax_editor_target, root
# CMakeLists.txt:636-707, adapted: no imgui, no editor libs)
# ---------------------------------------------------------------------------
function(configure_doriax_engine_target target_name)
    target_include_directories(${target_name} PRIVATE
        ${DORIAX_ENGINE_RUNNER_DIR}
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
    # (engine/platform/win/DoriaxWin.cpp:23-38; the standalone engine build
    # adds them at engine/CMakeLists.txt:74-78). DEFAULT_WINDOW_* have no
    # #ifndef fallback in that file, so they must be set here. --window
    # overrides them at runtime through System::setWindowSize.
    target_compile_definitions(${target_name} PRIVATE
        DEFAULT_WINDOW_WIDTH=1280
        DEFAULT_WINDOW_HEIGHT=720
    )
    if(UNIX AND NOT APPLE)
        target_compile_definitions(${target_name} PRIVATE DORIAX_LINUX)
    endif()
    target_link_libraries(
        ${target_name}

        PRIVATE

        doriax
        ${DORIAX_ENGINE_LIBS}
    )

    # Dev convenience: when no engine/ dir is copied next to the exe (the
    # editor build ships one via copy_doriax_editor_engine), fall back to the
    # source-tree engine so the demo logo and templates resolve from a build
    # tree. Quoted here; tools/engine/main.cpp has an unquoted-empty default.
    get_filename_component(DORIAX_ENGINE_SOURCE_ENGINE "${DORIAX_ROOT}" ABSOLUTE)
    target_compile_definitions(${target_name} PRIVATE
        DORIAX_ENGINE_DIR_FALLBACK="${DORIAX_ENGINE_SOURCE_ENGINE}"
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
        # entry point is main() (engine/CMakeLists.txt:317-324,
        # root CMakeLists.txt:90-93 and 724-726).
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

add_executable(
    doriax-engine

    ${PLATFORM_EXEC_FLAGS}

    ${DORIAX_ENGINE_SOURCES}
)

configure_doriax_engine_target(doriax-engine)

if(TARGET copy_doriax_editor_engine)
    # Ship engine/ next to the exe like the editor does; the runner's
    # getEngineDir() (tools/engine/main.cpp) reads it for the demo assets.
    add_dependencies(doriax-engine copy_doriax_editor_engine)
endif()
