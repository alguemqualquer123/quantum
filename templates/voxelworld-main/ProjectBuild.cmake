# Hand-written project build settings. The editor regenerates CMakeLists.txt on every build and
# includes this file last, with QUANTUM_TARGET and QUANTUM_SCRIPTS_DIR already set, so the same file
# serves the editor hotreload plugin, the standalone exe and the exported build.

target_include_directories(${QUANTUM_TARGET} PRIVATE "${QUANTUM_SCRIPTS_DIR}/scripts")

# The generated CMakeLists lists the engine-api include dirs by hand and the list lags behind the
# mirror (core/thread was missing, so a module that includes ThreadPoolManager.h could not see it).
# Every mirrored core subdirectory is a valid include root, so add them all instead of chasing the
# generator for one folder at a time.
file(GLOB VOXEL_ENGINE_API_DIRS RELATIVE "${QUANTUM_SCRIPTS_DIR}/.quantum/engine-api/core"
     "${QUANTUM_SCRIPTS_DIR}/.quantum/engine-api/core/*")
set(VOXEL_ENGINE_API_INCLUDES "")
foreach(dir ${VOXEL_ENGINE_API_DIRS})
    if(IS_DIRECTORY "${QUANTUM_SCRIPTS_DIR}/.quantum/engine-api/core/${dir}")
        list(APPEND VOXEL_ENGINE_API_INCLUDES "${QUANTUM_SCRIPTS_DIR}/.quantum/engine-api/core/${dir}")
    endif()
endforeach()
target_include_directories(${QUANTUM_TARGET} PRIVATE ${VOXEL_ENGINE_API_INCLUDES})

if(WIN32)
    # VoxelNet talks to Winsock directly: the engine has no socket API of its own.
    # PRIVATE because the generated CMakeLists already links this target with keyword
    # signature, and CMake refuses to mix the two forms for one target.
    target_link_libraries(${QUANTUM_TARGET} PRIVATE ws2_32)
endif()

if(MSVC)
    # The chunk generator and the mesher are long, table-driven functions; MSVC's 64k section limit
    # per object file is reachable once every block module lands in one translation unit.
    target_compile_options(${QUANTUM_TARGET} PRIVATE /bigobj)
endif()
