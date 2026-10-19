# (c) Eduardo Doria
# SPDX-License-Identifier: MIT
#
# doriax-ui-tests: GPU-free unit tests of the retained-mode UI module
# (engine/core/ui/). Include from the top-level CMakeLists.txt AFTER
# add_subdirectory(${DORIAX_ROOT}) so the `doriax` link target exists.
# EXCLUDE_FROM_ALL: never part of a default build.
#
# doctest is vendored as a single header under libs/doctest (dev-only usage).

set(DORIAX_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(
    doriax-ui-tests

    EXCLUDE_FROM_ALL

    ${DORIAX_TESTS_DIR}/ui/ui_module_tests.cpp
    ${DORIAX_TESTS_DIR}/ui/ui_layout_tests.cpp
)

target_include_directories(doriax-ui-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/doctest
)

target_link_libraries(
    doriax-ui-tests

    PRIVATE

    doriax
    Threads::Threads
)

if(MSVC)
    target_compile_options(doriax-ui-tests PRIVATE /utf-8)
endif()

set_target_properties(
    doriax-ui-tests

    PROPERTIES

    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    # Next to doriax.dll so it resolves at run time (engine/CMakeLists.txt:906-916).
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_dependencies(doriax-ui-tests doriax)
