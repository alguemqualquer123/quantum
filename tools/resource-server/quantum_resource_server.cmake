# (c) SR VINIX
# SPDX-License-Identifier: MIT
#
# quantum_resource_server.cmake - defines the quantum-resource-server standalone
# target: a local asset service that indexes a project's resource roots
# (assets/, lua/, scenes/, scripts/) with SHA-1 content hashes and answers the
# editor over newline-delimited JSON on 127.0.0.1 (see tools/resource-server/
# ResourceServer.h for the protocol).
#
# GUARD: This file expects to be include()d from the root CMakeLists.txt
# (include(${CMAKE_SOURCE_DIR}/tools/resource-server/quantum_resource_server.cmake)).
# It must not be add_subdirectory()d as a standalone project; like the
# quantum-reflector block it relies on the root project()'d compilers and
# standard settings.
#
# Deliberately links NOTHING but the C++17 standard library and, on Windows, the
# system libraries for its sockets, tray icon and plain Win32 connections window:
# no engine code, no editor code, no third-party dependencies (no imgui, no
# sokol), so it builds fast and independently of the rest of the suite.

if(TARGET quantum-resource-server)
    return()
endif()

set(QUANTUM_RESOURCE_SERVER_DIR "${CMAKE_CURRENT_LIST_DIR}")

if(WIN32)
    # The tray icon outlives the window, so the executable carries the editor
    # brand icon and reads it back with LoadImageW() for both. Its own resource
    # because every suite executable installs to the same folder.
    quantum_add_windows_resource(
        QUANTUM_RESOURCE_SERVER_ICON_OBJECTS
        quantum-resource-server
        "quantum-editor.ico"
        "quantum-resource-server.exe"
        "Quantum Resource Server"
    )
endif()

add_executable(quantum-resource-server
    "${QUANTUM_RESOURCE_SERVER_DIR}/main.cpp"
    "${QUANTUM_RESOURCE_SERVER_DIR}/Connections.cpp"
    "${QUANTUM_RESOURCE_SERVER_DIR}/JsonMini.cpp"
    "${QUANTUM_RESOURCE_SERVER_DIR}/ResourceIndex.cpp"
    "${QUANTUM_RESOURCE_SERVER_DIR}/ResourceServer.cpp"
    "${QUANTUM_RESOURCE_SERVER_DIR}/ServerWindow.cpp"
    "${QUANTUM_RESOURCE_SERVER_DIR}/Sha1.cpp"
    "${QUANTUM_RESOURCE_SERVER_DIR}/TrayIcon.cpp"
    ${QUANTUM_RESOURCE_SERVER_ICON_OBJECTS}
)

set_target_properties(quantum-resource-server PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "tools"
)

if(WIN32)
    # WIN32_LEAN_AND_MEAN keeps <windows.h> from pulling in <winsock.h>, which
    # redefines sockaddr and friends against <winsock2.h> (this repo already
    # hit that cascade once - see engine/CMakeLists.txt around the `quantum`
    # target). The sources include <winsock2.h> before <windows.h>; NOMINMAX
    # avoids the min/max macros clashing with std::min/std::max in <algorithm>.
    target_compile_definitions(quantum-resource-server PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    # ws2_32 serves the socket; shell32 is Shell_NotifyIconW and user32 the
    # window and tray window procedures; gdi32 + comctl32 draw the connections
    # window's lists; iphlpapi reads the TCP table that names the client process.
    target_link_libraries(quantum-resource-server PRIVATE
        ws2_32 shell32 user32 gdi32 comctl32 iphlpapi
    )
    # The window is DPI aware, so its 96-px layout is scaled by the real
    # logical DPI instead of being bitmap-stretched and unreadable.
    if(COMMAND quantum_add_windows_manifest)
        quantum_add_windows_manifest(quantum-resource-server)
    endif()
endif()

# Install beside the editor executables: editor/cli/ResourceServerClient.cpp
# looks for quantum-resource-server in the editor's own directory first.
install(TARGETS quantum-resource-server
        CONFIGURATIONS Release MinSizeRel
        RUNTIME DESTINATION bin
)
