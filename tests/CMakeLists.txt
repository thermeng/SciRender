# ---------------------------------------------------------------------------
# Test suite.
#
# A subdirectory of the root project: the tests link the real CoreLib /
# FvmCore / CfdLib / RenderCore targets, so there is exactly one copy of the
# production sources in the build graph and the suite can never drift from
# the application.
#
# Every test binary is discovered by CTest with one row per doctest test case
# (see cmake/DiscoverDoctestTests.cmake).
# ---------------------------------------------------------------------------

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DiscoverDoctestTests.cmake")

file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/samples" SCIRENDER_SAMPLES_DIR)

set(SCIRENDER_TEST_SUPPORT_SOURCES
    support/doctest_main.cpp
    support/test_support.cpp
    support/mesh_fixtures.cpp)

# scirender_add_test_suite(<target> SOURCES <files...> LIBRARIES <libs...>)
#
# Adds a test executable, wires up the shared support code, points it at the
# repository's sample fixtures and registers every test case with CTest.
function(scirender_add_test_suite target)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES" "" ${ARGN})

    add_executable(${target}
        ${ARG_SOURCES}
        ${SCIRENDER_TEST_SUPPORT_SOURCES})

    target_link_libraries(${target} PRIVATE ${ARG_LIBRARIES} doctest::doctest)

    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

    target_compile_features(${target} PRIVATE cxx_std_20)

    target_compile_definitions(${target} PRIVATE
        SCIRENDER_SAMPLES_DIR="${SCIRENDER_SAMPLES_DIR}"
        SCIRENDER_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

    scirender_discover_tests(${target})
endfunction()

# ---------------------------------------------------------------------------
# core — parsers, field store, post-processing, isosurface, camera math
# ---------------------------------------------------------------------------
scirender_add_test_suite(scirender_core_tests
    SOURCES
        core/parse_test.cpp
        core/isosurface_test.cpp
        core/pvd_test.cpp
        core/field_names_test.cpp
        core/anim_range_test.cpp
        core/post_fields_test.cpp
        core/post_fields_visualization_test.cpp
        core/camera_math_test.cpp
    LIBRARIES
        CoreLib)

# ---------------------------------------------------------------------------
# fvm — absorbed solver, AMG configuration, transient settings, Ghia benchmark
#
# The Ghia cavity case is named "slow.*" so that it carries the "slow" CTest
# label and can be excluded with `ctest -LE slow`.
# ---------------------------------------------------------------------------
scirender_add_test_suite(scirender_fvm_tests
    SOURCES
        fvm/amg_config_test.cpp
        fvm/transient_settings_test.cpp
        fvm/ghia_cavity_test.cpp
    LIBRARIES
        FvmCore
        CfdLib
        CoreLib)

# ---------------------------------------------------------------------------
# render — CPU-side render helpers (no GL context required)
# ---------------------------------------------------------------------------
scirender_add_test_suite(scirender_render_tests
    SOURCES
        render/streamline_test.cpp
    LIBRARIES
        RenderCore
        CoreLib
        glad)
