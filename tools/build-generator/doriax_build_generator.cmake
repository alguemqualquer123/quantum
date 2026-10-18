# (c) Eduardo Doria
# SPDX-License-Identifier: MIT
#
# doriax-build-generator: headless build orchestrator CLI (generate / configure /
# build / status). Lets CI and automation produce and compile a project without
# the editor GUI, mirroring the GameMaker-style split the editor already uses for
# doriax-resource-compiler.
#
# Same editor source list as doriax-editor / doriax-editor-cmd /
# doriax-resource-compiler, its own Windows icon and manifest, and
# DORIAX_CLI_FRONTEND so editor/main.cpp returns after CLI dispatch and never
# starts the backend (no window is ever created).

if(WIN32)
    # Its own icon because all four executables install to the same folder.
    doriax_add_windows_resource(
        DORIAX_BUILD_GENERATOR_ICON_OBJECTS
        doriax-build-generator
        "doriax-editor-cmd.ico"
        "doriax-build-generator.exe"
        "Doriax Engine Build Generator"
    )

    add_executable(
        doriax-build-generator

        EXCLUDE_FROM_ALL

        ${DORIAX_EDITOR_SOURCES}
        ${DORIAX_BUILD_GENERATOR_ICON_OBJECTS}
    )

    configure_doriax_editor_target(doriax-build-generator)
    target_compile_definitions(doriax-build-generator PRIVATE DORIAX_CLI_FRONTEND)
    doriax_add_windows_manifest(doriax-build-generator)
    add_dependencies(doriax-build-generator copy_doriax_editor_engine)

    # Own install rule instead of touching DORIAX_EDITOR_INSTALL_TARGETS, so
    # this file works regardless of where it is included.
    install(TARGETS doriax-build-generator
            CONFIGURATIONS Release MinSizeRel
            RUNTIME DESTINATION bin
    )
endif()
