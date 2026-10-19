cmake_minimum_required(VERSION 3.16)

project(UniversalUIComponentsLab
        VERSION 1.0.0
        DESCRIPTION "Modular UI component laboratory (Dear ImGui) with Unreal Slate/UMG and Unity Toolkit/IMGUI/uGUI patterns"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(UILAB_BUILD_APP       "Build the standalone laboratory executable"        ON)
option(UILAB_BUILD_EDITOR    "Build the dockable editor panels library"          ON)
option(UILAB_BUILD_DEMOS     "Build the demo page library"                       ON)
option(UILAB_SHARED_LIBS     "Build the uilab libraries as SHARED instead of STATIC" OFF)
option(UILAB_FETCH_DEPS      "Fetch third_party dependencies when missing"       ON)
option(UILAB_PROVIDE_THIRD_PARTY
       "Create the uilab_imgui / glfw targets (turn OFF when the host project supplies them)" ON)
# Not an option(): a host that adds this tree with add_subdirectory has no reason to
# install it, and the default depends on UILAB_TOP_LEVEL, which is only known below.
option(UILAB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(UILAB_TOP_LEVEL TRUE)
else()
    set(UILAB_TOP_LEVEL FALSE)
endif()

if(NOT DEFINED UILAB_EXPORT_TARGETS)
    set(UILAB_EXPORT_TARGETS "${UILAB_TOP_LEVEL}")
endif()

if(UILAB_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug/Release/RelWithDebInfo)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

if(UILAB_TOP_LEVEL AND WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

if(UILAB_PROVIDE_THIRD_PARTY)
    # Third parties are pinned git revisions (see README.md); consumers that already
    # build Dear ImGui / GLFW set UILAB_PROVIDE_THIRD_PARTY=OFF and point
    # UILAB_IMGUI_TARGET / UILAB_GLFW_TARGET at their own targets.
    set(UILAB_IMGUI_TAG "v1.93.0-docking")
    set(UILAB_GLFW_TAG  "3.5.1")

    set(UILAB_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
    set(UILAB_GLFW_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw")

    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

    if(EXISTS "${UILAB_GLFW_DIR}/CMakeLists.txt")
        add_subdirectory("${UILAB_GLFW_DIR}" "${CMAKE_BINARY_DIR}/_deps/glfw-build" EXCLUDE_FROM_ALL)
    elseif(UILAB_FETCH_DEPS)
        message(STATUS "third_party/glfw not found - fetching GLFW ${UILAB_GLFW_TAG}")
        include(FetchContent)
        FetchContent_Declare(uilab_glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG        ${UILAB_GLFW_TAG}
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(uilab_glfw)
    else()
        message(FATAL_ERROR "GLFW not found in third_party/glfw and UILAB_FETCH_DEPS=OFF")
    endif()

    if(NOT EXISTS "${UILAB_IMGUI_DIR}/imgui.cpp")
        if(UILAB_FETCH_DEPS)
            message(STATUS "third_party/imgui not found - fetching Dear ImGui ${UILAB_IMGUI_TAG}")
            include(FetchContent)
            FetchContent_Declare(uilab_imgui
                GIT_REPOSITORY https://github.com/ocornut/imgui.git
                GIT_TAG        ${UILAB_IMGUI_TAG}
                GIT_SHALLOW    TRUE)
            FetchContent_MakeAvailable(uilab_imgui)
            set(UILAB_IMGUI_DIR "${uilab_imgui_SOURCE_DIR}")
        else()
            message(FATAL_ERROR "Dear ImGui not found in third_party/imgui and UILAB_FETCH_DEPS=OFF")
        endif()
    endif()

    add_library(uilab_imgui STATIC
        "${UILAB_IMGUI_DIR}/imgui.cpp"
        "${UILAB_IMGUI_DIR}/imgui_draw.cpp"
        "${UILAB_IMGUI_DIR}/imgui_tables.cpp"
        "${UILAB_IMGUI_DIR}/imgui_widgets.cpp"
        "${UILAB_IMGUI_DIR}/imgui_demo.cpp"
        "${UILAB_IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp"
        "${UILAB_IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
        "${UILAB_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp")

    target_include_directories(uilab_imgui SYSTEM PUBLIC
        "${UILAB_IMGUI_DIR}"
        "${UILAB_IMGUI_DIR}/backends"
        "${UILAB_IMGUI_DIR}/misc/cpp")

    target_compile_definitions(uilab_imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
    target_link_libraries(uilab_imgui PUBLIC glfw)
    set_target_properties(uilab_imgui PROPERTIES FOLDER "third_party")

    set(UILAB_IMGUI_TARGET uilab_imgui CACHE STRING "Target that provides Dear ImGui + GLFW/OpenGL3 backends")
else()
    if(NOT DEFINED UILAB_IMGUI_TARGET)
        set(UILAB_IMGUI_TARGET "imgui::imgui" CACHE STRING "Target provided by the host project")
    endif()
    if(NOT DEFINED UILAB_GLFW_TARGET)
        set(UILAB_GLFW_TARGET "glfw" CACHE STRING "Target provided by the host project")
    endif()
    message(STATUS "UILAB_PROVIDE_THIRD_PARTY=OFF - using host targets "
                   "${UILAB_GLFW_TARGET} / ${UILAB_IMGUI_TARGET}")
endif()

set(UILAB_GLFW_TARGET "glfw" CACHE STRING "Target that provides GLFW")

# OpenGL
find_package(OpenGL QUIET)
if(TARGET OpenGL::GL)
    set(UILAB_OPENGL_TARGET OpenGL::GL)
elseif(WIN32)
    set(UILAB_OPENGL_TARGET opengl32)
else()
    set(UILAB_OPENGL_TARGET GL)
endif()

# ---------------------------------------------------------------------------
# Library plumbing
# ---------------------------------------------------------------------------
if(UILAB_SHARED_LIBS)
    set(UILAB_LIB_TYPE SHARED)
else()
    set(UILAB_LIB_TYPE STATIC)
endif()

function(uilab_target_setup target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "uilab")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /wd4244 /wd4267)
        # PRIVATE on purpose: these only steer this library's own translation units, and
        # a host that links these objects into a bigger target must not inherit them.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
        target_compile_definitions(${target} PRIVATE NOMINMAX)
    endif()
    # The host's ImGui target may be built without the free vector operators this
    # library's sources use, and imgui.h switches on them per translation unit.
    target_compile_definitions(${target} PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
    if(UILAB_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /WX)
        else()
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    if(UILAB_SHARED_LIBS AND WIN32)
        # The public classes carry no __declspec annotations, so let CMake emit the
        # export list for the DLL instead of editing every header.
        set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endif()
endfunction()

# ---- UILab::Core: model, theme, widgets, canvas ----------------------------
add_library(uilab_core ${UILAB_LIB_TYPE}
    src/core/AppContext.cpp
    src/core/Dialogs.cpp
    src/core/EventLog.cpp
    src/core/Notifications.cpp
    src/core/Registry.cpp
    src/core/Serialize.cpp
    src/core/Settings.cpp
    src/core/Types.cpp
    src/core/UndoStack.cpp
    src/core/Validation.cpp

    src/panels/AssetLibrary.cpp

    src/themes/Theme.cpp

    src/ui/Icons.cpp
    src/ui/Texture.cpp
    src/ui/Widgets.cpp

    src/canvas/CanvasScene.cpp
    src/canvas/CanvasRenderer.cpp)

add_library(UILab::Core ALIAS uilab_core)

target_include_directories(uilab_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include/uilab>)

# Dear ImGui is required while building inside this tree: an installed consumer is
# expected to provide its own ImGui + window backend. GLFW and OpenGL belong to the
# standalone application only (src/app is the sole code that opens a window), so a
# host that builds just the libraries does not need either.
target_link_libraries(uilab_core PUBLIC
    $<BUILD_INTERFACE:${UILAB_IMGUI_TARGET}>)
if(UILAB_BUILD_APP)
    target_link_libraries(uilab_core PUBLIC
        $<BUILD_INTERFACE:${UILAB_GLFW_TARGET}>
        $<BUILD_INTERFACE:${UILAB_OPENGL_TARGET}>)
endif()
uilab_target_setup(uilab_core)

# ---- UILab::Editor: dockable panels + docking layout -----------------------
if(UILAB_BUILD_EDITOR)
    add_library(uilab_editor ${UILAB_LIB_TYPE}
        src/layout/DockingLayout.cpp

        src/panels/AssetBrowserPanel.cpp
        src/panels/ComponentTreePanel.cpp
        src/panels/ConsolePanel.cpp
        src/panels/HierarchyPanel.cpp
        src/panels/InspectorPanel.cpp
        src/panels/MenuBarPanel.cpp
        src/panels/PanelCommon.cpp
        src/panels/StatusBarPanel.cpp
        src/panels/ThemeEditorPanel.cpp
        src/panels/ToolbarPanel.cpp)

    add_library(UILab::Editor ALIAS uilab_editor)
    target_link_libraries(uilab_editor PUBLIC UILab::Core)
    uilab_target_setup(uilab_editor)
endif()

# ---- UILab::Demos: the seven paradigm pages + the mini USS engine ----------
if(UILAB_BUILD_DEMOS)
    add_library(uilab_demos ${UILAB_LIB_TYPE}
        src/demo/DemoBasics.cpp
        src/demo/DemoHost.cpp
        src/demo/DemoImmediate.cpp
        src/demo/DemoLayout.cpp
        src/demo/DemoSlate.cpp
        src/demo/DemoUGui.cpp
        src/demo/DemoUIToolkit.cpp
        src/demo/DemoUMG.cpp
        src/demo/Stylesheet.cpp)

    add_library(UILab::Demos ALIAS uilab_demos)
    target_link_libraries(uilab_demos PUBLIC UILab::Core)
    if(UILAB_BUILD_EDITOR)
        target_link_libraries(uilab_demos PUBLIC UILab::Editor)
    endif()
    uilab_target_setup(uilab_demos)
endif()

# ---------------------------------------------------------------------------
# Standalone application (window + loop + self test)
# ---------------------------------------------------------------------------
if(UILAB_BUILD_APP)
    if(NOT UILAB_BUILD_EDITOR OR NOT UILAB_BUILD_DEMOS)
        message(FATAL_ERROR "UILAB_BUILD_APP requires UILAB_BUILD_EDITOR and UILAB_BUILD_DEMOS")
    endif()

    add_executable(UniversalUIComponentsLab
        src/main.cpp
        src/app/Application.cpp
        src/demo/SelfTest.cpp)

    target_link_libraries(UniversalUIComponentsLab PRIVATE UILab::Demos UILab::Editor UILab::Core)

    set_target_properties(UniversalUIComponentsLab PROPERTIES
        FOLDER "."
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    uilab_target_setup(UniversalUIComponentsLab)

    add_custom_command(TARGET UniversalUIComponentsLab POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Built: $<TARGET_FILE:UniversalUIComponentsLab>")
endif()

# ---------------------------------------------------------------------------
# Install / package export (used when another project consumes this as a lib)
# ---------------------------------------------------------------------------
if(UILAB_EXPORT_TARGETS)
    include(CMakePackageConfigHelpers)
    include(GNUInstallDirs)

    set(UILAB_EXPORTED_TARGETS uilab_core)
    if(UILAB_BUILD_EDITOR)
        list(APPEND UILAB_EXPORTED_TARGETS uilab_editor)
    endif()
    if(UILAB_BUILD_DEMOS)
        list(APPEND UILAB_EXPORTED_TARGETS uilab_demos)
    endif()

    # Exported names match the in-tree aliases: UILab::Core / UILab::Editor / UILab::Demos.
    set_target_properties(uilab_core PROPERTIES EXPORT_NAME Core)
    if(UILAB_BUILD_EDITOR)
        set_target_properties(uilab_editor PROPERTIES EXPORT_NAME Editor)
    endif()
    if(UILAB_BUILD_DEMOS)
        set_target_properties(uilab_demos PROPERTIES EXPORT_NAME Demos)
    endif()

    install(TARGETS ${UILAB_EXPORTED_TARGETS}
        EXPORT UILabTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    install(DIRECTORY src/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uilab
        FILES_MATCHING
        PATTERN "app" EXCLUDE
        PATTERN "*.h")

    install(EXPORT UILabTargets
        FILE UILabTargets.cmake
        NAMESPACE UILab::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/UILab)

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/UILabConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/UILabConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/UILabConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/UILab)

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/UILabConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/UILabConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/UILab)
endif()

if(UILAB_TOP_LEVEL)
    message(STATUS "----------------------------------------------------------------")
    message(STATUS " UniversalUIComponentsLab ${PROJECT_VERSION}")
    message(STATUS "   generator        : ${CMAKE_GENERATOR}")
    message(STATUS "   build type       : ${CMAKE_BUILD_TYPE}")
    message(STATUS "   libs             : core / editor=${UILAB_BUILD_EDITOR} demos=${UILAB_BUILD_DEMOS} shared=${UILAB_SHARED_LIBS}")
    message(STATUS "   app              : ${UILAB_BUILD_APP}")
    message(STATUS "   imgui target     : ${UILAB_IMGUI_TARGET}")
    message(STATUS "----------------------------------------------------------------")
endif()
