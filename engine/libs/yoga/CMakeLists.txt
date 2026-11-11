# Yoga v2.0.1 - https://github.com/facebook/yoga (MIT)
#
# Flexbox layout engine used by the retained-mode UI module (engine/core/ui).
# Only the core library is vendored; the tests, bindings and samples of the
# upstream tree are not copied. C++14 sources compiled with the engine standard.

file(GLOB_RECURSE YOGA_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/yoga/*.cpp"
)

add_library(
    yoga
    STATIC
    ${YOGA_SOURCES}
)

target_include_directories(yoga PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
