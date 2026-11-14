# (c) SR VINIX
# SPDX-License-Identifier: MIT
#
# quantum-ui-tests: GPU-free unit tests of the retained-mode UI module
# (engine/core/ui/). Include from the top-level CMakeLists.txt AFTER
# add_subdirectory(${QUANTUM_ROOT}) so the `quantum` link target exists.
# EXCLUDE_FROM_ALL: never part of a default build.
#
# doctest is vendored as a single header under libs/doctest (dev-only usage).

set(QUANTUM_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(
    quantum-ui-tests

    EXCLUDE_FROM_ALL

    ${QUANTUM_TESTS_DIR}/ui/ui_module_tests.cpp
    ${QUANTUM_TESTS_DIR}/ui/ui_layout_tests.cpp
)

target_include_directories(quantum-ui-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/doctest
)

target_link_libraries(
    quantum-ui-tests

    PRIVATE

    quantum
    Threads::Threads
)

if(MSVC)
    target_compile_options(quantum-ui-tests PRIVATE /utf-8)
endif()

set_target_properties(
    quantum-ui-tests

    PROPERTIES

    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    # Next to quantum.dll so it resolves at run time (engine/CMakeLists.txt:906-916).
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_dependencies(quantum-ui-tests quantum)
