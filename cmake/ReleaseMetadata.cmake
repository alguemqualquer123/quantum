# (c) SR VINIX
# SPDX-License-Identifier: MIT

# Keep release identity in one place so desktop packages, CI artifacts and
# downstream launchers agree on the same version and platform label.
if(NOT DEFINED QUANTUM_RELEASE_CHANNEL)
    set(QUANTUM_RELEASE_CHANNEL "stable" CACHE STRING "Release channel name")
endif()
set_property(CACHE QUANTUM_RELEASE_CHANNEL PROPERTY STRINGS stable beta nightly)

set(QUANTUM_PACKAGE_VERSION "0.0.0")
if(QUANTUMEDITOR_VERSION MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(QUANTUM_PACKAGE_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
elseif(QUANTUMEDITOR_VERSION MATCHES "^v?([0-9]+)\\.([0-9]+)")
    set(QUANTUM_PACKAGE_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.0")
endif()

if(CMAKE_SYSTEM_PROCESSOR)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" QUANTUM_PACKAGE_ARCH)
else()
    set(QUANTUM_PACKAGE_ARCH "unknown")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(QUANTUM_PACKAGE_PLATFORM "macos")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(QUANTUM_PACKAGE_PLATFORM "windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(QUANTUM_PACKAGE_PLATFORM "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(QUANTUM_PACKAGE_PLATFORM "android")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(QUANTUM_PACKAGE_PLATFORM "web")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(QUANTUM_PACKAGE_PLATFORM "ios")
else()
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" QUANTUM_PACKAGE_PLATFORM)
endif()

set(QUANTUM_PACKAGE_NAME
    "quantum-${QUANTUMEDITOR_VERSION}-${QUANTUM_PACKAGE_PLATFORM}-${QUANTUM_PACKAGE_ARCH}")

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/ReleaseManifest.json.in"
    "${CMAKE_BINARY_DIR}/release-manifest.json"
    @ONLY
)

# Multi-config generators (notably Visual Studio and Xcode) choose the
# configuration at install time, after this configure step has completed.
# Generate the installed manifest from a small install-time script so a
# Release install cannot inherit the first configured Debug value.
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/InstallReleaseManifest.cmake.in"
    "${CMAKE_BINARY_DIR}/InstallReleaseManifest.cmake"
    @ONLY
)
