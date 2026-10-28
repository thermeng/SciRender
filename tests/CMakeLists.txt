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

# The fvm fixtures need the solver's Mesh, so they only go into the suites
# that link FvmCore.
set(SCIRENDER_FVM_TEST_SUPPORT_SOURCES
    support/fvm_fixtures.cpp)

# scirender_add_test_suite(<target> SOURCES <files...> LIBRARIES <libs...>
#                          [REGISTRIES <files...>])
#
# Adds a test executable, wires up the shared support code, points it at the
# repository's sample fixtures and registers every test case with CTest.
# REGISTRIES lists known-failure files: the listed test cases are registered
# with WILL_FAIL TRUE so they pass while the defect is open (see
# cmake/DiscoverDoctestTests.cmake).
function(scirender_add_test_suite target)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES;REGISTRIES" "" ${ARGN})

    add_executable(${target}
        ${ARG_SOURCES}
        ${SCIRENDER_TEST_SUPPORT_SOURCES})

    target_link_libraries(${target} PRIVATE ${ARG_LIBRARIES} doctest::doctest)

    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

    target_compile_features(${target} PRIVATE cxx_std_20)

    target_compile_definitions(${target} PRIVATE
        SCIRENDER_SAMPLES_DIR="${SCIRENDER_SAMPLES_DIR}"
        SCIRENDER_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

    scirender_discover_tests(${target} REGISTRIES ${ARG_REGISTRIES})
endfunction()

# ---------------------------------------------------------------------------
# core — parsers, field store, post-processing, isosurface, camera math
# ---------------------------------------------------------------------------
scirender_add_test_suite(scirender_core_tests
    SOURCES
        core/parse_test.cpp
        core/isosurface_test.cpp
        core/cell_topology_test.cpp
        core/unstructured_volume_test.cpp
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
        fvm/face_state_test.cpp
        fvm/transient_settings_test.cpp
        fvm/ghia_cavity_test.cpp
        fvm/cavity_msh_test.cpp
        fvm/channel_test.cpp
        cfd/mesh_flatplate_debug_test.cpp
        cfd/mesh_flatplate_run_test.cpp
        cfd/turbulent_flatplate_benchmark_test.cpp
        fvm/turbulent_flat_plate_test.cpp
        fvm/sst_test.cpp
        fvm/channel_3d_test.cpp
        fvm/transient_channel_test.cpp
        fvm/cancellable_test.cpp
    LIBRARIES
        FvmCore
        CfdLib
        CoreLib
    REGISTRIES
        "${CMAKE_CURRENT_SOURCE_DIR}/fvm/known_failures.txt")

# ---------------------------------------------------------------------------
# render — CPU-side render helpers (no GL context required)
# ---------------------------------------------------------------------------
scirender_add_test_suite(scirender_render_tests
    SOURCES
        render/streamline_test.cpp
        render/lic_vector_texture_test.cpp
    LIBRARIES
        RenderCore
        CoreLib
        glad)

# ---------------------------------------------------------------------------
# ui — the Qt-only controller: .scfd round-trip, continuation and 3D solve.
#
# Guarded on SCIRENDER_BUILD_APP because the Qt-free `tests` preset does not
# find Qt at all. The binary is linked into the same directory as the deployed
# Qt DLLs, so discovery and ctest can start it without extra environment.
# ---------------------------------------------------------------------------
if(SCIRENDER_BUILD_APP)
    scirender_add_test_suite(scirender_ui_tests
        SOURCES
            ui/scfd_roundtrip_test.cpp
            ui/cfd_3d_mesh_test.cpp
            ui/unstructured_render_test.cpp
            ${PROJECT_SOURCE_DIR}/src/ui/controllers/cfd_controller.cpp
            ${PROJECT_SOURCE_DIR}/src/ui/controllers/cfd_controller.h
        LIBRARIES
            CfdLib
            FvmCore
            CoreLib
            Qt6::Core)
    # cfd_controller.h declares Q_OBJECT; there is no other target that builds
    # it into a test binary, so AUTOMOC has to run here.
    set_target_properties(scirender_ui_tests PROPERTIES AUTOMOC ON)
endif()

# ---------------------------------------------------------------------------
# cfd — the CFD fundamentals suite
#
# One test case per operator behaviour. The cases that encode a contract the
# solver does not yet honour are listed in cfd/known_failures.txt: they are
# registered with WILL_FAIL TRUE, so the suite stays green while the defects
# are open, and each one turns red as soon as the solver is fixed.
#
#   ctest --test-dir build-tests -L known-broken --output-on-failure
#
# lists the outstanding work with the real diagnostic of every defect.
# ---------------------------------------------------------------------------
scirender_add_test_suite(scirender_cfd_tests
    SOURCES
        cfd/mesh_geometry_test.cpp
        cfd/gradient_test.cpp
        cfd/divergence_test.cpp
        cfd/poisson_test.cpp
        cfd/convection_test.cpp
        cfd/momentum_convection_test.cpp
        cfd/pressure_correction_test.cpp
        cfd/velocity_correction_test.cpp
        cfd/boundary_conditions_test.cpp
        cfd/residual_test.cpp
        cfd/debug_coupling_test.cpp
        cfd/mesh_adapter_test.cpp
        ${SCIRENDER_FVM_TEST_SUPPORT_SOURCES}
    LIBRARIES
        FvmCore
        CfdLib
        CoreLib
    REGISTRIES
        "${CMAKE_CURRENT_SOURCE_DIR}/cfd/known_failures.txt")
