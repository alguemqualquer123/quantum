# (c) SR VINIX
# SPDX-License-Identifier: MIT
#
# quantum-resource-compiler: headless asset compiler (pak / shaders / inspect).
# Mirrors the quantum-editor-cmd block of the root CMakeLists.txt: same editor
# source list, its own Windows icon and manifest, and QUANTUM_CLI_FRONTEND so
# editor/main.cpp returns after CLI dispatch and never starts the backend.
#
# Include from the top-level CMakeLists right after the quantum-editor-cmd block
# (it needs QUANTUM_EDITOR_SOURCES, configure_quantum_editor_target,
# quantum_add_windows_resource, quantum_add_windows_manifest and the
# copy_quantum_editor_engine target, all defined above it), and make sure
# ${EDITOR_DIR}/cli/ResourceCompilerCli.cpp is in QUANTUM_EDITOR_SOURCES.

set(QUANTUM_RESOURCE_COMPILER_ICON_OBJECTS)
if(WIN32)
    # Its own icon because all three executables install to the same folder
    quantum_add_windows_resource(
        QUANTUM_RESOURCE_COMPILER_ICON_OBJECTS
        quantum-resource-compiler
        "quantum-editor-cmd.ico"
        "quantum-resource-compiler.exe"
        "Quantum Engine Resource Compiler"
    )
endif()

add_executable(
    quantum-resource-compiler

    EXCLUDE_FROM_ALL

    ${QUANTUM_EDITOR_SOURCES}
    ${QUANTUM_RESOURCE_COMPILER_ICON_OBJECTS}
)

configure_quantum_editor_target(quantum-resource-compiler)
target_compile_definitions(quantum-resource-compiler PRIVATE QUANTUM_CLI_FRONTEND)
if(WIN32)
    quantum_add_windows_manifest(quantum-resource-compiler)
endif()
add_dependencies(quantum-resource-compiler copy_quantum_editor_engine)

# Own install rule instead of touching QUANTUM_EDITOR_INSTALL_TARGETS, so this
# file works regardless of where it is included. The CLI is useful on every
# desktop host; Windows only adds its icon and console manifest above.
install(TARGETS quantum-resource-compiler
        CONFIGURATIONS Release MinSizeRel
        RUNTIME DESTINATION bin
)
