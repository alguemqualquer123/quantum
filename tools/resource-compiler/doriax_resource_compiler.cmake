# (c) Eduardo Doria
# SPDX-License-Identifier: MIT
#
# doriax-resource-compiler: headless asset compiler (pak / shaders / inspect).
# Mirrors the doriax-editor-cmd block of the root CMakeLists.txt: same editor
# source list, its own Windows icon and manifest, and DORIAX_CLI_FRONTEND so
# editor/main.cpp returns after CLI dispatch and never starts the backend.
#
# Include from the top-level CMakeLists right after the doriax-editor-cmd block
# (it needs DORIAX_EDITOR_SOURCES, configure_doriax_editor_target,
# doriax_add_windows_resource, doriax_add_windows_manifest and the
# copy_doriax_editor_engine target, all defined above it), and make sure
# ${EDITOR_DIR}/cli/ResourceCompilerCli.cpp is in DORIAX_EDITOR_SOURCES.

if(WIN32)
    # Its own icon because all three executables install to the same folder
    doriax_add_windows_resource(
        DORIAX_RESOURCE_COMPILER_ICON_OBJECTS
        doriax-resource-compiler
        "doriax-editor-cmd.ico"
        "doriax-resource-compiler.exe"
        "Doriax Engine Resource Compiler"
    )

    add_executable(
        doriax-resource-compiler

        EXCLUDE_FROM_ALL

        ${DORIAX_EDITOR_SOURCES}
        ${DORIAX_RESOURCE_COMPILER_ICON_OBJECTS}
    )

    configure_doriax_editor_target(doriax-resource-compiler)
    target_compile_definitions(doriax-resource-compiler PRIVATE DORIAX_CLI_FRONTEND)
    doriax_add_windows_manifest(doriax-resource-compiler)
    add_dependencies(doriax-resource-compiler copy_doriax_editor_engine)

    # Own install rule instead of touching DORIAX_EDITOR_INSTALL_TARGETS, so
    # this file works regardless of where it is included.
    install(TARGETS doriax-resource-compiler
            CONFIGURATIONS Release MinSizeRel
            RUNTIME DESTINATION bin
    )
endif()
