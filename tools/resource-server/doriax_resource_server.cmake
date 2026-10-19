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
# Deliberately links NOTHING but the C++17 standard library and, on Windows, the
# system libraries for its sockets, tray icon and plain Win32 connections window:
# no engine code, no editor code, no third-party dependencies (no imgui, no
# sokol), so it builds fast and independently of the rest of the suite.

if(TARGET doriax-resource-server)
    return()
endif()

set(DORIAX_RESOURCE_SERVER_DIR "${CMAKE_CURRENT_LIST_DIR}")

if(WIN32)
    # The tray icon outlives the window, so the executable carries the editor
    # brand icon and reads it back with LoadImageW() for both. Its own resource
    # because every suite executable installs to the same folder.
    doriax_add_windows_resource(
        DORIAX_RESOURCE_SERVER_ICON_OBJECTS
        doriax-resource-server
        "doriax-editor.ico"
        "doriax-resource-server.exe"
        "Doriax Resource Server"
    )
endif()

add_executable(doriax-resource-server
    "${DORIAX_RESOURCE_SERVER_DIR}/main.cpp"
    "${DORIAX_RESOURCE_SERVER_DIR}/Connections.cpp"
    "${DORIAX_RESOURCE_SERVER_DIR}/JsonMini.cpp"
    "${DORIAX_RESOURCE_SERVER_DIR}/ResourceIndex.cpp"
    "${DORIAX_RESOURCE_SERVER_DIR}/ResourceServer.cpp"
    "${DORIAX_RESOURCE_SERVER_DIR}/ServerWindow.cpp"
    "${DORIAX_RESOURCE_SERVER_DIR}/Sha1.cpp"
    "${DORIAX_RESOURCE_SERVER_DIR}/TrayIcon.cpp"
    ${DORIAX_RESOURCE_SERVER_ICON_OBJECTS}
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
    # ws2_32 serves the socket; shell32 is Shell_NotifyIconW and user32 the
    # window and tray window procedures; gdi32 + comctl32 draw the connections
    # window's lists; iphlpapi reads the TCP table that names the client process.
    target_link_libraries(doriax-resource-server PRIVATE
        ws2_32 shell32 user32 gdi32 comctl32 iphlpapi
    )
    # The window is DPI aware, so its 96-px layout is scaled by the real
    # logical DPI instead of being bitmap-stretched and unreadable.
    if(COMMAND doriax_add_windows_manifest)
        doriax_add_windows_manifest(doriax-resource-server)
    endif()
endif()

# Install beside the editor executables: editor/cli/ResourceServerClient.cpp
# looks for doriax-resource-server in the editor's own directory first.
install(TARGETS doriax-resource-server
        CONFIGURATIONS Release MinSizeRel
        RUNTIME DESTINATION bin
)
