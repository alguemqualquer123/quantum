# doriax_reflector.cmake - defines the doriax-reflector standalone CLI target.
#
# GUARD: This file expects to be include()d from the root CMakeLists.txt
# (e.g. include(${CMAKE_SOURCE_DIR}/tools/reflector/doriax_reflector.cmake)).
# It must not be add_subdirectory()d as a standalone project; it relies on the
# root project()'d compilers and standard settings.
#
# doriax-reflector replaces generate_api_suggestions.py: it parses
# engine/core/script/binding/*.cpp (+ engine headers for signatures) and emits
# engine_api_suggestions.h / a JSON API manifest. Pure C++17 std-only:
# it links against NO engine code and has NO third-party dependencies.

if(TARGET doriax-reflector)
    return()
endif()

set(DORIAX_REFLECTOR_DIR "${CMAKE_CURRENT_LIST_DIR}")

add_executable(doriax-reflector
    "${DORIAX_REFLECTOR_DIR}/reflector_main.cpp"
    "${DORIAX_REFLECTOR_DIR}/util.cpp"
    "${DORIAX_REFLECTOR_DIR}/parse_bindings.cpp"
    "${DORIAX_REFLECTOR_DIR}/parse_headers.cpp"
    "${DORIAX_REFLECTOR_DIR}/generate.cpp"
)

set_target_properties(doriax-reflector PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "tools"
)

# No include dirs or link libraries beyond the project default: the sources
# use only the C++17 standard library.

# Convenience: run the reflector over the real bindings dir.
# Usage: cmake --build . --target reflector_generate_api_suggestions
# (or invoke $<TARGET_FILE:doriax-reflector> directly from a custom command,
#  e.g. to replace the generate_api_suggestions.py add_custom_command.)
