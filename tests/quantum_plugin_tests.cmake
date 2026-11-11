# (c) SR VINIX
# SPDX-License-Identifier: MIT
#
# quantum-plugin-tests: GPU-free probe of engine/plugin/. Include from the top-level
# CMakeLists.txt AFTER add_subdirectory(engine) and after plugins/quantum_plugins.cmake, so both
# the `quantum` link target and the plugin modules the probe maps at run time exist.
# EXCLUDE_FROM_ALL: never part of a default build.
#
# This is the only thing in the repo that proves a plugin binary really loads. A clean compile
# proves the headers line up; it says nothing about LoadLibrary, the ABI check or the vtable
# crossing a module boundary, so those are asserted here rather than assumed.

set(QUANTUM_PLUGIN_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/plugin)

add_executable(
    quantum-plugin-tests

    EXCLUDE_FROM_ALL

    ${QUANTUM_PLUGIN_TESTS_DIR}/plugin_manifest_tests.cpp
    ${QUANTUM_PLUGIN_TESTS_DIR}/plugin_manager_tests.cpp
    ${QUANTUM_PLUGIN_TESTS_DIR}/plugin_catalog_tests.cpp
)

target_include_directories(quantum-plugin-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/doctest
    # engine/CMakeLists.txt keeps its include_directories inside its own subtree.
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/core
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/plugin
)

target_link_libraries(
    quantum-plugin-tests

    PRIVATE

    quantum
    Threads::Threads
)

target_compile_definitions(quantum-plugin-tests PRIVATE
    QUANTUM_TEST_PLUGINS_DIR="${QUANTUM_PLUGIN_OUTPUT_DIR}")

# The probe maps these, so they have to be built first.
add_dependencies(quantum-plugin-tests quantum-hello quantum-broken)

if(MSVC)
    target_compile_options(quantum-plugin-tests PRIVATE /utf-8)
endif()

set_target_properties(
    quantum-plugin-tests

    PROPERTIES

    CXX_STANDARD ${QUANTUM_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    # Next to quantum.dll so it resolves at run time (engine/CMakeLists.txt:906-916).
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
