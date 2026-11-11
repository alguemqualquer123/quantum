# (c) SR VINIX
# SPDX-License-Identifier: MIT
#
# The plugins that ship with the engine. Include from the top-level CMakeLists.txt AFTER
# add_subdirectory(engine) so the `quantum` link target exists: a plugin is a module that links
# the engine's import library, exactly like a generated game project does.
#
# Each plugin lands in ${QUANTUM_PLUGIN_OUTPUT_DIR}/<folder>/ with its plugin.json beside the
# binary, so that folder is the whole installed plugin and PluginManager can be pointed straight
# at it. Nothing is written into the source tree.

option(QUANTUM_BUILD_PLUGINS "Build the plugins that ship with the engine" ON)

# A plugin is a dynamic module by definition, and the platforms that ship everything statically
# (web, mobile) have no loader path to exercise.
if(NOT QUANTUM_BUILD_PLUGINS OR EMSCRIPTEN OR ANDROID OR IOS)
    set(QUANTUM_BUILD_PLUGINS OFF)
    return()
endif()

set(QUANTUM_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/plugins")
set(QUANTUM_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/engine")

# quantum_add_plugin(<target> <plugin folder> <source>...)
function(quantum_add_plugin target_name plugin_folder)
    add_library(${target_name} SHARED ${ARGN})

    target_link_libraries(${target_name} PRIVATE quantum)

    # engine/CMakeLists.txt sets its include_directories for its own subtree only, so a module
    # living outside it has to name the two folders its headers reach into.
    target_include_directories(${target_name} PRIVATE
        ${QUANTUM_ENGINE_ROOT}/core
        ${QUANTUM_ENGINE_ROOT}/plugin)

    set(plugin_out "${QUANTUM_PLUGIN_OUTPUT_DIR}/${plugin_folder}")

    # Windows puts a SHARED library's binary in the RUNTIME slot; POSIX in the LIBRARY one.
    # MSBuild then appends the config name unless the per-config property is set as well.
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD ${QUANTUM_CXX_STANDARD}
        CXX_STANDARD_REQUIRED ON
        FOLDER "plugins"
        RUNTIME_OUTPUT_DIRECTORY "${plugin_out}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${plugin_out}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${plugin_out}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${plugin_out}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${plugin_out}"
        LIBRARY_OUTPUT_DIRECTORY "${plugin_out}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${plugin_out}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${plugin_out}"
        LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${plugin_out}"
        LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${plugin_out}"
    )

    # The manifest belongs with the binary: PluginManager discovers a plugin by the plugin.json it
    # finds, and a folder holding only a DLL is not a plugin.
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${plugin_out}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${plugin_out}/plugin.json"
        VERBATIM)

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /utf-8)
    endif()
endfunction()

add_subdirectory(plugins/hello)
add_subdirectory(plugins/broken)

# The registry that ships with the engine: one index.json, no binaries. PluginManager only reads the
# folders it discovers, so this directory is what the package manager and the Packages window query
# when nothing else is configured - an offline install works from it alone.
add_custom_target(copy_quantum_plugin_catalog ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/plugins/catalog" "${QUANTUM_PLUGIN_OUTPUT_DIR}/catalog"
    COMMENT "Copying the built-in plugin catalog")
