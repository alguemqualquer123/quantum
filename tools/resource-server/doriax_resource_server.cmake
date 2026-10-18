# (c) Eduardo Doria
# SPDX-License-Identifier: MIT
#
# doriax_resource_server.cmake - defines the doriax-resource-server standalone
# target: a local asset service that indexes a project's resource roots
# (assets/, lua/, scenes/, scripts/) with SHA-1 content hashes and answers the
# editor over newline-delimited JSON on 127.0.0.1 (see tools/resource-server/
# ResourceServer.h for the protocol).
#
# GUARD: This file expects to be include()d from the root CMakeLists.txt
# (include(${CMAKE_SOURCE_DIR}/tools/resource-server/doriax_resource_server.cmake)).
# It must not be add_subdirectory()d as a standalone project; like the
# doriax-reflector block it relies on the root project()'d compilers and
# standard settings.
#
# Deliberately links NOTHING but the C++17 standard library and, on Windows,
# ws2_32: no engine code, no editor code, no third-party dependencies, so it
# builds fast and independently of the rest of the suite.

if(TARGET doriax-resource-server)
    return()
endif()

set(DORIAX_RESOURCE_SERVER_DIR "${CMAKE_CURRENT_LIST_DIR}")

add_executable(doriax-resource-server
    "${DORIAX_RESOURCE_SERVER_DIR}/main.cpp"
    "${DORIAX_RESOURCE_SERVER_DIR}/JsonMini.cpp"
    "${DORIAX_RESOURCE_SERVER_DIR}/ResourceIndex.cpp"
    "${DORIAX_RESOURCE_SERVER_DIR}/ResourceServer.cpp"
    "${DORIAX_RESOURCE_SERVER_DIR}/Sha1.cpp"
)

set_target_properties(doriax-resource-server PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "tools"
)

if(WIN32)
    # WIN32_LEAN_AND_MEAN keeps <windows.h> from pulling in <winsock.h>, which
    # redefines sockaddr and friends against <winsock2.h> (this repo already
    # hit that cascade once - see engine/CMakeLists.txt around the `doriax`
    # target). The sources include <winsock2.h> before <windows.h>; NOMINMAX
    # avoids the min/max macros clashing with std::min/std::max in <algorithm>.
    target_compile_definitions(doriax-resource-server PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_link_libraries(doriax-resource-server PRIVATE ws2_32)
endif()

# Install beside the editor executables: editor/cli/ResourceServerClient.cpp
# looks for doriax-resource-server in the editor's own directory first.
install(TARGETS doriax-resource-server
        CONFIGURATIONS Release MinSizeRel
        RUNTIME DESTINATION bin
)
