# (c) SR VINIX
# SPDX-License-Identifier: MIT
#
# quantum-model-tests: GPU-free unit tests of the modelling kernel
# (engine/core/model/). Include from the top-level CMakeLists.txt AFTER
# add_subdirectory(${QUANTUM_ROOT}) so the `quantum` link target exists.
# EXCLUDE_FROM_ALL: never part of a default build.
#
# The kernel is pure CPU geometry - no scene, no entity, no render device - so these
# tests are the only way to prove an editing operation before a viewport exists to show it.

set(QUANTUM_MODEL_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/model)

add_executable(
    quantum-model-tests

    EXCLUDE_FROM_ALL

    ${QUANTUM_MODEL_TESTS_DIR}/mesh_topology_tests.cpp
    ${QUANTUM_MODEL_TESTS_DIR}/mesh_modeling_tests.cpp
    ${QUANTUM_MODEL_TESTS_DIR}/mesh_picker_tests.cpp
    ${QUANTUM_MODEL_TESTS_DIR}/mesh_export_tests.cpp
    ${QUANTUM_MODEL_TESTS_DIR}/mesh_import_tests.cpp
    ${QUANTUM_MODEL_TESTS_DIR}/mesh_report_tests.cpp
)

target_include_directories(quantum-model-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/doctest
    # The exporters are proven by reading their output back with the libraries the engine
    # itself loads models with, so the test needs their headers on its own include path:
    # engine/CMakeLists.txt:768-769 sets those only for the engine subtree.
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/libs/tinygltf
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/libs/tinyobjloader
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/libs/json
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/libs/stb
)

target_link_libraries(
    quantum-model-tests

    PRIVATE

    quantum
    Threads::Threads
)

if(MSVC)
    target_compile_options(quantum-model-tests PRIVATE /utf-8)
endif()

set_target_properties(
    quantum-model-tests

    PROPERTIES

    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    # Next to quantum.dll so it resolves at run time (engine/CMakeLists.txt:906-916).
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_dependencies(quantum-model-tests quantum)
